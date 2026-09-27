/*
 * Broadcom set-top-box (brcmstb) BSC I2C controller
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_I2C_BRCMSTB_I2C_H
#define HW_I2C_BRCMSTB_I2C_H

#include "hw/core/sysbus.h"
#include "hw/i2c/i2c.h"
#include "qom/object.h"

#define TYPE_BRCMSTB_I2C "brcmstb-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(BrcmstbI2cState, BRCMSTB_I2C)

#define BRCMSTB_I2C_NUM_DATA_REGS   8
#define BRCMSTB_I2C_SIZE            0x58

struct BrcmstbI2cState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    I2CBus *bus;
    qemu_irq irq;

    uint32_t chip_address;
    uint32_t data_in[BRCMSTB_I2C_NUM_DATA_REGS];
    uint32_t cnt;
    uint32_t ctl;
    uint32_t iic_enable;
    uint32_t data_out[BRCMSTB_I2C_NUM_DATA_REGS];
    uint32_t ctlhi;
};

#endif /* HW_I2C_BRCMSTB_I2C_H */
