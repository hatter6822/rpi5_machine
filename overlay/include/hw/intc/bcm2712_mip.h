/*
 * Broadcom BCM2712 MIP MSI interrupt controller
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_INTC_BCM2712_MIP_H
#define HW_INTC_BCM2712_MIP_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_BCM2712_MIP "bcm2712-mip"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2712MIPState, BCM2712_MIP)

#define BCM2712_MIP_NUM_IRQS    64
#define BCM2712_MIP_SIZE        0xc0

/*
 * The outputs are the device's 64 unnamed GPIO lines, one per vector,
 * each for an SPI of its own: a PCIe device's MSI raises vector n by
 * writing n to the first register.
 */
struct BCM2712MIPState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq[BCM2712_MIP_NUM_IRQS];

    uint64_t status;        /* raised and not cleared */
    uint64_t cfg;           /* 1 = edge, 0 = level */
    uint64_t mask;          /* 1 = masked from the host */
    uint64_t vpu_mask;      /* kept for the VideoCore, which is not there */
};

#endif /* HW_INTC_BCM2712_MIP_H */
