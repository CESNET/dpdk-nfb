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

	uint16_t *hdr_off;
	uint16_t *hdr_len;
	uint16_t *flags;

	struct ndp_rx_offload_parser * ofp;
	uint8_t hdr_id;

	nc_ndp_ctrl_hhp_update(&ctrl->c);
	count = (ctrl->c.hhp - shp) & mhp;
	if (nb_pkts < count)
		count = nb_pkts;

	nb_rx = count;

	if (!nb_rx) {
		while (nfb_ndp_ctrl_fill_rx_descs(q));
		nc_ndp_ctrl_sdp_flush(&ctrl->c);
		return 0;
	}

	for (i = 0; i < nb_rx; ++i) {
		mbuf = q->mbufs[shp];
		hdr = &hdrs[shp];

		mbuf->data_len = hdr->frame_len;
		mbuf->pkt_len = mbuf->data_len;
		mbuf->ol_flags = 0;

		if (nfb_ndp_df_header_enable) {
			hdr_off = RTE_MBUF_DYNFIELD(mbuf, nfb_ndp_df_header_offset, uint16_t*);
			hdr_len = RTE_MBUF_DYNFIELD(mbuf, nfb_ndp_df_header_length, uint16_t*);
			flags   = RTE_MBUF_DYNFIELD(mbuf, nfb_ndp_df_flags, uint16_t*);

			*hdr_off = mbuf->data_off;
			*hdr_len = hdr->hdr_len;
			*flags = hdr->meta;

			mbuf->ol_flags |= nfb_ndp_df_header_vld;
		}

		hdr_id = hdr->meta & 0x03;
		if (hdr_id < NDP_RXHDR_CNT) {
			ofp = &q->ofp[hdr_id];
			if (hdr->hdr_len >= ofp->hdr_minlen) {
				nfb_rx_fetch_timestamp(ofp, mbuf, rte_pktmbuf_mtod(mbuf, unsigned char*));
				nfb_rx_fetch_metadata(ofp, mbuf, rte_pktmbuf_mtod(mbuf, unsigned char*));
			}
		}

		rte_pktmbuf_adj(mbuf, hdr->hdr_len);

		num_bytes += mbuf->pkt_len;
		bufs[i] = mbuf;
		shp = (shp + 1) & mhp;
#if 0
		int j;
		for (j = 0; j < mbuf->data_len * 0; j++) {
			printf("%02x", *(rte_pktmbuf_mtod(mbuf, char*) + j));
			if (j % 4 == 3)
				printf(" ");
			if (j % 32 == 31)
				printf("\n");
		}
#endif
	}

	q->rx_pkts += nb_rx;
	q->rx_bytes += num_bytes;

	ctrl->c.shp = shp;
	while (nfb_ndp_ctrl_fill_rx_descs(q));
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

	struct rte_mbuf** src_mbufs = ctrl->local_mbufs;
	struct rte_mbuf** dst_mbufs = q->mbufs;

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
#if 0
	/* TODO: [OPT] bound to desc boundary and header (mbufs) boundary */
	if (free_desc < count)
		count = free_desc;
	if (free_hdrs < count)
		count = free_hdrs;
	if (count < 16)
		return 0;
#else
	if (free_hdrs < count || free_desc < count)
		return 0;
#endif

	/* TODO: alloc directly to q->mbufs, but check header boundary */
	if (unlikely(rte_pktmbuf_alloc_bulk(q->mb_pool, src_mbufs, count) != 0))
		return 0;

	descs = q->mz_desc->addr;

	for (i = 0; i < count; i++) {
		iova = rte_mbuf_data_iova_default(src_mbufs[i]);
		if (unlikely(NDP_CTRL_DESC_UPPER_ADDR(iova) != last_upper_addr)) {
			if (unlikely(free_desc == 0)) {
				break;
			}

			last_upper_addr = NDP_CTRL_DESC_UPPER_ADDR(iova);
			ctrl->c.last_upper_addr = last_upper_addr;

			descs[sdp] = nc_ndp_rx_desc0(iova);
			sdp = (sdp + 1) & mdp;
			free_desc--;
		}

		if (unlikely(free_desc == 0)) {
			break;
		}

		/* TODO: alloc directly to q->mbufs, but check header boundary */
		dst_mbufs[php] = src_mbufs[i];

		descs[sdp] = nc_ndp_rx_desc2(iova, buf_size, 0);
		sdp = (sdp + 1) & mdp;
		php = (php + 1) & mhp;
		free_desc--;
	}

	/* TODO: Check if count is in most cases 1 or 2 => bulk can be expensive? */
	if (i < count) {
		rte_pktmbuf_free_bulk(src_mbufs + i, count - i);
	}

	ctrl->php = php;
	ctrl->c.sdp = sdp;

	return i;
}

static inline struct rte_mbuf *nfb_ndp_queue_tx_undersized(struct rte_mbuf *mbuf, uint32_t len, uint32_t tu_min)
{
	struct rte_mbuf *mbuf_orig;
	void *data;
	if (rte_mbuf_refcnt_read(mbuf) != 1) {
		mbuf_orig = mbuf;
		mbuf = rte_pktmbuf_copy(mbuf, mbuf->pool, 0, UINT32_MAX);
		if (mbuf == NULL)
			return NULL;

		/* Autor was too lazy to implement appendment of the last segment */
		if (rte_pktmbuf_linearize(mbuf)) {
			rte_pktmbuf_free(mbuf);
			return NULL;
		}

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

	struct rte_mbuf** dst_mbufs = q->mbufs;

	uint32_t i;
	uint32_t len, min_len;

	rte_iova_t iova;
	uint32_t free_desc, count;

	uint32_t sdp = ctrl->c.sdp;
	uint32_t mdp = ctrl->c.mdp;

	nc_ndp_ctrl_hdp_update(&ctrl->c);
	ndp_ctrl_tx_free_mbufs(q);

	free_desc = (ctrl->c.hdp - sdp - 1) & mdp;

	/* TODO: [OPT] bound to desc boundary and q->mbufs boundary */

	/* FIXME: wait for empty space? */
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
			if (unlikely(free_desc < 2)) {
				break;
			}

			last_upper_addr = NDP_CTRL_DESC_UPPER_ADDR(iova);
			ctrl->c.last_upper_addr = last_upper_addr;

			descs[sdp] = nc_ndp_tx_desc0(iova);

			dst_mbufs[sdp] = NULL;
			free_desc--;
			sdp = (sdp + 1) & mdp;
		}

		if (unlikely(free_desc == 0)) {
			break;
		}

		free_desc--;
		dst_mbufs[sdp] = mbuf;

		/* TODO: implement & check next flag */
		descs[sdp] = nc_ndp_tx_desc2(iova, len, 0, 0);
		sdp = (sdp + 1) & mdp;

		/* TODO: use mbuf->ol_flags and fill header */

		num_bytes += mbuf->pkt_len;
	}

	/* FIXME: not enough free descripts */
	if (i != count) {
	}

#if 0 /* INFO: Replaced by bonding q->mbufs with descriptors, assign q->mbuf[dp] = NULL for desc0 */
	if (q->smp + i > q->cmp) {
		k = q->cmp - q->smp;
		rte_memcpy(q->mbufs + q->smp, *bufs, sizeof(*bufs) * k);
		rte_memcpy(q->mbufs, *bufs + k, sizeof(*bufs) * (i - k));
		q->smp = (i - k);
	} else {
		rte_memcpy(q->mbufs + q->smp, *bufs, sizeof(*bufs) * i);
		q->smp += i;
	}
	/* INFO: can be used instead of the two assignments into q->smp above */
	// q->smp = (q->smp + i) & (q->cmp - 1);
#endif

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

#if 0
	while (fdp != hdp) {
		if (q->mbufs[fdp]) {
			rte_pktmbuf_free(q->mbufs[fdp]);
			q->mbufs[fdp] = NULL;
			ret++;
		}
		fdp = (fdp + 1) & mdp;
	}
	ctrl->fdp = fdp;
#else
	ret = (hdp - fdp) & mdp;
	if (fdp > hdp) {
		rte_pktmbuf_free_bulk(q->mbufs + fdp, mdp + 1 - fdp);
		rte_pktmbuf_free_bulk(q->mbufs + 0, hdp);
		ctrl->fdp = hdp;
	} else if (ret) {
		rte_pktmbuf_free_bulk(q->mbufs + fdp, ret);
		ctrl->fdp = hdp;
	}

#endif
	return ret;
}

int nfb_ndp_rx_queue_start(struct rte_eth_dev *dev __rte_unused, struct ndp_rx_queue *q)
{
	int ret;

	struct nc_ndp_ctrl_start_params sp;
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
		RTE_LOG(ERR, PMD, "NDP RxQ %d is in dirty state, can't be started\n",
				q->rx_queue_id);
		nc_ndp_ctrl_stop_force(&ctrl->c);
		rte_delay_ms(10);
		ret = nc_ndp_ctrl_stop(&ctrl->c);
		if (ret == 0) {
			RTE_LOG(ERR, PMD, "NDP RxQ %d restart OK\n", q->rx_queue_id);
			goto ndp_ctrl_try_start_again;
		} else {
			RTE_LOG(ERR, PMD, "NDP RxQ %d restart unsuccessfull\n", q->rx_queue_id);
		}
	} else if (ret == -EEXIST) {
		RTE_LOG(ERR, PMD, "NDP RxQ %d is used by other process\n",
				q->rx_queue_id);
	}

	if (ret)
		return ret;

	ctrl->php = 0;

	while (nfb_ndp_ctrl_fill_rx_descs(q));
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
		RTE_LOG(ERR, PMD, "NDP queue rx %d didn't stop in 1 sec. "
			"This may be due to firmware error.\n", q->rx_queue_id);
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
	unsigned flags = RTE_MEMZONE_IOVA_CONTIG | RTE_MEMZONE_SIZE_HINT_ONLY | RTE_MEMZONE_2MB;
	char mz_name[RTE_MEMZONE_NAMESIZE];
	struct pmd_internals *priv = dev->process_private;

	q->nb_rx_desc = nb_rx_desc;
	q->nb_rx_hdr = nb_rx_desc;

	/* CHECKME: use rte_memzone_reserve_bounded? */
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

	q->mbufs = rte_zmalloc("nfb_rxq_mbufs", sizeof(struct rte_mbuf*) * q->nb_rx_hdr,
			RTE_CACHE_LINE_SIZE);
	if (q->mbufs == NULL)
		goto err_malloc_mbufs;

	fdt_offset = nfb_comp_find(priv->nfb, "netcope,dma_ctrl_ndp_rx", rx_queue_id);
	ret = nc_ndp_ctrl_open(priv->nfb, fdt_offset, &q->ctrl->c);
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
	struct nc_ndp_ctrl_start_params sp;
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
		RTE_LOG(ERR, PMD, "NDP TxQ queue %d is in dirty state, can't be started\n",
				q->tx_queue_id);

		nc_ndp_ctrl_stop_force(&ctrl->c);
		rte_delay_ms(10);
		ret = nc_ndp_ctrl_stop(&ctrl->c);
		if (ret == 0) {
			RTE_LOG(ERR, PMD, "NDP TxQ %d restart OK\n", q->tx_queue_id);
			goto ndp_ctrl_try_start_again;
		} else {
			RTE_LOG(ERR, PMD, "NDP TxQ %d restart unsuccessfull\n", q->tx_queue_id);
		}
	} else if (ret == -EEXIST) {
		RTE_LOG(ERR, PMD, "NDP TxQ %d is used by other process\n",
				q->tx_queue_id);
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
		RTE_LOG(ERR, PMD, "NDP TxQ %d didn't stop in 1 sec. "
			"This may be due to firmware error.\n", q->tx_queue_id);
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
	unsigned flags = RTE_MEMZONE_IOVA_CONTIG | RTE_MEMZONE_SIZE_HINT_ONLY | RTE_MEMZONE_2MB;
	char mz_name[RTE_MEMZONE_NAMESIZE];
	struct pmd_internals *priv = dev->process_private;

	q->nb_tx_desc = nb_tx_desc;

	/* CHECKME: use rte_memzone_reserve_bounded? */
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

	q->mbufs = rte_zmalloc("nfb_txq_mbufs", sizeof(struct rte_mbuf*) * q->nb_tx_desc,
			RTE_CACHE_LINE_SIZE);
	if (q->mbufs == NULL)
		goto err_malloc_mbufs;

	q->ctrl->fdp = 0;

	fdt_offset = nfb_comp_find(priv->nfb, "netcope,dma_ctrl_ndp_tx", tx_queue_id);
	ret = nc_ndp_ctrl_open(priv->nfb, fdt_offset, &q->ctrl->c);
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
