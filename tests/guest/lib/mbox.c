/*
 * The VideoCore mailbox and the firmware property interface.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <bm/io.h>
#include <bm/mbox.h>
#include <bm/runtime.h>
#include <bm/timer.h>

#define MBOX_READ               0x00
#define MBOX_STATUS             0x18
#define MBOX_WRITE              0x20
#define MBOX_WRITE_STATUS       0x38
#define MBOX_STATUS_FULL        BIT(31)
#define MBOX_STATUS_EMPTY       BIT(30)
#define MBOX_CHAN_PROPERTY      8

/* The VideoCore sees the first GiB of RAM here (dma-ranges of "soc") */
#define VC_BUS_RAM              0xc0000000u
#define VC_RAM_WINDOW           0x40000000u

bool mbox_property(volatile uint32_t *buf, uint64_t wait_us)
{
    uintptr_t mbox = bm_plat.mbox;
    uint32_t msg = VC_BUS_RAM | (uint32_t)(uintptr_t)buf | MBOX_CHAN_PROPERTY;

    if ((uintptr_t)buf & 0xf || (uintptr_t)buf >= VC_RAM_WINDOW) {
        return false;
    }
    if (!wait_until(!(mmio_read32(mbox + MBOX_WRITE_STATUS) &
                      MBOX_STATUS_FULL), wait_us)) {
        return false;
    }
    dsb_sy();
    mmio_write32(mbox + MBOX_WRITE, msg);
    if (!wait_until(!(mmio_read32(mbox + MBOX_STATUS) & MBOX_STATUS_EMPTY),
                    wait_us)) {
        return false;
    }
    if (mmio_read32(mbox + MBOX_READ) != msg) {
        return false;
    }
    dsb_sy();
    return buf[1] == FW_SUCCESS;
}
