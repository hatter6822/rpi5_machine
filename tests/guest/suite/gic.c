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
