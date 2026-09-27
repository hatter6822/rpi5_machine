/*
 * Tests that run on every core: its MPIDR, per-core timers, SGIs between
 * every pair of cores, SPI routing, and PSCI CPU_ON/CPU_OFF.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Secondaries cannot print, so each reports its first failure through
 * core_error[] and core 0 turns it into the test's result.
 */

#include <bm/exception.h>
#include <bm/gic.h>
#include <bm/psci.h>
#include <bm/runtime.h>
#include <bm/test.h>
#include <bm/timer.h>

/* GIC-400 PPIs of the Cortex-A76 timers (bcm2712.dtsi "timer" node) */
#define PPI_NS_PHYS             GIC_PPI(14)
#define PPI_VIRT                GIC_PPI(11)

#define CNT_CTL_ENABLE          BIT(0)
#define CNT_CTL_IMASK           BIT(1)

/* Written by their own core only */
static volatile unsigned core_arrived[BM_MAX_CPUS];
static volatile bool core_done[BM_MAX_CPUS];
static const char *volatile core_error[BM_MAX_CPUS];
static volatile uint64_t core_error_value[BM_MAX_CPUS];

static void (*volatile core_body)(unsigned core);

#define CORE_FAIL(core, msg, value) do {                                \
    core_error[core] = (msg);                                           \
    core_error_value[core] = (value);                                   \
} while (0)

/* Wait until every core reaches barrier @gen (numbered from 1) */
static bool barrier(unsigned core, unsigned gen)
{
    core_arrived[core] = gen;
    sev();
    for (unsigned c = 0; c < bm_plat.num_cpus; c++) {
        if (!wait_until(core_arrived[c] >= gen, 1000000)) {
            return false;
        }
    }
    return true;
}

static void secondary(unsigned core)
{
    core_body(core);
    core_done[core] = true;
}

/*
 * Run @body on every core at once, core 0 included, and wait for all of
 * them; a core's failure is reported as the test's. Evaluates to whether
 * every core succeeded, so a test can stop at the first failure.
 */
static bool on_every_core(void (*body)(unsigned core))
{
    unsigned n = bm_plat.num_cpus;

    core_body = body;
    for (unsigned c = 0; c < BM_MAX_CPUS; c++) {
        core_arrived[c] = 0;
        core_done[c] = false;
        core_error[c] = NULL;
    }
    dsb_sy();
    for (unsigned c = 1; c < n; c++) {
        int64_t ret = bm_start_core(c, secondary);

        if (ret != PSCI_SUCCESS) {
            CORE_FAIL(c, "could not be started", (uint64_t)ret);
        }
    }
    body(0);
    for (unsigned c = 1; c < n; c++) {
        if (!core_error[c] &&
            !wait_until(core_done[c] && bm_core_is_off(c), 1000000)) {
            CORE_FAIL(c, "did not finish", 0);
        }
    }
    /* Secondaries first: a core that failed stalls the others' barriers */
    for (unsigned i = 1; i <= n; i++) {
        unsigned c = i % n;

        if (core_error[c]) {
            bm_test_fail(__FILE__, __LINE__, "core %u: %s (0x%lx)", c,
                         core_error[c], core_error_value[c]);
            return false;
        }
    }
    return true;
}

/*
 * smp/mpidr: each core's MPIDR_EL1 as the silicon reports it. A DynamIQ
 * core sets the MT bit and numbers itself in Aff1, with thread 0 in Aff0;
 * U is clear, as the cores share a cluster, and Aff2 and Aff3 are 0.
 */

#define MPIDR_RES1              BIT64(31)
#define MPIDR_MT                BIT64(24)

static void mpidr_body(unsigned core)
{
    uint64_t mpidr = read_sysreg(mpidr_el1);

    if (mpidr != (MPIDR_RES1 | MPIDR_MT | (uint64_t)core << 8)) {
        CORE_FAIL(core, "unexpected MPIDR_EL1", mpidr);
    }
}

TEST(smp_mpidr, "smp/mpidr")
{
    on_every_core(mpidr_body);
}

/* timer/every-core: the EL1 physical and virtual timers of each core */

enum { T_PHYS, T_VIRT, NUM_TIMERS };

static volatile unsigned timer_count[BM_MAX_CPUS][NUM_TIMERS];
static volatile uint64_t timer_at[BM_MAX_CPUS][NUM_TIMERS];

static uint64_t timer_now(unsigned t)
{
    isb();
    return t == T_PHYS ? read_sysreg(cntpct_el0) : read_sysreg(cntvct_el0);
}

static void timer_ctl(unsigned t, uint64_t ctl)
{
    if (t == T_PHYS) {
        write_sysreg(cntp_ctl_el0, ctl);
    } else {
        write_sysreg(cntv_ctl_el0, ctl);
    }
    isb();
}

static void timer_handler(unsigned intid, void *arg)
{
    unsigned t = (uintptr_t)arg, core = this_core();

    (void)intid;
    timer_at[core][t] = timer_now(t);
    timer_count[core][t]++;
    timer_ctl(t, CNT_CTL_IMASK);        /* level-sensitive: silence it */
}

/*
 * The virtual timer runs one second behind zero: CNTVOFF_EL2 is larger
 * than the physical count, so the virtual count has wrapped, and the
 * timer must still fire when it reaches CVAL.
 */
static void timers_body(unsigned core)
{
    static const unsigned intid[NUM_TIMERS] = { PPI_NS_PHYS, PPI_VIRT };
    uint64_t voff_saved = read_sysreg(cntvoff_el2);
    uint64_t voff = counter_now() + counter_freq();
    uint64_t p0, v, p1;

    /*
     * CNTVCT = CNTPCT - CNTVOFF, modulo 2^64: the physical count it gives
     * lies between the physical counts read before and after it, however
     * long a busy host deschedules the vCPU in between
     */
    write_sysreg(cntvoff_el2, voff);
    p0 = timer_now(T_PHYS);
    v = timer_now(T_VIRT);
    p1 = timer_now(T_PHYS);
    if (v + voff - p0 > p1 - p0) {
        CORE_FAIL(core, "CNTPCT - CNTVCT is not CNTVOFF", p0 - v);
    }

    for (unsigned t = 0; t < NUM_TIMERS && !core_error[core]; t++) {
        uint64_t cval = timer_now(t) + us_to_ticks(200);
        bool fired;

        timer_count[core][t] = 0;
        gic_enable(intid[t]);           /* banked: this core's PPI */
        if (t == T_PHYS) {
            write_sysreg(cntp_cval_el0, cval);
        } else {
            write_sysreg(cntv_cval_el0, cval);
        }
        timer_ctl(t, CNT_CTL_ENABLE);
        irq_unmask();
        fired = wait_until(timer_count[core][t], 100000);
        irq_mask();
        timer_ctl(t, 0);
        gic_disable(intid[t]);

        if (!fired) {
            CORE_FAIL(core, t == T_PHYS ? "physical timer did not fire"
                                        : "virtual timer did not fire", 0);
        } else if (timer_at[core][t] < cval) {
            CORE_FAIL(core, "timer fired early", timer_at[core][t]);
        } else if (timer_count[core][t] != 1) {
            CORE_FAIL(core, "timer fired more than once",
                      timer_count[core][t]);
        }
    }
    write_sysreg(cntvoff_el2, voff_saved);
}

TEST(timer_every_core, "timer/every-core")
{
    irq_register(PPI_NS_PHYS, timer_handler, (void *)(uintptr_t)T_PHYS);
    irq_register(PPI_VIRT, timer_handler, (void *)(uintptr_t)T_VIRT);
    on_every_core(timers_body);
    irq_unregister(PPI_NS_PHYS);
    irq_unregister(PPI_VIRT);
}

/*
 * smp/sgi: every core sends an SGI to every other core by target list,
 * then one to all the others by filter. Each must arrive exactly once,
 * with the sender's number in GICC_IAR.
 */

#define SGI_PAIRS               GIC_SGI(2)

/* sgi_seen[receiver][sender], written by the receiver only */
static volatile unsigned sgi_seen[BM_MAX_CPUS][BM_MAX_CPUS];

static void sgi_handler(unsigned intid, void *arg)
{
    (void)intid;
    (void)arg;
    sgi_seen[this_core()][irq_sgi_source()]++;
}

static bool sgi_row_done(unsigned core, unsigned count)
{
    for (unsigned c = 0; c < bm_plat.num_cpus; c++) {
        if (c != core && sgi_seen[core][c] < count) {
            return false;
        }
    }
    return true;
}

/*
 * A GICv2 SGI stays pending per sender, so a second one from the same
 * core merges with the first until that is taken: each round ends with a
 * barrier once every core has taken all of its SGIs.
 */
static void sgi_body(unsigned core)
{
    for (unsigned c = 0; c < BM_MAX_CPUS; c++) {
        sgi_seen[core][c] = 0;
    }
    gic_enable(SGI_PAIRS);
    irq_unmask();
    if (!barrier(core, 1)) {
        CORE_FAIL(core, "timed out at the start", 0);
        goto out;
    }
    for (unsigned c = 0; c < bm_plat.num_cpus; c++) {
        if (c != core) {
            gic_send_sgi(SGI_PAIRS, GIC_SGI_TO_LIST, 1u << c);
        }
    }
    if (!wait_until(sgi_row_done(core, 1), 100000)) {
        CORE_FAIL(core, "missing SGIs sent by target list", 0);
        goto out;
    }
    if (!barrier(core, 2)) {
        CORE_FAIL(core, "timed out after the target-list round", 0);
        goto out;
    }
    gic_send_sgi(SGI_PAIRS, GIC_SGI_TO_OTHERS, 0);
    if (!wait_until(sgi_row_done(core, 2), 100000)) {
        CORE_FAIL(core, "missing SGIs sent to all others", 0);
        goto out;
    }
    /* No one stops listening while another core may still send */
    if (!barrier(core, 3)) {
        CORE_FAIL(core, "timed out at the end", 0);
    }
out:
    irq_mask();
    gic_disable(SGI_PAIRS);
}

TEST(smp_sgi, "smp/sgi")
{
    irq_register(SGI_PAIRS, sgi_handler, NULL);
    bool ok = on_every_core(sgi_body);

    irq_unregister(SGI_PAIRS);
    if (!ok) {
        return;
    }
    for (unsigned to = 0; to < bm_plat.num_cpus; to++) {
        for (unsigned from = 0; from < bm_plat.num_cpus; from++) {
            ASSERT_MSG(sgi_seen[to][from] == (to == from ? 0 : 2),
                       "core %u got %u SGIs from core %u", to,
                       sgi_seen[to][from], from);
        }
    }
    bm_test_note("smp/sgi: %u cores", bm_plat.num_cpus);
}

/*
 * smp/spi-routing: system timer comparator 3 (one the ARM owns; the
 * VideoCore uses 0 and 2 on hardware) is routed to each core in turn with
 * GICD_ITARGETSR, and only that core may take it.
 */

#define ST_CS                   0x00
#define ST_CLO                  0x04
#define ST_C(n)                 (0x0c + 4 * (n))
#define ST_ROUTED               3

static volatile unsigned spi_taken[BM_MAX_CPUS];
static volatile bool spi_stop;

static void spi_handler(unsigned intid, void *arg)
{
    (void)intid;
    (void)arg;
    mmio_write32(bm_plat.systimer + ST_CS, BIT(ST_ROUTED));
    spi_taken[this_core()]++;
}

/*
 * Raise the SPI and wait for @target to take it. The comparator matches
 * when the counter's low 32 bits equal it, so a vCPU that a busy host
 * deschedules between reading the counter and writing the comparator can
 * arm it a wrap of the counter away: arm it again if the counter had
 * reached the match by the time it was written.
 */
static bool spi_raise(unsigned target)
{
    for (unsigned tries = 0; tries < 3; tries++) {
        uint32_t match = mmio_read32(bm_plat.systimer + ST_CLO) + 200;
        uint32_t armed;

        mmio_write32(bm_plat.systimer + ST_C(ST_ROUTED), match);
        armed = mmio_read32(bm_plat.systimer + ST_CLO);
        if (wait_until(spi_taken[target] == 1, 100000)) {
            return true;
        }
        if ((int32_t)(armed - match) < 0) {
            break;                      /* armed in time */
        }
    }
    return false;
}

static void spi_body(unsigned core)
{
    unsigned intid = bm_plat.systimer_intid[ST_ROUTED];

    spi_taken[core] = 0;
    irq_unmask();
    if (!barrier(core, 1)) {
        CORE_FAIL(core, "timed out at the start", 0);
    } else if (core != 0) {
        /* Core 0 takes up to 300 ms for each core */
        if (!wait_until(spi_stop, 2000000)) {
            CORE_FAIL(core, "not stopped", 0);
        }
    } else {
        for (unsigned target = 0; target < bm_plat.num_cpus; target++) {
            gic_set_target(intid, 1u << target);
            if (!spi_raise(target)) {
                CORE_FAIL(core, "the SPI did not reach its target", target);
                break;
            }
        }
        spi_stop = true;
        sev();
    }
    irq_mask();
}

TEST(smp_spi_routing, "smp/spi-routing")
{
    unsigned intid = bm_plat.systimer_intid[ST_ROUTED];
    bool ok;

    spi_stop = false;
    mmio_write32(bm_plat.systimer + ST_CS, BIT(ST_ROUTED));
    irq_register(intid, spi_handler, NULL);
    gic_enable(intid);
    ok = on_every_core(spi_body);
    gic_disable(intid);
    gic_set_target(intid, 1u << 0);
    irq_unregister(intid);
    if (!ok) {
        return;
    }
    for (unsigned c = 0; c < bm_plat.num_cpus; c++) {
        ASSERT_MSG(spi_taken[c] == 1, "core %u took the SPI %u times", c,
                   spi_taken[c]);
    }
}

/*
 * psci/cpu-on-off: CPU_ON of a running core is refused, a core that
 * turned itself off can be started again, and cores that do not exist
 * are rejected.
 */

static volatile bool hold_core;
static volatile unsigned held_runs;

static void held(unsigned core)
{
    (void)core;
    held_runs++;
    sev();
    wait_until(!hold_core, 1000000);
}

TEST(psci_cpu_on_off, "psci/cpu-on-off")
{
    const uint64_t absent = 0xffull << 8;

    if (current_el() == 3) {
        SKIP("no PSCI below a guest that owns EL3");
    }
    ASSERT_EQ(psci_cpu_on(0, 0, 0), PSCI_ALREADY_ON);  /* this core */
    ASSERT_EQ(psci_cpu_on(absent, 0, 0), PSCI_INVALID_PARAMS);
    ASSERT_EQ(psci_call(PSCI_AFFINITY_INFO_64, absent, 0, 0),
              PSCI_INVALID_PARAMS);
    ASSERT_EQ(psci_call(PSCI_AFFINITY_INFO_64, 0, 0, 0), 0);    /* ON */
    if (bm_plat.num_cpus < 2) {
        SKIP("one core");
    }

    held_runs = 0;
    for (unsigned run = 1; run <= 2; run++) {
        hold_core = true;
        ASSERT_EQ(bm_start_core(1, held), PSCI_SUCCESS);
        ASSERT(wait_until(held_runs == run, 1000000));
        ASSERT_EQ(bm_start_core(1, held), PSCI_ALREADY_ON);
        ASSERT_EQ(psci_call(PSCI_AFFINITY_INFO_64, 1 << 8, 0, 0), 0);
        hold_core = false;
        ASSERT_MSG(wait_until(bm_core_is_off(1), 1000000),
                   "run %u: core 1 did not turn off", run);
    }
}
