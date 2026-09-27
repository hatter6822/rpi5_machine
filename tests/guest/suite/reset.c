/*
 * System reset tests (WS3.6).
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Every way of resetting the machine must put every device back in the
 * same state, restart the image the same way and keep RAM. The runtime
 * calls bm_early() before it touches any device, which is where the state
 * the reset left is recorded, as one hash per block.
 */

#include <bm/fdt.h>
#include <bm/gic.h>
#include <bm/io.h>
#include <bm/pm.h>
#include <bm/psci.h>
#include <bm/rng.h>
#include <bm/runtime.h>
#include <bm/test.h>
#include <bm/timer.h>

#define GICD_CTLR               0x000
#define GICD_IGROUPR(n)         (0x080 + 4 * (n))
#define GICD_ISENABLER(n)       (0x100 + 4 * (n))
#define GICD_ISPENDR(n)         (0x200 + 4 * (n))
#define GICD_ISACTIVER(n)       (0x300 + 4 * (n))
#define GICD_IPRIORITYR(n)      (0x400 + 4 * (n))
#define GICD_ITARGETSR(n)       (0x800 + 4 * (n))
#define GICD_ICFGR(n)           (0xc00 + 4 * (n))
#define GICC_CTLR               0x000
#define GICC_PMR                0x004
#define GICC_BPR                0x008

#define ST_CS                   0x00
#define ST_C(n)                 (0x0c + 4 * (n))

#define UART_IBRD               0x24
#define UART_FBRD               0x28
#define UART_LCRH               0x2c
#define UART_CR                 0x30
#define UART_IFLS               0x34
#define UART_IMSC               0x38

#define CNT_CTL_ENABLE          BIT(0)
#define CNT_CTL_IMASK           BIT(1)

/* The SPIs the dirtying touches: unused on raspi5b */
#define SPARE_SPI               GIC_SPI(200)

#define PSCI_RESETS             3       /* then one through the watchdog */

#define MBOX_STATUS             0x18
#define MBOX_CONFIG             0x1c

enum block {
    GIC_DIST, GIC_CPU, SYSTIMER, PM, RNG, MBOX, UART, CPU, NUM_BLOCKS
};

static const char *const block_names[NUM_BLOCKS] = {
    "GIC distributor", "GIC CPU interface", "system timer", "PM", "RNG",
    "mailbox", "UART", "CPU",
};

static uint64_t early[NUM_BLOCKS];

/* FNV-1a over 32-bit words */
static void hash(uint64_t *h, uint32_t word)
{
    for (int i = 0; i < 4; i++) {
        *h = (*h ^ ((word >> (8 * i)) & 0xff)) * 0x100000001b3ull;
    }
}

static void hash_regs(uint64_t *h, uintptr_t base, uint32_t first,
                      uint32_t last)
{
    for (uint32_t off = first; off <= last; off += 4) {
        hash(h, mmio_read32(base + off));
    }
}

/* Every register whose value a reset defines, and no free-running count */
static void snapshot(uint64_t out[NUM_BLOCKS])
{
    uintptr_t d = bm_plat.gicd, c = bm_plat.gicc;
    unsigned words = gic_num_irqs() / 32;

    for (int b = 0; b < NUM_BLOCKS; b++) {
        out[b] = 0xcbf29ce484222325ull;
    }

    hash(&out[GIC_DIST], mmio_read32(d + GICD_CTLR));
    hash_regs(&out[GIC_DIST], d, GICD_IGROUPR(0), GICD_IGROUPR(words - 1));
    hash_regs(&out[GIC_DIST], d, GICD_ISENABLER(0),
              GICD_ISENABLER(words - 1));
    hash_regs(&out[GIC_DIST], d, GICD_ISPENDR(0), GICD_ISPENDR(words - 1));
    hash_regs(&out[GIC_DIST], d, GICD_ISACTIVER(0),
              GICD_ISACTIVER(words - 1));
    hash_regs(&out[GIC_DIST], d, GICD_IPRIORITYR(0),
              GICD_IPRIORITYR(8 * words - 1));
    hash_regs(&out[GIC_DIST], d, GICD_ITARGETSR(8),
              GICD_ITARGETSR(8 * words - 1));
    hash_regs(&out[GIC_DIST], d, GICD_ICFGR(0), GICD_ICFGR(2 * words - 1));
    hash_regs(&out[GIC_CPU], c, GICC_CTLR, GICC_BPR);

    hash(&out[SYSTIMER], mmio_read32(bm_plat.systimer + ST_CS));
    hash_regs(&out[SYSTIMER], bm_plat.systimer, ST_C(0), ST_C(3));

    /* RSTS is left out: it records how the reset happened */
    hash(&out[PM], pm_read(PM_RSTC));
    hash(&out[PM], pm_read(PM_WDOG));

    /* The bit and FIFO counts run free while the generator does */
    hash(&out[RNG], rng_read(RNG_CTRL));
    hash(&out[RNG], rng_read(RNG_TOTAL_BIT_COUNT_THRESHOLD));
    hash(&out[RNG], rng_read(RNG_INT_ENABLE));
    hash(&out[RNG], rng_read(RNG_FIFO_COUNT) & ~RNG_FIFO_COUNT_MASK);

    hash_regs(&out[MBOX], bm_plat.mbox, MBOX_STATUS, MBOX_CONFIG);

    hash_regs(&out[UART], bm_plat.uart, UART_IBRD, UART_IMSC);

    hash(&out[CPU], read_sysreg(cntp_ctl_el0));
    hash(&out[CPU], read_sysreg(cntv_ctl_el0));
    hash(&out[CPU], read_sysreg(daif));
}

void bm_early(void)
{
    snapshot(early);
}

/* Leave something in every block for the reset to clear */
static void dirty(void)
{
    uint64_t far = counter_now() + 1000 * counter_freq();

    gic_set_priority(SPARE_SPI, 0x10);
    gic_set_edge(SPARE_SPI, true);
    gic_set_target(SPARE_SPI, 0x2);
    mmio_write32(bm_plat.gicd + GICD_ISPENDR(SPARE_SPI / 32),
                 BIT(SPARE_SPI % 32));
    gic_enable(SPARE_SPI);
    mmio_write32(bm_plat.gicc + GICC_PMR, 0x80);

    mmio_write32(bm_plat.systimer + ST_C(2), 0xdeadbeef);
    pm_write(PM_WDOG, 0x12345);
    rng_write(RNG_INT_ENABLE, RNG_INT_STATUS_NIST_FAIL);
    rng_write(RNG_TOTAL_BIT_COUNT_THRESHOLD, 0x1234);
    mmio_write32(bm_plat.mbox + MBOX_CONFIG, BIT(0));
    mmio_write32(bm_plat.uart + UART_IMSC, BIT(4));

    write_sysreg(cntp_cval_el0, far);
    write_sysreg(cntp_ctl_el0, CNT_CTL_ENABLE | CNT_CTL_IMASK);
    write_sysreg(cntv_cval_el0, far);
    write_sysreg(cntv_ctl_el0, CNT_CTL_ENABLE | CNT_CTL_IMASK);
    isb();
}

/* Undo dirty() when no reset came to do it */
static void clean(void)
{
    gic_disable(SPARE_SPI);
    gic_clear_pending(SPARE_SPI);
    gic_set_edge(SPARE_SPI, false);
    gic_set_priority(SPARE_SPI, GIC_PRIO_DEFAULT);
    gic_set_target(SPARE_SPI, 0x1);
    mmio_write32(bm_plat.gicc + GICC_PMR, 0xff);
    mmio_write32(bm_plat.uart + UART_IMSC, 0);
    mmio_write32(bm_plat.mbox + MBOX_CONFIG, 0);
    pm_write(PM_WDOG, 0);
    rng_write(RNG_INT_ENABLE, 0);
    rng_write(RNG_TOTAL_BIT_COUNT_THRESHOLD, 0);
    write_sysreg(cntp_ctl_el0, 0);
    write_sysreg(cntv_ctl_el0, 0);
    isb();
}

static void spin(unsigned core)
{
    (void)core;
    for (;;) {
        wfe();
    }
}

/*
 * Reset three times through PSCI and once through the watchdog, dirtying
 * every block before each, and check that each reset leaves the same
 * state as the boot the test started in had (which came from power-on or
 * from the watchdog reset of pm/watchdog-reset), and that the firmware
 * counts every boot in the device tree and gives each a new KASLR seed.
 *
 * Scratch, saved on the first run: [0] the boot count, [1] EL, cores and
 * DT address, [2] the firmware's boot count (FW_NONE without one), [4..]
 * the hashes of the state the boot started with; and on every run, [3]
 * the KASLR seed of that boot.
 */
#define FW_NONE UINT64_MAX

TEST(reset_system, "reset/system-reset")
{
    uint64_t *saved = bm_test_scratch();
    unsigned resets = bm_test_resets();
    bool psci = current_el() < 3;
    uint64_t boot = (uint64_t)current_el() << 56 |
                    (uint64_t)bm_plat.num_cpus << 48 |
                    (bm_plat.has_dtb ? bm_plat.dtb | BIT64(47) : 0);
    int fw = fdt_path_offset("/chosen/bootloader");
    uint32_t count;
    uint64_t seed;

    _Static_assert(4 + NUM_BLOCKS <= BM_TEST_SCRATCH_WORDS, "scratch");

    if (resets == 0) {
        saved[0] = bm_boot_count();
        saved[1] = boot;
        saved[2] = fdt_prop_u32(fw, "count", &count) ? count : FW_NONE;
        for (int b = 0; b < NUM_BLOCKS; b++) {
            saved[4 + b] = early[b];
        }
    } else {
        ASSERT_EQ(bm_boot_count(), saved[0] + resets);
        ASSERT_EQ(boot, saved[1]);
        for (int b = 0; b < NUM_BLOCKS; b++) {
            ASSERT_MSG(early[b] == saved[4 + b],
                       "%s differs after reset %u from when this boot "
                       "started", block_names[b], resets);
        }
        if (saved[2] != FW_NONE) {
            ASSERT(fdt_prop_u32(fw, "count", &count));
            ASSERT_EQ(count, (saved[2] + resets) & 0xff);
            ASSERT(fdt_prop_u64(fdt_path_offset("/chosen"), "kaslr-seed",
                                &seed));
            ASSERT_MSG(seed != saved[3], "the KASLR seed of reset %u is "
                       "the last boot's", resets);
        }
        if (psci) {
            /* Secondaries are off again until the image starts them */
            for (unsigned core = 1; core < bm_plat.num_cpus; core++) {
                int64_t state = psci_call(PSCI_AFFINITY_INFO_64,
                                          (uint64_t)core << 8, 0, 0);

                ASSERT_MSG(state == 1, "core %u: AFFINITY_INFO %ld", core,
                           state);      /* 1 is OFF */
            }
        }
    }
    if (saved[2] != FW_NONE) {
        ASSERT(fdt_prop_u64(fdt_path_offset("/chosen"), "kaslr-seed",
                            &saved[3]));
    }
    if (resets == PSCI_RESETS + 1) {
        bm_test_note("reset/system-reset: %u resets, %s", resets,
                     psci ? "PSCI SYSTEM_RESET then the watchdog"
                          : "the watchdog (no PSCI at EL3)");
        return;
    }

    if (psci && bm_plat.num_cpus > 1) {
        ASSERT_EQ(bm_start_core(1, spin), PSCI_SUCCESS);
    }
    dirty();
    if (psci && resets < PSCI_RESETS) {
        psci_call(PSCI_SYSTEM_RESET, 0, 0, 0);
        clean();
        ASSERT_MSG(false, "PSCI SYSTEM_RESET returned");
    }
    pm_watchdog_start(10);
    wait_until(false, 100000);
    pm_watchdog_stop();
    clean();
    ASSERT_MSG(false, "no reset 100 ms after arming the watchdog");
}
