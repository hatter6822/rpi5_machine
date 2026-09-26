/*
 * Broadcom RNG200 random number generator (BCM2711, BCM2712)
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_BCM2711_RNG200_H
#define HW_MISC_BCM2711_RNG200_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_BCM2711_RNG200 "bcm2711-rng200"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2711Rng200State, BCM2711_RNG200)

#define BCM2711_RNG200_MMIO_SIZE    0x28
#define BCM2711_RNG200_FIFO_DEPTH   16      /* words */

struct BCM2711Rng200State {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t ctrl;
    uint32_t rng_soft_reset;
    uint32_t rbg_soft_reset;
    uint32_t total_bit_count;
    uint32_t total_bit_count_threshold;
    uint32_t int_status;
    uint32_t int_enable;
    uint32_t fifo_threshold;
    uint32_t fifo_count;
};

#endif /* HW_MISC_BCM2711_RNG200_H */
