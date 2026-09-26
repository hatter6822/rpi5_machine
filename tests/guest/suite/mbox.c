/*
 * VideoCore mailbox and firmware property tests (WS2.2, WS2.3a).
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <bm/fdt.h>
#include <bm/mbox.h>
#include <bm/runtime.h>
#include <bm/test.h>

/* The firmware answers with the board revision the DT publishes */
TEST(mbox_board_revision, "mbox/board-revision")
{
    static volatile uint32_t buf[8] __attribute__((aligned(16)));
    const void *prop;
    uint32_t len;
    int node;

    buf[0] = sizeof(buf);
    buf[1] = FW_REQUEST;
    buf[2] = FW_TAG_BOARD_REVISION;
    buf[3] = 4;                 /* value buffer */
    buf[4] = 0;                 /* request */
    buf[5] = 0;
    buf[6] = 0;                 /* end tag */
    buf[7] = 0;

    ASSERT_MSG(mbox_property(buf, 100000), "no answer from the firmware");
    ASSERT_EQ(buf[4], FW_SUCCESS | 4);
    /* New-style revision code: bit 23 set, Broadcom 2712 processor */
    ASSERT(buf[5] & (1u << 23));
    ASSERT_EQ((buf[5] >> 12) & 0xf, 4);

    node = fdt_path_offset("/system");
    prop = node >= 0 ? fdt_getprop(node, "linux,revision", &len) : 0;
    if (prop && len == 4) {
        ASSERT_EQ(buf[5], fdt_cell(prop, 0));
    }
}

/* Offsets of each tag's code word and value in the identity request */
enum {
    ID_VARIANT = 2,             /* value: 1 word */
    ID_HASH = ID_VARIANT + 4,   /* 5 words */
    ID_SERIAL = ID_HASH + 8,    /* 2 words */
    ID_ARM_MEMORY = ID_SERIAL + 5,
    ID_VC_MEMORY = ID_ARM_MEMORY + 5,
    ID_DMA = ID_VC_MEMORY + 5,  /* 1 word */
    ID_END = ID_DMA + 4,
    ID_WORDS = (ID_END + 4) & ~3,   /* end tag, padding to 16 bytes */
};

static void id_tag(volatile uint32_t *buf, unsigned at, uint32_t tag,
                   uint32_t size)
{
    buf[at] = tag;
    buf[at + 1] = size;
    buf[at + 2] = 0;
    for (unsigned i = 0; i < size / 4; i++) {
        buf[at + 3 + i] = 0;
    }
}

static uint32_t id_code(volatile uint32_t *buf, unsigned at)
{
    return buf[at + 2];
}

static uint32_t id_value(volatile uint32_t *buf, unsigned at, unsigned word)
{
    return buf[at + 3 + word];
}

/*
 * One request for everything that identifies the board and firmware, as
 * bare-metal code asks at start-up. The answers must be well formed and
 * agree with what the firmware put in the device tree: the ARM memory is
 * the first range of the memory node, the VideoCore's follows it, and
 * both lie in the first GiB.
 */
TEST(mbox_identity, "mbox/identity")
{
    static volatile uint32_t buf[ID_WORDS] __attribute__((aligned(16)));
    uint64_t base, size, serial;
    uint32_t arm_end, vc_end, len;
    const void *prop;
    int node;

    _Static_assert(ID_WORDS % 4 == 0, "whole 16-byte blocks");
    buf[0] = sizeof(buf);
    buf[1] = FW_REQUEST;
    id_tag(buf, ID_VARIANT, FW_TAG_FIRMWARE_VARIANT, 4);
    id_tag(buf, ID_HASH, FW_TAG_FIRMWARE_HASH, 20);
    id_tag(buf, ID_SERIAL, FW_TAG_BOARD_SERIAL, 8);
    id_tag(buf, ID_ARM_MEMORY, FW_TAG_ARM_MEMORY, 8);
    id_tag(buf, ID_VC_MEMORY, FW_TAG_VC_MEMORY, 8);
    id_tag(buf, ID_DMA, FW_TAG_DMA_CHANNELS, 4);
    for (unsigned i = ID_END; i < ID_WORDS; i++) {
        buf[i] = 0;
    }

    ASSERT_MSG(mbox_property(buf, 100000), "no answer from the firmware");
    ASSERT_EQ(id_code(buf, ID_VARIANT), FW_TAG_RESPONSE | 4);
    ASSERT_EQ(id_code(buf, ID_HASH), FW_TAG_RESPONSE | 20);
    ASSERT_EQ(id_code(buf, ID_SERIAL), FW_TAG_RESPONSE | 8);
    ASSERT_EQ(id_code(buf, ID_ARM_MEMORY), FW_TAG_RESPONSE | 8);
    ASSERT_EQ(id_code(buf, ID_VC_MEMORY), FW_TAG_RESPONSE | 8);
    ASSERT_EQ(id_code(buf, ID_DMA), FW_TAG_RESPONSE | 4);

    /* Linux knows variants 1-4 (start, start_x, start_db, start_cd) */
    ASSERT_LE(id_value(buf, ID_VARIANT, 0), 4);
    ASSERT_NE(id_value(buf, ID_DMA, 0), 0);

    ASSERT_EQ(id_value(buf, ID_ARM_MEMORY, 0), 0);
    arm_end = id_value(buf, ID_ARM_MEMORY, 1);
    ASSERT_EQ(id_value(buf, ID_VC_MEMORY, 0), arm_end);
    vc_end = arm_end + id_value(buf, ID_VC_MEMORY, 1);
    ASSERT_GE(vc_end, arm_end);
    ASSERT_LE(vc_end, 0x40000000u);
    bm_test_note("firmware variant %u, serial %08x%08x, ARM memory 0x0-0x%x, "
                 "VideoCore 0x%x-0x%x, DMA channels 0x%x",
                 id_value(buf, ID_VARIANT, 0), id_value(buf, ID_SERIAL, 1),
                 id_value(buf, ID_SERIAL, 0), arm_end, arm_end, vc_end,
                 id_value(buf, ID_DMA, 0));

    node = fdt_path_offset("/memory");
    if (node >= 0) {
        ASSERT(fdt_reg(node, 0, &base, &size));
        ASSERT_EQ(base, 0);
        ASSERT_EQ(size, arm_end);
    }
    node = fdt_path_offset("/system");
    prop = node >= 0 ? fdt_getprop(node, "linux,serial", &len) : 0;
    if (prop && len == 8) {
        serial = (uint64_t)fdt_cell(prop, 0) << 32 | fdt_cell(prop, 1);
        ASSERT_EQ(serial, (uint64_t)id_value(buf, ID_SERIAL, 1) << 32 |
                          id_value(buf, ID_SERIAL, 0));
    }
}
