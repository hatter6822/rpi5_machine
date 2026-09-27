/*
 * QTest testcase for the Raspberry Pi 5 (raspi5b) machine
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define UART10_BASE             0x107d001000ULL
#define GICD_BASE               0x107fff9000ULL
#define MBOX_BASE               0x107c013880ULL
#define SYSTIMER_BASE           0x107c003000ULL
#define PM_BASE                 0x107d200000ULL
#define SOC_WINDOW_BASE         0x107c000000ULL
#define HVS_BASE                0x107c580000ULL
#define RNG_BASE                0x107d208000ULL

#define PL011_PERIPHID0         0xfe0
#define PL011_PERIPHID1         0xfe4
#define PL011_PCELLID0          0xff0
#define PL011_PCELLID1          0xff4

#define GICD_TYPER              0x004
#define GICD_IPRIORITYR         0x400
#define GICD_TYPER_ITLINES(v)   ((v) & 0x1f)
#define GICD_TYPER_CPUNUM(v)    (((v) >> 5) & 0x7)
#define GICD_TYPER_SECEXT       (1u << 10)
#define GICD_ISPENDR            0x200

/* BCM2835 system timer registers */
#define ST_CS                   0x00
#define ST_CLO                  0x04
#define ST_CHI                  0x08
#define ST_C(n)                 (0x0c + 4 * (n))
#define ST_COUNT                4
#define ST_SPI(n)               (64 + (n))      /* bcm2712.dtsi */

/* Linux drivers/mailbox/bcm2835-mailbox.c */
#define MBOX_READ               0x00
#define MBOX_STATUS             0x18
#define MBOX_CONFIG             0x1c
#define MBOX_WRITE              0x20
#define MBOX_STATUS_EMPTY       BIT(30)
#define MBOX_CONFIG_DATA_IRQ    BIT(0)
#define MBOX_SPI                33              /* bcm2712.dtsi */
#define MBOX_CHAN_PROPERTY      8

/* include/soc/bcm2835/raspberrypi-firmware.h */
#define FW_REQUEST              0
#define FW_SUCCESS              0x80000000
#define FW_ERROR                0x80000001      /* error parsing the request */
#define FW_TAG_FIRMWARE_VARIANT 0x00000002
#define FW_TAG_FIRMWARE_HASH    0x00000003
#define FW_TAG_BOARD_MODEL      0x00010001
#define FW_TAG_BOARD_REVISION   0x00010002
#define FW_TAG_BOARD_SERIAL     0x00010004
#define FW_TAG_ARM_MEMORY       0x00010005
#define FW_TAG_VC_MEMORY        0x00010006
#define FW_TAG_DMA_CHANNELS     0x00060001
#define FW_TAG_COMMAND_LINE     0x00050001
#define FW_TAG_CLOCK_RATE       0x00030002
#define FW_TAG_OVERSCAN         0x0004000a
#define FW_TAG_RESPONSE         BIT(31)

/* The alias of the first GiB of RAM that code for older Pis uses */
#define VC_BUS_RAM              0xc0000000u

/* Linux drivers/watchdog/bcm2835_wdt.c */
#define PM_RSTC                 0x1c
#define PM_RSTS                 0x20
#define PM_WDOG                 0x24
#define PM_PASSWORD             0x5a000000
#define PM_RSTC_WRCFG_MASK      0x30
#define PM_RSTC_WRCFG_FULL_RESET 0x20
#define PM_RSTC_RESET           0x102
#define PM_RSTS_HADWRF          0x20
#define PM_RSTS_HADPOR          0x1000
#define PM_RSTS_HALT            0x555           /* partition 63 */
#define PM_WDOG_TICKS_PER_SEC   65536

static void test_uart10_ids(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");

    g_assert_cmphex(qtest_readl(qts, UART10_BASE + PL011_PERIPHID0), ==, 0x11);
    g_assert_cmphex(qtest_readl(qts, UART10_BASE + PL011_PERIPHID1), ==, 0x10);
    g_assert_cmphex(qtest_readl(qts, UART10_BASE + PL011_PCELLID0), ==, 0x0d);
    g_assert_cmphex(qtest_readl(qts, UART10_BASE + PL011_PCELLID1), ==, 0xf0);

    qtest_quit(qts);
}

static void check_gicd_typer(const char *args, uint32_t cpus, bool secext)
{
    QTestState *qts = qtest_init(args);
    uint32_t typer = qtest_readl(qts, GICD_BASE + GICD_TYPER);

    /* 288 SPIs + 32 private interrupts = 32 * (9 + 1) */
    g_assert_cmpuint(GICD_TYPER_ITLINES(typer), ==, 9);
    g_assert_cmpuint(GICD_TYPER_CPUNUM(typer), ==, cpus - 1);
    g_assert_cmpint(!!(typer & GICD_TYPER_SECEXT), ==, secext);

    qtest_quit(qts);
}

static void test_gic_default(void)
{
    check_gicd_typer("-machine raspi5b", 4, false);
}

/* GIC-400 implements 5 priority bits: the low 3 bits of IPRIORITYR are RAZ */
static void test_gic_priority_bits(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");

    qtest_writel(qts, GICD_BASE + GICD_IPRIORITYR + 4 * 8, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + GICD_IPRIORITYR + 4 * 8),
                    ==, 0xf8f8f8f8);

    qtest_quit(qts);
}

static void test_gic_smp2_secure(void)
{
    check_gicd_typer("-machine raspi5b,secure=on -smp 2", 2, true);
}

static void test_ram(void)
{
    QTestState *qts = qtest_init("-machine raspi5b -m 8G");
    const uint64_t top = 8 * GiB - sizeof(uint64_t);

    qtest_writeq(qts, 0x0, 0x0123456789abcdefULL);
    qtest_writeq(qts, top, 0xfedcba9876543210ULL);
    g_assert_cmphex(qtest_readq(qts, 0x0), ==, 0x0123456789abcdefULL);
    g_assert_cmphex(qtest_readq(qts, top), ==, 0xfedcba9876543210ULL);

    qtest_quit(qts);
}

static bool gic_spi_pending(QTestState *qts, int spi)
{
    int intid = 32 + spi;

    return qtest_readl(qts, GICD_BASE + GICD_ISPENDR + 4 * (intid / 32)) &
           BIT(intid % 32);
}

static uint32_t systimer_readl(QTestState *qts, uint32_t reg)
{
    return qtest_readl(qts, SYSTIMER_BASE + reg);
}

static void systimer_writel(QTestState *qts, uint32_t reg, uint32_t val)
{
    qtest_writel(qts, SYSTIMER_BASE + reg, val);
}

/* Counter at 1 MHz from the virtual clock, which qtest steps explicitly */
static void test_systimer_counter(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");
    uint32_t lo = systimer_readl(qts, ST_CLO);

    qtest_clock_step(qts, 1000 * SCALE_US);
    g_assert_cmpuint(systimer_readl(qts, ST_CLO) - lo, ==, 1000);
    g_assert_cmpuint(systimer_readl(qts, ST_CHI), ==, 0);

    /* The high word follows the low word across 2^32 microseconds */
    qtest_clock_step(qts, BIT_ULL(32) * SCALE_US);
    g_assert_cmpuint(systimer_readl(qts, ST_CHI), ==, 1);
    g_assert_cmpuint(systimer_readl(qts, ST_CLO) - lo, ==, 1000);

    /* The counter is read-only */
    systimer_writel(qts, ST_CLO, 0);
    g_assert_cmpuint(systimer_readl(qts, ST_CLO) - lo, ==, 1000);

    qtest_quit(qts);
}

/* Arm comparator n to match 100 us from now */
static void systimer_arm(QTestState *qts, int n)
{
    systimer_writel(qts, ST_C(n), systimer_readl(qts, ST_CLO) + 100);
    g_assert_cmpuint(systimer_readl(qts, ST_C(n)), ==,
                     systimer_readl(qts, ST_CLO) + 100);
}

/* Each comparator matches, raises its own SPI and is acknowledged via CS */
static void test_systimer_compare(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");

    for (int n = 0; n < ST_COUNT; n++) {
        systimer_arm(qts, n);

        qtest_clock_step(qts, 99 * SCALE_US);
        g_assert_cmphex(systimer_readl(qts, ST_CS), ==, 0);
        g_assert_false(gic_spi_pending(qts, ST_SPI(n)));

        qtest_clock_step(qts, 1 * SCALE_US);
        g_assert_cmphex(systimer_readl(qts, ST_CS), ==, BIT(n));
        for (int i = 0; i < ST_COUNT; i++) {
            g_assert_cmpint(gic_spi_pending(qts, ST_SPI(i)), ==, i == n);
        }

        /* Writing a zero bit leaves the match alone; a one clears it */
        systimer_writel(qts, ST_CS, ~BIT(n));
        g_assert_cmphex(systimer_readl(qts, ST_CS), ==, BIT(n));
        systimer_writel(qts, ST_CS, BIT(n));
        g_assert_cmphex(systimer_readl(qts, ST_CS), ==, 0);
        g_assert_false(gic_spi_pending(qts, ST_SPI(n)));
    }

    qtest_quit(qts);
}

/* Reset cancels armed comparators and drops raised interrupts */
static void test_systimer_reset(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");

    systimer_arm(qts, 0);
    qtest_clock_step(qts, 100 * SCALE_US);
    systimer_arm(qts, 1);
    g_assert_true(gic_spi_pending(qts, ST_SPI(0)));

    qtest_system_reset(qts);
    g_assert_cmphex(systimer_readl(qts, ST_CS), ==, 0);
    g_assert_cmphex(systimer_readl(qts, ST_C(0)), ==, 0);
    g_assert_false(gic_spi_pending(qts, ST_SPI(0)));

    qtest_clock_step(qts, 200 * SCALE_US);
    g_assert_cmphex(systimer_readl(qts, ST_CS), ==, 0);
    g_assert_false(gic_spi_pending(qts, ST_SPI(1)));

    qtest_quit(qts);
}

static uint32_t pm_readl(QTestState *qts, uint32_t reg)
{
    return qtest_readl(qts, PM_BASE + reg);
}

static void pm_writel(QTestState *qts, uint32_t reg, uint32_t val)
{
    qtest_writel(qts, PM_BASE + reg, PM_PASSWORD | val);
}

/* Linux's bcm2835_wdt_start(): load the timeout, then arm a full reset */
static void pm_wdog_start(QTestState *qts, uint32_t ticks)
{
    pm_writel(qts, PM_WDOG, ticks);
    pm_writel(qts, PM_RSTC, PM_RSTC_WRCFG_FULL_RESET);
}

/* Linux's __bcm2835_restart(): the partition goes in the even bits */
static uint32_t pm_partition(uint32_t partition)
{
    uint32_t rsts = 0;

    for (int i = 0; i < 6; i++) {
        rsts |= extract32(partition, i, 1) << (2 * i);
    }
    return rsts;
}

static void test_pm_registers(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");

    g_assert_cmphex(pm_readl(qts, PM_RSTC), ==, PM_RSTC_RESET);
    g_assert_cmphex(pm_readl(qts, PM_RSTS), ==, PM_RSTS_HADPOR);
    g_assert_cmphex(pm_readl(qts, PM_WDOG), ==, 0);

    /* Writes without the password are ignored */
    qtest_writel(qts, PM_BASE + PM_WDOG, 0x1234);
    qtest_writel(qts, PM_BASE + PM_RSTC, PM_RSTC_WRCFG_FULL_RESET);
    g_assert_cmphex(pm_readl(qts, PM_WDOG), ==, 0);
    g_assert_cmphex(pm_readl(qts, PM_RSTC), ==, PM_RSTC_RESET);

    /* The timeout is 20 bits wide, and does not run while disarmed */
    pm_writel(qts, PM_WDOG, 0xffffff);
    g_assert_cmphex(pm_readl(qts, PM_WDOG), ==, 0xfffff);
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND);
    g_assert_cmphex(pm_readl(qts, PM_WDOG), ==, 0xfffff);

    /*
     * Only WRCFG = full reset arms it: the other configurations are not
     * modelled, and do nothing rather than reset as if they were
     */
    pm_writel(qts, PM_RSTC, PM_RSTC_WRCFG_MASK);
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND);
    g_assert_cmphex(pm_readl(qts, PM_WDOG), ==, 0xfffff);
    g_assert_cmphex(pm_readl(qts, PM_RSTS), ==, PM_RSTS_HADPOR);

    qtest_quit(qts);
}

/* The watchdog counts down in 1/65536 s ticks and can be paused */
static void test_pm_watchdog_countdown(void)
{
    QTestState *qts = qtest_init("-machine raspi5b -action watchdog=none");
    QDict *event;

    pm_wdog_start(qts, 2 * PM_WDOG_TICKS_PER_SEC);
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND);
    g_assert_cmpuint(pm_readl(qts, PM_WDOG), ==, PM_WDOG_TICKS_PER_SEC);
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND / 2);
    g_assert_cmpuint(pm_readl(qts, PM_WDOG), ==, PM_WDOG_TICKS_PER_SEC / 2);

    /* bcm2835_wdt_stop() */
    pm_writel(qts, PM_RSTC, PM_RSTC_RESET);
    qtest_clock_step(qts, 10 * NANOSECONDS_PER_SECOND);
    g_assert_cmpuint(pm_readl(qts, PM_WDOG), ==, PM_WDOG_TICKS_PER_SEC / 2);

    /* Arming again resumes from where it stopped */
    pm_writel(qts, PM_RSTC, PM_RSTC_WRCFG_FULL_RESET);
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND / 2 - 1);
    g_assert_cmpuint(pm_readl(qts, PM_WDOG), ==, 1);
    g_assert_cmphex(pm_readl(qts, PM_RSTS), ==, PM_RSTS_HADPOR);
    qtest_clock_step(qts, 1);
    event = qtest_qmp_eventwait_ref(qts, "WATCHDOG");
    g_assert_cmpstr(qdict_get_str(qdict_get_qdict(event, "data"), "action"),
                    ==, "none");
    qobject_unref(event);
    g_assert_cmpuint(pm_readl(qts, PM_WDOG), ==, 0);
    g_assert_cmphex(pm_readl(qts, PM_RSTS), ==, PM_RSTS_HADWRF);

    qtest_quit(qts);
}

/* Kicking the watchdog (bcm2835_wdt_start() again) restarts the timeout */
static void test_pm_watchdog_kick(void)
{
    QTestState *qts = qtest_init("-machine raspi5b -action watchdog=none");

    pm_wdog_start(qts, PM_WDOG_TICKS_PER_SEC);
    for (int i = 0; i < 4; i++) {
        qtest_clock_step(qts, NANOSECONDS_PER_SECOND * 3 / 4);
        pm_wdog_start(qts, PM_WDOG_TICKS_PER_SEC);
    }
    g_assert_cmpuint(pm_readl(qts, PM_WDOG), ==, PM_WDOG_TICKS_PER_SEC);
    g_assert_cmphex(pm_readl(qts, PM_RSTS), ==, PM_RSTS_HADPOR);

    qtest_quit(qts);
}

/*
 * Linux reboots through the watchdog with a 10 tick timeout. The reset
 * status survives the reset and tells the boot code why it happened and
 * which partition to boot.
 */
static void test_pm_watchdog_reset(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");
    QDict *event;

    pm_writel(qts, PM_RSTS, PM_RSTS_HADPOR | pm_partition(5));
    pm_wdog_start(qts, 10);
    qtest_clock_step(qts, 200 * SCALE_US);
    event = qtest_qmp_eventwait_ref(qts, "RESET");
    g_assert_cmpstr(qdict_get_str(qdict_get_qdict(event, "data"), "reason"),
                    ==, "guest-reset");
    qobject_unref(event);

    g_assert_cmphex(pm_readl(qts, PM_RSTS), ==,
                    PM_RSTS_HADWRF | pm_partition(5));
    g_assert_cmphex(pm_readl(qts, PM_RSTC), ==, PM_RSTC_RESET);
    g_assert_cmphex(pm_readl(qts, PM_WDOG), ==, 0);

    /* Any other system reset leaves it alone too */
    qtest_system_reset(qts);
    g_assert_cmphex(pm_readl(qts, PM_RSTS), ==,
                    PM_RSTS_HADWRF | pm_partition(5));

    qtest_quit(qts);
}

/* Partition 63 asks the firmware to halt: QEMU powers off instead */
static void test_pm_halt(void)
{
    QTestState *qts = qtest_init("-machine raspi5b -action shutdown=pause");
    QDict *event;

    g_assert_cmphex(pm_partition(63), ==, PM_RSTS_HALT);
    /* Earlier reset flags in RSTS do not matter */
    pm_writel(qts, PM_RSTS, PM_RSTS_HADWRF | PM_RSTS_HALT);
    pm_wdog_start(qts, 10);
    qtest_clock_step(qts, 200 * SCALE_US);
    event = qtest_qmp_eventwait_ref(qts, "SHUTDOWN");
    g_assert_cmpstr(qdict_get_str(qdict_get_qdict(event, "data"), "reason"),
                    ==, "guest-shutdown");
    qobject_unref(event);

    qtest_quit(qts);
}

/* ...and QEMU exits with status 0, as for PSCI SYSTEM_OFF */
static void test_pm_halt_exit(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");

    pm_writel(qts, PM_RSTS, PM_RSTS_HALT);
    pm_wdog_start(qts, 10);
    qtest_clock_step(qts, 200 * SCALE_US);
    qtest_wait_qemu(qts);       /* checks the exit status */
    qtest_quit(qts);
}

static void wait_for_migration(QTestState *qts)
{
    for (;;) {
        QDict *rsp = qtest_qmp(qts, "{ 'execute': 'query-migrate' }");
        QDict *ret = qdict_get_qdict(rsp, "return");
        const char *status = qdict_haskey(ret, "status") ?
                             qdict_get_str(ret, "status") : "none";
        bool done = !strcmp(status, "completed");

        g_assert_cmpstr(status, !=, "failed");
        qobject_unref(rsp);
        if (done) {
            return;
        }
        g_usleep(10 * 1000);
    }
}

/* A comparator armed at migration time still fires on the destination */
static void test_systimer_migrate(void)
{
    g_autofree char *file = g_strdup_printf("%s/raspi5b-systimer-%d.mig",
                                            g_get_tmp_dir(), getpid());
    g_autofree char *out = g_strdup_printf("exec:cat > %s", file);
    g_autofree char *in = g_strdup_printf("exec:cat %s", file);
    const char *args = "-machine raspi5b -m 1G";
    QTestState *src, *dst;
    int64_t now;

    src = qtest_init(args);
    qtest_clock_step(src, 1234 * SCALE_US);
    now = 1234 * SCALE_US;
    systimer_arm(src, 3);
    qtest_qmp_assert_success(src, "{ 'execute': 'migrate',"
                             "  'arguments': { 'uri': %s } }", out);
    wait_for_migration(src);
    qtest_quit(src);

    dst = qtest_initf("%s -incoming defer", args);
    qtest_qmp_assert_success(dst, "{ 'execute': 'migrate-incoming',"
                             "  'arguments': { 'uri': %s } }", in);
    wait_for_migration(dst);

    /* The qtest clock is not migrated: carry it over by hand */
    qtest_clock_set(dst, now + 99 * SCALE_US);
    g_assert_cmphex(systimer_readl(dst, ST_CS), ==, 0);
    qtest_clock_step(dst, 1 * SCALE_US);
    g_assert_cmphex(systimer_readl(dst, ST_CS), ==, BIT(3));
    g_assert_true(gic_spi_pending(dst, ST_SPI(3)));

    qtest_quit(dst);
    unlink(file);
}

/* An armed watchdog keeps counting on the destination */
static void test_pm_migrate(void)
{
    g_autofree char *file = g_strdup_printf("%s/raspi5b-pm-%d.mig",
                                            g_get_tmp_dir(), getpid());
    g_autofree char *out = g_strdup_printf("exec:cat > %s", file);
    g_autofree char *in = g_strdup_printf("exec:cat %s", file);
    const char *args = "-machine raspi5b -m 1G -action watchdog=none";
    QTestState *src, *dst;
    int64_t now;

    src = qtest_init(args);
    pm_wdog_start(src, PM_WDOG_TICKS_PER_SEC);
    qtest_clock_step(src, NANOSECONDS_PER_SECOND / 4);
    now = NANOSECONDS_PER_SECOND / 4;
    qtest_qmp_assert_success(src, "{ 'execute': 'migrate',"
                             "  'arguments': { 'uri': %s } }", out);
    wait_for_migration(src);
    qtest_quit(src);

    dst = qtest_initf("%s -incoming defer", args);
    qtest_qmp_assert_success(dst, "{ 'execute': 'migrate-incoming',"
                             "  'arguments': { 'uri': %s } }", in);
    wait_for_migration(dst);

    /* The qtest clock is not migrated: carry it over by hand */
    qtest_clock_set(dst, now);
    g_assert_cmpuint(pm_readl(dst, PM_WDOG), ==,
                     PM_WDOG_TICKS_PER_SEC * 3 / 4);
    qtest_clock_step(dst, NANOSECONDS_PER_SECOND * 3 / 4);
    qtest_qmp_eventwait(dst, "WATCHDOG");
    g_assert_cmphex(pm_readl(dst, PM_RSTS), ==, PM_RSTS_HADWRF);

    qtest_quit(dst);
    unlink(file);
}

/*
 * Post a property request for @tag, with an 8-byte value buffer, at RAM
 * address @buf, handing the firmware bus address @bus_addr.
 */
static void mbox_request(QTestState *qts, uint64_t buf, uint32_t bus_addr,
                         uint32_t tag)
{
    const uint32_t req[] = {
        8 * 4, FW_REQUEST,
        tag, 8, 0, 0, 0,
        0,                                      /* end tag */
    };

    for (int i = 0; i < ARRAY_SIZE(req); i++) {
        qtest_writel(qts, buf + 4 * i, req[i]);
    }
    qtest_writel(qts, MBOX_BASE + MBOX_WRITE, bus_addr | MBOX_CHAN_PROPERTY);
}

static bool mbox_has_response(QTestState *qts)
{
    return !(qtest_readl(qts, MBOX_BASE + MBOX_STATUS) & MBOX_STATUS_EMPTY);
}

/* Check the response to mbox_request() and return the value's two words */
static void mbox_response(QTestState *qts, uint64_t buf, uint32_t bus_addr,
                          uint32_t val[2])
{
    g_assert_true(mbox_has_response(qts));
    g_assert_cmphex(qtest_readl(qts, MBOX_BASE + MBOX_READ), ==,
                    bus_addr | MBOX_CHAN_PROPERTY);
    g_assert_cmphex(qtest_readl(qts, buf + 4), ==, FW_SUCCESS);
    val[0] = qtest_readl(qts, buf + 20);
    val[1] = qtest_readl(qts, buf + 24);
}

/*
 * The firmware reads requests through the VideoCore's view of the first
 * GiB of RAM: at bus address 0, as Linux passes them, and at 0xc000_0000
 * for code written for older Pis.
 */
static void test_mbox_board_revision(void)
{
    QTestState *qts = qtest_init("-machine raspi5b -m 4G");
    const uint64_t buf = 0x10000;
    uint32_t val[2];

    qtest_writel(qts, MBOX_BASE + MBOX_CONFIG, MBOX_CONFIG_DATA_IRQ);
    g_assert_false(mbox_has_response(qts));

    mbox_request(qts, buf, VC_BUS_RAM | buf, FW_TAG_BOARD_REVISION);
    g_assert_true(gic_spi_pending(qts, MBOX_SPI));
    mbox_response(qts, buf, VC_BUS_RAM | buf, val);
    g_assert_cmphex(val[0], ==, 0xc04170);
    g_assert_false(gic_spi_pending(qts, MBOX_SPI));
    g_assert_false(mbox_has_response(qts));

    mbox_request(qts, buf, buf, FW_TAG_BOARD_REVISION);
    mbox_response(qts, buf, buf, val);
    g_assert_cmphex(val[0], ==, 0xc04170);

    qtest_quit(qts);
}

/* A buffer the VideoCore cannot reach gets no response, as on hardware */
static void test_mbox_unreachable(void)
{
    QTestState *qts = qtest_init("-machine raspi5b -m 2G");
    /* RAM, but above the first GiB; then the same address on the bus */
    const uint64_t high = 1 * GiB + 0x10000;
    /* A header the VideoCore reaches, but a body running past its view */
    const uint64_t edge = 1 * GiB - 0x10;
    uint32_t val[2];

    mbox_request(qts, high, high, FW_TAG_BOARD_REVISION);
    g_assert_false(mbox_has_response(qts));
    g_assert_cmphex(qtest_readl(qts, high + 4), ==, FW_REQUEST);

    mbox_request(qts, edge, edge, FW_TAG_BOARD_REVISION);
    g_assert_false(mbox_has_response(qts));
    g_assert_cmphex(qtest_readl(qts, edge + 4), ==, FW_REQUEST);

    /* The channel still works afterwards */
    mbox_request(qts, 0x10000, VC_BUS_RAM | 0x10000, FW_TAG_BOARD_REVISION);
    mbox_response(qts, 0x10000, VC_BUS_RAM | 0x10000, val);

    qtest_quit(qts);
}

/*
 * Post the @n-word request @req at @buf, with a marker word after it, and
 * return the buffer's response code.
 */
static uint32_t mbox_post(QTestState *qts, uint64_t buf, const uint32_t *req,
                          unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        qtest_writel(qts, buf + 4 * i, req[i]);
    }
    qtest_writel(qts, buf + 4 * n, 0x5a5a5a5a);
    qtest_writel(qts, MBOX_BASE + MBOX_WRITE,
                 VC_BUS_RAM | buf | MBOX_CHAN_PROPERTY);

    g_assert_true(mbox_has_response(qts));
    g_assert_cmphex(qtest_readl(qts, MBOX_BASE + MBOX_READ), ==,
                    VC_BUS_RAM | buf | MBOX_CHAN_PROPERTY);
    return qtest_readl(qts, buf + 4);
}

/* The words @from..@n of a request at @buf, and the marker after it */
static void mbox_assert_untouched(QTestState *qts, uint64_t buf, unsigned from,
                                  unsigned n)
{
    for (unsigned i = from; i < n; i++) {
        g_assert_cmphex(qtest_readl(qts, buf + 4 * i), ==, 0xa5a5a5a5);
    }
    g_assert_cmphex(qtest_readl(qts, buf + 4 * n), ==, 0x5a5a5a5a);
}

/*
 * A request that its own length cuts short, inside a tag's header, its
 * value buffer or before the end tag, is neither read nor written past
 * that length, and its response code reports the failed parse; the tags
 * before the cut are still answered.
 */
static void test_mbox_malformed(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");
    const uint64_t buf = 0x10000;
    /* A 64-byte value buffer in a 32-byte request */
    const uint32_t overrun[] = {
        8 * 4, FW_REQUEST,
        FW_TAG_BOARD_SERIAL, 64, 0,
        0xa5a5a5a5, 0xa5a5a5a5, 0xa5a5a5a5,
    };
    /* A request that ends after the tag identifier */
    const uint32_t cut[] = {
        3 * 4, FW_REQUEST,
        FW_TAG_BOARD_REVISION,
        0xa5a5a5a5, 0xa5a5a5a5, 0xa5a5a5a5,
    };
    /* A complete tag, then no room for the end tag */
    const uint32_t no_end[] = {
        6 * 4, FW_REQUEST,
        FW_TAG_BOARD_REVISION, 4, 0, 0xa5a5a5a5,
        0xa5a5a5a5, 0xa5a5a5a5,
    };
    uint32_t val[2];

    g_assert_cmphex(mbox_post(qts, buf, overrun, ARRAY_SIZE(overrun)), ==,
                    FW_ERROR);
    g_assert_cmphex(qtest_readl(qts, buf + 16), ==, 0);
    mbox_assert_untouched(qts, buf, 5, ARRAY_SIZE(overrun));

    g_assert_cmphex(mbox_post(qts, buf, cut, ARRAY_SIZE(cut)), ==, FW_ERROR);
    mbox_assert_untouched(qts, buf, 3, ARRAY_SIZE(cut));

    g_assert_cmphex(mbox_post(qts, buf, no_end, ARRAY_SIZE(no_end)), ==,
                    FW_ERROR);
    g_assert_cmphex(qtest_readl(qts, buf + 16), ==, FW_TAG_RESPONSE | 4);
    g_assert_cmphex(qtest_readl(qts, buf + 20), ==, 0xb04170);  /* 2 GiB */
    mbox_assert_untouched(qts, buf, 6, ARRAY_SIZE(no_end));

    /* The channel still works afterwards */
    mbox_request(qts, buf, VC_BUS_RAM | buf, FW_TAG_BOARD_REVISION);
    mbox_response(qts, buf, VC_BUS_RAM | buf, val);
    g_assert_cmphex(val[0], ==, 0xb04170);

    qtest_quit(qts);
}

/* The VideoCore keeps the top 4 MiB of the first GiB */
static void test_mbox_memory_split(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");
    const uint64_t buf = 0x10000;
    uint32_t val[2];

    mbox_request(qts, buf, VC_BUS_RAM | buf, FW_TAG_ARM_MEMORY);
    mbox_response(qts, buf, VC_BUS_RAM | buf, val);
    g_assert_cmphex(val[0], ==, 0);
    g_assert_cmphex(val[1], ==, 0x3fc00000);

    mbox_request(qts, buf, VC_BUS_RAM | buf, FW_TAG_VC_MEMORY);
    mbox_response(qts, buf, VC_BUS_RAM | buf, val);
    g_assert_cmphex(val[0], ==, 0x3fc00000);
    g_assert_cmphex(val[1], ==, 4 * MiB);

    qtest_quit(qts);
}

/*
 * Ask for @tag with a @size-byte value buffer, filled with a marker so
 * that words the firmware leaves alone stand out, and copy the value
 * back into @val. Returns the tag's response code and length.
 */
static uint32_t mbox_tag(QTestState *qts, uint32_t tag, uint32_t size,
                         uint32_t *val)
{
    const uint64_t buf = 0x10000;
    const uint32_t words = DIV_ROUND_UP(size, 4);

    qtest_writel(qts, buf, (6 + words) * 4);
    qtest_writel(qts, buf + 4, FW_REQUEST);
    qtest_writel(qts, buf + 8, tag);
    qtest_writel(qts, buf + 12, size);
    qtest_writel(qts, buf + 16, 0);
    for (int i = 0; i < words; i++) {
        qtest_writel(qts, buf + 20 + 4 * i, 0xa5a5a5a5);
    }
    qtest_writel(qts, buf + 20 + 4 * words, 0);
    qtest_writel(qts, MBOX_BASE + MBOX_WRITE,
                 VC_BUS_RAM | buf | MBOX_CHAN_PROPERTY);

    g_assert_true(mbox_has_response(qts));
    g_assert_cmphex(qtest_readl(qts, MBOX_BASE + MBOX_READ), ==,
                    VC_BUS_RAM | buf | MBOX_CHAN_PROPERTY);
    g_assert_cmphex(qtest_readl(qts, buf + 4), ==, FW_SUCCESS);
    for (int i = 0; i < words; i++) {
        val[i] = qtest_readl(qts, buf + 20 + 4 * i);
    }
    return qtest_readl(qts, buf + 16);
}

/* The tags Linux's firmware driver and bare-metal code identify us by */
static void test_mbox_identity(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");
    uint32_t val[5];

    g_assert_cmphex(mbox_tag(qts, FW_TAG_FIRMWARE_VARIANT, 4, val), ==,
                    FW_TAG_RESPONSE | 4);
    g_assert_cmphex(val[0], ==, 1);                     /* "start" */
    /* A buffer too short for the value leaves the end tag after it alone */
    g_assert_cmphex(mbox_tag(qts, FW_TAG_FIRMWARE_VARIANT, 0, val), ==,
                    FW_TAG_RESPONSE | 4);
    g_assert_cmphex(qtest_readl(qts, 0x10000 + 20), ==, 0);

    g_assert_cmphex(mbox_tag(qts, FW_TAG_FIRMWARE_HASH, 20, val), ==,
                    FW_TAG_RESPONSE | 20);
    for (int i = 0; i < 5; i++) {
        g_assert_cmphex(val[i], ==, 0);
    }
    /* A short buffer gets the length it needs, and nothing past its end */
    qtest_writel(qts, 0x10000 + 32, 0x5a5a5a5a);
    g_assert_cmphex(mbox_tag(qts, FW_TAG_FIRMWARE_HASH, 8, val), ==,
                    FW_TAG_RESPONSE | 20);
    g_assert_cmphex(val[0], ==, 0);
    g_assert_cmphex(val[1], ==, 0);
    g_assert_cmphex(qtest_readl(qts, 0x10000 + 32), ==, 0x5a5a5a5a);

    g_assert_cmphex(mbox_tag(qts, FW_TAG_BOARD_MODEL, 4, val), ==,
                    FW_TAG_RESPONSE | 4);
    g_assert_cmphex(val[0], ==, 0);
    g_assert_cmphex(mbox_tag(qts, FW_TAG_BOARD_MODEL, 0, val), ==,
                    FW_TAG_RESPONSE | 4);
    g_assert_cmphex(qtest_readl(qts, 0x10000 + 20), ==, 0);

    g_assert_cmphex(mbox_tag(qts, FW_TAG_BOARD_SERIAL, 8, val), ==,
                    FW_TAG_RESPONSE | 8);
    g_assert_cmphex(val[0], ==, 0x89abcdef);
    g_assert_cmphex(val[1], ==, 0x01234567);
    /* Here too, and the end tag after the short buffer survives */
    g_assert_cmphex(mbox_tag(qts, FW_TAG_BOARD_SERIAL, 4, val), ==,
                    FW_TAG_RESPONSE | 8);
    g_assert_cmphex(val[0], ==, 0x89abcdef);
    g_assert_cmphex(qtest_readl(qts, 0x10000 + 24), ==, 0);

    g_assert_cmphex(mbox_tag(qts, FW_TAG_DMA_CHANNELS, 4, val), ==,
                    FW_TAG_RESPONSE | 4);
    g_assert_cmphex(val[0], ==, 0x7ff);
    g_assert_cmphex(mbox_tag(qts, FW_TAG_DMA_CHANNELS, 0, val), ==,
                    FW_TAG_RESPONSE | 4);
    g_assert_cmphex(qtest_readl(qts, 0x10000 + 20), ==, 0);

    qtest_quit(qts);

    qts = qtest_init("-machine raspi5b,serial=0x1122334455667788");
    mbox_tag(qts, FW_TAG_BOARD_SERIAL, 8, val);
    g_assert_cmphex(val[0], ==, 0x55667788);
    g_assert_cmphex(val[1], ==, 0x11223344);
    qtest_quit(qts);
}

/*
 * The tags the model answered before this series stay within the value
 * buffer too: what fits is written, the response length says what the
 * whole answer needs, and a request field the buffer does not hold reads
 * as zero.
 */
static void test_mbox_short_buffer(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");
    uint32_t val[4];

    /* ARM memory: the base fits, the size does not; the end tag survives */
    g_assert_cmphex(mbox_tag(qts, FW_TAG_ARM_MEMORY, 4, val), ==,
                    FW_TAG_RESPONSE | 8);
    g_assert_cmphex(val[0], ==, 0);
    g_assert_cmphex(qtest_readl(qts, 0x10000 + 24), ==, 0);

    /* A clock rate: the marker is an unknown clock id, so the default */
    g_assert_cmphex(mbox_tag(qts, FW_TAG_CLOCK_RATE, 8, val), ==,
                    FW_TAG_RESPONSE | 8);
    g_assert_cmphex(val[0], ==, 0xa5a5a5a5);
    g_assert_cmphex(val[1], ==, 700000000);
    /* With room for the id only, the rate is not written anywhere */
    g_assert_cmphex(mbox_tag(qts, FW_TAG_CLOCK_RATE, 4, val), ==,
                    FW_TAG_RESPONSE | 8);
    g_assert_cmphex(val[0], ==, 0xa5a5a5a5);
    g_assert_cmphex(qtest_readl(qts, 0x10000 + 24), ==, 0);

    /* Overscan: four words, of which two fit */
    g_assert_cmphex(mbox_tag(qts, FW_TAG_OVERSCAN, 8, val), ==,
                    FW_TAG_RESPONSE | 16);
    g_assert_cmphex(val[0], ==, 0);
    g_assert_cmphex(val[1], ==, 0);
    g_assert_cmphex(qtest_readl(qts, 0x10000 + 28), ==, 0);

    qtest_quit(qts);
}

/* A temporary file holding @data, for options that take a file name */
static char *tmp_file(const char *template, const void *data, size_t size)
{
    char *path;
    int fd = g_file_open_tmp(template, &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, data, size), ==, size);
    close(fd);
    return path;
}

/*
 * The firmware returns the command line without a terminator, and when
 * the buffer is too small, only the length it needs.
 */
static void test_mbox_command_line(void)
{
    static const char cmdline[] = "console=ttyAMA10,115200 quiet";
    static const uint32_t wfi_loop[] = { 0xd503207f, 0x17ffffff };
    /* -append needs a -kernel; the CPU never runs it under qtest */
    g_autofree char *kernel = tmp_file("raspi5b-kernel-XXXXXX", wfi_loop,
                                       sizeof(wfi_loop));
    QTestState *qts;
    uint32_t val[12];

    qts = qtest_initf("-machine raspi5b -kernel %s -append '%s'",
                      kernel, cmdline);

    g_assert_cmphex(mbox_tag(qts, FW_TAG_COMMAND_LINE, sizeof(val), val),
                    ==, FW_TAG_RESPONSE | strlen(cmdline));
    g_assert_cmpmem(val, strlen(cmdline), cmdline, strlen(cmdline));

    g_assert_cmphex(mbox_tag(qts, FW_TAG_COMMAND_LINE, 8, val), ==,
                    FW_TAG_RESPONSE | strlen(cmdline));
    g_assert_cmphex(val[0], ==, 0xa5a5a5a5);

    qtest_quit(qts);
    unlink(kernel);
}

/* RNG200, registers as in Linux drivers/char/hw_random/iproc-rng200.c */
#define RNG_CTRL                0x00
#define RNG_SOFT_RESET          0x04
#define RBG_SOFT_RESET          0x08
#define RNG_TOTAL_BIT_COUNT     0x0c
#define RNG_TOTAL_BIT_COUNT_THRESHOLD 0x10
#define RNG_INT_STATUS          0x18
#define RNG_INT_STATUS_TOTAL_BITS_COUNT         BIT(0)
#define RNG_INT_STATUS_STARTUP_TRANSITIONS_MET  BIT(17)
#define RNG_INT_ENABLE          0x1c
#define RNG_FIFO_DATA           0x20
#define RNG_FIFO_COUNT          0x24
#define RNG_FIFO_DEPTH          16
#define RNG_WARMUP_BITS         0x40000

static uint32_t rng_readl(QTestState *qts, uint32_t reg)
{
    return qtest_readl(qts, RNG_BASE + reg);
}

static void rng_writel(QTestState *qts, uint32_t reg, uint32_t val)
{
    qtest_writel(qts, RNG_BASE + reg, val);
}

/* Linux's bcm2711_rng200_init() */
static void rng_start(QTestState *qts)
{
    rng_writel(qts, RNG_TOTAL_BIT_COUNT_THRESHOLD, RNG_WARMUP_BITS);
    rng_writel(qts, RNG_FIFO_COUNT, 2 << 8);
    rng_writel(qts, RNG_CTRL, (3 << 13) | 0x1fff);
}

/* Stopped at reset: nothing counted, nothing to read */
static void test_rng_stopped(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");

    g_assert_cmphex(rng_readl(qts, RNG_CTRL), ==, 0);
    g_assert_cmphex(rng_readl(qts, RNG_TOTAL_BIT_COUNT), ==, 0);
    g_assert_cmphex(rng_readl(qts, RNG_INT_STATUS), ==, 0);
    g_assert_cmphex(rng_readl(qts, RNG_FIFO_COUNT), ==, 0);
    g_assert_cmphex(rng_readl(qts, RNG_FIFO_DATA), ==, 0);

    /* The bit count is read-only */
    rng_writel(qts, RNG_TOTAL_BIT_COUNT, 1234);
    g_assert_cmphex(rng_readl(qts, RNG_TOTAL_BIT_COUNT), ==, 0);

    qtest_quit(qts);
}

/*
 * Once started the FIFO is full and stays full; the count includes the
 * warm-up bits and every refill, and start-up raises both status bits,
 * which are write-one-to-clear.
 */
static void test_rng_start(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");
    uint32_t a, b;

    rng_start(qts);
    g_assert_cmphex(rng_readl(qts, RNG_FIFO_COUNT), ==,
                    (2 << 8) | RNG_FIFO_DEPTH);
    g_assert_cmpuint(rng_readl(qts, RNG_TOTAL_BIT_COUNT), ==,
                     RNG_WARMUP_BITS + RNG_FIFO_DEPTH * 32);
    g_assert_cmphex(rng_readl(qts, RNG_INT_STATUS), ==,
                    RNG_INT_STATUS_TOTAL_BITS_COUNT |
                    RNG_INT_STATUS_STARTUP_TRANSITIONS_MET);

    a = rng_readl(qts, RNG_FIFO_DATA);
    b = rng_readl(qts, RNG_FIFO_DATA);
    g_assert_cmphex(a, !=, b);
    g_assert_cmphex(rng_readl(qts, RNG_FIFO_COUNT) & 0xff, ==,
                    RNG_FIFO_DEPTH);
    g_assert_cmpuint(rng_readl(qts, RNG_TOTAL_BIT_COUNT), ==,
                     RNG_WARMUP_BITS + (RNG_FIFO_DEPTH + 2) * 32);

    rng_writel(qts, RNG_INT_STATUS, RNG_INT_STATUS_TOTAL_BITS_COUNT);
    g_assert_cmphex(rng_readl(qts, RNG_INT_STATUS), ==,
                    RNG_INT_STATUS_STARTUP_TRANSITIONS_MET);

    /* Stopped, the FIFO drains and is not refilled */
    rng_writel(qts, RNG_CTRL, 0);
    for (int i = RNG_FIFO_DEPTH; i > 0; i--) {
        g_assert_cmphex(rng_readl(qts, RNG_FIFO_COUNT) & 0xff, ==, i);
        rng_readl(qts, RNG_FIFO_DATA);
    }
    g_assert_cmphex(rng_readl(qts, RNG_FIFO_COUNT) & 0xff, ==, 0);

    qtest_quit(qts);
}

/* Either soft reset empties the FIFO and restarts the count */
static void test_rng_soft_reset(void)
{
    static const uint32_t regs[] = { RNG_SOFT_RESET, RBG_SOFT_RESET };
    QTestState *qts = qtest_init("-machine raspi5b");

    rng_start(qts);
    for (int i = 0; i < ARRAY_SIZE(regs); i++) {
        rng_writel(qts, regs[i], 1);
        g_assert_cmphex(rng_readl(qts, regs[i]), ==, 1);
        g_assert_cmphex(rng_readl(qts, RNG_FIFO_COUNT) & 0xff, ==, 0);
        g_assert_cmphex(rng_readl(qts, RNG_TOTAL_BIT_COUNT), ==, 0);
        rng_writel(qts, regs[i], 0);
        g_assert_cmphex(rng_readl(qts, RNG_FIFO_COUNT) & 0xff, ==,
                        RNG_FIFO_DEPTH);
        g_assert_cmpuint(rng_readl(qts, RNG_TOTAL_BIT_COUNT), ==,
                         RNG_WARMUP_BITS + RNG_FIFO_DEPTH * 32);
    }

    qtest_quit(qts);
}

/* A system reset stops the generator */
static void test_rng_reset(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");

    rng_start(qts);
    rng_writel(qts, RNG_INT_ENABLE, 0x21);
    qtest_system_reset(qts);
    for (uint32_t reg = RNG_CTRL; reg <= RNG_FIFO_COUNT; reg += 4) {
        if (reg != 0x14) {
            g_assert_cmphex(rng_readl(qts, reg), ==, 0);
        }
    }

    qtest_quit(qts);
}

/* With -seed, the data is the same on every run */
static void test_rng_seed(void)
{
    uint32_t words[2][4];

    for (int run = 0; run < 2; run++) {
        QTestState *qts = qtest_init("-machine raspi5b -seed 42");

        rng_start(qts);
        for (int i = 0; i < ARRAY_SIZE(words[run]); i++) {
            words[run][i] = rng_readl(qts, RNG_FIFO_DATA);
        }
        qtest_quit(qts);
    }
    g_assert_cmpmem(words[0], sizeof(words[0]), words[1], sizeof(words[1]));
}

/*
 * brcmstb level 2 interrupt controllers (Linux irq-brcmstb-l2.c). Their
 * inputs are driven from qtest through the SoC's children.
 */
#define L2_EDGE_STATUS          0x00
#define L2_EDGE_SET             0x04
#define L2_EDGE_CLEAR           0x08
#define L2_EDGE_MASK_STATUS     0x0c
#define L2_EDGE_MASK_SET        0x10
#define L2_EDGE_MASK_CLEAR      0x14
#define L2_LEVEL_STATUS         0x00
#define L2_LEVEL_MASK_STATUS    0x04
#define L2_LEVEL_MASK_SET       0x08
#define L2_LEVEL_MASK_CLEAR     0x0c

typedef struct L2Intc {
    const char *name;           /* child of /machine/soc */
    uint64_t base;
    int spi;                    /* bcm2712-rpi-5-b.dtb */
    bool edge;
} L2Intc;

static const L2Intc l2_intcs[] = {
    { "disp-intr",      0x107c502000ULL, 97,  true },
    { "cpu-l2-irq",     0x107d503000ULL, 238, true },
    { "bsc-irq",        0x107d508380ULL, 242, false },
    { "main-irq",       0x107d508400ULL, 244, false },
    { "aon-intr",       0x107d510600ULL, 239, true },
    { "l2-intc",        0x107d517000ULL, 247, false },
    { "main-aon-irq",   0x107d517ac0ULL, 245, false },
};

#define L2_MAIN_IRQ     (&l2_intcs[3])
#define L2_AON_INTR     (&l2_intcs[4])

static uint32_t l2_readl(QTestState *qts, const L2Intc *l2, uint32_t reg)
{
    return qtest_readl(qts, l2->base + reg);
}

static void l2_writel(QTestState *qts, const L2Intc *l2, uint32_t reg,
                      uint32_t val)
{
    qtest_writel(qts, l2->base + reg, val);
}

static void l2_set_input(QTestState *qts, const L2Intc *l2, int n, int level)
{
    g_autofree char *path = g_strdup_printf("/machine/soc/%s", l2->name);

    qtest_set_irq_in(qts, path, NULL, n, level);
}

static uint32_t l2_mask_status(QTestState *qts, const L2Intc *l2)
{
    return l2_readl(qts, l2, l2->edge ? L2_EDGE_MASK_STATUS
                                      : L2_LEVEL_MASK_STATUS);
}

/* Every input masked, nothing pending, output low */
static void l2_check_reset(QTestState *qts)
{
    for (int i = 0; i < ARRAY_SIZE(l2_intcs); i++) {
        const L2Intc *l2 = &l2_intcs[i];

        g_assert_cmphex(l2_readl(qts, l2, 0), ==, 0);
        g_assert_cmphex(l2_mask_status(qts, l2), ==, UINT32_MAX);
        g_assert_false(gic_spi_pending(qts, l2->spi));
    }
}

static void test_l2_intc_reset_values(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");

    l2_check_reset(qts);

    qtest_quit(qts);
}

/* Each controller drives its own SPI */
static void test_l2_intc_outputs(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");

    for (int i = 0; i < ARRAY_SIZE(l2_intcs); i++) {
        const L2Intc *l2 = &l2_intcs[i];

        l2_writel(qts, l2, l2->edge ? L2_EDGE_MASK_CLEAR
                                    : L2_LEVEL_MASK_CLEAR, BIT(31));
        l2_set_input(qts, l2, 31, 1);
        for (int j = 0; j < ARRAY_SIZE(l2_intcs); j++) {
            g_assert_cmpint(gic_spi_pending(qts, l2_intcs[j].spi), ==,
                            j == i);
        }
        l2_set_input(qts, l2, 31, 0);
        if (l2->edge) {
            l2_writel(qts, l2, L2_EDGE_CLEAR, BIT(31));
        }
        g_assert_false(gic_spi_pending(qts, l2->spi));
    }

    qtest_quit(qts);
}

static void test_l2_intc_mask(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");
    const L2Intc *l2 = L2_MAIN_IRQ;

    l2_writel(qts, l2, L2_LEVEL_MASK_CLEAR, BIT(0) | BIT(9));
    g_assert_cmphex(l2_mask_status(qts, l2), ==,
                    (uint32_t)~(BIT(0) | BIT(9)));
    l2_writel(qts, l2, L2_LEVEL_MASK_SET, BIT(0));
    g_assert_cmphex(l2_mask_status(qts, l2), ==, (uint32_t)~BIT(9));

    /* A masked input shows in STATUS but does not raise the output */
    l2_set_input(qts, l2, 0, 1);
    g_assert_cmphex(l2_readl(qts, l2, L2_LEVEL_STATUS), ==, BIT(0));
    g_assert_false(gic_spi_pending(qts, l2->spi));
    l2_set_input(qts, l2, 9, 1);
    g_assert_true(gic_spi_pending(qts, l2->spi));
    l2_writel(qts, l2, L2_LEVEL_MASK_SET, BIT(9));
    g_assert_false(gic_spi_pending(qts, l2->spi));
    l2_writel(qts, l2, L2_LEVEL_MASK_CLEAR, BIT(0));
    g_assert_true(gic_spi_pending(qts, l2->spi));

    /* MASK_STATUS is read-only */
    l2_writel(qts, l2, L2_LEVEL_MASK_STATUS, 0);
    g_assert_cmphex(l2_mask_status(qts, l2), ==, (uint32_t)~BIT(0));

    qtest_quit(qts);
}

/* Level layout: STATUS follows the inputs and cannot be written */
static void test_l2_intc_level(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");
    const L2Intc *l2 = L2_MAIN_IRQ;

    l2_writel(qts, l2, L2_LEVEL_MASK_CLEAR, BIT(5));
    l2_set_input(qts, l2, 5, 1);
    g_assert_cmphex(l2_readl(qts, l2, L2_LEVEL_STATUS), ==, BIT(5));
    g_assert_true(gic_spi_pending(qts, l2->spi));

    l2_writel(qts, l2, L2_LEVEL_STATUS, BIT(5));
    g_assert_cmphex(l2_readl(qts, l2, L2_LEVEL_STATUS), ==, BIT(5));
    g_assert_true(gic_spi_pending(qts, l2->spi));

    l2_set_input(qts, l2, 5, 0);
    g_assert_cmphex(l2_readl(qts, l2, L2_LEVEL_STATUS), ==, 0);
    g_assert_false(gic_spi_pending(qts, l2->spi));

    qtest_quit(qts);
}

/* Edge layout: STATUS latches rising inputs until cleared */
static void test_l2_intc_edge(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");
    const L2Intc *l2 = L2_AON_INTR;

    l2_writel(qts, l2, L2_EDGE_MASK_CLEAR, BIT(3));
    l2_set_input(qts, l2, 3, 1);
    l2_set_input(qts, l2, 3, 0);
    g_assert_cmphex(l2_readl(qts, l2, L2_EDGE_STATUS), ==, BIT(3));
    g_assert_true(gic_spi_pending(qts, l2->spi));

    l2_writel(qts, l2, L2_EDGE_CLEAR, BIT(3));
    g_assert_cmphex(l2_readl(qts, l2, L2_EDGE_STATUS), ==, 0);
    g_assert_false(gic_spi_pending(qts, l2->spi));

    /* A held input latches once: clearing it waits for the next edge */
    l2_set_input(qts, l2, 3, 1);
    l2_writel(qts, l2, L2_EDGE_CLEAR, BIT(3));
    l2_set_input(qts, l2, 3, 1);
    g_assert_cmphex(l2_readl(qts, l2, L2_EDGE_STATUS), ==, 0);
    l2_set_input(qts, l2, 3, 0);
    l2_set_input(qts, l2, 3, 1);
    g_assert_cmphex(l2_readl(qts, l2, L2_EDGE_STATUS), ==, BIT(3));

    /* STATUS itself is read-only; the write-only registers read as zero */
    l2_writel(qts, l2, L2_EDGE_STATUS, 0);
    g_assert_cmphex(l2_readl(qts, l2, L2_EDGE_STATUS), ==, BIT(3));
    for (uint32_t reg = L2_EDGE_SET; reg <= L2_EDGE_MASK_CLEAR; reg += 4) {
        if (reg != L2_EDGE_MASK_STATUS) {
            g_assert_cmphex(l2_readl(qts, l2, reg), ==, 0);
        }
    }

    qtest_quit(qts);
}

/* Edge layout: SET raises status bits from software */
static void test_l2_intc_software_set(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");
    const L2Intc *l2 = L2_AON_INTR;

    l2_writel(qts, l2, L2_EDGE_SET, BIT(7) | BIT(30));
    g_assert_cmphex(l2_readl(qts, l2, L2_EDGE_STATUS), ==, BIT(7) | BIT(30));
    g_assert_false(gic_spi_pending(qts, l2->spi));
    l2_writel(qts, l2, L2_EDGE_MASK_CLEAR, BIT(30));
    g_assert_true(gic_spi_pending(qts, l2->spi));
    l2_writel(qts, l2, L2_EDGE_CLEAR, BIT(30));
    g_assert_cmphex(l2_readl(qts, l2, L2_EDGE_STATUS), ==, BIT(7));
    g_assert_false(gic_spi_pending(qts, l2->spi));

    qtest_quit(qts);
}

/* Reset masks everything and drops latched status; levels stay visible */
static void test_l2_intc_reset(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");

    for (int i = 0; i < ARRAY_SIZE(l2_intcs); i++) {
        const L2Intc *l2 = &l2_intcs[i];

        l2_writel(qts, l2, l2->edge ? L2_EDGE_MASK_CLEAR
                                    : L2_LEVEL_MASK_CLEAR, UINT32_MAX);
        if (l2->edge) {
            l2_writel(qts, l2, L2_EDGE_SET, BIT(1));
        }
    }
    l2_set_input(qts, L2_MAIN_IRQ, 2, 1);
    qtest_system_reset(qts);

    l2_set_input(qts, L2_MAIN_IRQ, 2, 0);
    l2_check_reset(qts);
    l2_set_input(qts, L2_MAIN_IRQ, 2, 1);
    g_assert_cmphex(l2_readl(qts, L2_MAIN_IRQ, L2_LEVEL_STATUS), ==, BIT(2));

    qtest_quit(qts);
}

/* Latched status and the mask survive migration */
static void test_l2_intc_migrate(void)
{
    g_autofree char *file = g_strdup_printf("%s/raspi5b-l2-%d.mig",
                                            g_get_tmp_dir(), getpid());
    g_autofree char *out = g_strdup_printf("exec:cat > %s", file);
    g_autofree char *in = g_strdup_printf("exec:cat %s", file);
    const char *args = "-machine raspi5b -m 1G";
    const L2Intc *l2 = L2_AON_INTR;
    QTestState *src, *dst;

    src = qtest_init(args);
    l2_set_input(src, l2, 4, 1);
    l2_writel(src, l2, L2_EDGE_MASK_CLEAR, BIT(4));
    qtest_qmp_assert_success(src, "{ 'execute': 'migrate',"
                             "  'arguments': { 'uri': %s } }", out);
    wait_for_migration(src);
    qtest_quit(src);

    dst = qtest_initf("%s -incoming defer", args);
    qtest_qmp_assert_success(dst, "{ 'execute': 'migrate-incoming',"
                             "  'arguments': { 'uri': %s } }", in);
    wait_for_migration(dst);

    g_assert_cmphex(l2_readl(dst, l2, L2_EDGE_STATUS), ==, BIT(4));
    g_assert_cmphex(l2_mask_status(dst, l2), ==, (uint32_t)~BIT(4));
    g_assert_true(gic_spi_pending(dst, l2->spi));
    /* The input is still high: no new edge until it falls */
    l2_writel(dst, l2, L2_EDGE_CLEAR, BIT(4));
    l2_set_input(dst, l2, 4, 1);
    g_assert_cmphex(l2_readl(dst, l2, L2_EDGE_STATUS), ==, 0);

    qtest_quit(dst);
    unlink(file);
}

/*
 * brcmstb GPIO blocks (Linux gpio-brcmstb.c): banks of up to 32 lines,
 * each with eight registers at a 0x20 stride. Their inputs are driven and
 * their outputs watched from qtest through the SoC's children.
 */
#define GIO_ODEN                0x00
#define GIO_DATA                0x04
#define GIO_IODIR               0x08
#define GIO_EC                  0x0c
#define GIO_EI                  0x10
#define GIO_MASK                0x14
#define GIO_LEVEL               0x18
#define GIO_STAT                0x1c
#define GIO_BANK_SIZE           0x20
#define GIO_BANK_LINES          32

typedef struct Gio {
    const char *name;           /* child of /machine/soc */
    uint64_t base;
    uint32_t widths[2];         /* bcm2712.dtsi */
    uint32_t high[2];           /* lines the board pulls up */
} Gio;

/* The board's use of the lines (raspi5b.c) */
#define PWR_BUTTON_GIO          20      /* low while pressed */
#define PWR_BUTTON_PRESS_NS     (200 * SCALE_MS)
#define ACT_LED_AON_GPIO        9       /* lit while low */

static const Gio gios[] = {
    { "gio",        0x107d508500ULL, { 32, 22 }, { BIT(PWR_BUTTON_GIO) } },
    { "gio-aon",    0x107d517c00ULL, { 17, 6 }, { BIT(ACT_LED_AON_GPIO) } },
};

#define GIO             (&gios[0])
#define GIO_AON         (&gios[1])

static uint32_t gio_readl(QTestState *qts, const Gio *gio, int bank,
                          uint32_t reg)
{
    return qtest_readl(qts, gio->base + bank * GIO_BANK_SIZE + reg);
}

static void gio_writel(QTestState *qts, const Gio *gio, int bank,
                       uint32_t reg, uint32_t val)
{
    qtest_writel(qts, gio->base + bank * GIO_BANK_SIZE + reg, val);
}

/* Drive @line (bank @line / 32) from outside */
static void gio_set_input(QTestState *qts, const Gio *gio, int line,
                          int level)
{
    g_autofree char *path = g_strdup_printf("/machine/soc/%s", gio->name);

    qtest_set_irq_in(qts, path, NULL, line, level);
}

static uint32_t gio_valid(const Gio *gio, int bank)
{
    return MAKE_64BIT_MASK(0, gio->widths[bank]);
}

/* GIO interrupts through main_irq, which resets with every input masked */
static void gio_unmask_main_irq(QTestState *qts)
{
    l2_writel(qts, L2_MAIN_IRQ, L2_LEVEL_MASK_CLEAR, BIT(0));
}

static bool gio_irq(QTestState *qts)
{
    bool status = l2_readl(qts, L2_MAIN_IRQ, L2_LEVEL_STATUS) & BIT(0);

    g_assert_cmpint(gic_spi_pending(qts, L2_MAIN_IRQ->spi), ==, status);
    return status;
}

static void gio_check_reset(QTestState *qts)
{
    for (int i = 0; i < ARRAY_SIZE(gios); i++) {
        for (int bank = 0; bank < ARRAY_SIZE(gios[i].widths); bank++) {
            const Gio *gio = &gios[i];

            g_assert_cmphex(gio_readl(qts, gio, bank, GIO_ODEN), ==, 0);
            g_assert_cmphex(gio_readl(qts, gio, bank, GIO_IODIR), ==,
                            gio_valid(gio, bank));
            g_assert_cmphex(gio_readl(qts, gio, bank, GIO_EC), ==, 0);
            g_assert_cmphex(gio_readl(qts, gio, bank, GIO_EI), ==, 0);
            g_assert_cmphex(gio_readl(qts, gio, bank, GIO_MASK), ==, 0);
            g_assert_cmphex(gio_readl(qts, gio, bank, GIO_LEVEL), ==, 0);
            g_assert_cmphex(gio_readl(qts, gio, bank, GIO_STAT), ==, 0);
        }
    }
    g_assert_false(l2_readl(qts, L2_MAIN_IRQ, L2_LEVEL_STATUS) & BIT(0));
}

/*
 * Every line an input, every interrupt disabled, nothing pending; the
 * lines read low but for those the board pulls up
 */
static void test_gio_reset_values(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");

    gio_check_reset(qts);
    for (int i = 0; i < ARRAY_SIZE(gios); i++) {
        for (int bank = 0; bank < ARRAY_SIZE(gios[i].widths); bank++) {
            g_assert_cmphex(gio_readl(qts, &gios[i], bank, GIO_DATA), ==,
                            gios[i].high[bank]);
        }
    }
    qtest_quit(qts);
}

/* Bits beyond a bank's width read as zero, whatever is written or driven */
static void test_gio_widths(void)
{
    static const struct {
        uint32_t reg, reset;
    } regs[] = {
        { GIO_ODEN, 0 }, { GIO_IODIR, UINT32_MAX }, { GIO_EC, 0 },
        { GIO_EI, 0 }, { GIO_MASK, 0 }, { GIO_LEVEL, 0 },
    };
    QTestState *qts = qtest_init("-machine raspi5b");

    for (int i = 0; i < ARRAY_SIZE(gios); i++) {
        for (int bank = 0; bank < ARRAY_SIZE(gios[i].widths); bank++) {
            const Gio *gio = &gios[i];
            uint32_t valid = gio_valid(gio, bank);

            for (int r = 0; r < ARRAY_SIZE(regs); r++) {
                gio_writel(qts, gio, bank, regs[r].reg, UINT32_MAX);
                g_assert_cmphex(gio_readl(qts, gio, bank, regs[r].reg), ==,
                                valid);
                gio_writel(qts, gio, bank, regs[r].reg, regs[r].reset);
            }

            /* Every line an output driving high, then an input again */
            gio_writel(qts, gio, bank, GIO_IODIR, 0);
            gio_writel(qts, gio, bank, GIO_DATA, UINT32_MAX);
            g_assert_cmphex(gio_readl(qts, gio, bank, GIO_DATA), ==, valid);
            gio_writel(qts, gio, bank, GIO_IODIR, UINT32_MAX);

            /* Low levels, then falling edges, latched every line */
            g_assert_cmphex(gio_readl(qts, gio, bank, GIO_STAT), ==, valid);
            gio_writel(qts, gio, bank, GIO_STAT, UINT32_MAX);
            g_assert_cmphex(gio_readl(qts, gio, bank, GIO_STAT), ==, 0);

            /* The inputs past the last line lead nowhere */
            for (int bit = gio->widths[bank]; bit < GIO_BANK_LINES; bit++) {
                gio_set_input(qts, gio, bank * GIO_BANK_LINES + bit, 1);
            }
            g_assert_cmphex(gio_readl(qts, gio, bank, GIO_DATA), ==,
                            gio->high[bank]);
            g_assert_cmphex(gio_readl(qts, gio, bank, GIO_STAT), ==, 0);
        }
    }
    qtest_quit(qts);
}

/*
 * DATA reads the level of the lines: an input's from outside, an
 * output's own unless it is open-drain and released. The same levels
 * come out of the block's GPIO outputs.
 */
static void test_gio_data(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");
    const Gio *gio = GIO;
    const int line = GIO_BANK_LINES + 3;        /* bank 1, bit 3 */

    qtest_irq_intercept_out(qts, "/machine/soc/gio");

    gio_set_input(qts, gio, line, 1);
    g_assert_cmphex(gio_readl(qts, gio, 1, GIO_DATA), ==, BIT(3));
    g_assert_true(qtest_get_irq(qts, line));

    /* An output drives low whatever comes in, and high */
    gio_writel(qts, gio, 1, GIO_IODIR, gio_valid(gio, 1) & ~BIT(3));
    g_assert_cmphex(gio_readl(qts, gio, 1, GIO_DATA), ==, 0);
    g_assert_false(qtest_get_irq(qts, line));
    gio_writel(qts, gio, 1, GIO_DATA, BIT(3));
    gio_set_input(qts, gio, line, 0);
    g_assert_cmphex(gio_readl(qts, gio, 1, GIO_DATA), ==, BIT(3));
    g_assert_true(qtest_get_irq(qts, line));

    /* Open-drain: high releases the line to its input level */
    gio_writel(qts, gio, 1, GIO_ODEN, BIT(3));
    g_assert_cmphex(gio_readl(qts, gio, 1, GIO_DATA), ==, 0);
    g_assert_false(qtest_get_irq(qts, line));
    gio_set_input(qts, gio, line, 1);
    g_assert_cmphex(gio_readl(qts, gio, 1, GIO_DATA), ==, BIT(3));
    gio_writel(qts, gio, 1, GIO_DATA, 0);
    g_assert_cmphex(gio_readl(qts, gio, 1, GIO_DATA), ==, 0);
    g_assert_false(qtest_get_irq(qts, line));

    /* Back to an input: the level from outside again */
    gio_writel(qts, gio, 1, GIO_IODIR, gio_valid(gio, 1));
    g_assert_cmphex(gio_readl(qts, gio, 1, GIO_DATA), ==, BIT(3));
    g_assert_true(qtest_get_irq(qts, line));
    /* Other lines stayed where they were */
    g_assert_cmphex(gio_readl(qts, gio, 0, GIO_DATA), ==, gio->high[0]);

    qtest_quit(qts);
}

/*
 * Edge detection: falling with EC clear, rising with it set, both with EI
 * set. An edge latches its STAT bit until written with 1, masked or not,
 * and interrupts through main_irq while MASK enables it. Outputs are
 * watched too.
 */
static void test_gio_edges(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");
    const Gio *gio = GIO;

    gio_unmask_main_irq(qts);

    /* Falling */
    gio_writel(qts, gio, 0, GIO_MASK, BIT(7));
    gio_set_input(qts, gio, 7, 1);
    g_assert_cmphex(gio_readl(qts, gio, 0, GIO_STAT), ==, 0);
    g_assert_false(gio_irq(qts));
    gio_set_input(qts, gio, 7, 0);
    g_assert_cmphex(gio_readl(qts, gio, 0, GIO_STAT), ==, BIT(7));
    g_assert_true(gio_irq(qts));
    gio_writel(qts, gio, 0, GIO_STAT, BIT(7));
    g_assert_cmphex(gio_readl(qts, gio, 0, GIO_STAT), ==, 0);
    g_assert_false(gio_irq(qts));

    /* Rising */
    gio_writel(qts, gio, 0, GIO_EC, BIT(7));
    gio_set_input(qts, gio, 7, 1);
    g_assert_cmphex(gio_readl(qts, gio, 0, GIO_STAT), ==, BIT(7));
    gio_writel(qts, gio, 0, GIO_STAT, BIT(7));
    gio_set_input(qts, gio, 7, 0);
    g_assert_cmphex(gio_readl(qts, gio, 0, GIO_STAT), ==, 0);

    /* Both */
    gio_writel(qts, gio, 0, GIO_EI, BIT(7));
    gio_set_input(qts, gio, 7, 1);
    g_assert_cmphex(gio_readl(qts, gio, 0, GIO_STAT), ==, BIT(7));
    gio_writel(qts, gio, 0, GIO_STAT, BIT(7));
    gio_set_input(qts, gio, 7, 0);
    g_assert_cmphex(gio_readl(qts, gio, 0, GIO_STAT), ==, BIT(7));
    gio_writel(qts, gio, 0, GIO_STAT, BIT(7));
    g_assert_false(gio_irq(qts));

    /* Masked: latched all the same, and interrupts once enabled */
    gio_writel(qts, gio, 0, GIO_MASK, 0);
    gio_set_input(qts, gio, 7, 1);
    g_assert_cmphex(gio_readl(qts, gio, 0, GIO_STAT), ==, BIT(7));
    g_assert_false(gio_irq(qts));
    gio_writel(qts, gio, 0, GIO_MASK, BIT(7));
    g_assert_true(gio_irq(qts));
    gio_writel(qts, gio, 0, GIO_STAT, BIT(7));
    g_assert_false(gio_irq(qts));

    /* An output's own edge, in the other bank: one interrupt for both */
    gio_writel(qts, gio, 1, GIO_EC, BIT(21));
    gio_writel(qts, gio, 1, GIO_MASK, BIT(21));
    gio_writel(qts, gio, 1, GIO_IODIR, gio_valid(gio, 1) & ~BIT(21));
    g_assert_false(gio_irq(qts));
    gio_writel(qts, gio, 1, GIO_DATA, BIT(21));
    g_assert_cmphex(gio_readl(qts, gio, 1, GIO_STAT), ==, BIT(21));
    g_assert_true(gio_irq(qts));
    gio_set_input(qts, gio, 7, 0);
    gio_set_input(qts, gio, 7, 1);
    gio_writel(qts, gio, 1, GIO_STAT, BIT(21));
    g_assert_true(gio_irq(qts));
    gio_writel(qts, gio, 0, GIO_STAT, BIT(7));
    g_assert_false(gio_irq(qts));

    qtest_quit(qts);
}

/*
 * Level detection: STAT is set for as long as the line is at its active
 * level, high with EC set, low with it clear, so writing it with 1 only
 * clears it once the line has left that level
 */
static void test_gio_levels(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");
    const Gio *gio = GIO;

    gio_unmask_main_irq(qts);
    gio_writel(qts, gio, 0, GIO_MASK, BIT(9) | BIT(10));

    /* High */
    gio_writel(qts, gio, 0, GIO_EC, BIT(9));
    gio_writel(qts, gio, 0, GIO_LEVEL, BIT(9));
    g_assert_cmphex(gio_readl(qts, gio, 0, GIO_STAT), ==, 0);
    gio_set_input(qts, gio, 9, 1);
    g_assert_cmphex(gio_readl(qts, gio, 0, GIO_STAT), ==, BIT(9));
    g_assert_true(gio_irq(qts));
    gio_writel(qts, gio, 0, GIO_STAT, BIT(9));
    g_assert_cmphex(gio_readl(qts, gio, 0, GIO_STAT), ==, BIT(9));
    gio_set_input(qts, gio, 9, 0);
    g_assert_cmphex(gio_readl(qts, gio, 0, GIO_STAT), ==, BIT(9));
    gio_writel(qts, gio, 0, GIO_STAT, BIT(9));
    g_assert_cmphex(gio_readl(qts, gio, 0, GIO_STAT), ==, 0);
    g_assert_false(gio_irq(qts));

    /* Low: a line already low is active as soon as it is configured */
    gio_writel(qts, gio, 0, GIO_LEVEL, BIT(9) | BIT(10));
    g_assert_cmphex(gio_readl(qts, gio, 0, GIO_STAT), ==, BIT(10));
    g_assert_true(gio_irq(qts));
    gio_set_input(qts, gio, 10, 1);
    gio_writel(qts, gio, 0, GIO_STAT, BIT(10));
    g_assert_cmphex(gio_readl(qts, gio, 0, GIO_STAT), ==, 0);
    g_assert_false(gio_irq(qts));

    qtest_quit(qts);
}

/* GIO AON latches like GIO, but its interrupt goes nowhere */
static void test_gio_aon(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");
    const Gio *gio = GIO_AON;

    for (int i = 0; i < ARRAY_SIZE(l2_intcs); i++) {
        l2_writel(qts, &l2_intcs[i], l2_intcs[i].edge ? L2_EDGE_MASK_CLEAR
                                                     : L2_LEVEL_MASK_CLEAR,
                  UINT32_MAX);
    }
    gio_writel(qts, gio, 0, GIO_EC, BIT(5));
    gio_writel(qts, gio, 0, GIO_MASK, BIT(5));
    gio_set_input(qts, gio, 5, 1);
    g_assert_cmphex(gio_readl(qts, gio, 0, GIO_DATA), ==,
                    BIT(5) | gio->high[0]);
    g_assert_cmphex(gio_readl(qts, gio, 0, GIO_STAT), ==, BIT(5));
    for (int i = 0; i < ARRAY_SIZE(l2_intcs); i++) {
        g_assert_cmphex(l2_readl(qts, &l2_intcs[i], 0), ==, 0);
        g_assert_false(gic_spi_pending(qts, l2_intcs[i].spi));
    }

    qtest_quit(qts);
}

/*
 * Reset brings back the reset values without latching the edges it
 * makes: the lines keep the levels driven into them
 */
static void test_gio_reset(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");
    const Gio *gio = GIO;

    qtest_irq_intercept_out(qts, "/machine/soc/gio");
    for (int i = 0; i < ARRAY_SIZE(gios); i++) {
        for (int bank = 0; bank < ARRAY_SIZE(gios[i].widths); bank++) {
            static const uint32_t regs[] = {
                GIO_EC, GIO_EI, GIO_MASK, GIO_LEVEL, GIO_ODEN, GIO_DATA,
            };

            for (int r = 0; r < ARRAY_SIZE(regs); r++) {
                gio_writel(qts, &gios[i], bank, regs[r], BIT(1));
            }
            gio_writel(qts, &gios[i], bank, GIO_IODIR, 0);
        }
    }
    gio_set_input(qts, gio, 2, 1);
    gio_writel(qts, gio, 0, GIO_EI, BIT(2) | BIT(3));
    gio_writel(qts, gio, 0, GIO_IODIR, BIT(2));
    gio_writel(qts, gio, 0, GIO_DATA, BIT(3));
    g_assert_cmphex(gio_readl(qts, gio, 0, GIO_DATA), ==, BIT(2) | BIT(3));
    g_assert_true(qtest_get_irq(qts, 3));

    qtest_system_reset(qts);

    gio_check_reset(qts);
    g_assert_cmphex(gio_readl(qts, gio, 0, GIO_DATA), ==,
                    BIT(2) | gio->high[0]);
    g_assert_true(qtest_get_irq(qts, 2));
    g_assert_false(qtest_get_irq(qts, 3));

    qtest_quit(qts);
}

/* Registers, driven levels and latched status survive migration */
static void test_gio_migrate(void)
{
    g_autofree char *file = g_strdup_printf("%s/raspi5b-gio-%d.mig",
                                            g_get_tmp_dir(), getpid());
    g_autofree char *out = g_strdup_printf("exec:cat > %s", file);
    g_autofree char *in = g_strdup_printf("exec:cat %s", file);
    const char *args = "-machine raspi5b -m 1G";
    const Gio *gio = GIO;
    QTestState *src, *dst;

    src = qtest_init(args);
    gio_unmask_main_irq(src);
    gio_writel(src, gio, 0, GIO_EC, BIT(4));
    gio_writel(src, gio, 0, GIO_MASK, BIT(4));
    gio_set_input(src, gio, 4, 1);
    gio_writel(src, gio, 1, GIO_IODIR, gio_valid(gio, 1) & ~BIT(0));
    gio_writel(src, gio, 1, GIO_DATA, BIT(0));
    qtest_qmp_assert_success(src, "{ 'execute': 'migrate',"
                             "  'arguments': { 'uri': %s } }", out);
    wait_for_migration(src);
    qtest_quit(src);

    dst = qtest_initf("%s -incoming defer", args);
    qtest_qmp_assert_success(dst, "{ 'execute': 'migrate-incoming',"
                             "  'arguments': { 'uri': %s } }", in);
    wait_for_migration(dst);

    g_assert_cmphex(gio_readl(dst, gio, 0, GIO_STAT), ==, BIT(4));
    g_assert_cmphex(gio_readl(dst, gio, 0, GIO_DATA), ==,
                    BIT(4) | gio->high[0]);
    g_assert_cmphex(gio_readl(dst, gio, 1, GIO_DATA), ==, BIT(0));
    g_assert_true(gio_irq(dst));
    /* The input is still high: no new edge until it falls and rises */
    gio_writel(dst, gio, 0, GIO_STAT, BIT(4));
    gio_set_input(dst, gio, 4, 1);
    g_assert_cmphex(gio_readl(dst, gio, 0, GIO_STAT), ==, 0);
    g_assert_false(gio_irq(dst));
    gio_set_input(dst, gio, 4, 0);
    gio_set_input(dst, gio, 4, 1);
    g_assert_cmphex(gio_readl(dst, gio, 0, GIO_STAT), ==, BIT(4));

    qtest_quit(dst);
    unlink(file);
}

/*
 * brcmstb pin controllers (Linux pinctrl-brcmstb.c): registers that hold
 * the function and pull of each pin, as many as the device tree node
 * spans. They store what is written and nothing more.
 */
typedef struct Pinctrl {
    uint64_t base;
    int num_regs;
} Pinctrl;

static const Pinctrl pinctrls[] = {
    { 0x107d504100ULL, 12 },
    { 0x107d510700ULL, 8 },     /* always-on */
};

/* A different value for each register */
static uint32_t pinctrl_pattern(int block, int reg)
{
    return 0xa5a5a5a5u ^ (block << 28) ^ (reg * 0x01010101u);
}

static void pinctrl_fill(QTestState *qts)
{
    for (int i = 0; i < ARRAY_SIZE(pinctrls); i++) {
        for (int r = 0; r < pinctrls[i].num_regs; r++) {
            qtest_writel(qts, pinctrls[i].base + 4 * r, pinctrl_pattern(i, r));
        }
    }
}

static void pinctrl_check(QTestState *qts, bool filled)
{
    for (int i = 0; i < ARRAY_SIZE(pinctrls); i++) {
        for (int r = 0; r < pinctrls[i].num_regs; r++) {
            g_assert_cmphex(qtest_readl(qts, pinctrls[i].base + 4 * r), ==,
                            filled ? pinctrl_pattern(i, r) : 0);
        }
    }
}

/* TODO(WS0.4): every register reads as zero until checked on hardware */
static void test_pinctrl_reset_values(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");

    pinctrl_check(qts, false);
    qtest_quit(qts);
}

/*
 * Every register reads back what was written, and the block ends where
 * its node does. The settings leave the GPIO lines alone: the pulled-up
 * lines stay high whatever the pulls and functions of their pins.
 */
static void test_pinctrl_read_back(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");

    pinctrl_fill(qts);
    for (int i = 0; i < ARRAY_SIZE(pinctrls); i++) {
        uint64_t end = pinctrls[i].base + 4 * pinctrls[i].num_regs;

        qtest_writel(qts, end, 0xffffffff);
        g_assert_cmphex(qtest_readl(qts, end), ==, 0);
    }
    pinctrl_check(qts, true);
    for (int i = 0; i < ARRAY_SIZE(gios); i++) {
        g_assert_cmphex(gio_readl(qts, &gios[i], 0, GIO_DATA), ==,
                        gios[i].high[0]);
    }

    qtest_quit(qts);
}

static void test_pinctrl_reset(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");

    pinctrl_fill(qts);
    qtest_system_reset(qts);
    pinctrl_check(qts, false);

    qtest_quit(qts);
}

static void test_pinctrl_migrate(void)
{
    g_autofree char *file = g_strdup_printf("%s/raspi5b-pinctrl-%d.mig",
                                            g_get_tmp_dir(), getpid());
    g_autofree char *out = g_strdup_printf("exec:cat > %s", file);
    g_autofree char *in = g_strdup_printf("exec:cat %s", file);
    const char *args = "-machine raspi5b -m 1G";
    QTestState *src, *dst;

    src = qtest_init(args);
    pinctrl_fill(src);
    qtest_qmp_assert_success(src, "{ 'execute': 'migrate',"
                             "  'arguments': { 'uri': %s } }", out);
    wait_for_migration(src);
    qtest_quit(src);

    dst = qtest_initf("%s -incoming defer", args);
    qtest_qmp_assert_success(dst, "{ 'execute': 'migrate-incoming',"
                             "  'arguments': { 'uri': %s } }", in);
    wait_for_migration(dst);
    pinctrl_check(dst, true);

    qtest_quit(dst);
    unlink(file);
}

/*
 * brcmstb BSC I2C controllers (Linux i2c-brcmstb.c), the DDC buses of
 * the HDMI ports. The board puts a monitor's EDID at 0x50 on HDMI0's;
 * nothing answers on HDMI1's.
 */
#define BSC_CHIP_ADDRESS        0x00
#define BSC_DATA_IN(n)          (0x04 + 4 * (n))
#define BSC_CNT_REG             0x24
#define BSC_CTL_REG             0x28
#define BSC_IIC_ENABLE          0x2c
#define BSC_DATA_OUT(n)         (0x30 + 4 * (n))
#define BSC_CTLHI_REG           0x50
#define BSC_SCL_PARAM           0x54
#define BSC_SIZE                0x58
#define BSC_NUM_DATA_REGS       8
#define BSC_CTL_DTF_READ        1
#define BSC_CTL_DTF_WR_RD       3
#define BSC_CTL_INT_EN          BIT(6)
#define BSC_IIC_EN_ENABLE       BIT(0)
#define BSC_IIC_EN_INTRP        BIT(1)
#define BSC_IIC_EN_NOACK        BIT(2)
#define BSC_IIC_EN_NOSTOP       BIT(4)
#define BSC_IIC_EN_NOSTART      BIT(5)
#define BSC_IIC_EN_RESTART      BIT(6)
#define BSC_CTLHI_IGNORE_ACK    BIT(1)
#define BSC_CTLHI_DATAREG_SIZE  BIT(6)

#define EDID_ADDR               0x50
#define EDID_SIZE               128

typedef struct Bsc {
    uint64_t base;
    int irq;                    /* input of bsc_irq */
} Bsc;

static const Bsc bscs[] = {
    { 0x107d508200ULL, 1 },     /* HDMI0 */
    { 0x107d508280ULL, 2 },     /* HDMI1 */
};

#define BSC_IRQ     (&l2_intcs[2])

static uint32_t bsc_readl(QTestState *qts, const Bsc *bsc, uint32_t reg)
{
    return qtest_readl(qts, bsc->base + reg);
}

static void bsc_writel(QTestState *qts, const Bsc *bsc, uint32_t reg,
                       uint32_t val)
{
    qtest_writel(qts, bsc->base + reg, val);
}

/* Byte @i of a transfer, in data registers of @regsz bytes */
static uint32_t bsc_reg(int i, int regsz)
{
    return i / regsz;
}

static int bsc_shift(int i, int regsz)
{
    return 8 * (i % regsz);
}

/*
 * One command, as brcmstb_i2c_xfer_bsc_data() issues it: the address,
 * the count, the bytes to write, the direction with the interrupt
 * enabled, then ENABLE with the start and stop @flags. Reads the bytes
 * read into @buf and returns IIC_ENABLE as the command left it; the
 * caller ends the command.
 */
static uint32_t bsc_command(QTestState *qts, const Bsc *bsc, uint8_t addr,
                            bool read, uint8_t *buf, int count,
                            uint32_t flags)
{
    int regsz = bsc_readl(qts, bsc, BSC_CTLHI_REG) & BSC_CTLHI_DATAREG_SIZE ?
                4 : 1;
    uint32_t status;

    bsc_writel(qts, bsc, BSC_CHIP_ADDRESS, addr << 1 | read);
    bsc_writel(qts, bsc, BSC_CNT_REG, count);
    if (!read) {
        uint32_t words[BSC_NUM_DATA_REGS] = { 0 };

        for (int i = 0; i < count; i++) {
            words[bsc_reg(i, regsz)] |= buf[i] << bsc_shift(i, regsz);
        }
        for (int r = 0; r < DIV_ROUND_UP(count, regsz); r++) {
            bsc_writel(qts, bsc, BSC_DATA_IN(r), words[r]);
        }
    }
    bsc_writel(qts, bsc, BSC_CTL_REG,
               (read ? BSC_CTL_DTF_READ : 0) | BSC_CTL_INT_EN);
    bsc_writel(qts, bsc, BSC_IIC_ENABLE, flags | BSC_IIC_EN_ENABLE);
    status = bsc_readl(qts, bsc, BSC_IIC_ENABLE);
    if (read) {
        for (int i = 0; i < count; i++) {
            buf[i] = bsc_readl(qts, bsc, BSC_DATA_OUT(bsc_reg(i, regsz))) >>
                     bsc_shift(i, regsz);
        }
    }
    return status;
}

/* End a command as brcmstb_send_i2c_cmd() does */
static void bsc_end(QTestState *qts, const Bsc *bsc)
{
    bsc_writel(qts, bsc, BSC_CTL_REG,
               bsc_readl(qts, bsc, BSC_CTL_REG) & ~BSC_CTL_INT_EN);
    bsc_writel(qts, bsc, BSC_CNT_REG, 0);
    bsc_writel(qts, bsc, BSC_IIC_ENABLE, 0);
}

/* A command that completes without a NACK, and its end */
static void bsc_command_ok(QTestState *qts, const Bsc *bsc, uint8_t addr,
                           bool read, uint8_t *buf, int count,
                           uint32_t flags)
{
    g_assert_cmphex(bsc_command(qts, bsc, addr, read, buf, count, flags), ==,
                    flags | BSC_IIC_EN_ENABLE | BSC_IIC_EN_INTRP);
    bsc_end(qts, bsc);
}

/*
 * Read @len bytes of the EDID from @offset, as brcmstb_i2c_xfer() does
 * with the two messages of an EDID read: the offset, without a stop, then
 * the bytes in chunks as large as the data registers hold, all but the
 * last without a stop and all but the first without a start.
 */
static void bsc_edid_read(QTestState *qts, const Bsc *bsc, uint8_t offset,
                          uint8_t *buf, int len)
{
    int regsz = bsc_readl(qts, bsc, BSC_CTLHI_REG) & BSC_CTLHI_DATAREG_SIZE ?
                4 : 1;
    int chunk = BSC_NUM_DATA_REGS * regsz;

    bsc_command_ok(qts, bsc, EDID_ADDR, false, &offset, 1,
                   BSC_IIC_EN_RESTART | BSC_IIC_EN_NOSTOP);
    for (int done = 0; done < len; done += chunk) {
        int n = MIN(len - done, chunk);
        uint32_t flags = done ? BSC_IIC_EN_NOSTART : 0;

        if (done + n < len) {
            flags |= BSC_IIC_EN_NOSTOP;
        }
        bsc_command_ok(qts, bsc, EDID_ADDR, true, buf + done, n, flags);
    }
}

/* The fixed header, and a checksum that makes the block sum to zero */
static void check_edid(const uint8_t *edid)
{
    static const uint8_t header[] = {
        0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00,
    };
    uint8_t sum = 0;

    g_assert_cmpmem(edid, sizeof(header), header, sizeof(header));
    for (int i = 0; i < EDID_SIZE; i++) {
        sum += edid[i];
    }
    g_assert_cmpuint(sum, ==, 0);
}

/* TODO(WS0.4): every register reads as zero until checked on hardware */
static void bsc_check_reset(QTestState *qts)
{
    for (int i = 0; i < ARRAY_SIZE(bscs); i++) {
        for (uint32_t reg = 0; reg < BSC_SIZE; reg += 4) {
            g_assert_cmphex(bsc_readl(qts, &bscs[i], reg), ==, 0);
        }
    }
    g_assert_cmphex(l2_readl(qts, BSC_IRQ, L2_LEVEL_STATUS), ==, 0);
}

static void test_bsc_reset_values(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");

    bsc_check_reset(qts);
    qtest_quit(qts);
}

/*
 * The registers keep the fields Linux names; DATA_OUT ignores writes, and
 * SCL_PARAM and the space after the block read as zero
 */
static void test_bsc_registers(void)
{
    static const struct {
        uint32_t reg, mask;
    } fields[] = {
        { BSC_CHIP_ADDRESS, 0xff },
        { BSC_CNT_REG, 0x3f },
        { BSC_CTL_REG, 0xf3 },
        { BSC_CTLHI_REG, 0xc3 },
        { BSC_SCL_PARAM, 0 },
    };
    QTestState *qts = qtest_init("-machine raspi5b");

    for (int i = 0; i < ARRAY_SIZE(bscs); i++) {
        const Bsc *bsc = &bscs[i];

        for (int f = 0; f < ARRAY_SIZE(fields); f++) {
            bsc_writel(qts, bsc, fields[f].reg, UINT32_MAX);
            g_assert_cmphex(bsc_readl(qts, bsc, fields[f].reg), ==,
                            fields[f].mask);
        }
        for (int r = 0; r < BSC_NUM_DATA_REGS; r++) {
            bsc_writel(qts, bsc, BSC_DATA_IN(r), 0x01020304u * (r + 1));
            bsc_writel(qts, bsc, BSC_DATA_OUT(r), UINT32_MAX);
        }
        for (int r = 0; r < BSC_NUM_DATA_REGS; r++) {
            g_assert_cmphex(bsc_readl(qts, bsc, BSC_DATA_IN(r)), ==,
                            0x01020304u * (r + 1));
            g_assert_cmphex(bsc_readl(qts, bsc, BSC_DATA_OUT(r)), ==, 0);
        }
        /* The flags without ENABLE start nothing */
        bsc_writel(qts, bsc, BSC_IIC_ENABLE, UINT32_MAX & ~BSC_IIC_EN_ENABLE);
        g_assert_cmphex(bsc_readl(qts, bsc, BSC_IIC_ENABLE), ==,
                        BSC_IIC_EN_NOSTOP | BSC_IIC_EN_NOSTART |
                        BSC_IIC_EN_RESTART);
        bsc_writel(qts, bsc, BSC_SIZE, UINT32_MAX);
        g_assert_cmphex(bsc_readl(qts, bsc, BSC_SIZE), ==, 0);
    }
    g_assert_cmphex(l2_readl(qts, BSC_IRQ, L2_LEVEL_STATUS), ==, 0);

    qtest_quit(qts);
}

/*
 * The EDID of HDMI0's monitor, read as Linux reads it, 32 bytes at a
 * time; offsets wrap at the end of the block
 */
static void test_bsc_edid(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");
    const Bsc *bsc = &bscs[0];
    uint8_t edid[EDID_SIZE], part[40];

    bsc_writel(qts, bsc, BSC_CTLHI_REG, BSC_CTLHI_DATAREG_SIZE);
    bsc_edid_read(qts, bsc, 0, edid, sizeof(edid));
    check_edid(edid);

    bsc_edid_read(qts, bsc, 0x7c, part, sizeof(part));
    g_assert_cmpmem(part, 4, edid + 0x7c, 4);
    g_assert_cmpmem(part + 4, sizeof(part) - 4, edid, sizeof(part) - 4);

    /* Only setting ENABLE starts a transfer: writing it again does not */
    bsc_command(qts, bsc, EDID_ADDR, true, part, 4, 0);
    bsc_writel(qts, bsc, BSC_IIC_ENABLE, BSC_IIC_EN_ENABLE);
    g_assert_cmphex(bsc_readl(qts, bsc, BSC_IIC_ENABLE), ==,
                    BSC_IIC_EN_ENABLE | BSC_IIC_EN_INTRP);
    g_assert_cmphex(bsc_readl(qts, bsc, BSC_DATA_OUT(0)), ==,
                    (uint32_t)ldl_le_p(edid + 0x24));
    bsc_end(qts, bsc);

    /* Software cannot set the status bits */
    g_assert_cmphex(bsc_command(qts, bsc, EDID_ADDR, true, part, 4,
                                BSC_IIC_EN_NOACK), ==,
                    BSC_IIC_EN_ENABLE | BSC_IIC_EN_INTRP);
    bsc_end(qts, bsc);

    qtest_quit(qts);
}

/* With 1-byte data registers, a transfer moves up to 8 bytes */
static void test_bsc_byte_registers(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");
    const Bsc *bsc = &bscs[0];
    uint8_t edid[EDID_SIZE], part[8];

    bsc_writel(qts, bsc, BSC_CTLHI_REG, BSC_CTLHI_DATAREG_SIZE);
    bsc_edid_read(qts, bsc, 0, edid, sizeof(edid));

    bsc_writel(qts, bsc, BSC_CTLHI_REG, 0);
    bsc_edid_read(qts, bsc, 0x10, part, sizeof(part));
    g_assert_cmpmem(part, sizeof(part), edid + 0x10, sizeof(part));
    for (int r = 0; r < BSC_NUM_DATA_REGS; r++) {
        g_assert_cmphex(bsc_readl(qts, bsc, BSC_DATA_OUT(r)), ==,
                        edid[0x10 + r]);
    }

    /* The count is bits 3:0: 0x10 reads nothing, but clears DATA_OUT */
    bsc_command_ok(qts, bsc, EDID_ADDR, true, part, 0x10, 0);
    for (int r = 0; r < BSC_NUM_DATA_REGS; r++) {
        g_assert_cmphex(bsc_readl(qts, bsc, BSC_DATA_OUT(r)), ==, 0);
    }

    /* A count beyond the 8 registers reads 8 bytes */
    part[0] = 0x20;
    bsc_command_ok(qts, bsc, EDID_ADDR, false, part, 1, BSC_IIC_EN_NOSTOP);
    bsc_writel(qts, bsc, BSC_CNT_REG, 0xf);
    bsc_writel(qts, bsc, BSC_CHIP_ADDRESS, EDID_ADDR << 1 | 1);
    bsc_writel(qts, bsc, BSC_CTL_REG, BSC_CTL_DTF_READ);
    bsc_writel(qts, bsc, BSC_IIC_ENABLE, BSC_IIC_EN_ENABLE);
    g_assert_cmphex(bsc_readl(qts, bsc, BSC_IIC_ENABLE), ==,
                    BSC_IIC_EN_ENABLE | BSC_IIC_EN_INTRP);
    for (int r = 0; r < BSC_NUM_DATA_REGS; r++) {
        g_assert_cmphex(bsc_readl(qts, bsc, BSC_DATA_OUT(r)), ==,
                        edid[0x20 + r]);
    }
    bsc_end(qts, bsc);

    qtest_quit(qts);
}

/*
 * Nothing answers at 0x51 on HDMI0, nor on HDMI1's bus at all: NOACK,
 * until ENABLE is cleared. With IGNORE_ACK the transfer goes on, and a
 * read sees the bus's pull-ups.
 */
static void test_bsc_nack(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");
    uint8_t buf[4] = { 0x12, 0x34, 0x56, 0x78 };
    const uint32_t nack = BSC_IIC_EN_ENABLE | BSC_IIC_EN_INTRP |
                          BSC_IIC_EN_NOACK;

    bsc_writel(qts, &bscs[0], BSC_CTLHI_REG, BSC_CTLHI_DATAREG_SIZE);
    g_assert_cmphex(bsc_command(qts, &bscs[0], EDID_ADDR + 1, true, buf,
                                sizeof(buf), 0), ==, nack);
    bsc_end(qts, &bscs[0]);
    g_assert_cmphex(bsc_readl(qts, &bscs[0], BSC_IIC_ENABLE), ==, 0);
    g_assert_cmphex(bsc_command(qts, &bscs[1], EDID_ADDR, false, buf,
                                sizeof(buf), 0), ==, nack);
    bsc_end(qts, &bscs[1]);

    bsc_writel(qts, &bscs[1], BSC_CTLHI_REG,
               BSC_CTLHI_DATAREG_SIZE | BSC_CTLHI_IGNORE_ACK);
    g_assert_cmphex(bsc_command(qts, &bscs[1], EDID_ADDR, true, buf,
                                sizeof(buf), 0), ==, nack);
    g_assert_cmphex(bsc_readl(qts, &bscs[1], BSC_DATA_OUT(0)), ==,
                    UINT32_MAX);
    bsc_end(qts, &bscs[1]);

    /* A byte with no transfer to go on with finds no target either */
    bsc_writel(qts, &bscs[0], BSC_CTLHI_REG, BSC_CTLHI_DATAREG_SIZE);
    g_assert_cmphex(bsc_command(qts, &bscs[0], EDID_ADDR, false, buf, 1,
                                BSC_IIC_EN_NOSTART), ==,
                    nack | BSC_IIC_EN_NOSTART);
    bsc_end(qts, &bscs[0]);

    /* The combined formats are not modelled: they fail at once */
    bsc_writel(qts, &bscs[0], BSC_CNT_REG, 1);
    bsc_writel(qts, &bscs[0], BSC_CHIP_ADDRESS, EDID_ADDR << 1);
    bsc_writel(qts, &bscs[0], BSC_CTL_REG, BSC_CTL_DTF_WR_RD);
    bsc_writel(qts, &bscs[0], BSC_IIC_ENABLE, BSC_IIC_EN_ENABLE);
    g_assert_cmphex(bsc_readl(qts, &bscs[0], BSC_IIC_ENABLE), ==, nack);
    bsc_end(qts, &bscs[0]);

    /* HDMI0's bus still works after all that */
    bsc_writel(qts, &bscs[0], BSC_CTL_REG, 0);
    bsc_edid_read(qts, &bscs[0], 0, buf, sizeof(buf));
    g_assert_cmphex(ldl_be_p(buf), ==, 0x00ffffff);

    qtest_quit(qts);
}

/*
 * The interrupt is INTRP && INT_EN: raised when a command completes,
 * lowered by the handler clearing INT_EN, as Linux's does, or by the end
 * of the command clearing INTRP
 */
static void test_bsc_interrupt(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");
    uint8_t offset = 0;

    l2_writel(qts, BSC_IRQ, L2_LEVEL_MASK_CLEAR, BIT(1) | BIT(2));
    for (int i = 0; i < ARRAY_SIZE(bscs); i++) {
        const Bsc *bsc = &bscs[i];

        bsc_command(qts, bsc, EDID_ADDR, false, &offset, 1, 0);
        g_assert_cmphex(l2_readl(qts, BSC_IRQ, L2_LEVEL_STATUS), ==,
                        BIT(bsc->irq));
        g_assert_true(gic_spi_pending(qts, BSC_IRQ->spi));
        bsc_writel(qts, bsc, BSC_CTL_REG, 0);
        g_assert_cmphex(l2_readl(qts, BSC_IRQ, L2_LEVEL_STATUS), ==, 0);
        bsc_writel(qts, bsc, BSC_CTL_REG, BSC_CTL_INT_EN);
        g_assert_cmphex(l2_readl(qts, BSC_IRQ, L2_LEVEL_STATUS), ==,
                        BIT(bsc->irq));
        bsc_writel(qts, bsc, BSC_IIC_ENABLE, 0);
        g_assert_cmphex(bsc_readl(qts, bsc, BSC_IIC_ENABLE), ==, 0);
        g_assert_cmphex(l2_readl(qts, BSC_IRQ, L2_LEVEL_STATUS), ==, 0);
        bsc_end(qts, bsc);
    }

    /* Without INT_EN a command completes all the same, for polling */
    bsc_writel(qts, &bscs[0], BSC_CNT_REG, 1);
    bsc_writel(qts, &bscs[0], BSC_CHIP_ADDRESS, EDID_ADDR << 1);
    bsc_writel(qts, &bscs[0], BSC_IIC_ENABLE, BSC_IIC_EN_ENABLE);
    g_assert_cmphex(bsc_readl(qts, &bscs[0], BSC_IIC_ENABLE), ==,
                    BSC_IIC_EN_ENABLE | BSC_IIC_EN_INTRP);
    g_assert_cmphex(l2_readl(qts, BSC_IRQ, L2_LEVEL_STATUS), ==, 0);

    qtest_quit(qts);
}

/* A reset clears the registers and frees the bus a command held */
static void test_bsc_reset(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");
    const Bsc *bsc = &bscs[0];
    uint8_t offset = 0, buf[4];

    l2_writel(qts, BSC_IRQ, L2_LEVEL_MASK_CLEAR, BIT(1));
    bsc_writel(qts, bsc, BSC_CTLHI_REG, BSC_CTLHI_DATAREG_SIZE);
    bsc_command(qts, bsc, EDID_ADDR, false, &offset, 1, BSC_IIC_EN_NOSTOP);
    g_assert_true(gic_spi_pending(qts, BSC_IRQ->spi));
    qtest_system_reset(qts);
    l2_writel(qts, BSC_IRQ, L2_LEVEL_MASK_CLEAR, BIT(1));
    bsc_check_reset(qts);
    g_assert_false(gic_spi_pending(qts, BSC_IRQ->spi));

    /* Going on without a start finds no target */
    bsc_writel(qts, bsc, BSC_CTLHI_REG, BSC_CTLHI_DATAREG_SIZE);
    bsc_command_ok(qts, bsc, EDID_ADDR, true, buf, sizeof(buf),
                   BSC_IIC_EN_NOSTART);
    g_assert_cmphex((uint32_t)ldl_le_p(buf), ==, UINT32_MAX);

    qtest_quit(qts);
}

/*
 * The registers survive a migration, and so does a read that holds the
 * bus: it goes on where it stopped
 */
static void test_bsc_migrate(void)
{
    g_autofree char *file = g_strdup_printf("%s/raspi5b-bsc-%d.mig",
                                            g_get_tmp_dir(), getpid());
    g_autofree char *out = g_strdup_printf("exec:cat > %s", file);
    g_autofree char *in = g_strdup_printf("exec:cat %s", file);
    const char *args = "-machine raspi5b -m 1G";
    const Bsc *bsc = &bscs[0];
    uint8_t offset = 0x20, edid[EDID_SIZE], part[8];
    QTestState *src, *dst;

    src = qtest_init(args);
    l2_writel(src, BSC_IRQ, L2_LEVEL_MASK_CLEAR, BIT(1));
    bsc_writel(src, bsc, BSC_CTLHI_REG, BSC_CTLHI_DATAREG_SIZE);
    bsc_edid_read(src, bsc, 0, edid, sizeof(edid));
    bsc_command_ok(src, bsc, EDID_ADDR, false, &offset, 1,
                   BSC_IIC_EN_RESTART | BSC_IIC_EN_NOSTOP);
    bsc_command(src, bsc, EDID_ADDR, true, part, sizeof(part),
                BSC_IIC_EN_NOSTOP);
    g_assert_cmpmem(part, sizeof(part), edid + offset, sizeof(part));
    qtest_qmp_assert_success(src, "{ 'execute': 'migrate',"
                             "  'arguments': { 'uri': %s } }", out);
    wait_for_migration(src);
    qtest_quit(src);

    dst = qtest_initf("%s -incoming defer", args);
    qtest_qmp_assert_success(dst, "{ 'execute': 'migrate-incoming',"
                             "  'arguments': { 'uri': %s } }", in);
    wait_for_migration(dst);
    g_assert_cmphex(bsc_readl(dst, bsc, BSC_CHIP_ADDRESS), ==,
                    EDID_ADDR << 1 | 1);
    g_assert_cmphex(bsc_readl(dst, bsc, BSC_DATA_IN(0)), ==, offset);
    g_assert_cmphex(bsc_readl(dst, bsc, BSC_CNT_REG), ==, sizeof(part));
    g_assert_cmphex(bsc_readl(dst, bsc, BSC_CTL_REG), ==,
                    BSC_CTL_DTF_READ | BSC_CTL_INT_EN);
    g_assert_cmphex(bsc_readl(dst, bsc, BSC_IIC_ENABLE), ==,
                    BSC_IIC_EN_NOSTOP | BSC_IIC_EN_ENABLE | BSC_IIC_EN_INTRP);
    g_assert_cmphex(bsc_readl(dst, bsc, BSC_CTLHI_REG), ==,
                    BSC_CTLHI_DATAREG_SIZE);
    for (int r = 0; r < BSC_NUM_DATA_REGS; r++) {
        g_assert_cmphex(bsc_readl(dst, bsc, BSC_DATA_OUT(r)), ==,
                        r < 2 ? (uint32_t)ldl_le_p(edid + offset + 4 * r) : 0);
    }
    g_assert_true(gic_spi_pending(dst, BSC_IRQ->spi));

    bsc_end(dst, bsc);
    bsc_command_ok(dst, bsc, EDID_ADDR, true, part, sizeof(part),
                   BSC_IIC_EN_NOSTART);
    g_assert_cmpmem(part, sizeof(part), edid + offset + sizeof(part),
                    sizeof(part));

    qtest_quit(dst);
    unlink(file);
}

/*
 * The board: system_powerdown presses the power button, pulling GIO 20
 * low for 200 ms. Here with both edges enabled, as Linux gpio-keys has it.
 */
static QTestState *power_button_init(const char *args)
{
    QTestState *qts = qtest_init(args);

    g_assert_cmphex(gio_readl(qts, GIO, 0, GIO_DATA) & BIT(PWR_BUTTON_GIO),
                    ==, BIT(PWR_BUTTON_GIO));
    gio_unmask_main_irq(qts);
    gio_writel(qts, GIO, 0, GIO_EI, BIT(PWR_BUTTON_GIO));
    gio_writel(qts, GIO, 0, GIO_MASK, BIT(PWR_BUTTON_GIO));
    return qts;
}

static void power_button_press(QTestState *qts)
{
    qtest_qmp_assert_success(qts, "{ 'execute': 'system_powerdown' }");
    qtest_qmp_eventwait(qts, "POWERDOWN");
}

static bool power_button_pressed(QTestState *qts)
{
    return !(gio_readl(qts, GIO, 0, GIO_DATA) & BIT(PWR_BUTTON_GIO));
}

/* Takes the interrupt the last edge raised */
static void power_button_edge(QTestState *qts)
{
    g_assert_cmphex(gio_readl(qts, GIO, 0, GIO_STAT), ==,
                    BIT(PWR_BUTTON_GIO));
    g_assert_true(gio_irq(qts));
    gio_writel(qts, GIO, 0, GIO_STAT, BIT(PWR_BUTTON_GIO));
    g_assert_false(gio_irq(qts));
}

static void test_power_button(void)
{
    QTestState *qts = power_button_init("-machine raspi5b");

    power_button_press(qts);
    g_assert_true(power_button_pressed(qts));
    power_button_edge(qts);
    qtest_clock_step(qts, PWR_BUTTON_PRESS_NS - 1);
    g_assert_true(power_button_pressed(qts));
    g_assert_cmphex(gio_readl(qts, GIO, 0, GIO_STAT), ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_false(power_button_pressed(qts));
    power_button_edge(qts);

    /* A second request during a press holds the button down longer */
    power_button_press(qts);
    qtest_clock_step(qts, PWR_BUTTON_PRESS_NS / 2);
    power_button_press(qts);
    qtest_clock_step(qts, PWR_BUTTON_PRESS_NS - 1);
    g_assert_true(power_button_pressed(qts));
    qtest_clock_step(qts, 1);
    g_assert_false(power_button_pressed(qts));

    qtest_quit(qts);
}

/* A reset during a press leaves the button down until it is released */
static void test_power_button_reset(void)
{
    QTestState *qts = power_button_init("-machine raspi5b");

    power_button_press(qts);
    qtest_clock_step(qts, PWR_BUTTON_PRESS_NS / 4);
    qtest_system_reset(qts);
    gio_check_reset(qts);
    g_assert_true(power_button_pressed(qts));
    qtest_clock_step(qts, PWR_BUTTON_PRESS_NS * 3 / 4 - 1);
    g_assert_true(power_button_pressed(qts));
    qtest_clock_step(qts, 1);
    g_assert_false(power_button_pressed(qts));

    qtest_quit(qts);
}

/* A press in progress carries on on the destination */
static void test_power_button_migrate(void)
{
    g_autofree char *file = g_strdup_printf("%s/raspi5b-pwr-%d.mig",
                                            g_get_tmp_dir(), getpid());
    g_autofree char *out = g_strdup_printf("exec:cat > %s", file);
    g_autofree char *in = g_strdup_printf("exec:cat %s", file);
    const char *args = "-machine raspi5b -m 1G";
    QTestState *src, *dst;
    int64_t now;

    src = power_button_init(args);
    power_button_press(src);
    qtest_clock_step(src, PWR_BUTTON_PRESS_NS / 4);
    now = PWR_BUTTON_PRESS_NS / 4;
    qtest_qmp_assert_success(src, "{ 'execute': 'migrate',"
                             "  'arguments': { 'uri': %s } }", out);
    wait_for_migration(src);
    qtest_quit(src);

    dst = qtest_initf("%s -incoming defer", args);
    qtest_qmp_assert_success(dst, "{ 'execute': 'migrate-incoming',"
                             "  'arguments': { 'uri': %s } }", in);
    wait_for_migration(dst);

    /* The qtest clock is not migrated: carry it over by hand */
    qtest_clock_set(dst, now);
    g_assert_true(power_button_pressed(dst));
    power_button_edge(dst);
    qtest_clock_step(dst, PWR_BUTTON_PRESS_NS * 3 / 4 - 1);
    g_assert_true(power_button_pressed(dst));
    qtest_clock_step(dst, 1);
    g_assert_false(power_button_pressed(dst));
    power_button_edge(dst);

    qtest_quit(dst);
    unlink(file);
}

/* The ACT LED follows AON GPIO 9, which the board pulls up */
static void test_act_led(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");
    const Gio *gio = GIO_AON;

    qtest_irq_intercept_in(qts, "/machine/act");

    /* An output, as Linux gpio-leds sets it up: dark, then lit, dark */
    gio_writel(qts, gio, 0, GIO_DATA, BIT(ACT_LED_AON_GPIO));
    gio_writel(qts, gio, 0, GIO_IODIR,
               gio_valid(gio, 0) & ~BIT(ACT_LED_AON_GPIO));
    gio_writel(qts, gio, 0, GIO_DATA, 0);
    gio_writel(qts, gio, 0, GIO_DATA, BIT(ACT_LED_AON_GPIO));
    g_assert_true(qtest_get_irq(qts, 0));
    gio_writel(qts, gio, 0, GIO_DATA, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    /* Released, the line goes back up */
    gio_writel(qts, gio, 0, GIO_IODIR, gio_valid(gio, 0));
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmphex(gio_readl(qts, gio, 0, GIO_DATA), ==,
                    BIT(ACT_LED_AON_GPIO));

    qtest_quit(qts);
}

static void test_unimplemented_regions(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");

    /* Named placeholder and catch-all window both read as zero */
    g_assert_cmphex(qtest_readl(qts, HVS_BASE), ==, 0);
    g_assert_cmphex(qtest_readl(qts, SOC_WINDOW_BASE), ==, 0);

    qtest_quit(qts);
}

/*
 * -bios loads the armstub at 0 and, when it carries the firmware's header
 * (TF-A's plat/rpi/common/aarch64/armstub8_header.S), clears its magic
 * and fills in where the device tree and the kernel are
 */
#define ARMSTUB_MAGIC           0x5afe570b
#define ARMSTUB_MAGIC_OFFSET    0xf0
#define ARMSTUB_DTB_OFFSET      0xf8
#define ARMSTUB_KERNEL_OFFSET   0xfc
#define ARMSTUB_WORDS           64

/* The firmware's kernel_address for 64-bit kernels */
#define BIOS_KERNEL_ADDR        0x200000
/* The first address after the kernel for the device tree, as for -kernel */
#define BIOS_DTB_ADDR           (128 * MiB)

/* The arm64 Linux Image header (Documentation/arch/arm64/booting.rst) */
#define IMAGE_TEXT_OFFSET       8
#define IMAGE_SIZE              16
#define IMAGE_MAGIC             56

/* An armstub whose every word holds its own offset, and maybe the magic */
static char *armstub_file(bool header)
{
    uint32_t stub[ARMSTUB_WORDS];

    for (int i = 0; i < ARMSTUB_WORDS; i++) {
        stub[i] = cpu_to_le32(0xa5000000 | i * 4);
    }
    if (header) {
        stub[ARMSTUB_MAGIC_OFFSET / 4] = cpu_to_le32(ARMSTUB_MAGIC);
    }
    return tmp_file("raspi5b-armstub-XXXXXX", stub, sizeof(stub));
}

static void assert_fdt_at(QTestState *qts, uint64_t addr)
{
    uint8_t magic[4];

    qtest_memread(qts, addr, magic, sizeof(magic));
    g_assert_cmpmem(magic, sizeof(magic), "\xd0\x0d\xfe\xed", 4);
}

static void test_bios_header(void)
{
    g_autofree char *stub = armstub_file(true);
    QTestState *qts;

    qts = qtest_initf("-machine raspi5b,secure=on -bios %s", stub);
    g_assert_cmphex(qtest_readl(qts, 0), ==, 0xa5000000);
    g_assert_cmphex(qtest_readl(qts, ARMSTUB_MAGIC_OFFSET - 4), ==,
                    0xa5000000 | (ARMSTUB_MAGIC_OFFSET - 4));
    g_assert_cmphex(qtest_readl(qts, ARMSTUB_MAGIC_OFFSET), ==, 0);
    g_assert_cmphex(qtest_readl(qts, ARMSTUB_DTB_OFFSET), ==, BIOS_DTB_ADDR);
    g_assert_cmphex(qtest_readl(qts, ARMSTUB_KERNEL_OFFSET), ==,
                    BIOS_KERNEL_ADDR);
    assert_fdt_at(qts, BIOS_DTB_ADDR);
    qtest_quit(qts);

    /* device_tree_address= */
    qts = qtest_initf("-machine raspi5b,secure=on,dtb-address=0x1f0000 "
                      "-bios %s", stub);
    g_assert_cmphex(qtest_readl(qts, ARMSTUB_DTB_OFFSET), ==, 0x1f0000);
    assert_fdt_at(qts, 0x1f0000);
    qtest_quit(qts);

    /* Right after the armstub, as EDK2 has it */
    qts = qtest_initf("-machine raspi5b,secure=on,dtb-address=0x%x "
                      "-bios %s", ARMSTUB_WORDS * 4, stub);
    g_assert_cmphex(qtest_readl(qts, ARMSTUB_DTB_OFFSET), ==,
                    ARMSTUB_WORDS * 4);
    g_assert_cmphex(qtest_readl(qts, ARMSTUB_DTB_OFFSET - 4), ==,
                    0xa5000000 | (ARMSTUB_DTB_OFFSET - 4));
    assert_fdt_at(qts, ARMSTUB_WORDS * 4);
    qtest_quit(qts);

    /* No device tree at all */
    qts = qtest_initf("-machine raspi5b,secure=on,builtin-dtb=off -bios %s",
                      stub);
    g_assert_cmphex(qtest_readl(qts, ARMSTUB_DTB_OFFSET), ==, 0);
    g_assert_cmphex(qtest_readl(qts, BIOS_DTB_ADDR), ==, 0);
    qtest_quit(qts);

    unlink(stub);
}

/* An image without the header is loaded as it is */
static void test_bios_no_header(void)
{
    g_autofree char *stub = armstub_file(false);
    QTestState *qts = qtest_initf("-machine raspi5b,secure=on -bios %s",
                                  stub);

    for (int i = 0; i < ARMSTUB_WORDS; i++) {
        g_assert_cmphex(qtest_readl(qts, i * 4), ==, 0xa5000000 | i * 4);
    }

    qtest_quit(qts);
    unlink(stub);
}

/*
 * A Linux Image goes at the kernel address plus its text_offset, and the
 * initrd and device tree above all the memory it declares, BSS included
 */
static void test_bios_kernel(void)
{
    const uint64_t text_offset = 0x80000, image_size = 144 * MiB;
    const uint64_t kernel = BIOS_KERNEL_ADDR + text_offset;
    const uint64_t initrd = kernel + image_size;
    g_autofree char *stub = armstub_file(true);
    g_autofree char *kernel_file = NULL, *initrd_file = NULL;
    uint8_t image[4 * KiB] = { 0 }, ramdisk[4 * KiB];
    QTestState *qts;

    stq_le_p(image + IMAGE_TEXT_OFFSET, text_offset);
    stq_le_p(image + IMAGE_SIZE, image_size);
    memcpy(image + IMAGE_MAGIC, "ARM\x64", 4);
    memset(ramdisk, 0x5a, sizeof(ramdisk));
    kernel_file = tmp_file("raspi5b-image-XXXXXX", image, sizeof(image));
    initrd_file = tmp_file("raspi5b-initrd-XXXXXX", ramdisk, sizeof(ramdisk));

    qts = qtest_initf("-machine raspi5b,secure=on -bios %s -kernel %s "
                      "-initrd %s", stub, kernel_file, initrd_file);
    g_assert_cmphex(qtest_readl(qts, ARMSTUB_KERNEL_OFFSET), ==, kernel);
    g_assert_cmphex(qtest_readl(qts, kernel + IMAGE_MAGIC), ==,
                    ldl_le_p("ARM\x64"));
    g_assert_cmphex(qtest_readl(qts, initrd), ==, 0x5a5a5a5a);
    g_assert_cmphex(qtest_readl(qts, ARMSTUB_DTB_OFFSET), ==,
                    ROUND_UP(initrd + sizeof(ramdisk), 2 * MiB));
    assert_fdt_at(qts, ROUND_UP(initrd + sizeof(ramdisk), 2 * MiB));
    qtest_quit(qts);

    unlink(stub);
    unlink(kernel_file);
    unlink(initrd_file);
}

/*
 * A kernel from before Linux 3.17 has no image_size in its header, and a
 * text_offset of 0x80000 in its own byte order: a big-endian one's is
 * taken as that too
 */
static void test_bios_old_kernel(void)
{
    const uint64_t kernel = BIOS_KERNEL_ADDR + 0x80000;
    g_autofree char *stub = armstub_file(true);
    g_autofree char *kernel_file = NULL;
    uint8_t image[4 * KiB] = { 0 };
    QTestState *qts;

    stq_be_p(image + IMAGE_TEXT_OFFSET, 0x80000);
    memcpy(image + IMAGE_MAGIC, "ARM\x64", 4);
    kernel_file = tmp_file("raspi5b-image-XXXXXX", image, sizeof(image));

    qts = qtest_initf("-machine raspi5b,secure=on -bios %s -kernel %s",
                      stub, kernel_file);
    g_assert_cmphex(qtest_readl(qts, ARMSTUB_KERNEL_OFFSET), ==, kernel);
    g_assert_cmphex(qtest_readl(qts, kernel + IMAGE_MAGIC), ==,
                    ldl_le_p("ARM\x64"));
    qtest_quit(qts);

    unlink(stub);
    unlink(kernel_file);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/raspi5b/uart10/ids", test_uart10_ids);
    qtest_add_func("/raspi5b/gic/default", test_gic_default);
    qtest_add_func("/raspi5b/gic/priority-bits", test_gic_priority_bits);
    qtest_add_func("/raspi5b/gic/smp2-secure", test_gic_smp2_secure);
    qtest_add_func("/raspi5b/ram", test_ram);
    qtest_add_func("/raspi5b/unimplemented", test_unimplemented_regions);
    qtest_add_func("/raspi5b/systimer/counter", test_systimer_counter);
    qtest_add_func("/raspi5b/systimer/compare", test_systimer_compare);
    qtest_add_func("/raspi5b/systimer/reset", test_systimer_reset);
    qtest_add_func("/raspi5b/systimer/migrate", test_systimer_migrate);
    qtest_add_func("/raspi5b/mbox/board-revision", test_mbox_board_revision);
    qtest_add_func("/raspi5b/mbox/unreachable", test_mbox_unreachable);
    qtest_add_func("/raspi5b/mbox/malformed", test_mbox_malformed);
    qtest_add_func("/raspi5b/mbox/memory-split", test_mbox_memory_split);
    qtest_add_func("/raspi5b/mbox/identity", test_mbox_identity);
    qtest_add_func("/raspi5b/mbox/short-buffer", test_mbox_short_buffer);
    qtest_add_func("/raspi5b/mbox/command-line", test_mbox_command_line);
    qtest_add_func("/raspi5b/rng/stopped", test_rng_stopped);
    qtest_add_func("/raspi5b/rng/start", test_rng_start);
    qtest_add_func("/raspi5b/rng/soft-reset", test_rng_soft_reset);
    qtest_add_func("/raspi5b/rng/reset", test_rng_reset);
    qtest_add_func("/raspi5b/rng/seed", test_rng_seed);
    qtest_add_func("/raspi5b/l2-intc/reset-values",
                   test_l2_intc_reset_values);
    qtest_add_func("/raspi5b/l2-intc/outputs", test_l2_intc_outputs);
    qtest_add_func("/raspi5b/l2-intc/mask", test_l2_intc_mask);
    qtest_add_func("/raspi5b/l2-intc/level", test_l2_intc_level);
    qtest_add_func("/raspi5b/l2-intc/edge", test_l2_intc_edge);
    qtest_add_func("/raspi5b/l2-intc/software-set",
                   test_l2_intc_software_set);
    qtest_add_func("/raspi5b/l2-intc/reset", test_l2_intc_reset);
    qtest_add_func("/raspi5b/l2-intc/migrate", test_l2_intc_migrate);
    qtest_add_func("/raspi5b/gpio/reset-values", test_gio_reset_values);
    qtest_add_func("/raspi5b/gpio/widths", test_gio_widths);
    qtest_add_func("/raspi5b/gpio/data", test_gio_data);
    qtest_add_func("/raspi5b/gpio/edges", test_gio_edges);
    qtest_add_func("/raspi5b/gpio/levels", test_gio_levels);
    qtest_add_func("/raspi5b/gpio/aon", test_gio_aon);
    qtest_add_func("/raspi5b/gpio/reset", test_gio_reset);
    qtest_add_func("/raspi5b/gpio/migrate", test_gio_migrate);
    qtest_add_func("/raspi5b/pinctrl/reset-values",
                   test_pinctrl_reset_values);
    qtest_add_func("/raspi5b/pinctrl/read-back", test_pinctrl_read_back);
    qtest_add_func("/raspi5b/pinctrl/reset", test_pinctrl_reset);
    qtest_add_func("/raspi5b/pinctrl/migrate", test_pinctrl_migrate);
    qtest_add_func("/raspi5b/bsc/reset-values", test_bsc_reset_values);
    qtest_add_func("/raspi5b/bsc/registers", test_bsc_registers);
    qtest_add_func("/raspi5b/bsc/edid", test_bsc_edid);
    qtest_add_func("/raspi5b/bsc/byte-registers", test_bsc_byte_registers);
    qtest_add_func("/raspi5b/bsc/nack", test_bsc_nack);
    qtest_add_func("/raspi5b/bsc/interrupt", test_bsc_interrupt);
    qtest_add_func("/raspi5b/bsc/reset", test_bsc_reset);
    qtest_add_func("/raspi5b/bsc/migrate", test_bsc_migrate);
    qtest_add_func("/raspi5b/board/power-button", test_power_button);
    qtest_add_func("/raspi5b/board/power-button-reset",
                   test_power_button_reset);
    qtest_add_func("/raspi5b/board/power-button-migrate",
                   test_power_button_migrate);
    qtest_add_func("/raspi5b/board/act-led", test_act_led);
    qtest_add_func("/raspi5b/pm/registers", test_pm_registers);
    qtest_add_func("/raspi5b/pm/watchdog-countdown",
                   test_pm_watchdog_countdown);
    qtest_add_func("/raspi5b/pm/watchdog-kick", test_pm_watchdog_kick);
    qtest_add_func("/raspi5b/pm/watchdog-reset", test_pm_watchdog_reset);
    qtest_add_func("/raspi5b/pm/halt", test_pm_halt);
    qtest_add_func("/raspi5b/pm/halt-exit", test_pm_halt_exit);
    qtest_add_func("/raspi5b/pm/migrate", test_pm_migrate);
    qtest_add_func("/raspi5b/bios/header", test_bios_header);
    qtest_add_func("/raspi5b/bios/no-header", test_bios_no_header);
    qtest_add_func("/raspi5b/bios/kernel", test_bios_kernel);
    qtest_add_func("/raspi5b/bios/old-kernel", test_bios_old_kernel);

    return g_test_run();
}
