/*
 * The VideoCore mailbox and the firmware property interface.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Registers as used by Linux drivers/mailbox/bcm2835-mailbox.c, tags from
 * include/soc/bcm2835/raspberrypi-firmware.h.
 */

#ifndef BM_MBOX_H
#define BM_MBOX_H

#include <stdbool.h>
#include <stdint.h>

#define FW_REQUEST              0
#define FW_SUCCESS              0x80000000u
#define FW_TAG_BOARD_REVISION   0x00010002u

/*
 * Send a property request: @buf is the whole message (size, code, tags,
 * end tag), 16-byte aligned and in the first GiB of RAM, the only memory
 * the VideoCore reaches. Returns whether the firmware answered within
 * @wait_us and reported success.
 */
bool mbox_property(volatile uint32_t *buf, uint64_t wait_us);

#endif /* BM_MBOX_H */
