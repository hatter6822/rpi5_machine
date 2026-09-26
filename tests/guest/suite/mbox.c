/*
 * VideoCore mailbox tests (WS2.2).
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
