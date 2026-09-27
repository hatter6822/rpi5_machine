/*
 * Broadcom set-top-box (brcmstb) level 2 interrupt controller
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_INTC_BRCMSTB_L2_INTC_H
#define HW_INTC_BRCMSTB_L2_INTC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_BRCMSTB_L2_INTC "brcmstb-l2-intc"
OBJECT_DECLARE_SIMPLE_TYPE(BrcmstbL2IntcState, BRCMSTB_L2_INTC)

#define BRCMSTB_L2_INTC_NUM_IRQS    32

/* Register block sizes: with SET and CLEAR (edge, bcm2711), and without */
#define BRCMSTB_L2_INTC_EDGE_SIZE   0x18
#define BRCMSTB_L2_INTC_LEVEL_SIZE  0x10

/*
 * Inputs are the device's 32 unnamed GPIO lines; the output is its sysbus
 * IRQ 0. The "edge" property selects the brcm,l2-intc variant and the
 * "bcm2711" property the brcm,bcm2711-l2-intc one; neither is the
 * brcm,bcm7271-l2-intc level controller.
 */
struct BrcmstbL2IntcState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    bool edge;
    bool bcm2711;

    uint32_t input;     /* levels of the input lines */
    uint32_t status;    /* latched by edges (edge) or SET (edge, bcm2711) */
    uint32_t mask;      /* 1 = masked */
};

#endif /* HW_INTC_BRCMSTB_L2_INTC_H */
