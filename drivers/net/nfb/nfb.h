/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2019 Cesnet
 * Copyright(c) 2019 Netcope Technologies, a.s. <info@netcope.com>
 * All rights reserved.
 */

#ifndef _NFB_H_
#define _NFB_H_

#include <nfb/nfb.h>
#include <nfb/ndp.h>
#include <netcope/rxmac.h>
#include <netcope/txmac.h>
#include <netcope/mdio_if_info.h>
/* TODO: move queue map info to queue_mapper.h (can be unit in hw) */
#include <netcope/info.h>
#include <netcope/nic_rss.h>

#include "nfb_rx.h"
#include "nfb_tx.h"

/* PCI Vendor ID */
#define PCI_VENDOR_ID_NETCOPE 0x1b26
#define PCI_VENDOR_ID_SILICOM 0x1c2c
#define PCI_VENDOR_ID_CESNET  0x18ec

/* PCI Device IDs */
#define PCI_DEVICE_ID_NFB_40G2  0xcb80
#define PCI_DEVICE_ID_NFB_100G2 0xc2c1
#define PCI_DEVICE_ID_NFB_200G2QL 0xc250
#define PCI_DEVICE_ID_NFB_200G2QL_E1 0xc251
#define PCI_DEVICE_ID_FB2CGG3   0x00d0
#define PCI_DEVICE_ID_FB2CGHH   0x00d2
#define PCI_DEVICE_ID_FB2CGG3D  0xc240
#define PCI_DEVICE_ID_COMBO400G1 0xc400

/* Max index of ndp rx/tx queues */
#define RTE_ETH_NDP_MAX_RX_QUEUES 32
#define RTE_ETH_NDP_MAX_TX_QUEUES 32

#define RTE_NFB_DRIVER_NAME net_nfb

#define NFB_QUEUE_DRIVER_NDP_SHARED 1
#define NFB_QUEUE_DRIVER_NATIVE 2

/* Device arguments */
#define NFB_ARG_RXHDR_DYNFIELD "rxhdr_dynfield"
#define NFB_ARG_QUEUE_DRIVER "queue_driver"

static const char * const VALID_KEYS[] = {NFB_ARG_RXHDR_DYNFIELD, NFB_ARG_QUEUE_DRIVER, NULL};

struct eth_node{
	int channel_id;
	struct mdio_if_info if_info;
};

struct pmd_internals {
	uint16_t         max_rxmac;
	uint16_t         max_txmac;
	uint16_t         max_eth;
	struct nc_rxmac **rxmac;
	struct nc_txmac **txmac;
	struct eth_node *eth_node;
	int             *queue_map_rx;
	int             *queue_map_tx;

	TAILQ_ENTRY(pmd_internals) eth_dev_list;
	struct rte_pci_device *pci_dev;
	struct rte_eth_dev *eth_dev;

	struct nfb_device *nfb;
	int nfb_id;
	int flags;

	struct nc_nic_rss *comp_rss;
};

struct pmd_priv {
	int max_rx_queues;
	int max_tx_queues;
};

struct nfb_init_params {
	const char *path;
	int nfb_id;

	struct nc_ifc_map_info map_info;
	struct nc_ifc_info *ifc_info;
};
#endif /* _NFB_H_ */
