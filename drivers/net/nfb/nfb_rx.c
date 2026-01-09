/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2019 Cesnet
 * Copyright(c) 2019 Netcope Technologies, a.s. <info@netcope.com>
 * All rights reserved.
 */

#include <rte_kvargs.h>

#include "nfb.h"
#include "nfb_rx.h"

uint64_t nfb_timestamp_rx_dynflag;
int nfb_timestamp_dynfield_offset = -1;

int nfb_ndp_df_header_offset;
int nfb_ndp_df_header_length;
int nfb_ndp_df_flags;
uint64_t nfb_ndp_df_header_vld;

int
nfb_eth_rx_queue_start(struct rte_eth_dev *dev, uint16_t rxq_id)
{
	struct ndp_rx_queue *rxq = dev->data->rx_queues[rxq_id];
	int ret;

	if (rxq->queue == NULL) {
		NFB_LOG(ERR, "RX NDP queue is NULL");
		return -EINVAL;
	}

	ret = ndp_queue_start(rxq->queue);
	if (ret != 0)
		goto err;
	dev->data->rx_queue_state[rxq_id] = RTE_ETH_QUEUE_STATE_STARTED;
	return 0;

err:
	return -EINVAL;
}

int
nfb_eth_rx_queue_stop(struct rte_eth_dev *dev, uint16_t rxq_id)
{
	struct ndp_rx_queue *rxq = dev->data->rx_queues[rxq_id];
	int ret;

	if (rxq->queue == NULL) {
		NFB_LOG(ERR, "RX NDP queue is NULL");
		return -EINVAL;
	}

	ret = ndp_queue_stop(rxq->queue);
	if (ret != 0)
		return -EINVAL;

	dev->data->rx_queue_state[rxq_id] = RTE_ETH_QUEUE_STATE_STOPPED;
	return 0;
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
	struct pmd_priv *priv = dev->data->dev_private;

	int ret;
	int qid;
	struct ndp_rx_queue *rxq;

	if (rx_queue_id >= priv->max_rx_queues)
		return -EINVAL;

	rxq = rte_zmalloc_socket("ndp rx queue", sizeof(struct ndp_rx_queue),
			RTE_CACHE_LINE_SIZE, socket_id);

	if (rxq == NULL) {
		NFB_LOG(ERR, "rte_zmalloc_socket() failed for rx queue id %" PRIu16,
			rx_queue_id);
		return -ENOMEM;
	}

	rxq->df_header_enable = priv->flags & NFB_FLAG_NDP_DF_HEADER ? 1 : 0;

	qid = priv->queue_map_rx[rx_queue_id];

	ret = nfb_eth_rx_queue_init(internals->nfb, qid, dev->data->port_id, mb_pool, rxq);
	if (ret)
		goto err_queue_init;

	dev->data->rx_queues[rx_queue_id] = rxq;
	return 0;

err_queue_init:
	rte_free(rxq);
	return ret;
}

static inline void him_update(uint16_t *offset, uint16_t *width,
		int16_t new_offset, int16_t new_width)
{
	/* new_width is in bytes */
	if (new_offset + new_width > *offset + *width) {
		*offset = new_offset;
		*width = new_width;
	}
}

int
nfb_eth_rx_queue_init(struct nfb_device *nfb,
		int qid,
		uint16_t port_id,
		struct rte_mempool *mb_pool,
		struct ndp_rx_queue *rxq)
{
	int i;
	const struct rte_pktmbuf_pool_private *mbp_priv =
		rte_mempool_get_priv(mb_pool);

	struct nfb_fdt_packed_item pi;
	int off;

	uint16_t him_o; /* header item maximal offset */
	uint16_t him_w; /* header item maximal width */

	const void *fdt;
	struct ndp_rx_offload_parser *ofp;

	if (nfb == NULL)
		return -EINVAL;

	rxq->queue = ndp_open_rx_queue(nfb, qid);
	if (rxq->queue == NULL)
		return -EINVAL;

	fdt = nfb_get_fdt(nfb);

	rxq->nfb = nfb;
	rxq->in_port = port_id;
	rxq->mb_pool = mb_pool;
	rxq->buf_size = (uint16_t)(mbp_priv->mbuf_data_room_size -
		RTE_PKTMBUF_HEADROOM);

	rxq->rx_pkts = 0;
	rxq->rx_bytes = 0;
	rxq->err_pkts = 0;

	for (i = 0; i < NDP_RXHDR_CNT; i++) {
		ofp = &rxq->ofp[i];
		him_o = 0;
		him_w = 0;

		ofp->hdr_minlen = -1;

		ofp->timestamp_off = -1;
		ofp->timestamp_vld_off = -1;
		ofp->timestamp_vld_val = 1;
		ofp->timestamp_vld_mask = 0;

		ofp->flow_hash_off = -1;
		ofp->vlan_tci_off = -1;
		ofp->vlan_vld_off = -1;
		ofp->vlan_stripped_off = -1;
		ofp->l3_csum_status_off = -1;
		ofp->l4_csum_status_off = -1;
		ofp->ptype_off = -1;

		off = ndp_header_fdt_node_offset(fdt, 0, i);
		if (off < 0)
			continue;

		pi = nfb_fdt_packed_item_by_name(fdt, off, "timestamp");
		if (pi.width == 64 && pi.offset % 64 == 0 &&
				nfb_timestamp_dynfield_offset >= 0) {
			ofp->timestamp_off = pi.offset / 8;

			/* bit 31 of the timestamp can be used as a non-valid flag ...*/
			ofp->timestamp_vld_off = ofp->timestamp_off + 3;
			ofp->timestamp_vld_mask = (1 << 7);
			ofp->timestamp_vld_val = 0;

			him_update(&him_o, &him_w, ofp->timestamp_off, 8);

			/* ... unless the timestamp.vld is explicitly present in header */
			pi = nfb_fdt_packed_item_by_name(fdt, off, "timestamp.vld");
			if (pi.width == 1) {
				ofp->timestamp_vld_off = pi.offset / 8;
				ofp->timestamp_vld_mask = (1 << (pi.offset % 8));
				ofp->timestamp_vld_val  = (1 << (pi.offset % 8));
				him_update(&him_o, &him_w, ofp->timestamp_vld_off, 1);
			}
		}

		pi = nfb_fdt_packed_item_by_name(fdt, off, "flow.hash");
		if (pi.width == 64 && pi.offset % 8 == 0) {
			/* INFO: only 32 bits are filled into mbuf */
			ofp->flow_hash_off = pi.offset / 8;
			him_update(&him_o, &him_w, ofp->flow_hash_off, 8);
		} else {
			pi = nfb_fdt_packed_item_by_name(fdt, off, "rss.hash");
			if (pi.width == 32 && pi.offset % 8 == 0) {
				ofp->flow_hash_off = pi.offset / 8;
				him_update(&him_o, &him_w, ofp->flow_hash_off, 4);
			}
		}

		pi = nfb_fdt_packed_item_by_name(fdt, off, "vlan.vld");
		if (pi.width == 1) {
			ofp->vlan_vld_off = pi.offset / 8;
			ofp->vlan_vld_shift = (pi.offset % 8);
		}

		pi = nfb_fdt_packed_item_by_name(fdt, off, "vlan.tci");
		if (pi.width == 16 && pi.offset % 8 == 0)
			ofp->vlan_tci_off = pi.offset / 8;

		pi = nfb_fdt_packed_item_by_name(fdt, off, "vlan.stripped");
		if (pi.width == 1) {
			ofp->vlan_stripped_off = pi.offset / 8;
			ofp->vlan_stripped_shift = (pi.offset % 8);
			him_update(&him_o, &him_w, ofp->vlan_stripped_off, 1);
		}

		if (ofp->vlan_vld_off == -1 || ofp->vlan_tci_off == -1) {
			ofp->vlan_vld_off = -1;
			ofp->vlan_tci_off = -1;
		} else {
			him_update(&him_o, &him_w, ofp->vlan_vld_off, 1);
			him_update(&him_o, &him_w, ofp->vlan_tci_off, 2);
		}

		pi = nfb_fdt_packed_item_by_name(fdt, off, "l3.csum_status");
		if (pi.width == 2) {
			ofp->l3_csum_status_off = pi.offset / 8;
			ofp->l3_csum_status_shift = (pi.offset % 8);
			him_update(&him_o, &him_w, ofp->l3_csum_status_off, 1);
		}

		pi = nfb_fdt_packed_item_by_name(fdt, off, "l4.csum_status");
		if (pi.width == 2) {
			ofp->l4_csum_status_off = pi.offset / 8;
			ofp->l4_csum_status_shift = (pi.offset % 8);
			him_update(&him_o, &him_w, ofp->l4_csum_status_off, 1);
		}

		pi = nfb_fdt_packed_item_by_name(fdt, off, "l2.ptype");
		if (pi.width == 4 && pi.offset % 8 == 0) {
			ofp->ptype_off = pi.offset / 8;
			him_update(&him_o, &him_w, ofp->ptype_off, 1);
		}

		ofp->hdr_minlen = him_o + him_w;
	}
	return 0;
}

void
nfb_eth_rx_queue_release(struct rte_eth_dev *dev, uint16_t qid)
{
	struct ndp_rx_queue *rxq = dev->data->rx_queues[qid];

	if (rxq->queue != NULL) {
		ndp_close_rx_queue(rxq->queue);
		rxq->queue = NULL;
		rte_free(rxq);
	}
}
