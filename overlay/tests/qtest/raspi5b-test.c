/*
 * QTest testcase for the Raspberry Pi 5 (raspi5b) machine
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
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
#define FW_TAG_FIRMWARE_VARIANT 0x00000002
#define FW_TAG_FIRMWARE_HASH    0x00000003
#define FW_TAG_BOARD_MODEL      0x00010001
#define FW_TAG_BOARD_REVISION   0x00010002
#define FW_TAG_BOARD_SERIAL     0x00010004
#define FW_TAG_ARM_MEMORY       0x00010005
#define FW_TAG_VC_MEMORY        0x00010006
#define FW_TAG_DMA_CHANNELS     0x00060001
#define FW_TAG_COMMAND_LINE     0x00050001
#define FW_TAG_RESPONSE         BIT(31)

/* Where the VideoCore sees the first GiB of RAM (dma-ranges of "soc") */
#define VC_BUS_RAM              0xc0000000u

/* Linux drivers/watchdog/bcm2835_wdt.c */
#define PM_RSTC                 0x1c
#define PM_RSTS                 0x20
#define PM_WDOG                 0x24
#define PM_PASSWORD             0x5a000000
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
 * GiB of RAM: at bus address 0xc000_0000, as Linux passes them, and at 0
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
    uint32_t val[2];

    mbox_request(qts, high, high, FW_TAG_BOARD_REVISION);
    g_assert_false(mbox_has_response(qts));
    g_assert_cmphex(qtest_readl(qts, high + 4), ==, FW_REQUEST);

    /* The channel still works afterwards */
    mbox_request(qts, 0x10000, VC_BUS_RAM | 0x10000, FW_TAG_BOARD_REVISION);
    mbox_response(qts, 0x10000, VC_BUS_RAM | 0x10000, val);

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

    g_assert_cmphex(mbox_tag(qts, FW_TAG_BOARD_SERIAL, 8, val), ==,
                    FW_TAG_RESPONSE | 8);
    g_assert_cmphex(val[0], ==, 0x89abcdef);
    g_assert_cmphex(val[1], ==, 0x01234567);

    g_assert_cmphex(mbox_tag(qts, FW_TAG_DMA_CHANNELS, 4, val), ==,
                    FW_TAG_RESPONSE | 4);
    g_assert_cmphex(val[0], ==, 0x7ff);

    qtest_quit(qts);

    qts = qtest_init("-machine raspi5b,serial=0x1122334455667788");
    mbox_tag(qts, FW_TAG_BOARD_SERIAL, 8, val);
    g_assert_cmphex(val[0], ==, 0x55667788);
    g_assert_cmphex(val[1], ==, 0x11223344);
    qtest_quit(qts);
}

/*
 * The firmware returns the command line without a terminator, and when
 * the buffer is too small, only the length it needs.
 */
static void test_mbox_command_line(void)
{
    static const char cmdline[] = "console=ttyAMA10,115200 quiet";
    static const uint32_t wfi_loop[] = { 0xd503207f, 0x17ffffff };
    g_autofree char *kernel = NULL;
    QTestState *qts;
    uint32_t val[12];
    int fd;

    /* -append needs a -kernel; the CPU never runs it under qtest */
    fd = g_file_open_tmp("raspi5b-kernel-XXXXXX", &kernel, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, wfi_loop, sizeof(wfi_loop)), ==,
                    sizeof(wfi_loop));
    close(fd);
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
    qtest_add_func("/raspi5b/mbox/memory-split", test_mbox_memory_split);
    qtest_add_func("/raspi5b/mbox/identity", test_mbox_identity);
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

    return g_test_run();
}
