/*
 * VideoCore mailbox and firmware property tests (WS2.2, WS2.3).
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <bm/fdt.h>
#include <bm/io.h>
#include <bm/mbox.h>
#include <bm/pm.h>
#include <bm/runtime.h>
#include <bm/test.h>
#include <bm/timer.h>

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

/*
 * The clocks the Raspberry Pi 5's firmware lists, with config.txt's
 * defaults for their least and most rates: the table the model starts
 * from (unverified, PLAN.md P09)
 */
static const struct {
    uint32_t id;
    const char *name;
    uint32_t min_mhz, max_mhz;
} clocks[] = {
    { 3, "arm", 1500, 2400 },
    { 4, "core", 500, 910 },
    { 5, "v3d", 500, 960 },
    { 7, "isp", 500, 910 },
    { 11, "hevc", 500, 910 },
};

/* A clock tag's answer for clock @id, which comes back with it */
static bool clock_query(uint32_t tag, uint32_t id, uint32_t *answer)
{
    uint32_t val[2] = { id, 0 };

    if (mbox_tag(tag, val, 2) != 8 || val[0] != id) {
        return false;
    }
    *answer = val[1];
    return true;
}

/*
 * The firmware's clock tags, for every clock it lists: each runs, at its
 * most, and its range and the temperature limit are the Raspberry Pi 5's
 * defaults
 */
TEST(mbox_clocks, "mbox/clocks")
{
    uint32_t list[2 * (ARRAY_SIZE(clocks) + 1)] = { 0 };
    uint32_t state, rate, measured, min, max, limit;
    int len;

    len = mbox_tag(FW_TAG_GET_CLOCKS, list, ARRAY_SIZE(list));
    ASSERT_MSG(len >= 0, "no answer from the firmware");
    ASSERT_EQ(len, 8 * ARRAY_SIZE(clocks));
    for (unsigned i = 0; i < ARRAY_SIZE(clocks); i++) {
        const uint32_t id = clocks[i].id;

        ASSERT_EQ(list[2 * i], 0);              /* no parent */
        ASSERT_EQ(list[2 * i + 1], id);
        ASSERT(clock_query(FW_TAG_CLOCK_STATE, id, &state));
        ASSERT(clock_query(FW_TAG_CLOCK_RATE, id, &rate));
        ASSERT(clock_query(FW_TAG_CLOCK_MEASURED, id, &measured));
        ASSERT(clock_query(FW_TAG_MIN_CLOCK_RATE, id, &min));
        ASSERT(clock_query(FW_TAG_MAX_CLOCK_RATE, id, &max));
        bm_test_note("clock %s: state 0x%x, %u Hz, measured %u Hz, "
                     "range %u-%u Hz", clocks[i].name, state, rate, measured,
                     min, max);
        ASSERT_EQ(state, 1);                    /* on, and it exists */
        ASSERT_EQ(min, clocks[i].min_mhz * 1000000u);
        ASSERT_EQ(max, clocks[i].max_mhz * 1000000u);
        ASSERT_EQ(rate, max);
        /* Within 1%: hardware measures what the model reports exactly */
        ASSERT_LE(measured > rate ? measured - rate : rate - measured,
                  rate / 100);
    }

    ASSERT(clock_query(FW_TAG_MAX_TEMPERATURE, 0, &limit));
    bm_test_note("temperature limit %u", limit);
    ASSERT_EQ(limit, 85000);                    /* 85 degrees C */
}

/* The real-time clock's registers, as Linux's rtc-rpi driver numbers them */
enum {
    RTC_TIME,                   /* seconds since 1970, UTC */
    RTC_ALARM,
    RTC_ALARM_PENDING,          /* write 1 to clear */
    RTC_ALARM_ENABLE,
    RTC_CHARGE,                 /* the battery's charging voltage, uV */
    RTC_CHARGE_MIN,
    RTC_CHARGE_MAX,
    RTC_BATTERY,                /* the battery's voltage, uV */
};

/* The system timer's counter, 1 MHz: BCM2835 ARM Peripherals, 12.1 */
#define ST_CLO                  0x04

/* Read real-time clock register @reg, or write @value to it first */
static bool rtc_reg(uint32_t reg, bool write, uint32_t *value)
{
    uint32_t val[2] = { reg, write ? *value : 0 };

    if (mbox_tag(write ? FW_TAG_SET_RTC_REG : FW_TAG_RTC_REG, val, 2) != 8 ||
        val[0] != reg) {
        return false;
    }
    *value = val[1];
    return true;
}

/*
 * The real-time clock as Linux's rtc-rpi driver, and through it hwclock,
 * use it: it counts seconds, takes a new time and an alarm, and reports
 * the range of the backup battery's charger (unverified, PLAN.md
 * P10). The test puts back what it changes, as on hardware the
 * time is the board's and an enabled alarm powers it on, and leaves the
 * charger alone, which would harm a battery that is not rechargeable.
 */
TEST(mbox_rtc, "mbox/rtc")
{
    uint32_t time, next, set, value, alarm, enabled, charge, min, max, bat;
    uint32_t start, us;

    /*
     * A second of the system timer's, within 10%: the error is how late
     * each of the two polls sees the second change
     */
    ASSERT_MSG(rtc_reg(RTC_TIME, false, &time), "no real-time clock");
    ASSERT(wait_until(rtc_reg(RTC_TIME, false, &next) && next != time,
                      1500000));
    start = mmio_read32(bm_plat.systimer + ST_CLO);
    ASSERT(wait_until(rtc_reg(RTC_TIME, false, &time) && time != next,
                      1500000));
    us = mmio_read32(bm_plat.systimer + ST_CLO) - start;
    bm_test_note("time %u, a second in %u us", time, us);
    ASSERT_EQ(time, next + 1);
    ASSERT_GE(us, 900000);
    ASSERT_LE(us, 1100000);

    /* hwclock -w, a million seconds on, then back */
    set = value = time + 1000000;
    ASSERT(rtc_reg(RTC_TIME, true, &value));
    ASSERT_LE(value - set, 1);
    ASSERT(rtc_reg(RTC_TIME, false, &value));
    ASSERT_LE(value - set, 1);
    value -= 1000000;
    ASSERT(rtc_reg(RTC_TIME, true, &value));

    /* An alarm an hour on, enabled and not; clearing it, as rtc-rpi does */
    ASSERT(rtc_reg(RTC_ALARM, false, &alarm));
    ASSERT(rtc_reg(RTC_ALARM_ENABLE, false, &enabled));
    bm_test_note("alarm %u, enabled %u", alarm, enabled);
    value = time + 3600;
    ASSERT(rtc_reg(RTC_ALARM, true, &value));
    ASSERT_EQ(value, time + 3600);
    value = !enabled;
    ASSERT(rtc_reg(RTC_ALARM_ENABLE, true, &value));
    ASSERT_EQ(value, !enabled);
    value = 1;
    ASSERT(rtc_reg(RTC_ALARM_PENDING, true, &value));
    ASSERT_EQ(value, 0);
    value = enabled;
    ASSERT(rtc_reg(RTC_ALARM_ENABLE, true, &value));
    ASSERT_EQ(value, enabled);
    value = alarm;
    ASSERT(rtc_reg(RTC_ALARM, true, &value));
    ASSERT_EQ(value, alarm);

    ASSERT(rtc_reg(RTC_CHARGE, false, &charge));
    ASSERT(rtc_reg(RTC_CHARGE_MIN, false, &min));
    ASSERT(rtc_reg(RTC_CHARGE_MAX, false, &max));
    ASSERT(rtc_reg(RTC_BATTERY, false, &bat));
    bm_test_note("charger %u uV (%u-%u uV), battery %u uV", charge, min, max,
                 bat);
    ASSERT_EQ(min, 1300000);
    ASSERT_EQ(max, 4400000);
    ASSERT(charge == 0 || (charge >= min && charge <= max));
}

/* Offsets of each tag's code word in the framebuffer request */
enum {
    FB_PHYSICAL = 2,                /* value: width, height */
    FB_VIRTUAL = FB_PHYSICAL + 5,   /* width, height */
    FB_DEPTH = FB_VIRTUAL + 5,      /* bits per pixel */
    FB_ALLOCATE = FB_DEPTH + 4,     /* alignment; then base, size */
    FB_PITCH = FB_ALLOCATE + 5,     /* bytes per line */
    FB_END = FB_PITCH + 4,
    FB_WORDS = (FB_END + 4) & ~3,   /* end tag, padding to 16 bytes */
};

/*
 * A framebuffer in one request, as bare-metal code sets up a display:
 * 640 x 480 pixels, then 1920 x 1080, at 32 bits per pixel. The firmware
 * keeps the width and depth and at least one line, all of them for the
 * smaller size, and allocates the buffer in its own memory, that of the
 * VideoCore, which the ARM's does not include: 16-byte aligned, as asked,
 * with a pitch that holds a line and room for every line it kept. On
 * raspi5b, 3 MiB of it hold 409 lines of the larger size.
 */
TEST(mbox_framebuffer, "mbox/framebuffer")
{
    static const uint32_t sizes[][2] = { { 640, 480 }, { 1920, 1080 } };
    static volatile uint32_t buf[FB_WORDS] __attribute__((aligned(16)));
    uint32_t displays = 0, arm[2] = { 0, 0 };

    _Static_assert(FB_WORDS % 4 == 0, "whole 16-byte blocks");
    ASSERT_EQ(mbox_tag(FW_TAG_FB_DISPLAYS, &displays, 1), 4);
    if (displays == 0) {
        SKIP("no display");
    }
    ASSERT_EQ(mbox_tag(FW_TAG_ARM_MEMORY, arm, 2), 8);
    ASSERT_EQ(arm[0], 0);

    for (unsigned i = 0; i < 2; i++) {
        const uint32_t width = sizes[i][0], height = sizes[i][1];
        uint32_t lines, pitch, base, size;

        buf[0] = sizeof(buf);
        buf[1] = FW_REQUEST;
        id_tag(buf, FB_PHYSICAL, FW_TAG_FB_SET_PHYSICAL, 8);
        buf[FB_PHYSICAL + 3] = width;
        buf[FB_PHYSICAL + 4] = height;
        id_tag(buf, FB_VIRTUAL, FW_TAG_FB_SET_VIRTUAL, 8);
        buf[FB_VIRTUAL + 3] = width;
        buf[FB_VIRTUAL + 4] = height;
        id_tag(buf, FB_DEPTH, FW_TAG_FB_SET_DEPTH, 4);
        buf[FB_DEPTH + 3] = 32;
        id_tag(buf, FB_ALLOCATE, FW_TAG_FB_ALLOCATE, 8);
        buf[FB_ALLOCATE + 3] = 16;
        id_tag(buf, FB_PITCH, FW_TAG_FB_PITCH, 4);
        for (unsigned w = FB_END; w < FB_WORDS; w++) {
            buf[w] = 0;
        }

        ASSERT_MSG(mbox_property(buf, 100000), "no answer from the firmware");
        ASSERT_EQ(id_code(buf, FB_PHYSICAL), FW_TAG_RESPONSE | 8);
        ASSERT_EQ(id_code(buf, FB_VIRTUAL), FW_TAG_RESPONSE | 8);
        ASSERT_EQ(id_code(buf, FB_DEPTH), FW_TAG_RESPONSE | 4);
        ASSERT_EQ(id_code(buf, FB_ALLOCATE), FW_TAG_RESPONSE | 8);
        ASSERT_EQ(id_code(buf, FB_PITCH), FW_TAG_RESPONSE | 4);

        lines = id_value(buf, FB_VIRTUAL, 1);
        pitch = id_value(buf, FB_PITCH, 0);
        /* A bus address: Linux strips the alias bits the same way */
        base = id_value(buf, FB_ALLOCATE, 0) & 0x3fffffffu;
        size = id_value(buf, FB_ALLOCATE, 1);
        bm_test_note("framebuffer %ux%u: %ux%u, pitch %u, 0x%x-0x%x", width,
                     height, id_value(buf, FB_VIRTUAL, 0), lines, pitch, base,
                     base + size);

        ASSERT_EQ(id_value(buf, FB_PHYSICAL, 0), width);
        ASSERT_EQ(id_value(buf, FB_PHYSICAL, 1), lines);
        ASSERT_EQ(id_value(buf, FB_VIRTUAL, 0), width);
        ASSERT_EQ(id_value(buf, FB_DEPTH, 0), 32);
        ASSERT_GE(lines, 1);
        ASSERT_LE(lines, height);
        if (i == 0) {
            ASSERT_EQ(lines, height);
        }
        ASSERT_GE(pitch, width * 4);
        ASSERT_EQ(base % 16, 0);
        ASSERT_GE(size, (uint64_t)pitch * lines);
        ASSERT_GE(base, arm[1]);
        ASSERT_LE((uint64_t)base + size, 0x40000000u);
    }
}

/*
 * The reboot flags a boot leaves are for the next boot only: the
 * bootloader takes them, and the firmware reports a tryboot in the device
 * tree it gives. The second of three boots is a tryboot, as after Linux's
 * "reboot 0 tryboot". On hardware, a tryboot reads tryboot.txt instead of
 * config.txt, so the boot partition needs a copy of it by that name.
 */
TEST(mbox_tryboot, "mbox/tryboot")
{
    const unsigned resets = bm_test_resets();
    const bool is_tryboot = resets == 1;
    int fw = fdt_path_offset("/chosen/bootloader");
    uint32_t flags = 0xa5a5a5a5, tryboot;

    ASSERT_EQ(mbox_tag(FW_TAG_REBOOT_FLAGS, &flags, 1), 4);
    ASSERT_EQ(flags, 0);
    if (fw >= 0) {
        ASSERT(fdt_prop_u32(fw, "tryboot", &tryboot));
        ASSERT_EQ(tryboot, is_tryboot);
    } else {
        bm_test_note("no /chosen/bootloader: tryboot not checked");
    }
    if (resets == 2) {
        return;
    }

    if (resets == 0) {
        /* Linux's rpi_firmware_notify_reboot() */
        flags = 1;
        ASSERT_EQ(mbox_tag(FW_TAG_SET_REBOOT_FLAGS, &flags, 1), 4);
        ASSERT_EQ(flags, 1);
        ASSERT_EQ(mbox_tag(FW_TAG_NOTIFY_REBOOT, 0, 0), 0);
    }
    pm_watchdog_start(10);
    wait_until(false, 100000);
    ASSERT_MSG(false, "no reset 100 ms after arming the watchdog");
}
