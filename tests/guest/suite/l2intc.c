/*
 * brcmstb level 2 interrupt controller tests (WS4.1).
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <bm/exception.h>
#include <bm/fdt.h>
#include <bm/gic.h>
#include <bm/io.h>
#include <bm/test.h>
#include <bm/timer.h>

/* Edge layout ("brcm,l2-intc"), Linux irq-brcmstb-l2.c */
#define L2_STATUS               0x00
#define L2_SET                  0x04
#define L2_CLEAR                0x08
#define L2_MASK_STATUS          0x0c
#define L2_MASK_SET             0x10
#define L2_MASK_CLEAR           0x14

/* Without a device tree: cpu_l2_irq, in both raspi5b trees */
#define DEFAULT_L2              0x107d503000ull
#define DEFAULT_L2_SPI          238

#define MAX_L2                  8
#define TEST_BIT                (1u << 19)

struct l2 {
    uintptr_t base;
    unsigned intid;
};

static volatile unsigned l2_taken;
static volatile uint32_t l2_seen;

/* What Linux's handler does: the unmasked status, acked through CLEAR */
static void l2_handler(unsigned intid, void *arg)
{
    const struct l2 *l2 = arg;
    uint32_t pending = mmio_read32(l2->base + L2_STATUS) &
                       ~mmio_read32(l2->base + L2_MASK_STATUS);

    (void)intid;
    mmio_write32(l2->base + L2_CLEAR, pending);
    l2_seen = pending;
    l2_taken++;
}

/* The enabled edge-layout controllers of the "soc" bus, or the default */
static unsigned find_l2s(struct l2 *l2s)
{
    int soc = fdt_path_offset("/soc@107c000000");
    unsigned n = 0;

    if (soc < 0) {
        l2s[0].base = DEFAULT_L2;
        l2s[0].intid = GIC_SPI(DEFAULT_L2_SPI);
        return 1;
    }
    for (int node = fdt_first_subnode(soc); node >= 0 && n < MAX_L2;
         node = fdt_next_subnode(node)) {
        uint64_t addr, size;

        if (fdt_node_is_compatible(node, "brcm,l2-intc") &&
            fdt_node_is_enabled(node) && fdt_reg(node, 0, &addr, &size) &&
            fdt_gic_intid(node, 0, &l2s[n].intid)) {
            l2s[n++].base = addr;
        }
    }
    return n;
}

/*
 * l2-intc/software-set: a status bit raised through SET waits while
 * masked, and once unmasked reaches the CPU through the controller's SPI,
 * where acking it through CLEAR drops it.
 */
TEST(l2_intc_software_set, "l2-intc/software-set")
{
    struct l2 l2s[MAX_L2];
    unsigned n = find_l2s(l2s);

    if (n == 0) {
        SKIP("no enabled brcm,l2-intc node");
    }
    bm_test_note("l2-intc: %u controller(s)", n);

    for (unsigned i = 0; i < n; i++) {
        const struct l2 *l2 = &l2s[i];
        bool masked_quiet;

        mmio_write32(l2->base + L2_MASK_SET, ~0u);
        mmio_write32(l2->base + L2_CLEAR, ~0u);
        l2_taken = 0;
        l2_seen = 0;
        irq_register(l2->intid, l2_handler, (void *)l2);
        gic_enable(l2->intid);
        irq_unmask();

        mmio_write32(l2->base + L2_SET, TEST_BIT);
        delay_us(100);
        masked_quiet = l2_taken == 0 && !gic_is_pending(l2->intid) &&
                       mmio_read32(l2->base + L2_STATUS) == TEST_BIT;
        mmio_write32(l2->base + L2_MASK_CLEAR, TEST_BIT);
        wait_until(l2_taken != 0, 10000);

        irq_mask();
        mmio_write32(l2->base + L2_MASK_SET, TEST_BIT);
        gic_disable(l2->intid);
        irq_unregister(l2->intid);

        ASSERT_MSG(masked_quiet, "0x%lx: a masked bit raised the SPI",
                   (unsigned long)l2->base);
        ASSERT_MSG(l2_taken == 1, "0x%lx: interrupt taken %u times",
                   (unsigned long)l2->base, l2_taken);
        ASSERT_EQ(l2_seen, TEST_BIT);
        ASSERT_EQ(mmio_read32(l2->base + L2_STATUS), 0);
        ASSERT(!gic_is_pending(l2->intid));
    }
}
