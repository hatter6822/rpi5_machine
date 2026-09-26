/*
 * GIC-400 tests.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <bm/exception.h>
#include <bm/gic.h>
#include <bm/test.h>
#include <bm/timer.h>
#include <bm/world.h>

/*
 * 288 SPIs + 32 private interrupts: TODO(WS1.3) until GICD_TYPER is read
 * on hardware. Five priority bits: GIC-400 TRM, section 3.1.
 */
TEST(gic_geometry, "gic/geometry")
{
    ASSERT_EQ(gic_num_irqs(), 320);
    ASSERT_EQ(gic_priority_bits(), 5);
}

static volatile unsigned sgi_count, sgi_intid, sgi_core;

static void sgi_handler(unsigned intid, void *arg)
{
    (void)arg;
    sgi_intid = intid;
    sgi_core = this_core();
    sgi_count++;
}

TEST(gic_sgi_self, "gic/sgi-self")
{
    const unsigned sgi = GIC_SGI(1);

    sgi_count = 0;
    irq_register(sgi, sgi_handler, NULL);
    gic_enable(sgi);
    irq_unmask();

    gic_send_sgi(sgi, GIC_SGI_TO_SELF, 0);
    ASSERT(wait_until(sgi_count == 1, 10000));
    ASSERT_EQ(sgi_intid, sgi);
    ASSERT_EQ(sgi_core, 0);

    /* Masked at the CPU, the SGI stays pending until unmasked */
    irq_mask();
    gic_send_sgi(sgi, GIC_SGI_TO_LIST, 1u << 0);
    delay_us(100);
    ASSERT_EQ(sgi_count, 1);
    ASSERT(gic_is_pending(sgi));
    irq_unmask();
    ASSERT(wait_until(sgi_count == 2, 10000));
    ASSERT(!gic_is_pending(sgi));

    irq_mask();
    gic_disable(sgi);
    irq_unregister(sgi);
}

/*
 * gic/security-groups, for a guest that owns EL3: with Group 0 signalled
 * as FIQ and routed to EL3, and Group 1 as IRQ to EL2, drop to
 * Non-secure EL2 and check what each world sees:
 * - a Group 1 SGI sent from Non-secure EL2 is taken there as an IRQ;
 * - a Group 0 SGI sent from there is not forwarded at all;
 * - the Non-secure view of GICD_IGROUPR is RAZ;
 * - the Secure physical timer (Group 0) interrupts Non-secure EL2 and is
 *   taken at EL3 as an FIQ.
 */

#define SGI_GROUP1              GIC_SGI(3)
#define SGI_GROUP0              GIC_SGI(4)
#define PPI_SEC_PHYS            GIC_PPI(13)
#define SPSR_MODE_EL2H          0x9

static volatile unsigned g1_count, g1_el, g0_count, fiq_count, fiq_el;
static volatile uint64_t fiq_interrupted;
static volatile uint32_t ns_igroupr;
static volatile unsigned ns_el;
static volatile bool ns_done;
static uint64_t ns_stack[512] __attribute__((aligned(16)));

static void group1_handler(unsigned intid, void *arg)
{
    (void)intid;
    (void)arg;
    g1_el = current_el();
    g1_count++;
}

static void group0_sgi_handler(unsigned intid, void *arg)
{
    (void)intid;
    (void)arg;
    g0_count++;
}

static void secure_timer_handler(unsigned intid, void *arg)
{
    (void)intid;
    (void)arg;
    fiq_el = current_el();
    fiq_interrupted = read_sysreg(spsr_el3) & 0xf;
    fiq_count++;
    write_sysreg(cntps_ctl_el1, BIT(1));        /* IMASK: silence it */
    isb();
}

/* Runs at Non-secure EL2 */
static void nonsecure_part(void)
{
    ns_el = current_el();
    ns_igroupr = gic_read_group_reg(SGI_GROUP1);
    irq_unmask();
    gic_send_sgi(SGI_GROUP0, GIC_SGI_TO_SELF, 0);
    gic_send_sgi(SGI_GROUP1, GIC_SGI_TO_SELF, 0);
    ns_done = wait_until(g1_count && fiq_count, 100000);
    irq_mask();
}

TEST(gic_security_groups, "gic/security-groups")
{
    uint32_t secure_igroupr;

    if (current_el() != 3) {
        SKIP("runs at EL3 only");
    }
    g1_count = g0_count = fiq_count = 0;
    ns_done = false;
    irq_register(SGI_GROUP1, group1_handler, NULL);
    irq_register(SGI_GROUP0, group0_sgi_handler, NULL);
    irq_register(PPI_SEC_PHYS, secure_timer_handler, NULL);
    gic_set_group(SGI_GROUP1, 1);
    secure_igroupr = gic_read_group_reg(SGI_GROUP1);
    gic_enable(SGI_GROUP1);
    gic_enable(SGI_GROUP0);
    gic_enable(PPI_SEC_PHYS);
    gic_group0_fiq(true);

    write_sysreg(cntps_cval_el1, counter_now() + us_to_ticks(500));
    write_sysreg(cntps_ctl_el1, BIT(0));
    isb();
    bm_run_nonsecure_el2(nonsecure_part, ns_stack + ARRAY_SIZE(ns_stack),
                         SCR_FIQ | SCR_EA);

    write_sysreg(cntps_ctl_el1, 0);
    isb();
    gic_group0_fiq(false);
    gic_disable(PPI_SEC_PHYS);
    gic_disable(SGI_GROUP0);
    gic_disable(SGI_GROUP1);
    gic_clear_pending(SGI_GROUP0);
    gic_set_group(SGI_GROUP1, 0);
    irq_unregister(PPI_SEC_PHYS);
    irq_unregister(SGI_GROUP0);
    irq_unregister(SGI_GROUP1);

    ASSERT_EQ(secure_igroupr, BIT(SGI_GROUP1));
    ASSERT_EQ(ns_el, 2);
    ASSERT_EQ(ns_igroupr, 0);
    ASSERT_MSG(ns_done, "Group 1 SGI taken %u times, FIQ %u times",
               g1_count, fiq_count);
    ASSERT_EQ(g1_count, 1);
    ASSERT_EQ(g1_el, 2);
    ASSERT_EQ(g0_count, 0);
    ASSERT_EQ(fiq_count, 1);
    ASSERT_EQ(fiq_el, 3);
    ASSERT_EQ(fiq_interrupted, SPSR_MODE_EL2H);
}
