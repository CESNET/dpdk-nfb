/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 CESNET, z.s.p.o.
 * All rights reserved.
 */

#include <stdint.h>
#include <rte_ethdev.h>

#include "mdio.h"

void
nfb_ieee802_3_pma_pmd_get_speed_capa(struct mdio_if_info *info, uint32_t *capa)
{
        int i;
        uint16_t reg;

        const int speed_ability[16] = {
                RTE_ETH_LINK_SPEED_10G,
                0,
                0,
                RTE_ETH_LINK_SPEED_50G,
                RTE_ETH_LINK_SPEED_1G,
                RTE_ETH_LINK_SPEED_100M,
                RTE_ETH_LINK_SPEED_10M,
                0,
                RTE_ETH_LINK_SPEED_40G,
                RTE_ETH_LINK_SPEED_100G,
                0,
                RTE_ETH_LINK_SPEED_25G,
                RTE_ETH_LINK_SPEED_200G,
                RTE_ETH_LINK_SPEED_2_5G,
                RTE_ETH_LINK_SPEED_5G,
                0/*RTE_ETH_LINK_SPEED_400G*/,
        };

        reg = info->mdio_read(info->dev, info->prtad, 1, 4);

        for (i = 0; i < 16; i++) {
                if (reg & (1 << i))
                        *capa |= speed_ability[i];
        }
}
