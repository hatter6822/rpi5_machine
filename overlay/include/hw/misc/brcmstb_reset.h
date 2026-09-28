/*
 * Broadcom set-top-box (brcmstb) reset controllers
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_BRCMSTB_RESET_H
#define HW_MISC_BRCMSTB_RESET_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_BRCMSTB_RESET "brcmstb-reset"
OBJECT_DECLARE_SIMPLE_TYPE(BrcmstbResetState, BRCMSTB_RESET)

#define TYPE_BRCMSTB_RESCAL "brcmstb-rescal"
OBJECT_DECLARE_SIMPLE_TYPE(BrcmstbRescalState, BRCMSTB_RESCAL)

/* Each bank of the software-init controller drives 32 reset lines */
#define BRCMSTB_RESET_BANK_SIZE     0x18
#define BRCMSTB_RESET_MAX_BANKS     8

#define BRCMSTB_RESCAL_SIZE         0x10

/*
 * The software-init reset controller (brcm,brcmstb-reset). The "num-banks"
 * property sets its size. Its outputs are the named GPIO lines "reset",
 * 32 per bank, each high while the line is asserted.
 */
struct BrcmstbResetState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t num_banks;
    qemu_irq lines[BRCMSTB_RESET_MAX_BANKS * 32];

    uint32_t asserted[BRCMSTB_RESET_MAX_BANKS];
};

/*
 * The resistor calibration of the SATA and PCIe PHYs
 * (brcm,bcm7216-pcie-sata-rescal), which finishes at once.
 */
struct BrcmstbRescalState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;

    uint32_t start;
    uint32_t ctrl;
    bool done;
};

#endif /* HW_MISC_BRCMSTB_RESET_H */
