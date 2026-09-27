/*
 * Broadcom set-top-box (brcmstb) pin controller
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_GPIO_BRCMSTB_PINCTRL_H
#define HW_GPIO_BRCMSTB_PINCTRL_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_BRCMSTB_PINCTRL "brcmstb-pinctrl"
OBJECT_DECLARE_SIMPLE_TYPE(BrcmstbPinctrlState, BRCMSTB_PINCTRL)

#define BRCMSTB_PINCTRL_MAX_REGS    16

/* The "num-regs" property gives the number of 32-bit registers */
struct BrcmstbPinctrlState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t num_regs;
    uint32_t regs[BRCMSTB_PINCTRL_MAX_REGS];
};

#endif /* HW_GPIO_BRCMSTB_PINCTRL_H */
