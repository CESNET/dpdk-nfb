/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2019 Cesnet
 * Copyright(c) 2019 Netcope Technologies, a.s. <info@netcope.com>
 * All rights reserved.
 */

#include <rte_ethdev.h>
#include <ethdev_driver.h>

#include "nfb.h"
#include "nfb_tx.h"

int
nfb_eth_tx_queue_start(struct rte_eth_dev *dev, uint16_t txq_id)
{
	struct ndp_tx_queue *txq = dev->data->tx_queues[txq_id];
	int ret = 0;

	if (txq->queue_driver == NFB_QUEUE_DRIVER_EMPTY) {
	} else if (txq->queue_driver == NFB_QUEUE_DRIVER_NATIVE) {
		ret = nfb_ndp_tx_queue_start(dev, txq);
	} else {
		if (txq->queue == NULL) {
			NFB_LOG(ERR, "TX NDP queue is NULL!");
			return -EINVAL;
		}

		ret = ndp_queue_start(txq->queue);
	}

	if (ret != 0)
		goto err;
	dev->data->tx_queue_state[txq_id] = RTE_ETH_QUEUE_STATE_STARTED;
	return 0;

err:
	return -EINVAL;
}

int
nfb_eth_tx_queue_stop(struct rte_eth_dev *dev, uint16_t txq_id)
{
	struct ndp_tx_queue *txq = dev->data->tx_queues[txq_id];
	int ret = 0;

	if (txq->queue_driver == NFB_QUEUE_DRIVER_EMPTY) {
	} else if (txq->queue_driver == NFB_QUEUE_DRIVER_NATIVE) {
		ret = nfb_ndp_tx_queue_stop(dev, txq);
	} else {
		if (txq->queue == NULL) {
			NFB_LOG(ERR, "TX NDP queue is NULL!");
			return -EINVAL;
		}

		ret = ndp_queue_stop(txq->queue);
	}

	if (ret != 0)
		return -EINVAL;
	dev->data->tx_queue_state[txq_id] = RTE_ETH_QUEUE_STATE_STOPPED;
	return 0;
}

int
nfb_eth_tx_queue_setup(struct rte_eth_dev *dev,
	uint16_t tx_queue_id,
	uint16_t nb_tx_desc,
	unsigned int socket_id,
	const struct rte_eth_txconf *tx_conf)
{
	struct pmd_priv *priv = dev->data->dev_private;

	int ret;
	int qid;
	struct ndp_tx_queue *txq;

	if (tx_queue_id >= priv->max_tx_queues)
		return -EINVAL;

	txq = rte_zmalloc_socket("ndp tx queue", sizeof(*txq), RTE_CACHE_LINE_SIZE, socket_id);

	if (txq == NULL) {
		NFB_LOG(ERR, "rte_zmalloc_socket() failed for tx queue id %" PRIu16,
			tx_queue_id);
		return -ENOMEM;
	}

	txq->queue_driver = priv->queue_driver;

	qid = priv->queue_map_tx[tx_queue_id];

	ret = nfb_eth_tx_queue_init(dev, qid, nb_tx_desc, socket_id, tx_conf, txq);
	if (ret)
		goto err_queue_init;

	dev->data->tx_queues[tx_queue_id] = txq;
	return 0;

err_queue_init:
	rte_free(txq);
	return ret;
}

int
nfb_eth_tx_queue_init(struct rte_eth_dev *dev,
	int qid,
	uint16_t nb_tx_desc,
	unsigned int socket_id,
	const struct rte_eth_txconf *tx_conf, struct ndp_tx_queue *txq)
{
	int ret;
	struct pmd_internals *internals = dev->process_private;

	if (txq->queue_driver == NFB_QUEUE_DRIVER_EMPTY) {
	} else if (txq->queue_driver == NFB_QUEUE_DRIVER_NATIVE) {
		ret = nfb_ndp_tx_queue_setup(dev, qid, nb_tx_desc, socket_id, tx_conf, txq);
		if (ret)
			return ret;
	} else if (txq->queue_driver == NFB_QUEUE_DRIVER_NDP_SHARED) {
		txq->queue = ndp_open_tx_queue(internals->nfb, qid);
		if (txq->queue == NULL)
			return -EINVAL;
	} else {
		return -EINVAL;
	}

	txq->nfb = internals->nfb;
	txq->qid = qid;

	txq->tx_pkts = 0;
	txq->tx_bytes = 0;
	txq->err_pkts = 0;

	txq->deferred_start = tx_conf->tx_deferred_start;

	return 0;
}

void
nfb_eth_tx_queue_release(struct rte_eth_dev *dev, uint16_t qid)
{
	struct ndp_tx_queue *txq = dev->data->tx_queues[qid];

	if (txq->queue_driver == NFB_QUEUE_DRIVER_EMPTY) {
	} else if (txq->queue_driver == NFB_QUEUE_DRIVER_NATIVE) {
		return nfb_ndp_tx_queue_release(dev, txq);
	} else if (txq->queue_driver == NFB_QUEUE_DRIVER_NDP_SHARED) {
		if (txq->queue != NULL) {
			ndp_close_tx_queue(txq->queue);
			txq->queue = NULL;
			rte_free(txq);
		}
	}
}
