/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2019 Cesnet
 * Copyright(c) 2019 Netcope Technologies, a.s. <info@netcope.com>
 * All rights reserved.
 */

#ifndef _NFB_RX_H_
#define _NFB_RX_H_

#include <nfb/nfb.h>
#include <nfb/ndp.h>

#include <rte_mbuf.h>
#include <rte_mbuf_dyn.h>
#include <rte_ethdev.h>
#include <rte_time.h>

extern uint64_t nfb_timestamp_rx_dynflag;
extern int nfb_timestamp_dynfield_offset;

extern int nfb_ndp_df_header_offset;
extern int nfb_ndp_df_header_length;
extern int nfb_ndp_df_flags;
extern uint64_t nfb_ndp_df_header_vld;

extern int nfb_ndp_df_header_enable;

static inline rte_mbuf_timestamp_t *
nfb_timestamp_dynfield(struct rte_mbuf *mbuf)
{
	return RTE_MBUF_DYNFIELD(mbuf,
		nfb_timestamp_dynfield_offset, rte_mbuf_timestamp_t *);
}

struct ndp_ctrl;

struct ndp_rx_queue {
	struct nfb_device *nfb;	     /* nfb dev structure */
	struct ndp_queue *queue;     /* rx queue */
	uint16_t rx_queue_id;	     /* index */
	uint8_t in_port;	     /* port */
	uint8_t flags;               /* setup flags */

	struct rte_mempool *mb_pool; /* memory pool to allocate packets */
	uint16_t buf_size;           /* mbuf size */

	int16_t  timestamp_off;
	int16_t  timestamp_vld_off;
	uint8_t  timestamp_vld_mask;

	volatile uint64_t rx_pkts;   /* packets read */
	volatile uint64_t rx_bytes;  /* bytes read */
	volatile uint64_t err_pkts;  /* erroneous packets */
	uint8_t state;

	int queue_driver;

	/* native queue driver variables*/
	struct ndp_ctrl* ctrl;
	struct rte_mbuf** mbufs;
	const struct rte_memzone *mz_desc;
	const struct rte_memzone *mz_hdr;
	const struct rte_memzone *mz_update;

	uint16_t nb_rx_desc;
	uint16_t nb_rx_hdr;
};


uint16_t nfb_ndp_queue_rx(void *queue, struct rte_mbuf **bufs, uint16_t nb_pkts);
int nfb_ndp_rx_queue_start(struct rte_eth_dev *dev, struct ndp_rx_queue *q);
int nfb_ndp_rx_queue_stop(struct rte_eth_dev *dev, struct ndp_rx_queue *q);
int nfb_ndp_rx_queue_setup(struct rte_eth_dev *dev __rte_unused,
                uint16_t rx_queue_id,
                uint16_t nb_rx_desc,
                unsigned int socket_id,
                const struct rte_eth_rxconf *rx_conf __rte_unused,
                struct rte_mempool *mb_pool, struct ndp_rx_queue *q);

void nfb_ndp_rx_queue_release(struct rte_eth_dev *dev, struct ndp_rx_queue *q);

/**
 * Initialize ndp_rx_queue structure
 *
 * @param nfb
 *   Pointer to nfb device structure.
 * @param rx_queue_id
 *   RX queue index.
 * @param port_id
 *   Device [external] port identifier.
 * @param mb_pool
 *   Memory pool for buffer allocations.
 * @param[out] rxq
 *   Pointer to ndp_rx_queue output structure
 * @return
 *   0 on success, a negative errno value otherwise.
 */
int
nfb_eth_rx_queue_init(struct rte_eth_dev *dev,
	uint16_t rx_queue_id,
	uint16_t nb_rx_desc,
	unsigned int socket_id,
	const struct rte_eth_rxconf *rx_conf,
	uint16_t port_id,
	struct rte_mempool *mb_pool,
	struct ndp_rx_queue *rxq);

/**
 * DPDK callback to setup a RX queue for use.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param idx
 *   RX queue index.
 * @param desc
 *   Number of descriptors to configure in queue.
 * @param socket
 *   NUMA socket on which memory must be allocated.
 * @param[in] conf
 *   Thresholds parameters.
 * @param mb_pool
 *   Memory pool for buffer allocations.
 *
 * @return
 *   0 on success, a negative errno value otherwise.
 */
int
nfb_eth_rx_queue_setup(struct rte_eth_dev *dev,
	uint16_t rx_queue_id,
	uint16_t nb_rx_desc __rte_unused,
	unsigned int socket_id,
	const struct rte_eth_rxconf *rx_conf __rte_unused,
	struct rte_mempool *mb_pool);

/**
 * DPDK callback to release a RX queue.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param qid
 *   Receive queue index.
 */
void
nfb_eth_rx_queue_release(struct rte_eth_dev *dev, uint16_t qid);

/**
 * Start traffic on Rx queue.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param txq_id
 *   RX queue index.
 * @return
 *   0 on success, a negative errno value otherwise.
 */
int
nfb_eth_rx_queue_start(struct rte_eth_dev *dev, uint16_t rxq_id);

/**
 * Stop traffic on Rx queue.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param txq_id
 *   RX queue index.
 */
int
nfb_eth_rx_queue_stop(struct rte_eth_dev *dev, uint16_t rxq_id);

static inline void nfb_rx_fetch_timestamp(struct ndp_rx_queue *q, struct rte_mbuf *mbuf,
		const unsigned char *header, int header_length)
{
	rte_mbuf_timestamp_t timestamp;

	/* INFO: already checked in nfb_eth_rx_queue_init */
	if (/*nfb_timestamp_dynfield_offset < 0 || */q->timestamp_off < 0)
		return;

	if (header_length < q->timestamp_off + 8)
		return;

	/* seconds */
	timestamp   = rte_le_to_cpu_32(*((const uint32_t *) (header + q->timestamp_off + 4)));
	timestamp  *= NSEC_PER_SEC;
	/* nanoseconds */
	timestamp  += rte_le_to_cpu_32(*((const uint32_t *) (header + q->timestamp_off + 0)));

	*nfb_timestamp_dynfield(mbuf) = timestamp;
	if (header_length > (q->timestamp_vld_off) && header[q->timestamp_vld_off] & q->timestamp_vld_mask)
		mbuf->ol_flags |= nfb_timestamp_rx_dynflag;
}

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
static __rte_always_inline uint16_t
nfb_eth_ndp_rx(void *queue,
	struct rte_mbuf **bufs,
	uint16_t nb_pkts)
{
	struct ndp_rx_queue *ndp = queue;
	uint16_t data_len;
	uint16_t hdr_len;
	uint16_t packet_size;
	uint64_t num_bytes = 0;
	uint16_t num_rx;
	unsigned int i;

	const uint16_t buf_size = ndp->buf_size;

	struct rte_mbuf *mbuf;
	struct ndp_packet packets[nb_pkts];

	struct rte_mbuf *mbufs[nb_pkts];

	uint16_t *df_hdr_off;
	uint16_t *df_hdr_len;
	uint16_t *flags;

	if (unlikely(ndp->queue == NULL || nb_pkts == 0)) {
		RTE_LOG(ERR, PMD, "RX invalid arguments!\n");
		return 0;
	}

	/* returns either all or nothing */
	i = rte_pktmbuf_alloc_bulk(ndp->mb_pool, mbufs, nb_pkts);
	if (unlikely(i != 0))
		return 0;

	num_rx = ndp_rx_burst_get(ndp->queue, packets, nb_pkts);

	if (unlikely(num_rx != nb_pkts)) {
		for (i = num_rx; i < nb_pkts; i++)
			rte_pktmbuf_free(mbufs[i]);
	}

	nb_pkts = num_rx;

	num_rx = 0;
	/*
	 * Reads the given number of packets from NDP queue given
	 * by queue and copies the packet data into a newly allocated mbuf
	 * to return.
	 */
	for (i = 0; i < nb_pkts; ++i) {
		mbuf = mbufs[i];

		/* get the space available for data in the mbuf */
		data_len = packets[i].data_length;
		hdr_len = packets[i].header_length;
		packet_size = data_len + hdr_len;

		if (likely(packet_size <= buf_size)) {
			/* NDP packet will fit in one mbuf, go ahead and copy */

			mbuf->pkt_len = packet_size;
			mbuf->data_len = (uint16_t)packet_size;
			mbuf->port = ndp->in_port;
			mbuf->ol_flags = 0;

			if (nfb_ndp_df_header_enable) {
				df_hdr_off = RTE_MBUF_DYNFIELD(mbuf, nfb_ndp_df_header_offset, uint16_t*);
				df_hdr_len = RTE_MBUF_DYNFIELD(mbuf, nfb_ndp_df_header_length, uint16_t*);
				flags      = RTE_MBUF_DYNFIELD(mbuf, nfb_ndp_df_flags, uint16_t*);

				*df_hdr_off = mbuf->data_off;
				*df_hdr_len = hdr_len;
				*flags = packets[i].flags;

				mbuf->ol_flags |= nfb_ndp_df_header_vld;

				rte_memcpy(rte_pktmbuf_mtod(mbuf, void *), packets[i].header, hdr_len);
			}
			rte_pktmbuf_adj(mbuf, hdr_len);
			rte_memcpy(rte_pktmbuf_mtod(mbuf, void *), packets[i].data, data_len);

			nfb_rx_fetch_timestamp(ndp, mbuf, packets[i].header, packets[i].header_length);

			bufs[num_rx++] = mbuf;
			num_bytes += packet_size;
		} else {
			/*
			 * NDP packet will not fit in one mbuf,
			 * scattered mode is not enabled, drop packet
			 */
			rte_pktmbuf_free(mbuf);
		}
	}

	ndp_rx_burst_put(ndp->queue);

	ndp->rx_pkts += num_rx;
	ndp->rx_bytes += num_bytes;
	return num_rx;
}

#endif /* _NFB_RX_H_ */
