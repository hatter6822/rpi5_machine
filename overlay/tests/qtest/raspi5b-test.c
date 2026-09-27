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
