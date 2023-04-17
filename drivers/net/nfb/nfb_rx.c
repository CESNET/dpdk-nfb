/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2019 Cesnet
 * Copyright(c) 2019 Netcope Technologies, a.s. <info@netcope.com>
 * All rights reserved.
 */

#include <rte_kvargs.h>

#include "nfb_rx.h"
#include "nfb.h"

uint64_t nfb_timestamp_rx_dynflag;
int nfb_timestamp_dynfield_offset = -1;

int nfb_ndp_df_header_offset;
int nfb_ndp_df_header_length;
int nfb_ndp_df_flags;
uint64_t nfb_ndp_df_header_vld;

int nfb_ndp_df_header_enable = 0;

int
nfb_eth_rx_queue_start(struct rte_eth_dev *dev, uint16_t rxq_id)
{
	struct ndp_rx_queue *rxq = dev->data->rx_queues[rxq_id];
	int ret = 0;

	if (rxq->queue_driver == NFB_QUEUE_DRIVER_NATIVE) {
		ret = nfb_ndp_rx_queue_start(dev, rxq);
	} else {
		if (rxq->queue == NULL) {
			RTE_LOG(ERR, PMD, "RX NDP queue is NULL!\n");
			return -EINVAL;
		}

		ret = ndp_queue_start(rxq->queue);
	}

	if (ret == 0)
		rxq->state = RTE_ETH_QUEUE_STATE_STARTED;

	return ret;
}

int
nfb_eth_rx_queue_stop(struct rte_eth_dev *dev, uint16_t rxq_id)
{
	struct ndp_rx_queue *rxq = dev->data->rx_queues[rxq_id];
	int ret = 0;

	if (rxq->state == RTE_ETH_QUEUE_STATE_STOPPED)
		return 0;

	if (rxq->queue_driver == NFB_QUEUE_DRIVER_NATIVE) {
		ret = nfb_ndp_rx_queue_stop(dev, rxq);
	} else {
		if (rxq->queue == NULL) {
			RTE_LOG(ERR, PMD, "RX NDP queue is NULL!\n");
			return -EINVAL;
		}

		ret = ndp_queue_stop(rxq->queue);
	}

	if (ret == 0)
		rxq->state = RTE_ETH_QUEUE_STATE_STOPPED;

	return ret;
}

int
nfb_eth_rx_queue_setup(struct rte_eth_dev *dev,
		uint16_t rx_queue_id,
		uint16_t nb_rx_desc __rte_unused,
		unsigned int socket_id,
		const struct rte_eth_rxconf *rx_conf __rte_unused,
		struct rte_mempool *mb_pool)
{
	struct pmd_internals *internals = dev->process_private;

	struct ndp_rx_queue *rxq;
	int nfb_qid;
	int ret;

	int nmp;
	if (rx_conf->rx_nmempool > 0) {
		nmp = rx_queue_id % rx_conf->rx_nmempool;
		mb_pool = rx_conf->rx_mempools[nmp];
	}
	rxq = rte_zmalloc_socket("ndp rx queue",
			sizeof(struct ndp_rx_queue),
			RTE_CACHE_LINE_SIZE, socket_id);

	if (rxq == NULL) {
		RTE_LOG(ERR, PMD, "rte_zmalloc_socket() failed for rx queue id "
				"%" PRIu16 "!\n", rx_queue_id);
		return -ENOMEM;
	}

	rxq->flags = 0;

	if (internals->flags & NFB_QUEUE_DRIVER_NDP_SHARED) {
		rxq->queue_driver = NFB_QUEUE_DRIVER_NDP_SHARED;
	} else {
		rxq->queue_driver = NFB_QUEUE_DRIVER_NATIVE;
	}

	/* nfb queue id doesn't neccessary corresponds to txq_id */
	nfb_qid = internals->queue_map_rx[rx_queue_id];

	ret = nfb_eth_rx_queue_init(dev, nfb_qid, nb_rx_desc, socket_id, rx_conf,
		dev->data->port_id, mb_pool, rxq);
	if (ret)
		goto err_queue_init;

	dev->data->rx_queues[rx_queue_id] = rxq;
	return 0;

err_queue_init:
	rte_free(rxq);
	return ret;
}

int
nfb_eth_rx_queue_init(struct rte_eth_dev *dev,
		uint16_t rx_queue_id,
		uint16_t nb_rx_desc,
		unsigned int socket_id,
		const struct rte_eth_rxconf *rx_conf,
		uint16_t port_id,
		struct rte_mempool *mb_pool,
		struct ndp_rx_queue *rxq)
{
	int ret;
	const struct rte_pktmbuf_pool_private *mbp_priv =
		rte_mempool_get_priv(mb_pool);

	struct nfb_fdt_packed_item pi;
	int off;

	struct pmd_internals *priv = dev->process_private;

	if (rxq->queue_driver == NFB_QUEUE_DRIVER_NATIVE) {
		ret = nfb_ndp_rx_queue_setup(dev, rx_queue_id, nb_rx_desc, socket_id, rx_conf, mb_pool, rxq);
		if (ret)
			return ret;
	} else if (rxq->queue_driver == NFB_QUEUE_DRIVER_NDP_SHARED) {
		rxq->queue = ndp_open_rx_queue(priv->nfb, rx_queue_id);
		if (rxq->queue == NULL)
			return -EINVAL;
	} else {
		return -EINVAL;
	}

	rxq->nfb = priv->nfb;
	rxq->rx_queue_id = rx_queue_id;
	rxq->in_port = port_id;
	rxq->mb_pool = mb_pool;
	rxq->buf_size = (uint16_t)(mbp_priv->mbuf_data_room_size - RTE_PKTMBUF_HEADROOM);

	rxq->rx_pkts = 0;
	rxq->rx_bytes = 0;
	rxq->err_pkts = 0;

	rxq->timestamp_off = -1;
	rxq->timestamp_vld_off = -1;
	rxq->timestamp_vld_val = 1;
	rxq->timestamp_vld_mask = 0;
	rxq->timestamp_hdr_minlen = -1;

	rxq->flow_hash_off = -1;
	rxq->vlan_tci_off = -1;
	rxq->vlan_vld_off = -1;
	rxq->vlan_stripped_off = -1;
	rxq->l3_csum_status_off = -1;
	rxq->l4_csum_status_off = -1;
	rxq->ptype_off = -1;

	/* FIXME: checking only header ID 0 */
	off = ndp_header_fdt_node_offset(nfb_get_fdt(priv->nfb), 0, 0);
	if (off >= 0 && nfb_timestamp_dynfield_offset >= 0) {
		pi = nfb_fdt_packed_item_by_name(nfb_get_fdt(priv->nfb), off, "timestamp");
		if (/*pi.offset >= 0 && */pi.width == 64 && pi.offset % 64 == 0)
			rxq->timestamp_off = pi.offset / 8;

		pi = nfb_fdt_packed_item_by_name(nfb_get_fdt(priv->nfb), off, "timestamp.vld");
		if (/*pi.offset >= 0 && */pi.width == 1) {
			rxq->timestamp_vld_off = pi.offset / 8;
			rxq->timestamp_vld_mask = (1 << (pi.offset % 8));
			rxq->timestamp_vld_val  = (1 << (pi.offset % 8));
		} else { /* alternatively, a bit 31 of timestamp can be used as a non-valid flag */
			rxq->timestamp_vld_val = 0;
			rxq->timestamp_vld_off = rxq->timestamp_off + 3;
			rxq->timestamp_vld_mask = (1 << 7);
		}

		rxq->timestamp_hdr_minlen = rxq->timestamp_off + 8;
		if (rxq->timestamp_vld_off + 1 > rxq->timestamp_hdr_minlen)
			rxq->timestamp_hdr_minlen = rxq->timestamp_vld_off + 1;
	}

	pi = nfb_fdt_packed_item_by_name(nfb_get_fdt(priv->nfb), off, "flow.hash");
	if (pi.width == 64 && pi.offset % 8 == 0) {
		rxq->flow_hash_off = pi.offset / 8;
	}

	pi = nfb_fdt_packed_item_by_name(nfb_get_fdt(priv->nfb), off, "vlan.tci");
	if (pi.width == 16 && pi.offset % 8 == 0) {
		rxq->vlan_tci_off = pi.offset / 8;
	}

	pi = nfb_fdt_packed_item_by_name(nfb_get_fdt(priv->nfb), off, "vlan.vld");
	if (pi.width == 1) {
		rxq->vlan_vld_off = pi.offset / 8;
		rxq->vlan_vld_shift = (pi.offset % 8);
	}

	pi = nfb_fdt_packed_item_by_name(nfb_get_fdt(priv->nfb), off, "vlan.stripped");
	if (pi.width == 1) {
		rxq->vlan_stripped_off = pi.offset / 8;
		rxq->vlan_stripped_shift = (pi.offset % 8);
	}

	pi = nfb_fdt_packed_item_by_name(nfb_get_fdt(priv->nfb), off, "l3.csum_status");
	if (pi.width == 2) {
		rxq->l3_csum_status_off = pi.offset / 8;
		rxq->l3_csum_status_shift = (pi.offset % 8);
	}

	pi = nfb_fdt_packed_item_by_name(nfb_get_fdt(priv->nfb), off, "l4.csum_status");
	if (pi.width == 2) {
		rxq->l4_csum_status_off = pi.offset / 8;
		rxq->l4_csum_status_shift = (pi.offset % 8);
	}

	pi = nfb_fdt_packed_item_by_name(nfb_get_fdt(priv->nfb), off, "l2.ptype");
	if (pi.width == 4 && pi.offset % 8 == 0) {
		rxq->ptype_off = pi.offset / 8;
	}

	return 0;
}

void
nfb_eth_rx_queue_release(struct rte_eth_dev *dev, uint16_t qid)
{
	struct ndp_rx_queue *rxq = dev->data->rx_queues[qid];

	if (rxq->queue_driver == NFB_QUEUE_DRIVER_NATIVE) {
		nfb_ndp_rx_queue_release(dev, rxq);
	} else if (rxq->queue_driver == NFB_QUEUE_DRIVER_NDP_SHARED) {
		/* FIXME: free rxq */
		if (rxq->queue != NULL) {
			ndp_close_rx_queue(rxq->queue);
			rxq->queue = NULL;
			rte_free(rxq);
		}
	}
}
