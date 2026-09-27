/*
 * Broadcom set-top-box (brcmstb) GPIO controller
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_GPIO_BRCMSTB_GPIO_H
#define HW_GPIO_BRCMSTB_GPIO_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_BRCMSTB_GPIO "brcmstb-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(BrcmstbGpioState, BRCMSTB_GPIO)

#define BRCMSTB_GPIO_MAX_BANKS      8
#define BRCMSTB_GPIO_BANK_LINES     32
#define BRCMSTB_GPIO_BANK_SIZE      0x20

/* The registers of one bank, and the levels driven into its lines */
typedef struct BrcmstbGpioBank {
    uint32_t oden;      /* open-drain enable */
    uint32_t data;      /* output latch */
    uint32_t iodir;     /* 1 = input */
    uint32_t ec;        /* 1 = rising edge or high level */
    uint32_t ei;        /* 1 = both edges */
    uint32_t mask;      /* 1 = interrupt enabled */
    uint32_t level;     /* 1 = level-sensitive */
    uint32_t stat;      /* interrupt status */
    uint32_t input;     /* levels driven into the lines from outside */

    /* Not migrated: derived from the fields above */
    uint32_t valid;     /* the lines the bank has */
    uint32_t pads;      /* the level of each line */
} BrcmstbGpioBank;

/*
 * Line n of the controller, bank n / 32, is its GPIO input n (a level
 * driven into the pad from outside) and GPIO output n (the level of the
 * pad); sysbus IRQ 0 is the interrupt shared by every bank. The
 * "bank-widths" array property gives the number of lines of each bank,
 * and so the number of banks.
 */
struct BrcmstbGpioState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    qemu_irq out[BRCMSTB_GPIO_MAX_BANKS * BRCMSTB_GPIO_BANK_LINES];

    uint32_t num_banks;
    uint32_t *bank_widths;

    BrcmstbGpioBank bank[BRCMSTB_GPIO_MAX_BANKS];
};

#endif /* HW_GPIO_BRCMSTB_GPIO_H */
