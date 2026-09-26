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
#define FW_TAG_RESPONSE         0x80000000u     /* in a tag's code word */
#define FW_TAG_FIRMWARE_VARIANT 0x00000002u
#define FW_TAG_FIRMWARE_HASH    0x00000003u
#define FW_TAG_BOARD_MODEL      0x00010001u
#define FW_TAG_BOARD_REVISION   0x00010002u
#define FW_TAG_BOARD_SERIAL     0x00010004u
#define FW_TAG_ARM_MEMORY       0x00010005u
#define FW_TAG_VC_MEMORY        0x00010006u
#define FW_TAG_DMA_CHANNELS     0x00060001u

/*
 * Send a property request: @buf is the whole message (size, code, tags,
 * end tag), 16-byte aligned and in the first GiB of RAM, the only memory
 * the VideoCore reaches. Returns whether the firmware answered within
 * @wait_us and reported success.
 */
bool mbox_property(volatile uint32_t *buf, uint64_t wait_us);

#endif /* BM_MBOX_H */
