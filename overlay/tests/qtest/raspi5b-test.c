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
#define SOC_WINDOW_BASE         0x107c000000ULL

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

static void test_unimplemented_regions(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");

    /* Named placeholder and catch-all window both read as zero */
    g_assert_cmphex(qtest_readl(qts, MBOX_BASE), ==, 0);
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

    return g_test_run();
}
