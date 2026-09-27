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
#define FW_TAG_GET_CLOCKS       0x00010007u
#define FW_TAG_CLOCK_STATE      0x00030001u
#define FW_TAG_CLOCK_RATE       0x00030002u
#define FW_TAG_MAX_CLOCK_RATE   0x00030004u
#define FW_TAG_MIN_CLOCK_RATE   0x00030007u
#define FW_TAG_CLOCK_MEASURED   0x00030047u
#define FW_TAG_TEMPERATURE      0x00030006u
#define FW_TAG_MAX_TEMPERATURE  0x0003000au
#define FW_TAG_NOTIFY_REBOOT    0x00030048u
#define FW_TAG_REBOOT_FLAGS     0x00030064u
#define FW_TAG_SET_REBOOT_FLAGS 0x00038064u
#define FW_TAG_RTC_REG          0x00030087u     /* Linux's rtc-rpi.c */
#define FW_TAG_SET_RTC_REG      0x00038087u
#define FW_TAG_FB_ALLOCATE      0x00040001u
#define FW_TAG_FB_PITCH         0x00040008u
#define FW_TAG_FB_DISPLAYS      0x00040013u
#define FW_TAG_FB_SET_PHYSICAL  0x00048003u
#define FW_TAG_FB_SET_VIRTUAL   0x00048004u
#define FW_TAG_FB_SET_DEPTH     0x00048005u

/*
 * Send a property request: @buf is the whole message (size, code, tags,
 * end tag), 16-byte aligned and in the first GiB of RAM, the only memory
 * the VideoCore reaches. Returns whether the firmware answered within
 * @wait_us and reported success.
 */
bool mbox_property(volatile uint32_t *buf, uint64_t wait_us);

/* The most words mbox_tag() sends in a value buffer */
#define MBOX_TAG_MAX_WORDS      32

/*
 * Send a request of the one tag @tag, whose value buffer holds the @words
 * words of @val, and copy the value back into @val. Returns the length
 * of the answer in bytes, which may exceed the buffer, or -1 if the
 * firmware did not answer the request or the tag.
 */
int mbox_tag(uint32_t tag, uint32_t *val, unsigned words);

#endif /* BM_MBOX_H */
