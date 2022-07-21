/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2019 Cesnet
 * Copyright(c) 2019 Netcope Technologies, a.s. <info@netcope.com>
 * All rights reserved.
 */

#include <nfb/nfb.h>
#include <nfb/ndp.h>
#include <netcope/eth.h>
#include <netcope/rxmac.h>
#include <netcope/txmac.h>
#include <netcope/mdio.h>
#include <netcope/ieee802_3.h>

#include <ethdev_pci.h>
#include <rte_kvargs.h>

#include "nfb_stats.h"
#include "nfb_rx.h"
#include "nfb_tx.h"
#include "nfb_rxmode.h"
#include "nfb.h"

#include "mdio.h"


static int nfb_eth_dev_uninit(struct rte_eth_dev *dev);

static int
nfb_mdio_read(void *priv, int prtad, int devad, uint16_t addr)
{
        return nc_mdio_read((struct nc_mdio*) priv, prtad, devad, addr);
}

static int
nfb_mdio_write(void *priv, int prtad, int devad, uint16_t addr,
		uint16_t val)
{
	return nc_mdio_write((struct nc_mdio*) priv, prtad, devad, addr, val);
}

/**
 * Default MAC addr
 */
static const struct rte_ether_addr eth_addr = {
	.addr_bytes = { 0x00, 0x11, 0x17, 0x00, 0x00, 0x00 }
};

/**
 * Open RX MAC components associated with current ifc
 *
 * @param priv
 *   Pointer to driver private structure
 * @param params
 *   Pointer to init parameters structure
 */
static int
nfb_nc_rxmac_init(struct pmd_internals *priv, struct nfb_init_params *params)
{
	int i, j;
	struct nc_ifc_info *ifc = params->ifc_info;
	struct nc_ifc_map_info *mi = &params->map_info;

	priv->rxmac = rte_zmalloc("NFB RxMAC", sizeof(*priv->rxmac) * ifc->eth_cnt, 0);
	if (priv->rxmac == NULL)
		return -ENOMEM;

	for (i = 0, j = 0; i < mi->eth_cnt && j < ifc->eth_cnt; i++) {
		if (mi->eth[i].ifc != ifc->id)
			continue;
		priv->rxmac[j] = nc_rxmac_open(priv->nfb, mi->eth[i].node_rxmac);
		if (priv->rxmac[j])
			j++;
	}

	priv->max_rxmac = j;
	return 0;
}

/**
 * Open TX MAC components associated with current ifc
 *
 * @param priv
 *   Pointer to driver private structure
 * @param params
 *   Pointer to init parameters structure
 */
static int
nfb_nc_txmac_init(struct pmd_internals *priv, struct nfb_init_params *params)
{

	int i, j;
	struct nc_ifc_info *ifc = params->ifc_info;
	struct nc_ifc_map_info *mi = &params->map_info;

	priv->txmac = rte_zmalloc("NFB TxMAC", sizeof(*priv->txmac) * ifc->eth_cnt, 0);
	if (priv->txmac == NULL)
		return -ENOMEM;

	for (i = 0, j = 0; i < mi->eth_cnt && j < ifc->eth_cnt; i++) {
		if (mi->eth[i].ifc != ifc->id)
			continue;
		priv->txmac[j] = nc_txmac_open(priv->nfb, mi->eth[i].node_txmac);
		if (priv->txmac[j])
			j++;
	}

	priv->max_txmac = j;
	return 0;
}

static int
nfb_nc_eth_init(struct pmd_internals *priv, struct nfb_init_params *params)
{
	int i, j;
	struct nc_ifc_info *ifc = params->ifc_info;
	struct nc_ifc_map_info *mi = &params->map_info;

	int node, node_cp;
	const int32_t *prop32;
	int proplen;
	const void *fdt;

	fdt = nfb_get_fdt(priv->nfb);

	priv->eth_node = rte_zmalloc("NFB eth", sizeof(*priv->eth_node) * ifc->eth_cnt, 0);
	if (priv->eth_node == NULL)
		return -ENOMEM;

	for (i = 0, j = 0; i < mi->eth_cnt && j < ifc->eth_cnt; i++) {
		if (mi->eth[i].ifc != ifc->id)
			continue;

		node = nc_eth_get_pcspma_control_node(fdt, mi->eth[i].node_eth, &node_cp);
		/* TODO: FIXME -1 */
		priv->eth_node[j].if_info.dev = nc_mdio_open(priv->nfb, node, -1);
		if (priv->eth_node[j].if_info.dev == NULL) {
			RTE_LOG(WARNING, PMD, "Cannot open MDIO for Eth\n");
			continue;
		}
		priv->eth_node[j].if_info.prtad = 0;
		priv->eth_node[j].if_info.mdio_read = nfb_mdio_read;
		priv->eth_node[j].if_info.mdio_write = nfb_mdio_write;

		prop32 = fdt_getprop(fdt, node_cp, "dev", &proplen);
		if (proplen == sizeof(*prop32)) {
			priv->eth_node[j].if_info.prtad = fdt32_to_cpu(*prop32);
		}

		j++;
	}
	priv->max_eth = j;
	return 0;
}

/**
 * Close all RX MAC components
 * @param priv
 *   Pointer to driver private structure
 */
static void
nfb_nc_rxmac_deinit(struct pmd_internals *priv)
{
	uint16_t i;
	for (i = 0; i < priv->max_rxmac; i++)
		nc_rxmac_close(priv->rxmac[i]);

	rte_free(priv->rxmac);
}

/**
 * Close all TX MAC components
 * @param priv
 *   Pointer to driver private structure
 */
static void
nfb_nc_txmac_deinit(struct pmd_internals *priv)
{
	uint16_t i;
	for (i = 0; i < priv->max_txmac; i++)
		nc_txmac_close(priv->txmac[i]);
	rte_free(priv->txmac);
}

static void
nfb_nc_eth_deinit(struct pmd_internals *priv)
{
	uint16_t i;
	for (i = 0; i < priv->max_eth; i++)
		nc_mdio_close(priv->eth_node[i].if_info.dev);
	rte_free(priv->eth_node);
}

/**
 * DPDK callback to start the device.
 *
 * Start device by starting all configured queues.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 *
 * @return
 *   0 on success, a negative errno value otherwise.
 */
static int
nfb_eth_dev_start(struct rte_eth_dev *dev)
{
	int ret;
	uint16_t i;
	uint16_t nb_rx = dev->data->nb_rx_queues;
	uint16_t nb_tx = dev->data->nb_tx_queues;

	for (i = 0; i < nb_rx; i++) {
		ret = nfb_eth_rx_queue_start(dev, i);
		if (ret != 0)
			goto err_rx;
	}

	for (i = 0; i < nb_tx; i++) {
		ret = nfb_eth_tx_queue_start(dev, i);
		if (ret != 0)
			goto err_tx;
	}

	return 0;

//	i = nb_tx;
err_tx:
	for (; i > 0; i--)
		nfb_eth_tx_queue_stop(dev, i-1);
	i = nb_rx;
err_rx:
	for (; i > 0; i--)
		nfb_eth_rx_queue_stop(dev, i-1);
	return ret;
}

/**
 * DPDK callback to stop the device.
 *
 * Stop device by stopping all configured queues.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 */
static int
nfb_eth_dev_stop(struct rte_eth_dev *dev)
{
	uint16_t i;
	uint16_t nb_rx = dev->data->nb_rx_queues;
	uint16_t nb_tx = dev->data->nb_tx_queues;

	for (i = 0; i < nb_tx; i++)
		nfb_eth_tx_queue_stop(dev, i);

	for (i = 0; i < nb_rx; i++)
		nfb_eth_rx_queue_stop(dev, i);

	return 0;
}

/**
 * DPDK callback for Ethernet device configuration.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 *
 * @return
 *   0 on success, a negative errno value otherwise.
 */
static int
nfb_eth_dev_configure(struct rte_eth_dev *dev)
{
	int ret;
	struct rte_eth_conf *dev_conf = &dev->data->dev_conf;

	if (dev_conf->rxmode.offloads & RTE_ETH_RX_OFFLOAD_TIMESTAMP) {
		ret = rte_mbuf_dyn_rx_timestamp_register
				(&nfb_timestamp_dynfield_offset,
				&nfb_timestamp_rx_dynflag);
		if (ret != 0) {
			RTE_LOG(ERR, PMD, "Cannot register Rx timestamp"
					" field/flag %d\n", ret);
			ret = -rte_errno;
			goto err_ts_register;
		}
	}

	return 0;

err_ts_register:
	nfb_eth_dev_uninit(dev);
	return ret;
}

static uint32_t
nfb_eth_get_max_mac_address_count(struct rte_eth_dev *dev)
{
	uint16_t i;
	uint32_t c;
	uint32_t ret = (uint32_t)-1;
	struct pmd_internals *internals = dev->process_private;

	/*
	 * Go through all RX MAC components in firmware and find
	 * the minimal indicated space size for MAC addresses.
	 */
	for (i = 0; i < internals->max_rxmac; i++) {
		c = nc_rxmac_mac_address_count(internals->rxmac[i]);
		ret = RTE_MIN(c, ret);
	}

	/* The driver must support at least 1 MAC address, pretend that */
	if (internals->max_rxmac == 0 || ret == 0)
		ret = 1;

	return ret;
}

/**
 * DPDK callback to get information about the device.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param[out] info
 *   Info structure output buffer.
 */
static int
nfb_eth_dev_info(struct rte_eth_dev *dev,
	struct rte_eth_dev_info *dev_info)
{
	struct pmd_priv *priv = dev->data->dev_private;

	dev_info->max_mac_addrs = nfb_eth_get_max_mac_address_count(dev);

	dev_info->max_rx_pktlen = (uint32_t)-1;
	dev_info->max_rx_queues = priv->max_rx_queues;
	dev_info->max_tx_queues = priv->max_tx_queues;
	dev_info->speed_capa = RTE_ETH_LINK_SPEED_100G;
	dev_info->rx_offload_capa =
		RTE_ETH_RX_OFFLOAD_TIMESTAMP;

	return 0;
}

/**
 * DPDK callback to close the device.
 *
 * Destroy all queues and objects, free memory.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 */
static int
nfb_eth_dev_close(struct rte_eth_dev *dev)
{
	uint16_t i;
	uint16_t nb_rx = dev->data->nb_rx_queues;
	uint16_t nb_tx = dev->data->nb_tx_queues;

	if (rte_eal_process_type() == RTE_PROC_PRIMARY) {
		for (i = 0; i < nb_rx; i++) {
			if (dev->data->rx_queues[i]) {
				nfb_eth_rx_queue_release(dev, i);
				dev->data->rx_queues[i] = NULL;
			}
		}
		for (i = 0; i < nb_tx; i++) {
			if (dev->data->tx_queues[i]) {
				nfb_eth_tx_queue_release(dev, i);
				dev->data->tx_queues[i] = NULL;
			}
		}
	}

	nfb_eth_dev_uninit(dev);

	return 0;
}

/**
 * DPDK callback to retrieve physical link information.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param[out] link
 *   Storage for current link status.
 *
 * @return
 *   0 on success, a negative errno value otherwise.
 */
static int
nfb_eth_link_update(struct rte_eth_dev *dev,
	int wait_to_complete __rte_unused)
{
	uint16_t i;
	struct nc_rxmac_status status;
	struct rte_eth_link link;
	memset(&link, 0, sizeof(link));

	struct pmd_internals *internals = dev->process_private;

	status.speed = MAC_SPEED_UNKNOWN;

	link.link_speed = RTE_ETH_SPEED_NUM_NONE;
	link.link_status = RTE_ETH_LINK_DOWN;
	link.link_duplex = RTE_ETH_LINK_FULL_DUPLEX;
	link.link_autoneg = RTE_ETH_LINK_SPEED_FIXED;

	if (internals->max_rxmac) {
		nc_rxmac_read_status(internals->rxmac[0], &status);

		switch (status.speed) {
		case MAC_SPEED_10G:
			link.link_speed = RTE_ETH_SPEED_NUM_10G;
			break;
		case MAC_SPEED_40G:
			link.link_speed = RTE_ETH_SPEED_NUM_40G;
			break;
		case MAC_SPEED_100G:
			link.link_speed = RTE_ETH_SPEED_NUM_100G;
			break;
		default:
			link.link_speed = RTE_ETH_SPEED_NUM_NONE;
			break;
		}
	}

	for (i = 0; i < internals->max_rxmac; ++i) {
		nc_rxmac_read_status(internals->rxmac[i], &status);

		if (status.enabled && status.link_up) {
			link.link_status = RTE_ETH_LINK_UP;
			break;
		}
	}

	rte_eth_linkstatus_set(dev, &link);

	return 0;
}

/**
 * DPDK callback to bring the link UP.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 *
 * @return
 *   0 on success, a negative errno value otherwise.
 */
static int
nfb_eth_dev_set_link_up(struct rte_eth_dev *dev)
{
	struct pmd_internals *internals = (struct pmd_internals *)
		dev->process_private;

	uint16_t i;
	for (i = 0; i < internals->max_rxmac; ++i)
		nc_rxmac_enable(internals->rxmac[i]);

	for (i = 0; i < internals->max_txmac; ++i)
		nc_txmac_enable(internals->txmac[i]);

	return 0;
}

/**
 * DPDK callback to bring the link DOWN.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 *
 * @return
 *   0 on success, a negative errno value otherwise.
 */
static int
nfb_eth_dev_set_link_down(struct rte_eth_dev *dev)
{
	struct pmd_internals *internals = (struct pmd_internals *)
		dev->process_private;

	uint16_t i;
	for (i = 0; i < internals->max_rxmac; ++i)
		nc_rxmac_disable(internals->rxmac[i]);

	for (i = 0; i < internals->max_txmac; ++i)
		nc_txmac_disable(internals->txmac[i]);

	return 0;
}

static uint64_t
nfb_eth_mac_addr_conv(struct rte_ether_addr *mac_addr)
{
	int i;
	uint64_t res = 0;
	for (i = 0; i < RTE_ETHER_ADDR_LEN; i++) {
		res <<= 8;
		res |= mac_addr->addr_bytes[i] & 0xFF;
	}
	return res;
}

/**
 * DPDK callback to set primary MAC address.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param mac_addr
 *   MAC address to register.
 *
 * @return
 *   0 on success, a negative errno value otherwise.
 */
static int
nfb_eth_mac_addr_set(struct rte_eth_dev *dev,
	struct rte_ether_addr *mac_addr)
{
	unsigned int i;
	uint64_t mac;
	struct pmd_internals *internals = (struct pmd_internals *)
		dev->process_private;

	mac = nfb_eth_mac_addr_conv(mac_addr);
	/* Until no real multi-port support, configure all RX MACs the same */
	for (i = 0; i < internals->max_rxmac; ++i)
		nc_rxmac_set_mac(internals->rxmac[i], 0, mac, 1);

	return 0;
}

static int
nfb_eth_mac_addr_add(struct rte_eth_dev *dev,
	struct rte_ether_addr *mac_addr, uint32_t index, uint32_t pool __rte_unused)
{
	unsigned int i;
	uint64_t mac;
	struct pmd_internals *internals = (struct pmd_internals *)
		dev->process_private;

	mac = nfb_eth_mac_addr_conv(mac_addr);
	for (i = 0; i < internals->max_rxmac; ++i)
		nc_rxmac_set_mac(internals->rxmac[i], index, mac, 1);

	return 0;
}

static void
nfb_eth_mac_addr_remove(struct rte_eth_dev *dev, uint32_t index)
{
	unsigned int i;
	struct pmd_internals *internals = (struct pmd_internals *)
		dev->process_private;

	for (i = 0; i < internals->max_rxmac; ++i)
		nc_rxmac_set_mac(internals->rxmac[i], index, 0, 0);
}

static int
nfb_eth_fw_version_get(struct rte_eth_dev *dev, char *fw_version,
		size_t fw_size)
{
	int ret;
	const char *proj_name, *proj_vers;
	struct pmd_internals *priv = (struct pmd_internals *)
			dev->process_private;

	proj_name = nc_info_get_fw_project_name(priv->nfb, NULL);
	proj_vers = nc_info_get_fw_project_version(priv->nfb, NULL);

	if (proj_name == NULL)
		proj_name = "";
	if (proj_vers == NULL)
		proj_vers = "";

	ret = snprintf(fw_version, fw_size, "%s;%s", proj_name, proj_vers);

	if (ret >= (signed)fw_size)
		return strlen(proj_name) + 1 + strlen(proj_vers) + 1;

	return 0;
}

static int
nfb_eth_fec_get(struct rte_eth_dev *dev, uint32_t *fec_capa)
{
	int fec_enabled;
	uint16_t reg;

	struct pmd_internals *priv = dev->process_private;
	struct mdio_if_info *if_info;

	if (priv->max_eth == 0)
		return -ENODEV;

	if_info = &priv->eth_node[0].if_info;

	reg = if_info->mdio_read(if_info->dev, if_info->prtad, 1, 200);
	fec_enabled = (reg & (1 << 2) ? 1 : 0);

	*fec_capa = fec_enabled ? RTE_ETH_FEC_MODE_CAPA_MASK(RS) :
			RTE_ETH_FEC_MODE_CAPA_MASK(NOFEC);

	return 0;
}

static int
nfb_eth_fec_set(struct rte_eth_dev *dev, uint32_t fec_capa)
{
	int ret;
	int fec_enabled;
	uint32_t fec_capa2 = 0;
	uint16_t reg;

	struct pmd_internals *priv = dev->process_private;
	struct mdio_if_info *if_info;

	if (priv->max_eth == 0)
		return -ENODEV;

	if_info = &priv->eth_node[0].if_info;

	if (fec_capa & RTE_ETH_FEC_MODE_CAPA_MASK(AUTO))
		return -ENOSYS;

	reg = if_info->mdio_read(if_info->dev, if_info->prtad, 1, 200);
	fec_enabled = fec_capa & RTE_ETH_FEC_MODE_CAPA_MASK(RS);

	reg = (fec_enabled) ? (reg | (1 << 2)) : (reg & ~(1 << 2));
	if_info->mdio_write(if_info->dev, if_info->prtad, 1, 200, reg);
	ret = nfb_eth_fec_get(dev, &fec_capa2);

	if (fec_capa != fec_capa2)
		return -ENOTSUP;

	return ret;
}

static const struct eth_dev_ops ops = {
	.dev_start = nfb_eth_dev_start,
	.dev_stop = nfb_eth_dev_stop,
	.dev_set_link_up = nfb_eth_dev_set_link_up,
	.dev_set_link_down = nfb_eth_dev_set_link_down,
	.dev_close = nfb_eth_dev_close,
	.dev_configure = nfb_eth_dev_configure,
	.dev_infos_get = nfb_eth_dev_info,
	.promiscuous_enable = nfb_eth_promiscuous_enable,
	.promiscuous_disable = nfb_eth_promiscuous_disable,
	.allmulticast_enable = nfb_eth_allmulticast_enable,
	.allmulticast_disable = nfb_eth_allmulticast_disable,
	.rx_queue_start = nfb_eth_rx_queue_start,
	.rx_queue_stop = nfb_eth_rx_queue_stop,
	.tx_queue_start = nfb_eth_tx_queue_start,
	.tx_queue_stop = nfb_eth_tx_queue_stop,
	.rx_queue_setup = nfb_eth_rx_queue_setup,
	.tx_queue_setup = nfb_eth_tx_queue_setup,
	.rx_queue_release = nfb_eth_rx_queue_release,
	.tx_queue_release = nfb_eth_tx_queue_release,
	.link_update = nfb_eth_link_update,
	.stats_get = nfb_eth_stats_get,
	.stats_reset = nfb_eth_stats_reset,
	.mac_addr_set = nfb_eth_mac_addr_set,
	.mac_addr_add = nfb_eth_mac_addr_add,
	.mac_addr_remove = nfb_eth_mac_addr_remove,
	.fw_version_get = nfb_eth_fw_version_get,
	.fec_get = nfb_eth_fec_get,
	.fec_set = nfb_eth_fec_set,
};

/**
 * DPDK callback to initialize an ethernet device
 *
 * @param dev
 *   Pointer to ethernet device structure
 *
 * @return
 *   0 on success, a negative errno value otherwise.
 */
static int
nfb_eth_dev_init(struct rte_eth_dev *dev, void *init_data)
{
	int i;
	int cnt;
	int ret;
	struct rte_eth_dev_data *data = dev->data;
	struct pmd_internals *internals;
	struct nfb_init_params *params = init_data;
	struct nc_ifc_info *ifc = params->ifc_info;
	struct nc_ifc_map_info *mi = &params->map_info;
	struct pmd_priv *priv = data->dev_private;
	struct rte_ether_addr eth_addr_init;
	struct rte_kvargs *kvlist;

	internals = (struct pmd_internals *) rte_zmalloc_socket("nfb_internals",
			sizeof(struct pmd_internals), RTE_CACHE_LINE_SIZE,
			dev->device->numa_node);
	if (internals == NULL) {
		ret = -ENOMEM;
		goto err_alloc_internals;
	}

	dev->process_private = internals;

	/* Check validity of device args */
	if (dev->device->devargs != NULL &&
			dev->device->devargs->args != NULL &&
			strlen(dev->device->devargs->args) > 0) {
		kvlist = rte_kvargs_parse(dev->device->devargs->args,
						VALID_KEYS);
		if (kvlist == NULL) {
			RTE_LOG(ERR, PMD, "Failed to parse device arguments %s",
				dev->device->devargs->args);
			ret = -EINVAL;
			goto err_devargs_inval;
		}
		rte_kvargs_free(kvlist);
	}

	/* Open device handle */
	internals->nfb = nfb_open(params->path);
	if (internals->nfb == NULL) {
		RTE_LOG(ERR, PMD, "nfb_open(): failed to open %s", params->path);
		ret = -EINVAL;
		goto err_nfb_open;
	}

	nfb_nc_rxmac_init(internals, params);
	nfb_nc_txmac_init(internals, params);
	nfb_nc_eth_init(internals, params);

	/* Set rx, tx burst functions */
	dev->rx_pkt_burst = nfb_eth_ndp_rx;
	dev->tx_pkt_burst = nfb_eth_ndp_tx;

	/* Get number of available DMA RX and TX queues */
	priv->max_rx_queues = ifc->rxq_cnt;
	priv->max_tx_queues = ifc->txq_cnt;

	internals->queue_map_rx = rte_malloc("NFB queue map",
			sizeof(*internals->queue_map_rx) *
			(priv->max_rx_queues + priv->max_tx_queues), 0);
	if (internals->queue_map_rx == NULL) {
		ret = -ENOMEM;
		goto err_alloc_queue_map;
	}
	internals->queue_map_tx = internals->queue_map_rx + priv->max_rx_queues;

	cnt = 0;
	for (i = 0; i < mi->rxq_cnt; i++) {
		if (mi->rxq[i].ifc == ifc->id) {
			internals->queue_map_rx[cnt++] = mi->rxq[i].id;
		}
	}

	cnt = 0;
	for (i = 0; i < mi->txq_cnt; i++) {
		if (mi->txq[i].ifc == ifc->id) {
			internals->queue_map_tx[cnt++] = mi->txq[i].id;
		}
	}

	/* Set function callbacks for Ethernet API */
	dev->dev_ops = &ops;

	/* Get link state */
	nfb_eth_link_update(dev, 0);

	if (rte_eal_process_type() == RTE_PROC_PRIMARY) {
		/* Allocate space for MAC addresses */
		cnt = nfb_eth_get_max_mac_address_count(dev);
		data->mac_addrs = rte_zmalloc(data->name,
			sizeof(struct rte_ether_addr) * cnt, RTE_CACHE_LINE_SIZE);
		if (data->mac_addrs == NULL) {
			RTE_LOG(ERR, PMD, "Could not alloc space for MAC address!\n");
			ret = -ENOMEM;
			goto err_malloc_mac_addrs;
		}

		ret = nc_ifc_get_default_mac(internals->nfb, ifc->id, eth_addr_init.addr_bytes,
			sizeof(eth_addr_init.addr_bytes));
		if (ret != 0) {
			rte_eth_random_addr(eth_addr_init.addr_bytes);
			eth_addr_init.addr_bytes[0] = eth_addr.addr_bytes[0];
			eth_addr_init.addr_bytes[1] = eth_addr.addr_bytes[1];
			eth_addr_init.addr_bytes[2] = eth_addr.addr_bytes[2];
		}

		nfb_eth_mac_addr_set(dev, &eth_addr_init);
		rte_ether_addr_copy(&eth_addr_init, &data->mac_addrs[0]);

		data->promiscuous = nfb_eth_promiscuous_get(dev);
		data->all_multicast = nfb_eth_allmulticast_get(dev);

		data->dev_flags |= RTE_ETH_DEV_AUTOFILL_QUEUE_XSTATS;
	}


	return 0;

err_malloc_mac_addrs:
	rte_free(internals->queue_map_rx);
err_alloc_queue_map:
	nfb_nc_rxmac_deinit(internals);
	nfb_nc_txmac_deinit(internals);
	nfb_nc_eth_deinit(internals);
	nfb_close(internals->nfb);

err_nfb_open:
err_devargs_inval:
	rte_free(internals);
err_alloc_internals:
	return ret;
}

/**
 * DPDK callback to uninitialize an ethernet device
 *
 * @param dev
 *   Pointer to ethernet device structure
 *
 * @return
 *   0 on success, a negative errno value otherwise.
 */
static int
nfb_eth_dev_uninit(struct rte_eth_dev *dev)
{
	struct pmd_internals *internals = dev->process_private;

	nfb_nc_rxmac_deinit(internals);
	nfb_nc_txmac_deinit(internals);
	nfb_nc_eth_deinit(internals);
	nfb_close(internals->nfb);

	rte_free(internals->queue_map_rx);
	rte_free(internals);

	return 0;
}

static const struct rte_pci_id nfb_pci_id_table[] = {
	{ RTE_PCI_DEVICE(PCI_VENDOR_ID_NETCOPE, PCI_DEVICE_ID_NFB_40G2) },
	{ RTE_PCI_DEVICE(PCI_VENDOR_ID_NETCOPE, PCI_DEVICE_ID_NFB_100G2) },
	{ RTE_PCI_DEVICE(PCI_VENDOR_ID_NETCOPE, PCI_DEVICE_ID_NFB_200G2QL) },
	{ RTE_PCI_DEVICE(PCI_VENDOR_ID_NETCOPE, PCI_DEVICE_ID_NFB_200G2QL_E1) },
	{ RTE_PCI_DEVICE(PCI_VENDOR_ID_SILICOM, PCI_DEVICE_ID_FB2CGG3) },
	{ RTE_PCI_DEVICE(PCI_VENDOR_ID_SILICOM, PCI_DEVICE_ID_FB2CGHH) },
	{ RTE_PCI_DEVICE(PCI_VENDOR_ID_SILICOM, PCI_DEVICE_ID_FB2CGG3D) },
	{ RTE_PCI_DEVICE(PCI_VENDOR_ID_CESNET,  PCI_DEVICE_ID_COMBO400G1) },
	{ .vendor_id = 0, }
};

/**
 * DPDK callback to register a PCI device.
 *
 * This function spawns Ethernet devices out of a given PCI device.
 *
 * @param[in] pci_drv
 *   PCI driver structure (nfb_driver).
 * @param[in] pci_dev
 *   PCI device information.
 *
 * @return
 *   0 on success, a negative errno value otherwise.
 */
static int
nfb_eth_pci_probe(struct rte_pci_driver *pci_drv __rte_unused,
		struct rte_pci_device *pci_dev)
{
	int i;
	int ret;
	int basename_len;
	char name[RTE_ETH_NAME_MAX_LEN];
	char path[PATH_MAX];

	struct nc_composed_device_info comp_dev_info;
	struct nc_ifc_info *ifc;
	struct nfb_device *nfb_dev;
	struct nfb_init_params params;

	rte_pci_device_name(&pci_dev->addr, name, sizeof(name));
	basename_len = strlen(name);

	/* NFB device can be composed from multiple PCI devices,
	 * find the base char device ID for the current PCI device */
	ret = nc_get_composed_device_info_by_pci(NULL, name, &comp_dev_info);
	if (ret) {
		RTE_LOG(ERR, PMD, "Could not find NFB device for %s\n", name);
		return -ENODEV;
	}

	ret = snprintf(path, sizeof(path), NFB_BASE_DEV_PATH "%d", comp_dev_info.nfb_id);
	RTE_ASSERT(ret > 0 && ret < sizeof(path));

	nfb_dev = nfb_open(path);
	if (nfb_dev == NULL) {
		RTE_LOG(ERR, PMD, "nfb_open(): failed to open %s", path);
		return -EINVAL;
	}

	params.path = path;

	ret = nc_ifc_map_info_create_ordinary(nfb_dev, &params.map_info);
	if (ret) {
		/* TODO: create old-style mapping */
	}

	params.nfb_id = comp_dev_info.nfb_id;
	for (i = 0; i < params.map_info.ifc_cnt; i++) {
		ifc = params.ifc_info = &params.map_info.ifc[i];

		/* Skip interfaces which doesn't belong to this PCI device */
		if (ifc->ep != comp_dev_info.ep_index ||
				(ifc->flags & NC_IFC_INFO_FLAG_ACTIVE) == 0)
			continue;

		snprintf(name + basename_len, sizeof(name) - basename_len,
				"_nfb%d_eth%d", comp_dev_info.nfb_id, params.ifc_info->id);

		ret = rte_eth_dev_create(&pci_dev->device, name,
				sizeof(struct pmd_priv),
				eth_dev_pci_specific_init, pci_dev,
				nfb_eth_dev_init, &params);
	}

	nc_map_info_destroy(&params.map_info);
	nfb_close(nfb_dev);

	return 0;
}

/**
 * DPDK callback to remove a PCI device.
 *
 * This function removes all Ethernet devices belong to a given PCI device.
 *
 * @param[in] pci_dev
 *   Pointer to the PCI device.
 *
 * @return
 *   0 on success, the function cannot fail.
 */
static int
nfb_eth_pci_remove(struct rte_pci_device *pci_dev __rte_unused)
{
	return 0;
}

static struct rte_pci_driver nfb_eth_driver = {
	.id_table = nfb_pci_id_table,
	.probe = nfb_eth_pci_probe,
	.remove = nfb_eth_pci_remove,
};

RTE_PMD_REGISTER_PCI(RTE_NFB_DRIVER_NAME, nfb_eth_driver);
RTE_PMD_REGISTER_PCI_TABLE(RTE_NFB_DRIVER_NAME, nfb_pci_id_table);
RTE_PMD_REGISTER_KMOD_DEP(RTE_NFB_DRIVER_NAME, "* nfb");
