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
#define AVS_BASE                0x107d542000ULL

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
#define MBOX_CHAN_FB            1
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
#define FW_TAG_OVERSCAN         0x0004000a
#define FW_TAG_GET_CLOCKS       0x00010007
#define FW_TAG_CLOCK_STATE      0x00030001
#define FW_TAG_SET_CLOCK_STATE  0x00038001
#define FW_TAG_CLOCK_RATE       0x00030002
#define FW_TAG_SET_CLOCK_RATE   0x00038002
#define FW_TAG_MAX_CLOCK_RATE   0x00030004
#define FW_TAG_MIN_CLOCK_RATE   0x00030007
#define FW_TAG_CLOCK_MEASURED   0x00030047
#define FW_TAG_POWER_STATE      0x00020001
#define FW_TAG_SET_POWER_STATE  0x00028001
#define FW_TAG_DOMAIN_STATE     0x00030030
#define FW_TAG_SET_DOMAIN_STATE 0x00038030
#define FW_TAG_TEMPERATURE      0x00030006
#define FW_TAG_MAX_TEMPERATURE  0x0003000a
#define FW_TAG_NOTIFY_REBOOT    0x00030048
#define FW_TAG_REBOOT_FLAGS     0x00030064
#define FW_TAG_SET_REBOOT_FLAGS 0x00038064
#define FW_TAG_RTC_REG          0x00030087      /* Linux's rtc-rpi.c */
#define FW_TAG_SET_RTC_REG      0x00038087
#define FW_TAG_FB_ALLOCATE      0x00040001
#define FW_TAG_FB_PHYSICAL      0x00040003      /* width, height */
#define FW_TAG_FB_SET_PHYSICAL  0x00048003
#define FW_TAG_FB_VIRTUAL       0x00040004
#define FW_TAG_FB_SET_VIRTUAL   0x00048004
#define FW_TAG_FB_DEPTH         0x00040005      /* bits per pixel */
#define FW_TAG_FB_SET_DEPTH     0x00048005
#define FW_TAG_FB_PITCH         0x00040008      /* bytes per line */
#define FW_TAG_FB_DISPLAYS      0x00040013
#define FW_TAG_RESPONSE         BIT(31)

/* The state word of the clock and power device tags */
#define FW_STATE_ON             BIT(0)
#define FW_STATE_WAIT           BIT(1)          /* in a power request */
#define FW_STATE_NO_DEVICE      BIT(1)          /* in an answer */

#define FW_CLK_ARM              3
#define FW_CLK_V3D              5
#define FW_DEV_USB              3               /* of the older interface */
#define FW_DEVICES              9
/* Linux's raspberrypi-power binding numbers the domains from 0 */
#define FW_DOMAIN_ARM           23
#define FW_DOMAINS              23

/* The real-time clock's registers, Linux's rtc-rpi.c */
#define FW_RTC_TIME             0
#define FW_RTC_ALARM            1
#define FW_RTC_ALARM_PENDING    2
#define FW_RTC_ALARM_ENABLE     3
#define FW_RTC_CHARGE           4               /* microvolts */
#define FW_RTC_CHARGE_MIN       5
#define FW_RTC_CHARGE_MAX       6
#define FW_RTC_BATTERY          7

/* A start for the real-time clock, -rtc base= */
#define RTC_BASE                "2026-01-02T03:04:05"
#define RTC_BASE_TIME           1767323045u

/* The alias of the first GiB of RAM that code for older Pis uses */
#define VC_BUS_RAM              0xc0000000u

/* The framebuffer: 1 MiB into the VideoCore memory, which ends at 1 GiB */
#define VC_FB_BASE              0x3fd00000u
#define VC_FB_END               0x40000000u

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
 * Ask for @tag with a @size-byte value buffer that holds the request in
 * @val, and copy the value back into @val. Returns the tag's response
 * code and length.
 */
static uint32_t mbox_call(QTestState *qts, uint32_t tag, uint32_t size,
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
        qtest_writel(qts, buf + 20 + 4 * i, val[i]);
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

/*
 * Ask for @tag with a @size-byte value buffer, filled with a marker so
 * that words the firmware leaves alone stand out, and copy the value
 * back into @val. Returns the tag's response code and length.
 */
static uint32_t mbox_tag(QTestState *qts, uint32_t tag, uint32_t size,
                         uint32_t *val)
{
    for (int i = 0; i < DIV_ROUND_UP(size, 4); i++) {
        val[i] = 0xa5a5a5a5;
    }
    return mbox_call(qts, tag, size, val);
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

    /* A clock rate: the marker is a clock that does not exist, rate 0 */
    g_assert_cmphex(mbox_tag(qts, FW_TAG_CLOCK_RATE, 8, val), ==,
                    FW_TAG_RESPONSE | 8);
    g_assert_cmphex(val[0], ==, 0xa5a5a5a5);
    g_assert_cmphex(val[1], ==, 0);
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

/*
 * Send @tag with the two-word request (@id, @arg) of the clock and power
 * tags, and return the answer's second word; the first keeps the id
 */
static uint32_t fw_request(QTestState *qts, uint32_t tag, uint32_t id,
                           uint32_t arg)
{
    uint32_t val[2] = { id, arg };

    g_assert_cmphex(mbox_call(qts, tag, sizeof(val), val), ==,
                    FW_TAG_RESPONSE | sizeof(val));
    g_assert_cmphex(val[0], ==, id);
    return val[1];
}

/*
 * Linux's raspberrypi_fw_set_rate(): the id, the rate and skip_turbo, of
 * which the answer has the first two. Returns the rate the clock got.
 */
static uint32_t fw_set_clock_rate(QTestState *qts, uint32_t id, uint32_t rate)
{
    uint32_t val[3] = { id, rate, 0 };

    g_assert_cmphex(mbox_call(qts, FW_TAG_SET_CLOCK_RATE, sizeof(val), val),
                    ==, FW_TAG_RESPONSE | 8);
    g_assert_cmphex(val[0], ==, id);
    return val[1];
}

/*
 * The clocks the Raspberry Pi 5's firmware lists, with the least and the
 * most rate of each: config.txt's defaults
 */
static const struct {
    uint32_t id;
    uint32_t min, max;                  /* Hz */
} fw_clocks[] = {
    { FW_CLK_ARM, 1500000000, 2400000000u },
    { 4, 500000000, 910000000 },        /* CORE */
    { FW_CLK_V3D, 500000000, 960000000 },
    { 7, 500000000, 910000000 },        /* ISP */
    { 11, 500000000, 910000000 },       /* HEVC */
};

/*
 * The firmware lists its clocks, as Linux's raspberrypi-clk driver asks,
 * each on and at its most: the rate, the rate measured, and the range
 */
static void test_mbox_clocks(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");
    uint32_t val[12];

    /* Each clock's parent, none, and id; the rest of the buffer is left */
    g_assert_cmphex(mbox_tag(qts, FW_TAG_GET_CLOCKS, sizeof(val), val), ==,
                    FW_TAG_RESPONSE | (8 * ARRAY_SIZE(fw_clocks)));
    for (int i = 0; i < ARRAY_SIZE(fw_clocks); i++) {
        g_assert_cmphex(val[2 * i], ==, 0);
        g_assert_cmphex(val[2 * i + 1], ==, fw_clocks[i].id);
    }
    for (int i = 2 * ARRAY_SIZE(fw_clocks); i < ARRAY_SIZE(val); i++) {
        g_assert_cmphex(val[i], ==, 0xa5a5a5a5);
    }

    for (int i = 0; i < ARRAY_SIZE(fw_clocks); i++) {
        uint32_t id = fw_clocks[i].id;

        g_assert_cmphex(fw_request(qts, FW_TAG_CLOCK_STATE, id, 0), ==,
                        FW_STATE_ON);
        g_assert_cmpuint(fw_request(qts, FW_TAG_CLOCK_RATE, id, 0), ==,
                         fw_clocks[i].max);
        g_assert_cmpuint(fw_request(qts, FW_TAG_CLOCK_MEASURED, id, 0), ==,
                         fw_clocks[i].max);
        g_assert_cmpuint(fw_request(qts, FW_TAG_MAX_CLOCK_RATE, id, 0), ==,
                         fw_clocks[i].max);
        g_assert_cmpuint(fw_request(qts, FW_TAG_MIN_CLOCK_RATE, id, 0), ==,
                         fw_clocks[i].min);
    }

    qtest_quit(qts);
}

/*
 * The firmware sets a clock to the rate nearest the one asked for, within
 * its range, as raspberrypi-cpufreq sets the ARM clock's. A clock off
 * keeps its rate for when it is on again, and measures 0. A reset brings
 * back the boot's rates and states.
 */
static void test_mbox_clock_set(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");

    g_assert_cmpuint(fw_set_clock_rate(qts, FW_CLK_ARM, 1800000000), ==,
                     1800000000);
    g_assert_cmpuint(fw_request(qts, FW_TAG_CLOCK_RATE, FW_CLK_ARM, 0), ==,
                     1800000000);
    g_assert_cmpuint(fw_request(qts, FW_TAG_CLOCK_MEASURED, FW_CLK_ARM, 0),
                     ==, 1800000000);
    g_assert_cmpuint(fw_set_clock_rate(qts, FW_CLK_ARM, 3000000000u), ==,
                     2400000000u);
    g_assert_cmpuint(fw_set_clock_rate(qts, FW_CLK_ARM, 600000000), ==,
                     1500000000);
    g_assert_cmpuint(fw_request(qts, FW_TAG_CLOCK_RATE, FW_CLK_V3D, 0), ==,
                     960000000);

    g_assert_cmphex(fw_request(qts, FW_TAG_SET_CLOCK_STATE, FW_CLK_V3D, 0),
                    ==, 0);
    g_assert_cmphex(fw_request(qts, FW_TAG_CLOCK_STATE, FW_CLK_V3D, 0), ==,
                    0);
    g_assert_cmpuint(fw_request(qts, FW_TAG_CLOCK_MEASURED, FW_CLK_V3D, 0),
                     ==, 0);
    g_assert_cmpuint(fw_set_clock_rate(qts, FW_CLK_V3D, 700000000), ==,
                     700000000);
    g_assert_cmpuint(fw_request(qts, FW_TAG_CLOCK_RATE, FW_CLK_V3D, 0), ==,
                     700000000);
    g_assert_cmpuint(fw_request(qts, FW_TAG_CLOCK_MEASURED, FW_CLK_V3D, 0),
                     ==, 0);
    g_assert_cmphex(fw_request(qts, FW_TAG_SET_CLOCK_STATE, FW_CLK_V3D,
                               FW_STATE_ON), ==, FW_STATE_ON);
    g_assert_cmpuint(fw_request(qts, FW_TAG_CLOCK_MEASURED, FW_CLK_V3D, 0),
                     ==, 700000000);
    g_assert_cmphex(fw_request(qts, FW_TAG_SET_CLOCK_STATE, FW_CLK_V3D, 0),
                    ==, 0);

    qtest_system_reset(qts);
    g_assert_cmpuint(fw_request(qts, FW_TAG_CLOCK_RATE, FW_CLK_ARM, 0), ==,
                     2400000000u);
    g_assert_cmphex(fw_request(qts, FW_TAG_CLOCK_STATE, FW_CLK_V3D, 0), ==,
                    FW_STATE_ON);
    g_assert_cmpuint(fw_request(qts, FW_TAG_CLOCK_MEASURED, FW_CLK_V3D, 0),
                     ==, 960000000);

    qtest_quit(qts);
}

/*
 * A clock the firmware does not list, such as the older Pis' EMMC clock
 * or the display's, has a rate of 0 and reports that it does not exist,
 * whatever a guest asks of it
 */
static void test_mbox_clock_unknown(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");
    const uint32_t ids[] = { 0, 1, 14, 16, 17, 0xa5a5a5a5 };

    for (int i = 0; i < ARRAY_SIZE(ids); i++) {
        g_assert_cmphex(fw_request(qts, FW_TAG_SET_CLOCK_STATE, ids[i],
                                   FW_STATE_ON), ==, FW_STATE_NO_DEVICE);
        g_assert_cmphex(fw_request(qts, FW_TAG_CLOCK_STATE, ids[i], 0), ==,
                        FW_STATE_NO_DEVICE);
        g_assert_cmpuint(fw_set_clock_rate(qts, ids[i], 500000000), ==, 0);
        g_assert_cmpuint(fw_request(qts, FW_TAG_CLOCK_RATE, ids[i], 0), ==,
                         0);
        g_assert_cmpuint(fw_request(qts, FW_TAG_CLOCK_MEASURED, ids[i], 0),
                         ==, 0);
        g_assert_cmpuint(fw_request(qts, FW_TAG_MAX_CLOCK_RATE, ids[i], 0),
                         ==, 0);
        g_assert_cmpuint(fw_request(qts, FW_TAG_MIN_CLOCK_RATE, ids[i], 0),
                         ==, 0);
    }

    qtest_quit(qts);
}

/* At boot, the ARM's power domain is on, and nothing else */
static void fw_check_boot_power(QTestState *qts)
{
    for (uint32_t domain = 0; domain <= FW_DOMAINS + 1; domain++) {
        g_assert_cmphex(fw_request(qts, FW_TAG_DOMAIN_STATE, domain, 0), ==,
                        domain == FW_DOMAIN_ARM);
    }
    for (uint32_t dev = 0; dev < FW_DEVICES; dev++) {
        g_assert_cmphex(fw_request(qts, FW_TAG_POWER_STATE, dev, 0), ==, 0);
    }
}

/*
 * Linux's raspberrypi-power driver switches the power domains, 1 to 23,
 * through the newer interface, and USB, one of the devices of the older
 * interface, 0 to 8, as U-Boot does too. A domain that does not exist
 * stays off; a device that does not exist says so. A reset switches back.
 */
static void test_mbox_power(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");

    fw_check_boot_power(qts);
    /* raspberrypi-power's probe for the newer interface: an answer */
    g_assert_cmphex(fw_request(qts, FW_TAG_DOMAIN_STATE, FW_DOMAIN_ARM, ~0u),
                    ==, 1);

    g_assert_cmphex(fw_request(qts, FW_TAG_SET_DOMAIN_STATE, 1, 1), ==, 1);
    g_assert_cmphex(fw_request(qts, FW_TAG_SET_DOMAIN_STATE, 22, 1), ==, 1);
    g_assert_cmphex(fw_request(qts, FW_TAG_SET_DOMAIN_STATE, FW_DOMAIN_ARM,
                               0), ==, 0);
    g_assert_cmphex(fw_request(qts, FW_TAG_SET_DOMAIN_STATE, 0, 1), ==, 0);
    g_assert_cmphex(fw_request(qts, FW_TAG_SET_DOMAIN_STATE, FW_DOMAINS + 1,
                               1), ==, 0);
    for (uint32_t domain = 0; domain <= FW_DOMAINS + 1; domain++) {
        g_assert_cmphex(fw_request(qts, FW_TAG_DOMAIN_STATE, domain, 0), ==,
                        domain == 1 || domain == 22);
    }

    /* U-Boot's bcm2835_power_on_module(), which waits for it */
    g_assert_cmphex(fw_request(qts, FW_TAG_SET_POWER_STATE, FW_DEV_USB,
                               FW_STATE_ON | FW_STATE_WAIT), ==, FW_STATE_ON);
    g_assert_cmphex(fw_request(qts, FW_TAG_POWER_STATE, FW_DEV_USB, 0), ==,
                    FW_STATE_ON);
    g_assert_cmphex(fw_request(qts, FW_TAG_SET_POWER_STATE, FW_DEVICES,
                               FW_STATE_ON), ==, FW_STATE_NO_DEVICE);
    g_assert_cmphex(fw_request(qts, FW_TAG_POWER_STATE, FW_DEVICES, 0), ==,
                    FW_STATE_NO_DEVICE);

    qtest_system_reset(qts);
    fw_check_boot_power(qts);

    qtest_quit(qts);
}

/* config.txt's temp_limit: 85 degrees C */
static void test_mbox_temperature(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");

    g_assert_cmpuint(fw_request(qts, FW_TAG_MAX_TEMPERATURE, 0, 0), ==,
                     85000);

    qtest_quit(qts);
}

static uint32_t fw_reboot_flags(QTestState *qts)
{
    uint32_t val[1];

    g_assert_cmphex(mbox_tag(qts, FW_TAG_REBOOT_FLAGS, sizeof(val), val),
                    ==, FW_TAG_RESPONSE | sizeof(val));
    return val[0];
}

/*
 * Linux's rpi_firmware_notify_reboot() for "reboot 0 tryboot": the flags,
 * then the notification, which has nothing to answer. The flags are for
 * the next boot only: the bootloader takes them at the reset, with a
 * device tree or without; the guest suite's mbox/tryboot checks what the
 * tree reports.
 */
static void check_reboot_flags(const char *args)
{
    QTestState *qts = qtest_init(args);
    uint32_t val[1] = { 1 };

    g_assert_cmphex(fw_reboot_flags(qts), ==, 0);
    g_assert_cmphex(mbox_call(qts, FW_TAG_SET_REBOOT_FLAGS, sizeof(val), val),
                    ==, FW_TAG_RESPONSE | sizeof(val));
    g_assert_cmphex(val[0], ==, 1);
    g_assert_cmphex(mbox_tag(qts, FW_TAG_NOTIFY_REBOOT, 0, val), ==,
                    FW_TAG_RESPONSE);
    g_assert_cmphex(fw_reboot_flags(qts), ==, 1);

    qtest_system_reset(qts);
    g_assert_cmphex(fw_reboot_flags(qts), ==, 0);

    qtest_quit(qts);
}

static void test_mbox_reboot_flags(void)
{
    check_reboot_flags("-machine raspi5b");
    check_reboot_flags("-machine raspi5b,builtin-dtb=off");
}

static uint32_t fw_rtc(QTestState *qts, uint32_t reg)
{
    return fw_request(qts, FW_TAG_RTC_REG, reg, 0);
}

/* Returns the register's value after the write */
static uint32_t fw_rtc_set(QTestState *qts, uint32_t reg, uint32_t value)
{
    return fw_request(qts, FW_TAG_SET_RTC_REG, reg, value);
}

static void clock_step_s(QTestState *qts, int64_t s)
{
    qtest_clock_step(qts, s * NANOSECONDS_PER_SECOND);
}

/*
 * The real-time clock starts at QEMU's RTC and counts seconds on
 * rtc_clock. Setting it, as Linux's rtc-rpi driver does for "hwclock -w",
 * tells the monitor how far the guest moved it; a reset leaves it
 * running.
 */
static void test_mbox_rtc_time(void)
{
    QTestState *qts = qtest_init("-machine raspi5b -rtc base=" RTC_BASE
                                 ",clock=vm");
    const uint32_t time = 2000000000;   /* 2033-05-18T03:33:20Z */
    QDict *event, *data;

    g_assert_cmpuint(fw_rtc(qts, FW_RTC_TIME), ==, RTC_BASE_TIME);
    clock_step_s(qts, 10);
    g_assert_cmpuint(fw_rtc(qts, FW_RTC_TIME), ==, RTC_BASE_TIME + 10);

    /* The event's offset is from the base as it runs on the host clock */
    g_assert_cmpuint(fw_rtc_set(qts, FW_RTC_TIME, time), ==, time);
    event = qtest_qmp_eventwait_ref(qts, "RTC_CHANGE");
    data = qdict_get_qdict(event, "data");
    g_assert_cmpint(qdict_get_int(data, "offset"), <=,
                    (int64_t)time - RTC_BASE_TIME);
    g_assert_cmpint(qdict_get_int(data, "offset"), >,
                    (int64_t)time - RTC_BASE_TIME - 600);
    g_assert_cmpstr(qdict_get_str(data, "qom-path"), ==,
                    "/machine/soc/property");
    qobject_unref(event);

    clock_step_s(qts, 1);
    g_assert_cmpuint(fw_rtc(qts, FW_RTC_TIME), ==, time + 1);

    qtest_system_reset(qts);
    g_assert_cmpuint(fw_rtc(qts, FW_RTC_TIME), ==, time + 1);
    clock_step_s(qts, 2);
    g_assert_cmpuint(fw_rtc(qts, FW_RTC_TIME), ==, time + 3);

    qtest_quit(qts);
}

/*
 * The alarm goes off, once, when the time reaches it while it is enabled,
 * and stays pending until cleared, through a reset too. An alarm the time
 * has passed, by counting or by being set past it, is not reached until
 * the time wraps.
 */
static void test_mbox_rtc_alarm(void)
{
    QTestState *qts = qtest_init("-machine raspi5b -rtc base=" RTC_BASE
                                 ",clock=vm");
    const uint32_t now = RTC_BASE_TIME;

    g_assert_cmpuint(fw_rtc(qts, FW_RTC_ALARM), ==, 0);
    g_assert_cmpuint(fw_rtc(qts, FW_RTC_ALARM_ENABLE), ==, 0);
    g_assert_cmpuint(fw_rtc(qts, FW_RTC_ALARM_PENDING), ==, 0);

    /* Linux's rpi_rtc_set_alarm(): the time, then the enable */
    g_assert_cmpuint(fw_rtc_set(qts, FW_RTC_ALARM, now + 10), ==, now + 10);
    g_assert_cmpuint(fw_rtc_set(qts, FW_RTC_ALARM_ENABLE, 1), ==, 1);
    clock_step_s(qts, 9);
    g_assert_cmpuint(fw_rtc(qts, FW_RTC_ALARM_PENDING), ==, 0);
    clock_step_s(qts, 1);
    g_assert_cmpuint(fw_rtc(qts, FW_RTC_ALARM_PENDING), ==, 1);

    /* Write 1 to clear */
    g_assert_cmpuint(fw_rtc_set(qts, FW_RTC_ALARM_PENDING, 0), ==, 1);
    qtest_system_reset(qts);
    g_assert_cmpuint(fw_rtc(qts, FW_RTC_ALARM_PENDING), ==, 1);
    g_assert_cmpuint(fw_rtc(qts, FW_RTC_ALARM_ENABLE), ==, 1);
    g_assert_cmpuint(fw_rtc(qts, FW_RTC_ALARM), ==, now + 10);
    g_assert_cmpuint(fw_rtc_set(qts, FW_RTC_ALARM_PENDING, 1), ==, 0);
    clock_step_s(qts, 100);
    g_assert_cmpuint(fw_rtc(qts, FW_RTC_ALARM_PENDING), ==, 0);

    /* Disabled, it does not go off */
    fw_rtc_set(qts, FW_RTC_ALARM_ENABLE, 0);
    fw_rtc_set(qts, FW_RTC_ALARM, now + 120);
    clock_step_s(qts, 20);
    g_assert_cmpuint(fw_rtc(qts, FW_RTC_ALARM_PENDING), ==, 0);

    /* Passed, nor when enabled */
    fw_rtc_set(qts, FW_RTC_ALARM_ENABLE, 1);
    clock_step_s(qts, 60);
    g_assert_cmpuint(fw_rtc(qts, FW_RTC_ALARM_PENDING), ==, 0);

    /* Nor when the time is set past it; set back, it is reached */
    fw_rtc_set(qts, FW_RTC_ALARM, now + 300);
    fw_rtc_set(qts, FW_RTC_TIME, now + 400);
    clock_step_s(qts, 60);
    g_assert_cmpuint(fw_rtc(qts, FW_RTC_ALARM_PENDING), ==, 0);
    fw_rtc_set(qts, FW_RTC_TIME, now + 298);
    clock_step_s(qts, 1);
    g_assert_cmpuint(fw_rtc(qts, FW_RTC_ALARM_PENDING), ==, 0);
    clock_step_s(qts, 1);
    g_assert_cmpuint(fw_rtc(qts, FW_RTC_ALARM_PENDING), ==, 1);

    /* One set to the time now goes off at once */
    fw_rtc_set(qts, FW_RTC_ALARM_PENDING, 1);
    fw_rtc_set(qts, FW_RTC_ALARM, fw_rtc(qts, FW_RTC_TIME));
    g_assert_cmpuint(fw_rtc(qts, FW_RTC_ALARM_PENDING), ==, 1);

    qtest_quit(qts);
}

/*
 * The backup battery's charger: off until set, within the range the
 * Raspberry Pi documentation gives, which rtc-rpi reports in sysfs. No
 * battery is fitted. A register that does not exist reads 0, and the
 * range and battery voltage stay as they are when written.
 */
static void test_mbox_rtc_battery(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");

    g_assert_cmpuint(fw_rtc(qts, FW_RTC_CHARGE), ==, 0);
    g_assert_cmpuint(fw_rtc(qts, FW_RTC_CHARGE_MIN), ==, 1300000);
    g_assert_cmpuint(fw_rtc(qts, FW_RTC_CHARGE_MAX), ==, 4400000);
    g_assert_cmpuint(fw_rtc(qts, FW_RTC_BATTERY), ==, 0);

    /* dtparam=rtc_bbat_vchg=3000000 */
    g_assert_cmpuint(fw_rtc_set(qts, FW_RTC_CHARGE, 3000000), ==, 3000000);
    g_assert_cmpuint(fw_rtc_set(qts, FW_RTC_CHARGE, 5000000), ==, 4400000);
    g_assert_cmpuint(fw_rtc_set(qts, FW_RTC_CHARGE, 1000000), ==, 1300000);
    qtest_system_reset(qts);
    g_assert_cmpuint(fw_rtc(qts, FW_RTC_CHARGE), ==, 1300000);
    g_assert_cmpuint(fw_rtc_set(qts, FW_RTC_CHARGE, 0), ==, 0);

    g_assert_cmpuint(fw_rtc_set(qts, FW_RTC_CHARGE_MIN, 1), ==, 1300000);
    g_assert_cmpuint(fw_rtc_set(qts, FW_RTC_CHARGE_MAX, 1), ==, 4400000);
    g_assert_cmpuint(fw_rtc_set(qts, FW_RTC_BATTERY, 1), ==, 0);
    g_assert_cmpuint(fw_rtc(qts, 8), ==, 0);
    g_assert_cmpuint(fw_rtc_set(qts, 8, 1), ==, 0);
    g_assert_cmpuint(fw_rtc(qts, 8), ==, 0);

    qtest_quit(qts);
}

/*
 * Set the framebuffer to @width x @height pixels, the size of the buffer
 * too, at @depth bits per pixel, and check that the firmware keeps it
 * all but the lines past @lines, answering with what it kept
 */
static void fb_set(QTestState *qts, uint32_t width, uint32_t height,
                   uint32_t depth, uint32_t lines)
{
    const uint32_t tags[] = { FW_TAG_FB_SET_PHYSICAL, FW_TAG_FB_SET_VIRTUAL };
    uint32_t val[2];

    val[0] = depth;
    g_assert_cmphex(mbox_call(qts, FW_TAG_FB_SET_DEPTH, 4, val), ==,
                    FW_TAG_RESPONSE | 4);
    g_assert_cmpuint(val[0], ==, depth);
    for (int i = 0; i < ARRAY_SIZE(tags); i++) {
        val[0] = width;
        val[1] = height;
        g_assert_cmphex(mbox_call(qts, tags[i], sizeof(val), val), ==,
                        FW_TAG_RESPONSE | sizeof(val));
        g_assert_cmpuint(val[0], ==, width);
        g_assert_cmpuint(val[1], ==, MIN(height, lines));
    }
}

/*
 * Check that the framebuffer is @width x @height pixels at @depth bits
 * per pixel, a buffer of the same size, where the VideoCore keeps it
 */
static void fb_check(QTestState *qts, uint32_t width, uint32_t height,
                     uint32_t depth)
{
    const uint32_t pitch = width * depth / 8;
    uint32_t val[2];

    g_assert_cmphex(mbox_tag(qts, FW_TAG_FB_PHYSICAL, 8, val), ==,
                    FW_TAG_RESPONSE | 8);
    g_assert_cmpuint(val[0], ==, width);
    g_assert_cmpuint(val[1], ==, height);
    g_assert_cmphex(mbox_tag(qts, FW_TAG_FB_VIRTUAL, 8, val), ==,
                    FW_TAG_RESPONSE | 8);
    g_assert_cmpuint(val[0], ==, width);
    g_assert_cmpuint(val[1], ==, height);
    g_assert_cmphex(mbox_tag(qts, FW_TAG_FB_DEPTH, 4, val), ==,
                    FW_TAG_RESPONSE | 4);
    g_assert_cmpuint(val[0], ==, depth);
    g_assert_cmphex(mbox_tag(qts, FW_TAG_FB_PITCH, 4, val), ==,
                    FW_TAG_RESPONSE | 4);
    g_assert_cmpuint(val[0], ==, pitch);

    /* The request's word is the alignment the guest wants */
    val[0] = 16;
    val[1] = 0;
    g_assert_cmphex(mbox_call(qts, FW_TAG_FB_ALLOCATE, sizeof(val), val),
                    ==, FW_TAG_RESPONSE | sizeof(val));
    g_assert_cmphex(val[0], ==, VC_FB_BASE);
    g_assert_cmpuint(val[1], ==, pitch * height);
    g_assert_cmphex(VC_FB_BASE + val[1], <=, VC_FB_END);
}

/*
 * Save the display with the screendump command, and return its width
 * and height and the RGB bytes of its first two pixels
 */
static void fb_screendump(QTestState *qts, int *width, int *height,
                          uint8_t rgb[6])
{
    g_autofree char *path = tmp_file("raspi5b-fb-XXXXXX", NULL, 0);
    g_autofree char *ppm = NULL;
    size_t len;
    int header;

    qtest_qmp_assert_success(qts, "{ 'execute': 'screendump',"
                             "  'arguments': { 'filename': %s } }", path);
    g_assert_true(g_file_get_contents(path, &ppm, &len, NULL));
    unlink(path);
    /* The header, then a single newline before the pixels */
    g_assert_cmpint(sscanf(ppm, "P6 %d %d 255%n", width, height, &header),
                    ==, 2);
    g_assert_cmpint(ppm[header++], ==, '\n');
    g_assert_cmpuint(len, ==, header + 3 * *width * *height);
    memcpy(rgb, ppm + header, 6);
}

/*
 * The firmware's framebuffer lies 1 MiB into the VideoCore memory, 640 x
 * 480 pixels at 16 bits per pixel at boot. A guest sets its size and
 * depth, which the display follows, but the VideoCore memory holds only
 * 3 MiB of it: the firmware keeps no more lines than fit.
 */
static void test_mbox_framebuffer(void)
{
    QTestState *qts = qtest_init("-machine raspi5b -m 2G");
    uint32_t val[1];
    uint8_t rgb[6];
    int width, height;

    g_assert_cmphex(mbox_tag(qts, FW_TAG_FB_DISPLAYS, 4, val), ==,
                    FW_TAG_RESPONSE | 4);
    g_assert_cmpuint(val[0], ==, 1);
    fb_check(qts, 640, 480, 16);

    /* Linux's bcm2708_fb: 800 x 480 at 32 bits per pixel */
    fb_set(qts, 800, 480, 32, UINT32_MAX);
    fb_check(qts, 800, 480, 32);
    qtest_writel(qts, VC_FB_BASE, 0x0000ff);        /* red, then black */
    qtest_writel(qts, VC_FB_BASE + 4, 0);
    fb_screendump(qts, &width, &height, rgb);
    g_assert_cmpint(width, ==, 800);
    g_assert_cmpint(height, ==, 480);
    g_assert_cmpmem(rgb, 6, "\xff\x00\x00\x00\x00\x00", 6);

    /* 3 MiB: all of the lines */
    fb_set(qts, 1024, 768, 32, UINT32_MAX);
    fb_check(qts, 1024, 768, 32);

    /* 7.9 MiB: the 409 lines that fit, which the display shows */
    fb_set(qts, 1920, 1080, 32, 409);
    fb_check(qts, 1920, 409, 32);
    fb_screendump(qts, &width, &height, rgb);
    g_assert_cmpint(width, ==, 1920);
    g_assert_cmpint(height, ==, 409);

    /* A reset brings back the size at boot */
    qtest_system_reset(qts);
    fb_check(qts, 640, 480, 16);

    qtest_quit(qts);
}

/*
 * The framebuffer channel of the older Pis sets the size and depth in one
 * request, and answers with the pitch, base and size of what it kept
 */
static void test_mbox_framebuffer_channel(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");
    const uint64_t buf = 0x10000;
    /* width, height, virtual width, height, pitch, depth, x, y, base, size */
    const uint32_t req[] = { 1920, 1080, 1920, 1080, 0, 32, 0, 0, 0, 0 };

    for (int i = 0; i < ARRAY_SIZE(req); i++) {
        qtest_writel(qts, buf + 4 * i, req[i]);
    }
    qtest_writel(qts, MBOX_BASE + MBOX_WRITE, VC_BUS_RAM | buf | MBOX_CHAN_FB);
    g_assert_true(mbox_has_response(qts));
    g_assert_cmphex(qtest_readl(qts, MBOX_BASE + MBOX_READ), ==, MBOX_CHAN_FB);
    g_assert_cmpuint(qtest_readl(qts, buf + 16), ==, 1920 * 4);
    g_assert_cmphex(qtest_readl(qts, buf + 32), ==, VC_FB_BASE);
    g_assert_cmpuint(qtest_readl(qts, buf + 36), ==, 1920 * 4 * 409);
    fb_check(qts, 1920, 409, 32);

    qtest_quit(qts);
}

/* What a guest sets through the firmware survives migration */
static void test_mbox_firmware_migrate(void)
{
    g_autofree char *file = g_strdup_printf("%s/raspi5b-firmware-%d.mig",
                                            g_get_tmp_dir(), getpid());
    g_autofree char *out = g_strdup_printf("exec:cat > %s", file);
    g_autofree char *in = g_strdup_printf("exec:cat %s", file);
    /* The qtest accelerator's clock starts at 0 on both sides */
    const char *args = "-machine raspi5b -m 1G -rtc clock=vm";
    const uint32_t time = 2000000000;
    QTestState *src, *dst;
    uint32_t val[1] = { 1 };

    src = qtest_init(args);
    fw_set_clock_rate(src, FW_CLK_ARM, 1800000000);
    fw_request(src, FW_TAG_SET_CLOCK_STATE, FW_CLK_V3D, 0);
    fw_request(src, FW_TAG_SET_DOMAIN_STATE, 5, 1);
    fw_request(src, FW_TAG_SET_POWER_STATE, FW_DEV_USB, FW_STATE_ON);
    mbox_call(src, FW_TAG_SET_REBOOT_FLAGS, sizeof(val), val);
    fw_rtc_set(src, FW_RTC_TIME, time);
    fw_rtc_set(src, FW_RTC_ALARM, time + 10);
    fw_rtc_set(src, FW_RTC_ALARM_ENABLE, 1);
    fw_rtc_set(src, FW_RTC_CHARGE, 3000000);
    qtest_qmp_assert_success(src, "{ 'execute': 'migrate',"
                             "  'arguments': { 'uri': %s } }", out);
    wait_for_migration(src);
    qtest_quit(src);

    dst = qtest_initf("%s -incoming defer", args);
    qtest_qmp_assert_success(dst, "{ 'execute': 'migrate-incoming',"
                             "  'arguments': { 'uri': %s } }", in);
    wait_for_migration(dst);

    g_assert_cmpuint(fw_request(dst, FW_TAG_CLOCK_RATE, FW_CLK_ARM, 0), ==,
                     1800000000);
    g_assert_cmphex(fw_request(dst, FW_TAG_CLOCK_STATE, FW_CLK_V3D, 0), ==,
                    0);
    g_assert_cmphex(fw_request(dst, FW_TAG_DOMAIN_STATE, 5, 0), ==, 1);
    g_assert_cmphex(fw_request(dst, FW_TAG_DOMAIN_STATE, FW_DOMAIN_ARM, 0),
                    ==, 1);
    g_assert_cmphex(fw_request(dst, FW_TAG_POWER_STATE, FW_DEV_USB, 0), ==,
                    FW_STATE_ON);
    g_assert_cmphex(fw_reboot_flags(dst), ==, 1);
    g_assert_cmpuint(fw_rtc(dst, FW_RTC_TIME), ==, time);
    g_assert_cmpuint(fw_rtc(dst, FW_RTC_ALARM), ==, time + 10);
    g_assert_cmpuint(fw_rtc(dst, FW_RTC_ALARM_ENABLE), ==, 1);
    g_assert_cmpuint(fw_rtc(dst, FW_RTC_CHARGE), ==, 3000000);

    /* The alarm is still set to go off */
    clock_step_s(dst, 9);
    g_assert_cmpuint(fw_rtc(dst, FW_RTC_ALARM_PENDING), ==, 0);
    clock_step_s(dst, 1);
    g_assert_cmpuint(fw_rtc(dst, FW_RTC_ALARM_PENDING), ==, 1);

    qtest_system_reset(dst);
    g_assert_cmphex(fw_reboot_flags(dst), ==, 0);

    qtest_quit(dst);
    unlink(file);
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
 * The AVS monitor's temperature sensor (Linux's bcm2711_thermal.c): a
 * 10-bit code, which the Pi 5's trees convert to millidegrees Celsius as
 * -550 * code + 450000
 */
#define AVS_SIZE                0xf00
#define AVS_RO_TEMP_STATUS      0x200
#define AVS_TEMP_VALID          (BIT(16) | BIT(10))
#define AVS_TEMP_CODE           0x3ff
#define AVS_PATH                "/machine/soc/avs-monitor"

static int64_t qom_get_int(QTestState *qts, const char *path,
                           const char *property)
{
    QDict *rsp = qtest_qmp(qts, "{ 'execute': 'qom-get', 'arguments':"
                           " { 'path': %s, 'property': %s } }",
                           path, property);
    int64_t val;

    g_assert(qdict_haskey(rsp, "return"));
    val = qdict_get_int(rsp, "return");
    qobject_unref(rsp);
    return val;
}

/* Set the chip's temperature: false if the sensor cannot report it */
static bool avs_set_temperature(QTestState *qts, int64_t temperature)
{
    QDict *rsp = qtest_qmp(qts, "{ 'execute': 'qom-set', 'arguments':"
                           " { 'path': %s, 'property': 'temperature',"
                           "   'value': %" PRId64 " } }",
                           AVS_PATH, temperature);
    bool ok = qdict_haskey(rsp, "return");

    qobject_unref(rsp);
    return ok;
}

/* The code the sensor reports, marked valid */
static uint32_t avs_code(QTestState *qts)
{
    uint32_t status = qtest_readl(qts, AVS_BASE + AVS_RO_TEMP_STATUS);

    g_assert_cmphex(status & ~AVS_TEMP_CODE, ==, AVS_TEMP_VALID);
    return status & AVS_TEMP_CODE;
}

/* The temperature the trees' coefficients make of @code */
static int32_t avs_code_temperature(uint32_t code)
{
    return 450000 - 550 * (int32_t)code;
}

/* The firmware's reading, the same */
static void check_avs_code(QTestState *qts, uint32_t code)
{
    g_assert_cmpuint(avs_code(qts), ==, code);
    g_assert_cmpint((int32_t)fw_request(qts, FW_TAG_TEMPERATURE, 0, 0), ==,
                    avs_code_temperature(code));
}

/*
 * 25 degrees C unless set, and the chip's temperature changes at run time:
 * the sensor, and the firmware, report the nearest code, a code and a half
 * rounding to the lower temperature. One the code cannot reach is refused.
 */
static void test_avs_temperature(void)
{
    static const struct {
        int32_t temperature;
        uint32_t code;
    } steps[] = {
        { 54000, 720 },
        { 54100, 720 },
        { 54275, 720 },
        { 54276, 719 },         /* 54.55 degrees C */
        { -40000, 891 },        /* -40.05 degrees C */
        { 450274, 0 },
        { -112924, 1023 },
    };
    QTestState *qts = qtest_init("-machine raspi5b");

    g_assert_cmpint(qom_get_int(qts, AVS_PATH, "slope"), ==, -550);
    g_assert_cmpint(qom_get_int(qts, AVS_PATH, "offset"), ==, 450000);
    g_assert_cmpint(qom_get_int(qts, AVS_PATH, "temperature"), ==, 25000);
    check_avs_code(qts, 773);   /* 24.85 degrees C */

    for (int i = 0; i < ARRAY_SIZE(steps); i++) {
        g_assert_true(avs_set_temperature(qts, steps[i].temperature));
        g_assert_cmpint(qom_get_int(qts, AVS_PATH, "temperature"), ==,
                        steps[i].temperature);
        check_avs_code(qts, steps[i].code);
    }

    g_assert_false(avs_set_temperature(qts, 450275));
    g_assert_false(avs_set_temperature(qts, -112925));
    g_assert_false(avs_set_temperature(qts, 1LL << 32));
    g_assert_cmpint(qom_get_int(qts, AVS_PATH, "temperature"), ==, -112924);
    check_avs_code(qts, 1023);

    qtest_quit(qts);
}

/* The temperature from the command line */
static void test_avs_global(void)
{
    QTestState *qts = qtest_init("-machine raspi5b"
                                 " -global bcm2711-avs-monitor.temperature="
                                 "65000");

    check_avs_code(qts, 700);

    qtest_quit(qts);
}

/* The other registers read as 0, and writes change nothing */
static void test_avs_registers(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");

    for (uint32_t reg = 0; reg < AVS_SIZE; reg += 4) {
        qtest_writel(qts, AVS_BASE + reg, UINT32_MAX);
    }
    for (uint32_t reg = 0; reg < AVS_SIZE; reg += 4) {
        if (reg != AVS_RO_TEMP_STATUS) {
            g_assert_cmphex(qtest_readl(qts, AVS_BASE + reg), ==, 0);
        }
    }
    check_avs_code(qts, 773);

    qtest_quit(qts);
}

/* A reset leaves the chip at its temperature */
static void test_avs_reset(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");

    g_assert_true(avs_set_temperature(qts, 65000));
    qtest_system_reset(qts);
    g_assert_cmpint(qom_get_int(qts, AVS_PATH, "temperature"), ==, 65000);
    check_avs_code(qts, 700);

    qtest_quit(qts);
}

/* The temperature survives migration */
static void test_avs_migrate(void)
{
    g_autofree char *file = g_strdup_printf("%s/raspi5b-avs-%d.mig",
                                            g_get_tmp_dir(), getpid());
    g_autofree char *out = g_strdup_printf("exec:cat > %s", file);
    g_autofree char *in = g_strdup_printf("exec:cat %s", file);
    QTestState *src, *dst;

    src = qtest_init("-machine raspi5b");
    g_assert_true(avs_set_temperature(src, 65000));
    qtest_qmp_assert_success(src, "{ 'execute': 'migrate',"
                             "  'arguments': { 'uri': %s } }", out);
    wait_for_migration(src);
    qtest_quit(src);

    dst = qtest_init("-machine raspi5b -incoming defer");
    qtest_qmp_assert_success(dst, "{ 'execute': 'migrate-incoming',"
                             "  'arguments': { 'uri': %s } }", in);
    wait_for_migration(dst);
    g_assert_cmpint(qom_get_int(dst, AVS_PATH, "temperature"), ==, 65000);
    check_avs_code(dst, 700);

    qtest_quit(dst);
    unlink(file);
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
#define SD_CDET_AON_GPIO        5       /* low while a card is in */
#define ACT_LED_AON_GPIO        9       /* lit while low */

static const Gio gios[] = {
    { "gio",        0x107d508500ULL, { 32, 22 }, { BIT(PWR_BUTTON_GIO) } },
    { "gio-aon",    0x107d517c00ULL, { 17, 6 },
      { BIT(SD_CDET_AON_GPIO) | BIT(ACT_LED_AON_GPIO) } },
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
    gio_writel(qts, gio, 0, GIO_EC, BIT(6));
    gio_writel(qts, gio, 0, GIO_MASK, BIT(6));
    gio_set_input(qts, gio, 6, 1);
    g_assert_cmphex(gio_readl(qts, gio, 0, GIO_DATA), ==,
                    BIT(6) | gio->high[0]);
    g_assert_cmphex(gio_readl(qts, gio, 0, GIO_STAT), ==, BIT(6));
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
 * UARTA, a 16550 whose 8-bit registers are 4 bytes apart (Linux
 * 8250_bcm7271.c), and the second serial port
 */
#define UARTA_BASE              0x107d50c000ULL
#define UARTA_SPI               276             /* bcm2712.dtsi */
#define UARTA_FIFO_SIZE         32
#define UART_RX                 0x00
#define UART_TX                 0x00
#define UART_DLL                0x00
#define UART_IER                0x04
#define UART_DLM                0x04
#define UART_IIR                0x08
#define UART_FCR                0x08
#define UART_LCR                0x0c
#define UART_MCR                0x10
#define UART_LSR                0x14
#define UART_MSR                0x18
#define UART_SCR                0x1c
#define UART_IER_RDI            BIT(0)
#define UART_IER_THRI           BIT(1)
#define UART_IIR_NO_INT         0x01
#define UART_IIR_THRI           0x02
#define UART_IIR_RDI            0x04
#define UART_IIR_CTI            0x0c
#define UART_IIR_FIFO_ENABLED   0xc0
#define UART_FCR_ENABLE_FIFO    BIT(0)
#define UART_FCR_TRIGGER_14     0xc0
#define UART_LCR_WLEN8          0x03
#define UART_LCR_DLAB           BIT(7)
#define UART_MCR_OUT2           BIT(3)
#define UART_MCR_LOOP           BIT(4)
#define UART_LSR_DR             BIT(0)
#define UART_LSR_OE             BIT(1)
#define UART_LSR_THRE           BIT(5)
#define UART_LSR_TEMT           BIT(6)
#define UART_MSR_CTS            BIT(4)
#define UART_MSR_DSR            BIT(5)
#define UART_MSR_DCD            BIT(7)

static uint32_t uarta_readl(QTestState *qts, uint32_t reg)
{
    return qtest_readl(qts, UARTA_BASE + reg);
}

static void uarta_writel(QTestState *qts, uint32_t reg, uint32_t val)
{
    qtest_writel(qts, UARTA_BASE + reg, val);
}

static void uarta_set_divisor(QTestState *qts, uint16_t divisor)
{
    uint32_t lcr = uarta_readl(qts, UART_LCR);

    uarta_writel(qts, UART_LCR, lcr | UART_LCR_DLAB);
    uarta_writel(qts, UART_DLL, divisor & 0xff);
    uarta_writel(qts, UART_DLM, divisor >> 8);
    uarta_writel(qts, UART_LCR, lcr);
}

static uint16_t uarta_divisor(QTestState *qts)
{
    uint32_t lcr = uarta_readl(qts, UART_LCR);
    uint16_t divisor;

    uarta_writel(qts, UART_LCR, lcr | UART_LCR_DLAB);
    divisor = uarta_readl(qts, UART_DLL) | uarta_readl(qts, UART_DLM) << 8;
    uarta_writel(qts, UART_LCR, lcr);
    return divisor;
}

/* Nothing to send or take and no interrupt, as a 16550 resets */
static void uarta_check_reset(QTestState *qts)
{
    g_assert_cmphex(uarta_readl(qts, UART_IER), ==, 0);
    g_assert_cmphex(uarta_readl(qts, UART_IIR), ==, UART_IIR_NO_INT);
    g_assert_cmphex(uarta_readl(qts, UART_LCR), ==, 0);
    /* QEMU's 16550 sets OUT2, which gates its interrupt on a PC */
    g_assert_cmphex(uarta_readl(qts, UART_MCR), ==, UART_MCR_OUT2);
    g_assert_cmphex(uarta_readl(qts, UART_LSR), ==,
                    UART_LSR_TEMT | UART_LSR_THRE);
    /* Without a backend that has modem lines, its inputs read as asserted */
    g_assert_cmphex(uarta_readl(qts, UART_MSR), ==,
                    UART_MSR_DCD | UART_MSR_DSR | UART_MSR_CTS);
    g_assert_cmphex(uarta_readl(qts, UART_SCR), ==, 0);
    g_assert_false(gic_spi_pending(qts, UARTA_SPI));
}

static void test_uarta_reset_values(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");

    uarta_check_reset(qts);

    qtest_quit(qts);
}

/* 32-bit accesses, each register in the low byte of its word */
static void test_uarta_registers(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");

    uarta_writel(qts, UART_SCR, 0xa5);
    g_assert_cmphex(uarta_readl(qts, UART_SCR), ==, 0xa5);
    uarta_writel(qts, UART_LCR, UART_LCR_WLEN8);
    g_assert_cmphex(uarta_readl(qts, UART_LCR), ==, UART_LCR_WLEN8);

    /* With DLAB set, the first two words are the divisor latch */
    uarta_set_divisor(qts, 0x1234);
    g_assert_cmphex(uarta_readl(qts, UART_IER), ==, 0);
    g_assert_cmphex(uarta_readl(qts, UART_LCR), ==, UART_LCR_WLEN8);
    g_assert_cmphex(uarta_divisor(qts), ==, 0x1234);
    uarta_writel(qts, UART_IER, UART_IER_RDI);
    g_assert_cmphex(uarta_readl(qts, UART_IER), ==, UART_IER_RDI);
    g_assert_cmphex(uarta_divisor(qts), ==, 0x1234);

    qtest_quit(qts);
}

/*
 * The baud rate is the 96 MHz clock divided by 16 and by the divisor,
 * which shows in the receive timeout: 4 characters' time after a byte
 * that leaves the FIFO below its trigger level
 */
static void test_uarta_baud_clock(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");

    /* 6 Mbaud with 8 data bits: 10 bits, 1667 ns a character */
    uarta_writel(qts, UART_LCR, UART_LCR_WLEN8);
    uarta_set_divisor(qts, 1);
    uarta_writel(qts, UART_FCR, UART_FCR_ENABLE_FIFO | UART_FCR_TRIGGER_14);
    uarta_writel(qts, UART_MCR, UART_MCR_LOOP);
    uarta_writel(qts, UART_IER, UART_IER_RDI);
    uarta_writel(qts, UART_TX, 'x');
    qtest_clock_step(qts, 6 * SCALE_US);
    g_assert_false(gic_spi_pending(qts, UARTA_SPI));
    qtest_clock_step(qts, SCALE_US);
    g_assert_true(gic_spi_pending(qts, UARTA_SPI));
    g_assert_cmphex(uarta_readl(qts, UART_IIR), ==,
                    UART_IIR_FIFO_ENABLED | UART_IIR_CTI);
    g_assert_cmphex(uarta_readl(qts, UART_RX), ==, 'x');
    g_assert_false(gic_spi_pending(qts, UARTA_SPI));

    qtest_quit(qts);
}

/* The FIFOs hold 32 bytes, as Linux's 8250 driver has them */
static void test_uarta_fifo(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");

    uarta_writel(qts, UART_FCR, UART_FCR_ENABLE_FIFO);
    uarta_writel(qts, UART_MCR, UART_MCR_LOOP);
    for (int i = 0; i < UARTA_FIFO_SIZE; i++) {
        uarta_writel(qts, UART_TX, 0x40 + i);
    }
    g_assert_cmphex(uarta_readl(qts, UART_LSR), ==,
                    UART_LSR_TEMT | UART_LSR_THRE | UART_LSR_DR);

    /* One more overruns it and is lost */
    uarta_writel(qts, UART_TX, 0x3f);
    g_assert_cmphex(uarta_readl(qts, UART_LSR), ==,
                    UART_LSR_TEMT | UART_LSR_THRE | UART_LSR_OE | UART_LSR_DR);
    for (int i = 0; i < UARTA_FIFO_SIZE; i++) {
        g_assert_cmphex(uarta_readl(qts, UART_RX), ==, 0x40 + i);
    }
    g_assert_cmphex(uarta_readl(qts, UART_LSR), ==,
                    UART_LSR_TEMT | UART_LSR_THRE);

    qtest_quit(qts);
}

#ifndef _WIN32
/* A backend slow to take the bytes leaves 32 waiting in the transmit FIFO */
static void test_uarta_transmit_fifo(void)
{
    g_autofree char *dir = g_dir_make_tmp("raspi5b-uarta-XXXXXX", NULL);
    g_autofree char *path = g_strdup_printf("%s/sock", dir);
    g_autoptr(GByteArray) sent = g_byte_array_new();
    struct timeval timeout = { .tv_sec = 10 };
    g_autofree uint8_t *received = NULL;
    int server = qtest_socket_server(path);
    QTestState *qts;
    gint64 deadline;
    int fd;

    qts = qtest_initf("-machine raspi5b -serial null "
                      "-chardev socket,id=ua,path=%s -serial chardev:ua",
                      path);
    fd = accept(server, NULL, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(server);
    unlink(path);
    rmdir(dir);

    /* Fill the socket until a byte has to wait to be sent */
    uarta_writel(qts, UART_FCR, UART_FCR_ENABLE_FIFO);
    deadline = g_get_monotonic_time() + 30 * G_USEC_PER_SEC;
    do {
        uint8_t byte = sent->len;

        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        uarta_writel(qts, UART_TX, byte);
        g_byte_array_append(sent, &byte, 1);
    } while (uarta_readl(qts, UART_LSR) & UART_LSR_TEMT);

    for (int i = 0; i < UARTA_FIFO_SIZE; i++) {
        uint8_t byte = 0x80 | i;

        uarta_writel(qts, UART_TX, byte);
        g_byte_array_append(sent, &byte, 1);
    }
    g_assert_cmphex(uarta_readl(qts, UART_LSR), ==, 0);

    /* Every byte goes out as the socket drains */
    received = g_malloc(sent->len);
    g_assert_cmpint(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                               sizeof(timeout)), ==, 0);
    for (size_t len = 0; len < sent->len;) {
        ssize_t n = recv(fd, received + len, sent->len - len, 0);

        g_assert_cmpint(n, >, 0);
        len += n;
    }
    g_assert_cmpmem(received, sent->len, sent->data, sent->len);
    g_assert_cmphex(uarta_readl(qts, UART_LSR), ==,
                    UART_LSR_TEMT | UART_LSR_THRE);

    qtest_quit(qts);
    close(fd);
}
#endif

static void test_uarta_interrupt(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");

    /* A byte received */
    uarta_writel(qts, UART_MCR, UART_MCR_LOOP);
    uarta_writel(qts, UART_IER, UART_IER_RDI);
    g_assert_false(gic_spi_pending(qts, UARTA_SPI));
    uarta_writel(qts, UART_TX, 'x');
    g_assert_true(gic_spi_pending(qts, UARTA_SPI));
    g_assert_cmphex(uarta_readl(qts, UART_IIR), ==, UART_IIR_RDI);
    g_assert_cmphex(uarta_readl(qts, UART_RX), ==, 'x');
    g_assert_cmphex(uarta_readl(qts, UART_IIR), ==, UART_IIR_NO_INT);
    g_assert_false(gic_spi_pending(qts, UARTA_SPI));

    /* Room to send, until IIR has been read */
    uarta_writel(qts, UART_IER, UART_IER_THRI);
    g_assert_true(gic_spi_pending(qts, UARTA_SPI));
    g_assert_cmphex(uarta_readl(qts, UART_IIR), ==, UART_IIR_THRI);
    g_assert_false(gic_spi_pending(qts, UARTA_SPI));

    qtest_quit(qts);
}

/* Waits for a byte from the backend and takes it */
static uint8_t uarta_receive(QTestState *qts)
{
    gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;

    while (!(uarta_readl(qts, UART_LSR) & UART_LSR_DR)) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    return uarta_readl(qts, UART_RX);
}

/* UARTA is the second serial port */
static void test_uarta_serial_port(void)
{
    static const char in[] = "ping", out[] = "pong";
    g_autofree char *in_file = tmp_file("raspi5b-uarta-in-XXXXXX", in,
                                        strlen(in));
    g_autofree char *out_file = tmp_file("raspi5b-uarta-out-XXXXXX", "", 0);
    g_autofree char *sent = NULL;
    size_t len;
    QTestState *qts;

    qts = qtest_initf("-machine raspi5b -serial null "
                      "-chardev file,id=ua,path=%s,input-path=%s "
                      "-serial chardev:ua", out_file, in_file);

    for (int i = 0; i < strlen(in); i++) {
        g_assert_cmphex(uarta_receive(qts), ==, in[i]);
    }
    for (int i = 0; i < strlen(out); i++) {
        uarta_writel(qts, UART_TX, out[i]);
    }
    g_assert_true(g_file_get_contents(out_file, &sent, &len, NULL));
    g_assert_cmpmem(sent, len, out, strlen(out));

    qtest_quit(qts);
    unlink(in_file);
    unlink(out_file);
}

static void test_uarta_reset(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");

    uarta_writel(qts, UART_LCR, UART_LCR_WLEN8);
    uarta_set_divisor(qts, 52);
    uarta_writel(qts, UART_FCR, UART_FCR_ENABLE_FIFO);
    uarta_writel(qts, UART_MCR, UART_MCR_LOOP);
    uarta_writel(qts, UART_SCR, 0x5a);
    uarta_writel(qts, UART_IER, UART_IER_RDI);
    uarta_writel(qts, UART_TX, 'x');
    g_assert_true(gic_spi_pending(qts, UARTA_SPI));
    qtest_system_reset(qts);
    uarta_check_reset(qts);
    /* Nothing is left in the receive FIFO */
    g_assert_cmphex(uarta_readl(qts, UART_RX), ==, 0);
    g_assert_cmphex(uarta_readl(qts, UART_LSR), ==,
                    UART_LSR_TEMT | UART_LSR_THRE);

    qtest_quit(qts);
}

/* The registers survive a migration, and so do the bytes in the FIFO */
static void test_uarta_migrate(void)
{
    g_autofree char *file = g_strdup_printf("%s/raspi5b-uarta-%d.mig",
                                            g_get_tmp_dir(), getpid());
    g_autofree char *out = g_strdup_printf("exec:cat > %s", file);
    g_autofree char *in = g_strdup_printf("exec:cat %s", file);
    const char *args = "-machine raspi5b -m 1G";
    const int count = UARTA_FIFO_SIZE - 4;
    QTestState *src, *dst;

    src = qtest_init(args);
    uarta_writel(src, UART_LCR, UART_LCR_WLEN8);
    uarta_set_divisor(src, 52);
    uarta_writel(src, UART_FCR, UART_FCR_ENABLE_FIFO | UART_FCR_TRIGGER_14);
    uarta_writel(src, UART_MCR, UART_MCR_LOOP);
    uarta_writel(src, UART_SCR, 0x5a);
    uarta_writel(src, UART_IER, UART_IER_RDI);
    for (int i = 0; i < count; i++) {
        uarta_writel(src, UART_TX, 0x40 + i);
    }
    g_assert_true(gic_spi_pending(src, UARTA_SPI));
    qtest_qmp_assert_success(src, "{ 'execute': 'migrate',"
                             "  'arguments': { 'uri': %s } }", out);
    wait_for_migration(src);
    qtest_quit(src);

    dst = qtest_initf("%s -incoming defer", args);
    qtest_qmp_assert_success(dst, "{ 'execute': 'migrate-incoming',"
                             "  'arguments': { 'uri': %s } }", in);
    wait_for_migration(dst);
    g_assert_cmphex(uarta_readl(dst, UART_LCR), ==, UART_LCR_WLEN8);
    g_assert_cmphex(uarta_divisor(dst), ==, 52);
    g_assert_cmphex(uarta_readl(dst, UART_MCR), ==, UART_MCR_LOOP);
    g_assert_cmphex(uarta_readl(dst, UART_SCR), ==, 0x5a);
    g_assert_cmphex(uarta_readl(dst, UART_IER), ==, UART_IER_RDI);
    g_assert_true(gic_spi_pending(dst, UARTA_SPI));
    g_assert_cmphex(uarta_readl(dst, UART_IIR), ==,
                    UART_IIR_FIFO_ENABLED | UART_IIR_RDI);
    for (int i = 0; i < count; i++) {
        g_assert_cmphex(uarta_readl(dst, UART_RX), ==, 0x40 + i);
    }
    g_assert_cmphex(uarta_readl(dst, UART_LSR), ==,
                    UART_LSR_TEMT | UART_LSR_THRE);
    g_assert_false(gic_spi_pending(dst, UARTA_SPI));

    qtest_quit(dst);
    unlink(file);
}

/*
 * The SD/eMMC host controllers (Linux sdhci-brcmstb.c): an SD Host
 * Controller 3.00 at the base of each, Broadcom's configuration registers
 * at 0x400. SDIO1 serves the SD card slot; SDIO2 the Wi-Fi radio, which
 * is not modelled, so nothing is on its bus.
 */
typedef struct Sdio {
    uint64_t base;
    int spi;                    /* bcm2712.dtsi */
} Sdio;

static const Sdio sdios[] = {
    { 0x1000fff000ULL, 273 },
    { 0x1001100000ULL, 274 },
};

#define SDIO1                   (&sdios[0])
#define SDIO2                   (&sdios[1])

/* Linux drivers/mmc/host/sdhci.h */
#define SDHCI_DMA_ADDRESS       0x00
#define SDHCI_BLOCK_SIZE        0x04
#define SDHCI_BLOCK_COUNT       0x06
#define SDHCI_ARGUMENT          0x08
#define SDHCI_TRANSFER_MODE     0x0c
#define SDHCI_COMMAND           0x0e
#define SDHCI_RESPONSE          0x10
#define SDHCI_BUFFER            0x20
#define SDHCI_PRESENT_STATE     0x24
#define SDHCI_HOST_CONTROL      0x28
#define SDHCI_POWER_CONTROL     0x29
#define SDHCI_CLOCK_CONTROL     0x2c
#define SDHCI_SOFTWARE_RESET    0x2f
#define SDHCI_INT_STATUS        0x30
#define SDHCI_INT_ENABLE        0x34
#define SDHCI_SIGNAL_ENABLE     0x38
#define SDHCI_CAPABILITIES      0x40
#define SDHCI_CAPABILITIES_1    0x44
#define SDHCI_ADMA_ADDRESS      0x58
#define SDHCI_ADMA_ADDRESS_HI   0x5c
#define SDHCI_HOST_VERSION      0xfe
#define SDHCI_TRNS_DMA          BIT(0)
#define SDHCI_TRNS_BLK_CNT_EN   BIT(1)
#define SDHCI_TRNS_AUTO_CMD12   BIT(2)
#define SDHCI_TRNS_READ         BIT(4)
#define SDHCI_TRNS_MULTI        BIT(5)
#define SDHCI_CMD_RESP_NONE     0x00
#define SDHCI_CMD_RESP_LONG     0x01
#define SDHCI_CMD_RESP_SHORT    0x02
#define SDHCI_CMD_RESP_SHORT_BUSY 0x03
#define SDHCI_CMD_CRC           BIT(3)
#define SDHCI_CMD_INDEX         BIT(4)
#define SDHCI_CMD_DATA          BIT(5)
#define SDHCI_CMD_R1            (SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC | \
                                 SDHCI_CMD_INDEX)
#define SDHCI_CMD_R1B           (SDHCI_CMD_RESP_SHORT_BUSY | SDHCI_CMD_CRC | \
                                 SDHCI_CMD_INDEX)
#define SDHCI_CARD_PRESENT      BIT(16)
#define SDHCI_CTRL_ADMA64       0x18
#define SDHCI_POWER_ON          0x01
#define SDHCI_POWER_330         0x0e
#define SDHCI_CLOCK_INT_EN      BIT(0)
#define SDHCI_CLOCK_CARD_EN     BIT(2)
#define SDHCI_RESET_ALL         BIT(0)
#define SDHCI_INT_RESPONSE      BIT(0)
#define SDHCI_INT_DATA_END      BIT(1)
#define SDHCI_INT_DMA_END       BIT(3)
#define SDHCI_INT_SPACE_AVAIL   BIT(4)
#define SDHCI_INT_DATA_AVAIL    BIT(5)
#define SDHCI_INT_CARD_INSERT   BIT(6)
#define SDHCI_INT_CARD_REMOVE   BIT(7)
#define SDHCI_INT_ERROR         BIT(15)
#define SDHCI_INT_TIMEOUT       BIT(16)

/*
 * The capabilities: a 50 MHz timeout clock, a 200 MHz base clock,
 * 512-byte blocks, 8-bit buses, ADMA2, high speed, SDMA, 3.3 V and 1.8 V,
 * 64-bit addresses; SDR50 with tuning, SDR104 and DDR50
 */
#define SDIO_CAPS               0x156cc8b2
#define SDIO_CAPS_1             0x00002007
#define SDIO_VERSION            0x2402          /* SD Host Controller 3.00 */

#define SDIO_CQE                0x200           /* not modelled */
#define SDIO_CFG                0x400
#define SDIO_CFG_SIZE           0x200

/* ADMA2 descriptors with 64-bit addresses: 12 bytes before version 4 */
#define ADMA2_VALID             BIT(0)
#define ADMA2_END               BIT(1)
#define ADMA2_TRAN              0x20
#define ADMA2_DESC_SIZE         12

/* SD Physical Layer Simplified Specification */
#define SD_GO_IDLE_STATE        0
#define SD_ALL_SEND_CID         2
#define SD_SEND_RELATIVE_ADDR   3
#define SD_SELECT_CARD          7
#define SD_SEND_IF_COND         8
#define SD_READ_SINGLE_BLOCK    17
#define SD_READ_MULTIPLE_BLOCK  18
#define SD_WRITE_SINGLE_BLOCK   24
#define SD_WRITE_MULTIPLE_BLOCK 25
#define SD_APP_OP_COND          41              /* after SD_APP_CMD */
#define SD_APP_CMD              55
#define SD_IF_COND_CHECK        0x1aa           /* 2.7-3.6 V, check pattern */
#define SD_OCR_VDD_32_34        (BIT(20) | BIT(21))
#define SD_OCR_HCS              BIT(30)
#define SD_OCR_BUSY             BIT(31)         /* set once powered up */
#define SD_BLOCK_SIZE           512

/* A card image, of the power-of-two size QEMU's cards need */
#define SD_IMAGE_SIZE           (1 * MiB)

/* The monitor's name for the card in the slot (raspi5b.c) */
#define SD_CARD_QOM_PATH        "/machine/sd-card"

static uint32_t sdio_readl(QTestState *qts, const Sdio *sdio, uint32_t reg)
{
    return qtest_readl(qts, sdio->base + reg);
}

static void sdio_writel(QTestState *qts, const Sdio *sdio, uint32_t reg,
                        uint32_t val)
{
    qtest_writel(qts, sdio->base + reg, val);
}

static void sdio_writew(QTestState *qts, const Sdio *sdio, uint32_t reg,
                        uint16_t val)
{
    qtest_writew(qts, sdio->base + reg, val);
}

static void sdio_writeb(QTestState *qts, const Sdio *sdio, uint32_t reg,
                        uint8_t val)
{
    qtest_writeb(qts, sdio->base + reg, val);
}

static bool sdio_card_present(QTestState *qts, const Sdio *sdio)
{
    return sdio_readl(qts, sdio, SDHCI_PRESENT_STATE) & SDHCI_CARD_PRESENT;
}

/* The card detect line, AON GPIO 5: high with the slot empty */
static bool sd_cdet_high(QTestState *qts)
{
    return gio_readl(qts, GIO_AON, 0, GIO_DATA) & BIT(SD_CDET_AON_GPIO);
}

/* Every byte of the image tells its offset from its neighbours' */
static uint8_t sd_image_byte(uint64_t offset)
{
    return offset * 7 + offset / SD_BLOCK_SIZE;
}

static char *sd_image(void)
{
    g_autofree uint8_t *data = g_malloc(SD_IMAGE_SIZE);
    char *path;
    int fd = g_file_open_tmp("raspi5b-sd-XXXXXX", &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    for (uint64_t i = 0; i < SD_IMAGE_SIZE; i++) {
        data[i] = sd_image_byte(i);
    }
    g_assert_cmpint(write(fd, data, SD_IMAGE_SIZE), ==, SD_IMAGE_SIZE);
    close(fd);
    return path;
}

static void sd_image_read(const char *path, uint64_t offset, void *buf,
                          size_t len)
{
    int fd = open(path, O_RDONLY);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(pread(fd, buf, len, offset), ==, len);
    close(fd);
}

static void sdio_issue(QTestState *qts, const Sdio *sdio, int index,
                       uint32_t arg, uint16_t flags)
{
    sdio_writel(qts, sdio, SDHCI_ARGUMENT, arg);
    sdio_writew(qts, sdio, SDHCI_COMMAND, index << 8 | flags);
}

/*
 * Send a command that the card answers, and acknowledge it (and the end
 * of the busy wait an R1b response brings); returns the response's first
 * word
 */
static uint32_t sdio_command(QTestState *qts, const Sdio *sdio, int index,
                             uint32_t arg, uint16_t flags)
{
    sdio_issue(qts, sdio, index, arg, flags);
    g_assert_cmphex(sdio_readl(qts, sdio, SDHCI_INT_STATUS) &
                    (SDHCI_INT_RESPONSE | SDHCI_INT_ERROR), ==,
                    SDHCI_INT_RESPONSE);
    sdio_writel(qts, sdio, SDHCI_INT_STATUS, SDHCI_INT_RESPONSE |
                (flags & SDHCI_CMD_DATA ? 0 : SDHCI_INT_DATA_END));
    return sdio_readl(qts, sdio, SDHCI_RESPONSE);
}

/* The host powered and clocked, with every status enabled */
static void sdio_start(QTestState *qts, const Sdio *sdio)
{
    sdio_writeb(qts, sdio, SDHCI_SOFTWARE_RESET, SDHCI_RESET_ALL);
    sdio_writeb(qts, sdio, SDHCI_POWER_CONTROL,
                SDHCI_POWER_330 | SDHCI_POWER_ON);
    sdio_writew(qts, sdio, SDHCI_CLOCK_CONTROL,
                SDHCI_CLOCK_INT_EN | SDHCI_CLOCK_CARD_EN);
    sdio_writel(qts, sdio, SDHCI_INT_ENABLE, UINT32_MAX);
}

/* Identify and select the card in SDIO1's slot, as Linux does */
static void sd_card_init(QTestState *qts)
{
    const Sdio *sdio = SDIO1;
    uint32_t rca;

    sdio_start(qts, sdio);
    sdio_command(qts, sdio, SD_GO_IDLE_STATE, 0, SDHCI_CMD_RESP_NONE);
    g_assert_cmphex(sdio_command(qts, sdio, SD_SEND_IF_COND,
                                 SD_IF_COND_CHECK, SDHCI_CMD_R1), ==,
                    SD_IF_COND_CHECK);
    sdio_command(qts, sdio, SD_APP_CMD, 0, SDHCI_CMD_R1);
    g_assert_cmphex(sdio_command(qts, sdio, SD_APP_OP_COND,
                                 SD_OCR_HCS | SD_OCR_VDD_32_34,
                                 SDHCI_CMD_RESP_SHORT) & SD_OCR_BUSY, ==,
                    SD_OCR_BUSY);
    sdio_command(qts, sdio, SD_ALL_SEND_CID, 0,
                 SDHCI_CMD_RESP_LONG | SDHCI_CMD_CRC);
    rca = sdio_command(qts, sdio, SD_SEND_RELATIVE_ADDR, 0,
                       SDHCI_CMD_R1) >> 16;
    sdio_command(qts, sdio, SD_SELECT_CARD, rca << 16, SDHCI_CMD_R1B);
}

/* Start reading @block from the card, a word at a time from the buffer */
static void sd_read_start(QTestState *qts, uint32_t block)
{
    const Sdio *sdio = SDIO1;

    sdio_writew(qts, sdio, SDHCI_BLOCK_SIZE, SD_BLOCK_SIZE);
    sdio_writew(qts, sdio, SDHCI_BLOCK_COUNT, 1);
    sdio_writew(qts, sdio, SDHCI_TRANSFER_MODE, SDHCI_TRNS_READ);
    /* The card has under 2 GiB: it takes byte addresses */
    sdio_command(qts, sdio, SD_READ_SINGLE_BLOCK, block * SD_BLOCK_SIZE,
                 SDHCI_CMD_R1 | SDHCI_CMD_DATA);
    g_assert_true(sdio_readl(qts, sdio, SDHCI_INT_STATUS) &
                  SDHCI_INT_DATA_AVAIL);
}

static void sd_read_words(QTestState *qts, uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i += 4) {
        stl_le_p(buf + i, sdio_readl(qts, SDIO1, SDHCI_BUFFER));
    }
}

static void sd_transfer_end(QTestState *qts, const Sdio *sdio)
{
    g_assert_cmphex(sdio_readl(qts, sdio, SDHCI_INT_STATUS) &
                    (SDHCI_INT_DATA_END | SDHCI_INT_ERROR), ==,
                    SDHCI_INT_DATA_END);
    sdio_writel(qts, sdio, SDHCI_INT_STATUS, UINT32_MAX);
}

static void sd_read_pio(QTestState *qts, uint32_t block, uint8_t *buf)
{
    sd_read_start(qts, block);
    sd_read_words(qts, buf, SD_BLOCK_SIZE);
    sd_transfer_end(qts, SDIO1);
}

static void sd_check_block(const uint8_t *buf, uint32_t block)
{
    for (int i = 0; i < SD_BLOCK_SIZE; i++) {
        g_assert_cmphex(buf[i], ==,
                        sd_image_byte((uint64_t)block * SD_BLOCK_SIZE + i));
    }
}

static void sdio_check_reset(QTestState *qts, const Sdio *sdio, bool card)
{
    g_assert_cmphex(sdio_readl(qts, sdio, SDHCI_CAPABILITIES), ==,
                    SDIO_CAPS);
    g_assert_cmphex(sdio_readl(qts, sdio, SDHCI_CAPABILITIES_1), ==,
                    SDIO_CAPS_1);
    g_assert_cmphex(qtest_readw(qts, sdio->base + SDHCI_HOST_VERSION), ==,
                    SDIO_VERSION);
    g_assert_cmpint(sdio_card_present(qts, sdio), ==, card);
    g_assert_cmphex(sdio_readl(qts, sdio, SDHCI_HOST_CONTROL), ==, 0);
    g_assert_cmphex(sdio_readl(qts, sdio, SDHCI_CLOCK_CONTROL), ==, 0);
    g_assert_cmphex(sdio_readl(qts, sdio, SDHCI_INT_STATUS), ==, 0);
    g_assert_cmphex(sdio_readl(qts, sdio, SDHCI_INT_ENABLE), ==, 0);
    g_assert_cmphex(sdio_readl(qts, sdio, SDHCI_SIGNAL_ENABLE), ==, 0);
    for (int r = 0; r < SDIO_CFG_SIZE; r += 4) {
        g_assert_cmphex(sdio_readl(qts, sdio, SDIO_CFG + r), ==, 0);
    }
    g_assert_false(gic_spi_pending(qts, sdio->spi));
}

/*
 * SD Host Controllers 3.00 with the capabilities of a Pi 5's, SDIO1's
 * slot empty; the gaps, the command queueing engine among them, read as
 * zero
 */
static void test_sdio_reset_values(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");

    for (int i = 0; i < ARRAY_SIZE(sdios); i++) {
        sdio_check_reset(qts, &sdios[i], false);
        g_assert_cmphex(sdio_readl(qts, &sdios[i], 0x100), ==, 0);
        g_assert_cmphex(sdio_readl(qts, &sdios[i], SDIO_CQE), ==, 0);
    }
    g_assert_true(sd_cdet_high(qts));

    qtest_quit(qts);
}

/* The configuration registers keep what is written, each block its own */
static void test_sdio_cfg(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");

    for (int i = 0; i < ARRAY_SIZE(sdios); i++) {
        for (int r = 0; r < SDIO_CFG_SIZE; r += 4) {
            sdio_writel(qts, &sdios[i], SDIO_CFG + r, (i + 1) << 28 | r);
        }
    }
    for (int i = 0; i < ARRAY_SIZE(sdios); i++) {
        for (int r = 0; r < SDIO_CFG_SIZE; r += 4) {
            g_assert_cmphex(sdio_readl(qts, &sdios[i], SDIO_CFG + r), ==,
                            (i + 1) << 28 | r);
        }
    }

    qtest_quit(qts);
}

/* Each controller interrupts on its own SPI */
static void test_sdio_interrupts(void)
{
    QTestState *qts = qtest_init("-machine raspi5b");

    for (int i = 0; i < ARRAY_SIZE(sdios); i++) {
        const Sdio *sdio = &sdios[i];

        sdio_start(qts, sdio);
        sdio_writel(qts, sdio, SDHCI_SIGNAL_ENABLE, SDHCI_INT_RESPONSE);
        sdio_issue(qts, sdio, SD_GO_IDLE_STATE, 0, SDHCI_CMD_RESP_NONE);
        g_assert_cmphex(sdio_readl(qts, sdio, SDHCI_INT_STATUS), ==,
                        SDHCI_INT_RESPONSE);
        g_assert_true(gic_spi_pending(qts, sdio->spi));
        g_assert_false(gic_spi_pending(qts, sdios[!i].spi));
        sdio_writel(qts, sdio, SDHCI_INT_STATUS, SDHCI_INT_RESPONSE);
        g_assert_false(gic_spi_pending(qts, sdio->spi));
    }

    qtest_quit(qts);
}

/*
 * Without a card, a command that expects a response times out; and with
 * -nodefaults and no -drive, the slot has no card to take
 */
static void test_sdio_no_card(void)
{
    QTestState *qts = qtest_init("-machine raspi5b -nodefaults");

    for (int i = 0; i < ARRAY_SIZE(sdios); i++) {
        const Sdio *sdio = &sdios[i];

        g_assert_false(sdio_card_present(qts, sdio));
        sdio_start(qts, sdio);
        sdio_issue(qts, sdio, SD_SEND_IF_COND, SD_IF_COND_CHECK,
                   SDHCI_CMD_R1);
        g_assert_cmphex(sdio_readl(qts, sdio, SDHCI_INT_STATUS), ==,
                        SDHCI_INT_TIMEOUT | SDHCI_INT_ERROR |
                        SDHCI_INT_RESPONSE);
    }
    g_assert_true(sd_cdet_high(qts));

    qtest_quit(qts);
}

static void sd_change(QTestState *qts, const char *image)
{
    qtest_qmp_assert_success(qts, "{ 'execute': 'blockdev-change-medium',"
                             "  'arguments': { 'id': %s, 'filename': %s,"
                             "                 'format': 'raw' } }",
                             SD_CARD_QOM_PATH, image);
}

static void sd_eject(QTestState *qts)
{
    qtest_qmp_assert_success(qts, "{ 'execute': 'eject',"
                             "  'arguments': { 'id': %s } }",
                             SD_CARD_QOM_PATH);
}

/*
 * The card detect switch: the monitor inserts and ejects the card, and
 * SDIO1 sees it come and go, with an interrupt, as does AON GPIO 5, low
 * while a card is in, which Linux watches through cd-gpios
 */
static void test_sdio_card_detect(void)
{
    g_autofree char *image = sd_image();
    QTestState *qts = qtest_init("-machine raspi5b");
    const Sdio *sdio = SDIO1;
    const uint32_t detect = SDHCI_INT_CARD_INSERT | SDHCI_INT_CARD_REMOVE;
    uint8_t buf[SD_BLOCK_SIZE];

    g_assert_false(sdio_card_present(qts, sdio));
    g_assert_true(sd_cdet_high(qts));
    sdio_writel(qts, sdio, SDHCI_INT_ENABLE, detect);
    sdio_writel(qts, sdio, SDHCI_SIGNAL_ENABLE, detect);

    sd_change(qts, image);
    g_assert_true(sdio_card_present(qts, sdio));
    g_assert_false(sd_cdet_high(qts));
    g_assert_cmphex(sdio_readl(qts, sdio, SDHCI_INT_STATUS), ==,
                    SDHCI_INT_CARD_INSERT);
    g_assert_true(gic_spi_pending(qts, sdio->spi));
    sdio_writel(qts, sdio, SDHCI_INT_STATUS, detect);
    g_assert_false(gic_spi_pending(qts, sdio->spi));
    sd_card_init(qts);
    sd_read_pio(qts, 3, buf);
    sd_check_block(buf, 3);
    sdio_writel(qts, sdio, SDHCI_SIGNAL_ENABLE, detect);

    sd_eject(qts);
    g_assert_false(sdio_card_present(qts, sdio));
    g_assert_true(sd_cdet_high(qts));
    g_assert_cmphex(sdio_readl(qts, sdio, SDHCI_INT_STATUS) & detect, ==,
                    SDHCI_INT_CARD_REMOVE);
    g_assert_true(gic_spi_pending(qts, sdio->spi));

    /*
     * A card back in before the guest has seen the last one go: the line
     * follows at once, the controller only once the removal is taken
     */
    sd_change(qts, image);
    g_assert_false(sd_cdet_high(qts));
    qtest_clock_step(qts, 2 * NANOSECONDS_PER_SECOND);
    g_assert_false(sdio_card_present(qts, sdio));
    sdio_writel(qts, sdio, SDHCI_INT_STATUS, detect);
    g_assert_false(gic_spi_pending(qts, sdio->spi));
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND);
    g_assert_true(sdio_card_present(qts, sdio));
    g_assert_cmphex(sdio_readl(qts, sdio, SDHCI_INT_STATUS) & detect, ==,
                    SDHCI_INT_CARD_INSERT);
    g_assert_true(gic_spi_pending(qts, sdio->spi));

    qtest_quit(qts);
    unlink(image);
}

/* Blocks read and written a word at a time through the buffer */
static void test_sdio_pio(void)
{
    g_autofree char *image = sd_image();
    QTestState *qts = qtest_initf("-machine raspi5b "
                                  "-drive if=sd,file=%s,format=raw", image);
    const Sdio *sdio = SDIO1;
    uint8_t buf[SD_BLOCK_SIZE], out[SD_BLOCK_SIZE];

    g_assert_true(sdio_card_present(qts, sdio));
    g_assert_false(sd_cdet_high(qts));
    sd_card_init(qts);
    sd_read_pio(qts, 0, buf);
    sd_check_block(buf, 0);
    sd_read_pio(qts, 1000, buf);
    sd_check_block(buf, 1000);

    for (int i = 0; i < SD_BLOCK_SIZE; i++) {
        out[i] = ~sd_image_byte(i);
    }
    sdio_writew(qts, sdio, SDHCI_BLOCK_SIZE, SD_BLOCK_SIZE);
    sdio_writew(qts, sdio, SDHCI_BLOCK_COUNT, 1);
    sdio_writew(qts, sdio, SDHCI_TRANSFER_MODE, 0);
    sdio_command(qts, sdio, SD_WRITE_SINGLE_BLOCK, 7 * SD_BLOCK_SIZE,
                 SDHCI_CMD_R1 | SDHCI_CMD_DATA);
    g_assert_true(sdio_readl(qts, sdio, SDHCI_INT_STATUS) &
                  SDHCI_INT_SPACE_AVAIL);
    for (int i = 0; i < SD_BLOCK_SIZE; i += 4) {
        sdio_writel(qts, sdio, SDHCI_BUFFER, ldl_le_p(out + i));
    }
    sd_transfer_end(qts, sdio);
    sd_image_read(image, 7 * SD_BLOCK_SIZE, buf, sizeof(buf));
    g_assert_cmpmem(buf, sizeof(buf), out, sizeof(out));
    sd_read_pio(qts, 6, buf);
    sd_check_block(buf, 6);

    qtest_quit(qts);
    unlink(image);
}

/* Fill an ADMA2 table at @table: @count blocks, @stride bytes apart */
static void adma2_table(QTestState *qts, uint64_t table, uint64_t addr,
                        uint64_t stride, int count)
{
    for (int i = 0; i < count; i++) {
        uint8_t desc[ADMA2_DESC_SIZE] = {
            ADMA2_VALID | ADMA2_TRAN | (i == count - 1 ? ADMA2_END : 0),
        };

        stw_le_p(desc + 2, SD_BLOCK_SIZE);
        stq_le_p(desc + 4, addr + i * stride);
        qtest_memwrite(qts, table + i * ADMA2_DESC_SIZE, desc, sizeof(desc));
    }
}

/* Transfer @count blocks at @block by ADMA2, with CMD12 sent at the end */
static void adma2_transfer(QTestState *qts, uint64_t table, uint32_t block,
                           int count, bool read)
{
    const Sdio *sdio = SDIO1;

    sdio_writeb(qts, sdio, SDHCI_HOST_CONTROL, SDHCI_CTRL_ADMA64);
    sdio_writel(qts, sdio, SDHCI_ADMA_ADDRESS, table);
    sdio_writel(qts, sdio, SDHCI_ADMA_ADDRESS_HI, table >> 32);
    sdio_writew(qts, sdio, SDHCI_BLOCK_SIZE, SD_BLOCK_SIZE);
    sdio_writew(qts, sdio, SDHCI_BLOCK_COUNT, count);
    sdio_writew(qts, sdio, SDHCI_TRANSFER_MODE,
                SDHCI_TRNS_DMA | SDHCI_TRNS_BLK_CNT_EN |
                SDHCI_TRNS_AUTO_CMD12 | SDHCI_TRNS_MULTI |
                (read ? SDHCI_TRNS_READ : 0));
    sdio_command(qts, sdio, read ? SD_READ_MULTIPLE_BLOCK
                                 : SD_WRITE_MULTIPLE_BLOCK,
                 block * SD_BLOCK_SIZE, SDHCI_CMD_R1 | SDHCI_CMD_DATA);
    qtest_clock_step(qts, SCALE_MS);
    g_assert_true(gic_spi_pending(qts, sdio->spi));
    sd_transfer_end(qts, sdio);
    g_assert_false(gic_spi_pending(qts, sdio->spi));
}

/*
 * ADMA2 with 64-bit addresses, as Linux uses it: the table and the
 * buffers above 4 GiB, a buffer to a block, the end of the transfer
 * signalled on the controller's SPI
 */
static void test_sdio_adma2(void)
{
    g_autofree char *image = sd_image();
    QTestState *qts = qtest_initf("-machine raspi5b -m 8G "
                                  "-drive if=sd,file=%s,format=raw", image);
    const uint64_t table = 5 * GiB + 0x40, bufs = 6 * GiB + 0x1000;
    const uint64_t stride = 64 * KiB;
    const int count = 3;
    uint8_t buf[SD_BLOCK_SIZE], out[SD_BLOCK_SIZE];

    sd_card_init(qts);
    sdio_writel(qts, SDIO1, SDHCI_SIGNAL_ENABLE, SDHCI_INT_DATA_END);

    adma2_table(qts, table, bufs, stride, count);
    adma2_transfer(qts, table, 20, count, true);
    for (int i = 0; i < count; i++) {
        qtest_memread(qts, bufs + i * stride, buf, sizeof(buf));
        sd_check_block(buf, 20 + i);
    }

    for (int i = 0; i < count; i++) {
        for (int j = 0; j < SD_BLOCK_SIZE; j++) {
            out[j] = i ^ j;
        }
        qtest_memwrite(qts, bufs + i * stride, out, sizeof(out));
    }
    adma2_transfer(qts, table, 40, count, false);
    for (int i = 0; i < count; i++) {
        for (int j = 0; j < SD_BLOCK_SIZE; j++) {
            out[j] = i ^ j;
        }
        sd_image_read(image, (40 + i) * SD_BLOCK_SIZE, buf, sizeof(buf));
        g_assert_cmpmem(buf, sizeof(buf), out, sizeof(out));
    }
    sd_read_pio(qts, 43, buf);
    sd_check_block(buf, 43);

    qtest_quit(qts);
    unlink(image);
}

/*
 * Transfer @count blocks at @block by SDMA to or from @addr, whose
 * multiple of 4 KiB makes each 4 KiB boundary a stop
 */
static void sdma_transfer(QTestState *qts, uint32_t addr, uint32_t block,
                          int count, bool read)
{
    const Sdio *sdio = SDIO1;
    const int per_boundary = 4 * KiB / SD_BLOCK_SIZE;

    sdio_writeb(qts, sdio, SDHCI_HOST_CONTROL, 0);         /* SDMA */
    sdio_writel(qts, sdio, SDHCI_DMA_ADDRESS, addr);
    /* A 4 KiB buffer boundary (0 in bits 14:12) */
    sdio_writew(qts, sdio, SDHCI_BLOCK_SIZE, SD_BLOCK_SIZE);
    sdio_writew(qts, sdio, SDHCI_BLOCK_COUNT, count);
    sdio_writew(qts, sdio, SDHCI_TRANSFER_MODE,
                SDHCI_TRNS_DMA | SDHCI_TRNS_BLK_CNT_EN |
                SDHCI_TRNS_AUTO_CMD12 | SDHCI_TRNS_MULTI |
                (read ? SDHCI_TRNS_READ : 0));
    sdio_command(qts, sdio, read ? SD_READ_MULTIPLE_BLOCK
                                 : SD_WRITE_MULTIPLE_BLOCK,
                 block * SD_BLOCK_SIZE, SDHCI_CMD_R1 | SDHCI_CMD_DATA);
    for (int done = per_boundary; done < count; done += per_boundary) {
        /* Stopped at the boundary until the address to go on at comes */
        g_assert_true(gic_spi_pending(qts, sdio->spi));
        g_assert_cmphex(sdio_readl(qts, sdio, SDHCI_INT_STATUS), ==,
                        SDHCI_INT_DMA_END);
        g_assert_cmphex(sdio_readl(qts, sdio, SDHCI_DMA_ADDRESS), ==,
                        addr + done * SD_BLOCK_SIZE);
        g_assert_cmpuint(sdio_readl(qts, sdio, SDHCI_BLOCK_SIZE) >> 16, ==,
                         count - done);
        sdio_writel(qts, sdio, SDHCI_INT_STATUS, SDHCI_INT_DMA_END);
        g_assert_false(gic_spi_pending(qts, sdio->spi));
        sdio_writel(qts, sdio, SDHCI_DMA_ADDRESS,
                    addr + done * SD_BLOCK_SIZE);
    }
    g_assert_true(gic_spi_pending(qts, sdio->spi));
    sd_transfer_end(qts, sdio);
    g_assert_false(gic_spi_pending(qts, sdio->spi));
}

/*
 * SDMA, as U-Boot uses it: the transfer stops at each buffer boundary
 * with a DMA interrupt, and goes on when the address to continue at is
 * written
 */
static void test_sdio_sdma(void)
{
    g_autofree char *image = sd_image();
    QTestState *qts = qtest_initf("-machine raspi5b "
                                  "-drive if=sd,file=%s,format=raw", image);
    const uint32_t addr = 512 * MiB;
    const int count = 20;           /* 10 KiB: two stops, then a half */
    uint8_t buf[SD_BLOCK_SIZE], out[SD_BLOCK_SIZE];

    sd_card_init(qts);
    sdio_writel(qts, SDIO1, SDHCI_SIGNAL_ENABLE,
                SDHCI_INT_DMA_END | SDHCI_INT_DATA_END);

    sdma_transfer(qts, addr, 60, count, true);
    for (int i = 0; i < count; i++) {
        qtest_memread(qts, addr + i * SD_BLOCK_SIZE, buf, sizeof(buf));
        sd_check_block(buf, 60 + i);
    }
    /* Nothing past the last block */
    qtest_memread(qts, addr + count * SD_BLOCK_SIZE, buf, sizeof(buf));
    memset(out, 0, sizeof(out));
    g_assert_cmpmem(buf, sizeof(buf), out, sizeof(out));

    for (int i = 0; i < count; i++) {
        for (int j = 0; j < SD_BLOCK_SIZE; j++) {
            out[j] = i + j;
        }
        qtest_memwrite(qts, addr + i * SD_BLOCK_SIZE, out, sizeof(out));
    }
    sdma_transfer(qts, addr, 100, count, false);
    for (int i = 0; i < count; i++) {
        for (int j = 0; j < SD_BLOCK_SIZE; j++) {
            out[j] = i + j;
        }
        sd_image_read(image, (100 + i) * SD_BLOCK_SIZE, buf, sizeof(buf));
        g_assert_cmpmem(buf, sizeof(buf), out, sizeof(out));
    }
    sd_read_pio(qts, 120, buf);
    sd_check_block(buf, 120);

    qtest_quit(qts);
    unlink(image);
}

/* A reset brings the controllers back to their reset values, the card in */
static void test_sdio_reset(void)
{
    g_autofree char *image = sd_image();
    QTestState *qts = qtest_initf("-machine raspi5b "
                                  "-drive if=sd,file=%s,format=raw", image);
    uint8_t buf[SD_BLOCK_SIZE];

    sd_card_init(qts);
    for (int i = 0; i < ARRAY_SIZE(sdios); i++) {
        sdio_writel(qts, &sdios[i], SDIO_CFG, 0xc0000000);
        sdio_writel(qts, &sdios[i], SDIO_CFG + SDIO_CFG_SIZE - 4, 1);
    }
    sdio_start(qts, SDIO2);
    sdio_writel(qts, SDIO2, SDHCI_SIGNAL_ENABLE, SDHCI_INT_RESPONSE);
    sdio_issue(qts, SDIO2, SD_GO_IDLE_STATE, 0, SDHCI_CMD_RESP_NONE);
    g_assert_true(gic_spi_pending(qts, SDIO2->spi));

    qtest_system_reset(qts);

    sdio_check_reset(qts, SDIO1, true);
    sdio_check_reset(qts, SDIO2, false);
    g_assert_false(sd_cdet_high(qts));
    /* The card starts over too, from its idle state */
    sd_card_init(qts);
    sd_read_pio(qts, 5, buf);
    sd_check_block(buf, 5);

    qtest_quit(qts);
    unlink(image);
}

/*
 * The registers survive a migration, and so do the card's state and a
 * transfer halfway through the buffer
 */
static void test_sdio_migrate(void)
{
    g_autofree char *image = sd_image();
    g_autofree char *file = g_strdup_printf("%s/raspi5b-sdio-%d.mig",
                                            g_get_tmp_dir(), getpid());
    g_autofree char *out = g_strdup_printf("exec:cat > %s", file);
    g_autofree char *in = g_strdup_printf("exec:cat %s", file);
    g_autofree char *args = g_strdup_printf(
        "-machine raspi5b -m 1G -drive if=sd,file=%s,format=raw", image);
    uint8_t buf[SD_BLOCK_SIZE];
    QTestState *src, *dst;

    src = qtest_init(args);
    for (int i = 0; i < ARRAY_SIZE(sdios); i++) {
        sdio_writel(src, &sdios[i], SDIO_CFG + 0x44, 2 + i);
    }
    sdio_start(src, SDIO2);
    sdio_writel(src, SDIO2, SDHCI_SIGNAL_ENABLE, SDHCI_INT_RESPONSE);
    sdio_issue(src, SDIO2, SD_GO_IDLE_STATE, 0, SDHCI_CMD_RESP_NONE);
    sd_card_init(src);
    sd_read_start(src, 9);
    sd_read_words(src, buf, SD_BLOCK_SIZE / 2);
    qtest_qmp_assert_success(src, "{ 'execute': 'migrate',"
                             "  'arguments': { 'uri': %s } }", out);
    wait_for_migration(src);
    qtest_quit(src);

    dst = qtest_initf("%s -incoming defer", args);
    qtest_qmp_assert_success(dst, "{ 'execute': 'migrate-incoming',"
                             "  'arguments': { 'uri': %s } }", in);
    wait_for_migration(dst);
    for (int i = 0; i < ARRAY_SIZE(sdios); i++) {
        g_assert_cmphex(sdio_readl(dst, &sdios[i], SDIO_CFG + 0x44), ==,
                        2 + i);
    }
    g_assert_cmphex(sdio_readl(dst, SDIO2, SDHCI_INT_STATUS), ==,
                    SDHCI_INT_RESPONSE);
    g_assert_true(gic_spi_pending(dst, SDIO2->spi));
    g_assert_true(sdio_card_present(dst, SDIO1));
    g_assert_false(sd_cdet_high(dst));
    sd_read_words(dst, buf + SD_BLOCK_SIZE / 2, SD_BLOCK_SIZE / 2);
    sd_transfer_end(dst, SDIO1);
    sd_check_block(buf, 9);
    sd_read_pio(dst, 10, buf);
    sd_check_block(buf, 10);

    qtest_quit(dst);
    unlink(file);
    unlink(image);
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
    g_assert_cmphex(gio_readl(qts, gio, 0, GIO_DATA), ==, gio->high[0]);

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
    qtest_add_func("/raspi5b/mbox/clocks", test_mbox_clocks);
    qtest_add_func("/raspi5b/mbox/clock-set", test_mbox_clock_set);
    qtest_add_func("/raspi5b/mbox/clock-unknown", test_mbox_clock_unknown);
    qtest_add_func("/raspi5b/mbox/power", test_mbox_power);
    qtest_add_func("/raspi5b/mbox/temperature", test_mbox_temperature);
    qtest_add_func("/raspi5b/mbox/reboot-flags", test_mbox_reboot_flags);
    qtest_add_func("/raspi5b/mbox/rtc-time", test_mbox_rtc_time);
    qtest_add_func("/raspi5b/mbox/rtc-alarm", test_mbox_rtc_alarm);
    qtest_add_func("/raspi5b/mbox/rtc-battery", test_mbox_rtc_battery);
    qtest_add_func("/raspi5b/mbox/framebuffer", test_mbox_framebuffer);
    qtest_add_func("/raspi5b/mbox/framebuffer-channel",
                   test_mbox_framebuffer_channel);
    qtest_add_func("/raspi5b/mbox/firmware-migrate",
                   test_mbox_firmware_migrate);
    qtest_add_func("/raspi5b/rng/stopped", test_rng_stopped);
    qtest_add_func("/raspi5b/rng/start", test_rng_start);
    qtest_add_func("/raspi5b/rng/soft-reset", test_rng_soft_reset);
    qtest_add_func("/raspi5b/rng/reset", test_rng_reset);
    qtest_add_func("/raspi5b/rng/seed", test_rng_seed);
    qtest_add_func("/raspi5b/avs/temperature", test_avs_temperature);
    qtest_add_func("/raspi5b/avs/global", test_avs_global);
    qtest_add_func("/raspi5b/avs/registers", test_avs_registers);
    qtest_add_func("/raspi5b/avs/reset", test_avs_reset);
    qtest_add_func("/raspi5b/avs/migrate", test_avs_migrate);
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
    qtest_add_func("/raspi5b/uarta/reset-values", test_uarta_reset_values);
    qtest_add_func("/raspi5b/uarta/registers", test_uarta_registers);
    qtest_add_func("/raspi5b/uarta/baud-clock", test_uarta_baud_clock);
    qtest_add_func("/raspi5b/uarta/fifo", test_uarta_fifo);
#ifndef _WIN32
    qtest_add_func("/raspi5b/uarta/transmit-fifo", test_uarta_transmit_fifo);
#endif
    qtest_add_func("/raspi5b/uarta/interrupt", test_uarta_interrupt);
    qtest_add_func("/raspi5b/uarta/serial-port", test_uarta_serial_port);
    qtest_add_func("/raspi5b/uarta/reset", test_uarta_reset);
    qtest_add_func("/raspi5b/uarta/migrate", test_uarta_migrate);
    qtest_add_func("/raspi5b/sdio/reset-values", test_sdio_reset_values);
    qtest_add_func("/raspi5b/sdio/cfg", test_sdio_cfg);
    qtest_add_func("/raspi5b/sdio/interrupts", test_sdio_interrupts);
    qtest_add_func("/raspi5b/sdio/no-card", test_sdio_no_card);
    qtest_add_func("/raspi5b/sdio/card-detect", test_sdio_card_detect);
    qtest_add_func("/raspi5b/sdio/pio", test_sdio_pio);
    qtest_add_func("/raspi5b/sdio/adma2", test_sdio_adma2);
    qtest_add_func("/raspi5b/sdio/sdma", test_sdio_sdma);
    qtest_add_func("/raspi5b/sdio/reset", test_sdio_reset);
    qtest_add_func("/raspi5b/sdio/migrate", test_sdio_migrate);
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
