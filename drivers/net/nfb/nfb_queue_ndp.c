/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 CESNET
 * All rights reserved.
 */

#include <rte_mbuf.h>
#include <rte_mbuf_dyn.h>
#include <rte_cycles.h>

#include <ethdev_pci.h>

#include <nfb/nfb.h>

typedef rte_iova_t dma_addr_t;
#include <netcope/dma_ctrl_ndp.h>

#include "nfb.h"
#include "nfb_rx.h"
#include "nfb_tx.h"

#define NFB_NDP_PKT_BURST 64

struct ndp_ctrl {
	struct nc_ndp_ctrl c;
	struct rte_mbuf *local_mbufs[NFB_NDP_PKT_BURST];
	union {
		uint32_t php; /* RX: processed header pointers */
		uint32_t fdp; /* TX: freed descriptor pointers */
	};
	uint32_t tu_min;
	uint32_t tu_max;
};

#define HW_BUFFER_ALIGN 4096

static int nfb_ndp_ctrl_fill_rx_descs(struct ndp_rx_queue *q);
static int ndp_ctrl_tx_free_mbufs(struct ndp_tx_queue *q);

/**
 * DPDK callback for RX.
 *
 * @param dpdk_rxq
 *   Generic pointer to RX queue structure.
 * @param[out] bufs
 *   Array to store received packets.
 * @param nb_pkts
 *   Maximum number of packets in array.
 *
 * @return
 *   Number of packets successfully received (<= nb_pkts).
 */
uint16_t
nfb_ndp_queue_rx(void *queue, struct rte_mbuf **bufs, uint16_t nb_pkts)
{
	struct ndp_rx_queue *q = queue;
	struct ndp_ctrl *ctrl = q->ctrl;
	struct rte_mbuf *mbuf;

	uint64_t num_bytes = 0;

	struct nc_ndp_hdr *hdrs = q->mz_hdr->addr;
	struct nc_ndp_hdr *hdr;
	unsigned int i;
	uint16_t nb_rx;
	uint32_t count;
	uint32_t shp = ctrl->c.shp;
	uint32_t mhp = ctrl->c.mhp;

	uint8_t hdr_id;

	nc_ndp_ctrl_hhp_update(&ctrl->c);
	count = (ctrl->c.hhp - shp) & mhp;
	if (nb_pkts < count)
		count = nb_pkts;

	nb_rx = count;

	if (!nb_rx) {
		i = 0;
		while (nfb_ndp_ctrl_fill_rx_descs(q))
			i = 1;
		if (i)
			nc_ndp_ctrl_sdp_flush(&ctrl->c);
		return 0;
	}

	for (i = 0; i < nb_rx; ++i) {
		mbuf = q->mbufs[shp];
		hdr = &hdrs[shp];

		mbuf->data_len = hdr->frame_len;
		mbuf->pkt_len = mbuf->data_len;
		mbuf->port = q->in_port;
		mbuf->ol_flags = 0;

		if (q->df_header_enable) {
			nfb_eth_ndp_rx_df_header_fill(mbuf, hdr->hdr_len, hdr->meta);
		}

		hdr_id = hdr->meta & 0x03;
		nfb_rx_fetch_fields(q, mbuf, hdr_id, hdr->hdr_len,
				rte_pktmbuf_mtod(mbuf, unsigned char*));

		rte_pktmbuf_adj(mbuf, hdr->hdr_len);

		num_bytes += mbuf->pkt_len;
		bufs[i] = mbuf;
		shp = (shp + 1) & mhp;
	}

	q->rx_pkts += nb_rx;
	q->rx_bytes += num_bytes;

	ctrl->c.shp = shp;
	while (nfb_ndp_ctrl_fill_rx_descs(q))
		;
	nc_ndp_ctrl_sp_flush(&ctrl->c);

	return nb_rx;
}

static int nfb_ndp_ctrl_fill_rx_descs(struct ndp_rx_queue *q)
{
	uint32_t i;
	uint32_t free_desc, free_hdrs, count;

	rte_iova_t iova;
	struct ndp_ctrl *ctrl = q->ctrl;
	struct nc_ndp_desc *descs;

	struct rte_mbuf **src_mbufs = ctrl->local_mbufs;
	struct rte_mbuf **dst_mbufs = q->mbufs;

	uint64_t last_upper_addr = ctrl->c.last_upper_addr;
	uint16_t buf_size = q->buf_size;

	uint32_t mdp = ctrl->c.mdp; /* Mask for descriptor pointer */
	uint32_t sdp = ctrl->c.sdp; /* Software descriptor pointer */
	uint32_t mhp = ctrl->c.mhp; /* Mask for header pointer */
	uint32_t php = ctrl->php;   /* Prepared header pointer */

	nc_ndp_ctrl_hdp_update(&ctrl->c);

	free_hdrs = (ctrl->c.shp - php - 1) & mhp;
	free_desc = (ctrl->c.hdp - sdp - 1) & mdp;

	count = NFB_NDP_PKT_BURST;

	if (free_hdrs < count || free_desc < count)
		return 0;

	if (unlikely(rte_pktmbuf_alloc_bulk(q->mb_pool, src_mbufs, count) != 0))
		return 0;

	descs = q->mz_desc->addr;

	for (i = 0; i < count; i++) {
		iova = rte_mbuf_data_iova_default(src_mbufs[i]);
		if (unlikely(NDP_CTRL_DESC_UPPER_ADDR(iova) != last_upper_addr)) {
			if (unlikely(free_desc < 2))
				break;

			last_upper_addr = NDP_CTRL_DESC_UPPER_ADDR(iova);
			ctrl->c.last_upper_addr = last_upper_addr;

			descs[sdp] = nc_ndp_rx_desc0(iova);
			sdp = (sdp + 1) & mdp;
			free_desc--;
		}

		if (unlikely(free_desc == 0))
			break;

		dst_mbufs[php] = src_mbufs[i];

		descs[sdp] = nc_ndp_rx_desc2(iova, buf_size, 0);
		sdp = (sdp + 1) & mdp;
		php = (php + 1) & mhp;
		free_desc--;
	}

	if (i < count)
		rte_pktmbuf_free_bulk(src_mbufs + i, count - i);

	ctrl->php = php;
	ctrl->c.sdp = sdp;

	return i;
}

static inline struct rte_mbuf *nfb_ndp_queue_tx_undersized(struct rte_mbuf *mbuf,
		uint32_t len, uint32_t tu_min)
{
	struct rte_mbuf *mbuf_orig;
	void *data;
	if (rte_mbuf_refcnt_read(mbuf) != 1) {
		mbuf_orig = mbuf;
		mbuf = rte_pktmbuf_copy(mbuf, mbuf->pool, 0, UINT32_MAX);
		if (mbuf == NULL)
			return NULL;

		data = rte_pktmbuf_append(mbuf, tu_min - len);
		if (data == NULL) {
			rte_pktmbuf_free(mbuf);
			return NULL;
		}

		rte_pktmbuf_free(mbuf_orig);
	} else {
		data = rte_pktmbuf_append(mbuf, tu_min - len);
		if (data == NULL)
			return NULL;
	}
	memset(data, 0, tu_min - len);
	return mbuf;
}

uint16_t
nfb_ndp_queue_tx(void *queue, struct rte_mbuf **bufs, uint16_t nb_pkts)
{
	struct ndp_tx_queue *q = queue;
	struct ndp_ctrl *ctrl = q->ctrl;
	struct rte_mbuf *mbuf;

	uint64_t num_bytes = 0;
	uint64_t last_upper_addr = ctrl->c.last_upper_addr;
	struct nc_ndp_desc *descs;

	struct rte_mbuf **dst_mbufs = q->mbufs;

	uint32_t i;
	uint32_t len, min_len;

	rte_iova_t iova;
	uint32_t free_desc, count;

	uint32_t sdp = ctrl->c.sdp;
	uint32_t mdp = ctrl->c.mdp;

	nc_ndp_ctrl_hdp_update(&ctrl->c);
	ndp_ctrl_tx_free_mbufs(q);

	free_desc = (ctrl->c.hdp - sdp - 1) & mdp;

	count = nb_pkts;
	if (free_desc < count)
		count = free_desc;
	if (!count)
		return 0;

	min_len = ctrl->tu_min;
	descs = q->mz_desc->addr;
	for (i = 0; i < count; ++i) {
		mbuf = bufs[i];
		iova = rte_mbuf_data_iova(mbuf);

		len = rte_pktmbuf_data_len(mbuf);
		if (unlikely(len < min_len)) {
			mbuf = nfb_ndp_queue_tx_undersized(mbuf, len, min_len);
			if (mbuf == NULL)
				break;
			len = min_len;
		}

		if (rte_pktmbuf_linearize(mbuf))
			break;

		if (unlikely(NDP_CTRL_DESC_UPPER_ADDR(iova) != last_upper_addr)) {
			if (unlikely(free_desc < 2))
				break;

			last_upper_addr = NDP_CTRL_DESC_UPPER_ADDR(iova);
			ctrl->c.last_upper_addr = last_upper_addr;

			descs[sdp] = nc_ndp_tx_desc0(iova);

			dst_mbufs[sdp] = NULL;
			free_desc--;
			sdp = (sdp + 1) & mdp;
		}

		if (unlikely(free_desc == 0))
			break;

		free_desc--;
		dst_mbufs[sdp] = mbuf;

		descs[sdp] = nc_ndp_tx_desc2(iova, len, 0, 0);
		sdp = (sdp + 1) & mdp;

		num_bytes += mbuf->pkt_len;
	}

	q->tx_pkts += i;
	q->tx_bytes += num_bytes;

	ctrl->c.sdp = sdp;
	nc_ndp_ctrl_sdp_flush(&ctrl->c);

	return i;
}

static inline int
ndp_ctrl_tx_free_mbufs(struct ndp_tx_queue *q)
{
	uint32_t ret = 0;
	struct ndp_ctrl *ctrl = q->ctrl;
	uint32_t hdp = ctrl->c.hdp;
	uint32_t mdp = ctrl->c.mdp;
	uint32_t fdp = ctrl->fdp;

	ret = (hdp - fdp) & mdp;
	if (fdp > hdp) {
		rte_pktmbuf_free_bulk(q->mbufs + fdp, mdp + 1 - fdp);
		rte_pktmbuf_free_bulk(q->mbufs + 0, hdp);
		ctrl->fdp = hdp;
	} else if (ret) {
		rte_pktmbuf_free_bulk(q->mbufs + fdp, ret);
		ctrl->fdp = hdp;
	}

	return ret;
}

int nfb_ndp_rx_queue_start(struct rte_eth_dev *dev __rte_unused, struct ndp_rx_queue *q)
{
	int ret;

	struct nc_ndp_ctrl_start_params sp = {0};
	struct ndp_ctrl *ctrl = q->ctrl;

	sp.update_buffer_virt  = q->mz_update->addr;
	sp.update_buffer = q->mz_update->iova;
	sp.desc_buffer = q->mz_desc->iova;
	sp.hdr_buffer = q->mz_hdr->iova;
	sp.nb_desc = q->nb_rx_desc;
	sp.nb_hdr = q->nb_rx_hdr;

ndp_ctrl_try_start_again:
	ret = nc_ndp_ctrl_start(&ctrl->c, &sp);
	if (ret == -EALREADY) {
		NFB_LOG(ERR, "NDP RxQ %d is in dirty state, can't be started", q->qid);
		nc_ndp_ctrl_stop_force(&ctrl->c);
		rte_delay_ms(10);
		ret = nc_ndp_ctrl_stop(&ctrl->c);
		if (ret == 0) {
			NFB_LOG(ERR, "NDP RxQ %d restart OK", q->qid);
			goto ndp_ctrl_try_start_again;
		} else {
			NFB_LOG(ERR, "NDP RxQ %d restart unsuccessful", q->qid);
		}
	} else if (ret == -EEXIST) {
		NFB_LOG(ERR, "NDP RxQ %d is used by other process", q->qid);
	}

	if (ret)
		return ret;

	ctrl->php = 0;

	while (nfb_ndp_ctrl_fill_rx_descs(q))
		;
	nc_ndp_ctrl_sp_flush(&ctrl->c);

	return 0;
}

int nfb_ndp_rx_queue_stop(struct rte_eth_dev *dev __rte_unused, struct ndp_rx_queue *q)
{
	int ret;
	int cnt = 0;
	do {
		ret = nc_ndp_ctrl_stop(&q->ctrl->c);
		if (ret != -EAGAIN)
			break;
		rte_delay_ms(10);
	} while (cnt++ < 100);

	if (ret) {
		nc_ndp_ctrl_stop_force(&q->ctrl->c);
		NFB_LOG(ERR, "NDP queue rx %d didn't stop in 1 sec. "
			"This may be due to firmware error.", q->qid);
	}

	return 0;
}

int nfb_ndp_rx_queue_setup(struct rte_eth_dev *dev,
		uint16_t rx_queue_id,
		uint16_t nb_rx_desc,
		unsigned int socket_id,
		const struct rte_eth_rxconf *rx_conf __rte_unused,
		struct rte_mempool *mb_pool __rte_unused, struct ndp_rx_queue *q)
{
	int ret = -ENOMEM;
	int fdt_offset;
	unsigned int flags = RTE_MEMZONE_IOVA_CONTIG | RTE_MEMZONE_SIZE_HINT_ONLY | RTE_MEMZONE_2MB;
	char mz_name[RTE_MEMZONE_NAMESIZE];
	struct pmd_internals *internals = dev->process_private;
	struct pmd_priv *priv = dev->data->dev_private;

	q->nb_rx_desc = nb_rx_desc;
	q->nb_rx_hdr = nb_rx_desc;

	snprintf(mz_name, sizeof(mz_name), "nfb%d_rxq%d_dsc", priv->nfb_id, rx_queue_id);
	q->mz_desc = rte_memzone_reserve_aligned(mz_name,
			RTE_ALIGN(sizeof(struct nc_ndp_desc) * q->nb_rx_desc, HW_BUFFER_ALIGN),
			socket_id, flags, HW_BUFFER_ALIGN);
	if (q->mz_desc == NULL)
		goto err_mz_res_desc;

	snprintf(mz_name, sizeof(mz_name), "nfb%d_rxq%d_hdr", priv->nfb_id, rx_queue_id);
	q->mz_hdr = rte_memzone_reserve_aligned(mz_name,
			RTE_ALIGN(sizeof(struct nc_ndp_hdr) * q->nb_rx_hdr, HW_BUFFER_ALIGN),
			socket_id, flags, HW_BUFFER_ALIGN);
	if (q->mz_hdr == NULL)
		goto err_mz_res_hdr;

	snprintf(mz_name, sizeof(mz_name), "nfb%d_rxq%d_upd", priv->nfb_id, rx_queue_id);
	q->mz_update = rte_memzone_reserve_aligned(mz_name,
			RTE_ALIGN(sizeof(uint32_t) * 2, HW_BUFFER_ALIGN),
			socket_id, flags, HW_BUFFER_ALIGN);
	if (q->mz_update == NULL)
		goto err_mz_res_update;

	q->ctrl = rte_zmalloc("nfb_rxq_ctrl", sizeof(struct ndp_ctrl), RTE_CACHE_LINE_SIZE);
	if (q->ctrl == NULL)
		goto err_malloc_ctrl;

	q->mbufs = rte_calloc("nfb_rxq_mbufs", q->nb_rx_hdr, sizeof(struct rte_mbuf *),
			RTE_CACHE_LINE_SIZE);
	if (q->mbufs == NULL)
		goto err_malloc_mbufs;

	fdt_offset = nfb_comp_find(internals->nfb, "netcope,dma_ctrl_ndp_rx", rx_queue_id);
	ret = nc_ndp_ctrl_open(internals->nfb, fdt_offset, &q->ctrl->c);
	if (ret)
		goto err_ctrl_open;

	ret = nc_ndp_ctrl_get_mtu(&q->ctrl->c, &q->ctrl->tu_min, &q->ctrl->tu_max);
	if (ret)
		goto err_ctrl_get_mtu;

	return 0;

err_ctrl_get_mtu:
	nc_ndp_ctrl_close(&q->ctrl->c);
err_ctrl_open:
	rte_free(q->mbufs);
err_malloc_mbufs:
	rte_free(q->ctrl);
err_malloc_ctrl:
	rte_memzone_free(q->mz_update);
err_mz_res_update:
	rte_memzone_free(q->mz_hdr);
err_mz_res_hdr:
	rte_memzone_free(q->mz_desc);
err_mz_res_desc:
	return ret;
}

void
nfb_ndp_rx_queue_release(struct rte_eth_dev *dev __rte_unused, struct ndp_rx_queue *q)
{
	nc_ndp_ctrl_close(&q->ctrl->c);
	rte_free(q->mbufs);
	rte_free(q->ctrl);

	rte_memzone_free(q->mz_update);
	rte_memzone_free(q->mz_hdr);
	rte_memzone_free(q->mz_desc);
}

int
nfb_ndp_tx_queue_start(struct rte_eth_dev *dev __rte_unused, struct ndp_tx_queue *q)
{
	int ret;
	struct nc_ndp_ctrl_start_params sp = {0};
	struct ndp_ctrl *ctrl = q->ctrl;

	sp.update_buffer_virt  = q->mz_update->addr;
	sp.update_buffer = q->mz_update->iova;
	sp.desc_buffer = q->mz_desc->iova;
	sp.hdr_buffer = 0;
	sp.nb_desc = q->nb_tx_desc;
	sp.nb_hdr = 0;

ndp_ctrl_try_start_again:
	ret = nc_ndp_ctrl_start(&ctrl->c, &sp);
	if (ret == -EALREADY) {
		NFB_LOG(ERR, "NDP TxQ queue %d is in dirty state, can't be started", q->qid);

		nc_ndp_ctrl_stop_force(&ctrl->c);
		rte_delay_ms(10);
		ret = nc_ndp_ctrl_stop(&ctrl->c);
		if (ret == 0) {
			NFB_LOG(ERR, "NDP TxQ %d restart OK", q->qid);
			goto ndp_ctrl_try_start_again;
		} else {
			NFB_LOG(ERR, "NDP TxQ %d restart unsuccessful", q->qid);
		}
	} else if (ret == -EEXIST) {
		NFB_LOG(ERR, "NDP TxQ %d is used by other process", q->qid);
	}

	if (ret)
		return ret;

	return ret;
}

int nfb_ndp_tx_queue_stop(struct rte_eth_dev *dev __rte_unused, struct ndp_tx_queue *q)
{
	int ret;
	int cnt = 0;
	do {
		ret = nc_ndp_ctrl_stop(&q->ctrl->c);
		if (ret != -EAGAIN && ret != -EINPROGRESS)
			break;
		rte_delay_ms(10);
	} while (cnt++ < 100);

	if (ret) {
		nc_ndp_ctrl_stop_force(&q->ctrl->c);
		NFB_LOG(ERR, "NDP TxQ %d didn't stop in 1 sec. "
			"This may be due to firmware error.", q->qid);
	}

	return 0;
}

int nfb_ndp_tx_queue_setup(struct rte_eth_dev *dev,
		uint16_t tx_queue_id,
		uint16_t nb_tx_desc,
		unsigned int socket_id,
		const struct rte_eth_txconf *tx_conf __rte_unused,
		struct ndp_tx_queue *q)
{
	int ret = -ENOMEM;
	int fdt_offset;
	unsigned int flags = RTE_MEMZONE_IOVA_CONTIG | RTE_MEMZONE_SIZE_HINT_ONLY | RTE_MEMZONE_2MB;
	char mz_name[RTE_MEMZONE_NAMESIZE];
	struct pmd_internals *internals = dev->process_private;
	struct pmd_priv *priv = dev->data->dev_private;

	q->nb_tx_desc = nb_tx_desc;

	snprintf(mz_name, sizeof(mz_name), "nfb%d_txq%d_dsc", priv->nfb_id, tx_queue_id);
	q->mz_desc = rte_memzone_reserve_aligned(mz_name,
			RTE_ALIGN(sizeof(struct nc_ndp_desc) * q->nb_tx_desc, HW_BUFFER_ALIGN),
			socket_id, flags, HW_BUFFER_ALIGN);
	if (q->mz_desc == NULL)
		goto err_mz_res_desc;

	snprintf(mz_name, sizeof(mz_name), "nfb%d_txq%d_upd", priv->nfb_id, tx_queue_id);
	q->mz_update = rte_memzone_reserve_aligned(mz_name,
			RTE_ALIGN(sizeof(uint32_t) * 2, HW_BUFFER_ALIGN),
			socket_id, flags, HW_BUFFER_ALIGN);
	if (q->mz_update == NULL)
		goto err_mz_res_update;

	q->ctrl = rte_zmalloc("nfb_txq_ctrl", sizeof(struct ndp_ctrl), RTE_CACHE_LINE_SIZE);
	if (q->ctrl == NULL)
		goto err_malloc_ctrl;

	q->mbufs = rte_calloc("nfb_txq_mbufs", q->nb_tx_desc, sizeof(struct rte_mbuf *),
			RTE_CACHE_LINE_SIZE);
	if (q->mbufs == NULL)
		goto err_malloc_mbufs;

	q->ctrl->fdp = 0;

	fdt_offset = nfb_comp_find(internals->nfb, "netcope,dma_ctrl_ndp_tx", tx_queue_id);
	ret = nc_ndp_ctrl_open(internals->nfb, fdt_offset, &q->ctrl->c);
	if (ret)
		goto err_ctrl_open;

	ret = nc_ndp_ctrl_get_mtu(&q->ctrl->c, &q->ctrl->tu_min, &q->ctrl->tu_max);
	if (ret)
		goto err_ctrl_get_mtu;

	return 0;

err_ctrl_get_mtu:
	nc_ndp_ctrl_close(&q->ctrl->c);
err_ctrl_open:
	rte_free(q->mbufs);
err_malloc_mbufs:
	rte_free(q->ctrl);
err_malloc_ctrl:
	rte_memzone_free(q->mz_update);
err_mz_res_update:
	rte_memzone_free(q->mz_desc);
err_mz_res_desc:
	return ret;
}

void
nfb_ndp_tx_queue_release(struct rte_eth_dev *dev __rte_unused, struct ndp_tx_queue *q)
{
	nc_ndp_ctrl_close(&q->ctrl->c);
	rte_free(q->mbufs);
	rte_free(q->ctrl);

	rte_memzone_free(q->mz_desc);
	rte_memzone_free(q->mz_update);
}

int
nfb_ndp_queue_get_desc_lim(struct rte_eth_dev *dev, int dir, struct rte_eth_desc_lim *dl)
{
	int ret;
	struct nc_ndp_ctrl ctrl;
	int fdt_offset;
	struct pmd_internals *priv = dev->process_private;
	uint32_t max_dp_mask, max_hp_mask;

	fdt_offset = nfb_comp_find(priv->nfb,
			dir == 0 ? COMP_NC_DMA_CTRL_NDP_RX : COMP_NC_DMA_CTRL_NDP_TX, 0);
	ret = nc_ndp_ctrl_open(priv->nfb, fdt_offset, &ctrl);
	if (ret)
		return ret;
	nc_ndp_ctrl_medusa_get_max_ptr_mask(&ctrl, &max_dp_mask, &max_hp_mask);

	dl->nb_max = RTE_MIN(max_dp_mask + 1, 32768u);
	dl->nb_min = NFB_NDP_PKT_BURST * 2;

	nc_ndp_ctrl_close(&ctrl);
	return 0;
}
