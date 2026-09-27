/*
 * brcmstb GPIO tests (WS4.2).
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

/* Registers of bank 0, Linux gpio-brcmstb.c */
#define GIO_DATA                0x04
#define GIO_IODIR               0x08
#define GIO_EC                  0x0c
#define GIO_EI                  0x10
#define GIO_MASK                0x14
#define GIO_LEVEL               0x18
#define GIO_STAT                0x1c

/* Level layout of its level 2 controller ("brcm,bcm7271-l2-intc") */
#define L2_STATUS               0x00
#define L2_MASK_STATUS          0x04
#define L2_MASK_SET             0x08
#define L2_MASK_CLEAR           0x0c

/* Without a device tree: GIO and main_irq, in both raspi5b trees */
#define DEFAULT_GIO             0x107d508500ull
#define DEFAULT_L2              0x107d508400ull
#define DEFAULT_L2_SPI          244
#define DEFAULT_L2_INPUT        0

/* GPIO 12: unused on the Pi 5 ("-" in the firmware's line names) */
#define TEST_LINE               12
#define TEST_BIT                (1u << TEST_LINE)

struct gio {
    uintptr_t base;
    uintptr_t l2;
    uint32_t l2_bit;
    unsigned intid;
};

static volatile unsigned gio_taken;
static volatile uint32_t gio_seen;

static void rmw32(uintptr_t addr, uint32_t clear, uint32_t set)
{
    mmio_write32(addr, (mmio_read32(addr) & ~clear) | set);
}

/*
 * What Linux does: the level 2 handler finds GIO's input pending, and
 * GIO's handler takes each line pending and enabled through
 * handle_level_irq: mask and ack it, handle it, unmask it
 */
static void gio_handler(unsigned intid, void *arg)
{
    const struct gio *gio = arg;
    uint32_t pending;

    (void)intid;
    if (!(mmio_read32(gio->l2 + L2_STATUS) &
          ~mmio_read32(gio->l2 + L2_MASK_STATUS) & gio->l2_bit)) {
        return;
    }
    while ((pending = mmio_read32(gio->base + GIO_STAT) &
                      mmio_read32(gio->base + GIO_MASK))) {
        rmw32(gio->base + GIO_MASK, pending, 0);
        mmio_write32(gio->base + GIO_STAT, pending);
        gio_seen |= pending;
        gio_taken++;
        rmw32(gio->base + GIO_MASK, 0, pending);
    }
}

/* The enabled GPIO block that interrupts, its L2 controller and SPI */
static bool find_gio(struct gio *gio)
{
    int soc = fdt_path_offset("/soc@107c000000");
    int node;

    if (soc < 0) {
        gio->base = DEFAULT_GIO;
        gio->l2 = DEFAULT_L2;
        gio->l2_bit = 1u << DEFAULT_L2_INPUT;
        gio->intid = GIC_SPI(DEFAULT_L2_SPI);
        return true;
    }
    for (node = fdt_first_subnode(soc); node >= 0;
         node = fdt_next_subnode(node)) {
        uint64_t addr, size, l2_addr;
        uint32_t len;
        const void *irqs = fdt_getprop(node, "interrupts", &len);
        int l2 = fdt_interrupt_parent(node);

        if (fdt_node_is_compatible(node, "brcm,brcmstb-gpio") &&
            fdt_node_is_enabled(node) &&
            fdt_getprop(node, "interrupt-controller", NULL) && irqs &&
            len == 4 && fdt_reg(node, 0, &addr, &size) &&
            fdt_node_is_compatible(l2, "brcm,bcm7271-l2-intc") &&
            fdt_node_is_enabled(l2) && fdt_reg(l2, 0, &l2_addr, &size) &&
            fdt_gic_intid(l2, 0, &gio->intid)) {
            gio->base = addr;
            gio->l2 = l2_addr;
            gio->l2_bit = 1u << fdt_cell(irqs, 0);
            return true;
        }
    }
    return false;
}

/*
 * gpio/loopback-irq: GPIO 12 as an output, with rising-edge detection:
 * the block watches the line whatever drives it, so raising it latches
 * its status, which waits while the line's interrupt is disabled and,
 * once enabled, reaches the CPU through the level 2 controller and its
 * SPI. A falling edge then goes unnoticed, and the next rising edge
 * interrupts again.
 */
TEST(gpio_loopback_irq, "gpio/loopback-irq")
{
    struct gio gio;
    bool masked_quiet, falling_quiet;
    unsigned taken_first;

    if (!find_gio(&gio)) {
        SKIP("no enabled brcm,brcmstb-gpio interrupt controller");
    }
    bm_test_note("gpio: 0x%lx, level 2 controller 0x%lx, INTID %u",
                 (unsigned long)gio.base, (unsigned long)gio.l2, gio.intid);

    /* An output driving low, detecting rising edges, disabled */
    mmio_write32(gio.base + GIO_MASK, 0);
    rmw32(gio.base + GIO_DATA, TEST_BIT, 0);
    rmw32(gio.base + GIO_IODIR, TEST_BIT, 0);
    rmw32(gio.base + GIO_LEVEL, TEST_BIT, 0);
    rmw32(gio.base + GIO_EI, TEST_BIT, 0);
    rmw32(gio.base + GIO_EC, 0, TEST_BIT);
    mmio_write32(gio.base + GIO_STAT, TEST_BIT);

    gio_taken = 0;
    gio_seen = 0;
    irq_register(gio.intid, gio_handler, &gio);
    gic_enable(gio.intid);
    mmio_write32(gio.l2 + L2_MASK_CLEAR, gio.l2_bit);
    irq_unmask();

    rmw32(gio.base + GIO_DATA, 0, TEST_BIT);
    delay_us(100);
    masked_quiet = gio_taken == 0 && !gic_is_pending(gio.intid) &&
                   (mmio_read32(gio.base + GIO_DATA) & TEST_BIT) &&
                   mmio_read32(gio.base + GIO_STAT) == TEST_BIT;
    rmw32(gio.base + GIO_MASK, 0, TEST_BIT);
    wait_until(gio_taken != 0, 10000);
    taken_first = gio_taken;

    rmw32(gio.base + GIO_DATA, TEST_BIT, 0);
    delay_us(100);
    falling_quiet = gio_taken == taken_first;
    rmw32(gio.base + GIO_DATA, 0, TEST_BIT);
    wait_until(gio_taken != taken_first, 10000);

    /* Back to an input with the line's interrupt disabled */
    irq_mask();
    rmw32(gio.base + GIO_MASK, TEST_BIT, 0);
    rmw32(gio.base + GIO_IODIR, 0, TEST_BIT);
    rmw32(gio.base + GIO_EC, TEST_BIT, 0);
    mmio_write32(gio.base + GIO_STAT, TEST_BIT);
    mmio_write32(gio.l2 + L2_MASK_SET, gio.l2_bit);
    gic_disable(gio.intid);
    irq_unregister(gio.intid);

    ASSERT_MSG(masked_quiet, "a disabled line interrupted, or did not latch");
    ASSERT_MSG(taken_first == 1, "rising edge taken %u times", taken_first);
    ASSERT_MSG(falling_quiet, "a falling edge interrupted");
    ASSERT_MSG(gio_taken == 2, "interrupt taken %u times", gio_taken);
    ASSERT_EQ(gio_seen, TEST_BIT);
    ASSERT_EQ(mmio_read32(gio.base + GIO_STAT), 0);
    ASSERT(!gic_is_pending(gio.intid));
}
