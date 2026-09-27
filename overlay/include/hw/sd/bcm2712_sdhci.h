/*
 * Broadcom BCM2712 SD/eMMC host controller
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SD_BCM2712_SDHCI_H
#define HW_SD_BCM2712_SDHCI_H

#include "hw/core/sysbus.h"
#include "hw/sd/sdhci.h"
#include "qom/object.h"

#define TYPE_BCM2712_SDHCI "bcm2712-sdhci"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2712SDHCIState, BCM2712_SDHCI)

/* The configuration registers, at this offset from the host's */
#define BCM2712_SDHCI_CFG_OFFSET    0x400
#define BCM2712_SDHCI_CFG_SIZE      0x200
#define BCM2712_SDHCI_SIZE          (BCM2712_SDHCI_CFG_OFFSET + \
                                     BCM2712_SDHCI_CFG_SIZE)

/*
 * One MMIO region, BCM2712_SDHCI_SIZE bytes, with the host's registers at
 * 0 and the configuration registers at BCM2712_SDHCI_CFG_OFFSET; one
 * interrupt; a "card-inserted" GPIO output, high while a card is in the
 * slot; and the slot itself, the SD bus "sd-bus" of @sdhci.
 */
struct BCM2712SDHCIState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    SDHCIState sdhci;
    MemoryRegion container;
    MemoryRegion cfg_iomem;
    uint32_t cfg[BCM2712_SDHCI_CFG_SIZE / 4];
};

#endif /* HW_SD_BCM2712_SDHCI_H */
