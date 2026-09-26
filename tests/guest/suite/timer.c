/*
 * Arm generic timer tests.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <bm/exception.h>
#include <bm/gic.h>
#include <bm/test.h>
#include <bm/timer.h>

/* GIC-400 PPIs of the Cortex-A76 timers (bcm2712.dtsi "timer" node) */
#define PPI_SEC_PHYS            GIC_PPI(13)
#define PPI_NS_PHYS             GIC_PPI(14)
#define PPI_HYP_PHYS            GIC_PPI(10)

#define CNT_CTL_ENABLE          BIT(0)
#define CNT_CTL_IMASK           BIT(1)
#define CNT_CTL_ISTATUS         BIT(2)

/* The 54 MHz crystal (Raspberry Pi processor documentation, BCM2712) */
TEST(timer_frequency, "timer/frequency")
{
    ASSERT_EQ(counter_freq(), 54000000);
}

enum which_timer { TIMER_EL1_PHYS, TIMER_EL2_PHYS, TIMER_SEC_PHYS };

static void timer_write_ctl(enum which_timer t, uint64_t v)
{
    switch (t) {
    case TIMER_EL1_PHYS:
        write_sysreg(cntp_ctl_el0, v);
        break;
    case TIMER_EL2_PHYS:
        write_sysreg(cnthp_ctl_el2, v);
        break;
    case TIMER_SEC_PHYS:
        write_sysreg(cntps_ctl_el1, v);
        break;
    }
    isb();
}

static void timer_write_cval(enum which_timer t, uint64_t v)
{
    switch (t) {
    case TIMER_EL1_PHYS:
        write_sysreg(cntp_cval_el0, v);
        break;
    case TIMER_EL2_PHYS:
        write_sysreg(cnthp_cval_el2, v);
        break;
    case TIMER_SEC_PHYS:
        write_sysreg(cntps_cval_el1, v);
        break;
    }
}

static volatile unsigned timer_fired;
static volatile uint64_t timer_fired_at;

static void timer_handler(unsigned intid, void *arg)
{
    enum which_timer t = (enum which_timer)(uintptr_t)arg;

    (void)intid;
    timer_fired_at = counter_now();
    timer_fired++;
    timer_write_ctl(t, CNT_CTL_IMASK);      /* level-sensitive: silence it */
}

/* Fire @t 200 us from now and check it arrives, on time, exactly once */
static void check_timer_irq(enum which_timer t, unsigned intid)
{
    uint64_t cval = counter_now() + us_to_ticks(200);

    timer_fired = 0;
    irq_register(intid, timer_handler, (void *)(uintptr_t)t);
    gic_enable(intid);
    timer_write_cval(t, cval);
    timer_write_ctl(t, CNT_CTL_ENABLE);
    irq_unmask();

    bool fired = wait_until(timer_fired, 100000);

    irq_mask();
    timer_write_ctl(t, 0);
    gic_disable(intid);
    irq_unregister(intid);
    ASSERT(fired);
    ASSERT_GE(timer_fired_at, cval);
    ASSERT_EQ(timer_fired, 1);
}

TEST(timer_phys, "timer/el1-physical")
{
    check_timer_irq(TIMER_EL1_PHYS, PPI_NS_PHYS);
}

TEST(timer_hyp, "timer/el2-physical")
{
    if (current_el() != 2) {
        SKIP("runs at EL2 only");
    }
    check_timer_irq(TIMER_EL2_PHYS, PPI_HYP_PHYS);
}

TEST(timer_secure, "timer/secure-physical")
{
    if (current_el() != 3) {
        SKIP("runs at EL3 only");
    }
    check_timer_irq(TIMER_SEC_PHYS, PPI_SEC_PHYS);
}
