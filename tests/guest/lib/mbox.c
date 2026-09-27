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

/* The VideoCore sees the first GiB of RAM here, as on older Pis */
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

int mbox_tag(uint32_t tag, uint32_t *val, unsigned words)
{
    /* Header, tag header, value buffer and end tag, in whole 16 bytes */
    static volatile uint32_t buf[(2 + 3 + MBOX_TAG_MAX_WORDS + 1 + 3) & ~3]
        __attribute__((aligned(16)));
    const unsigned size = (2 + 3 + words + 1 + 3) & ~3;
    uint32_t code;

    if (words > MBOX_TAG_MAX_WORDS) {
        return -1;
    }
    buf[0] = size * 4;
    buf[1] = FW_REQUEST;
    buf[2] = tag;
    buf[3] = words * 4;
    buf[4] = 0;
    for (unsigned i = 0; i < words; i++) {
        buf[5 + i] = val[i];
    }
    for (unsigned i = 5 + words; i < size; i++) {
        buf[i] = 0;                     /* the end tag, then padding */
    }
    if (!mbox_property(buf, 100000)) {
        return -1;
    }
    code = buf[4];
    if (!(code & FW_TAG_RESPONSE)) {
        return -1;
    }
    for (unsigned i = 0; i < words; i++) {
        val[i] = buf[5 + i];
    }
    return code & ~FW_TAG_RESPONSE;
}
