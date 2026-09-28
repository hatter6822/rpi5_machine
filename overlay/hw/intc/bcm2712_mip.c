/*
 * Broadcom BCM2712 MIP MSI interrupt controller
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * No public datasheet: registers and semantics as used by Linux
 * drivers/irqchip/irq-bcm2712-mip.c and the Raspberry Pi 5 device tree.
 *
 * The MIP turns message-signalled interrupts from PCIe into SPIs. The
 * device tree gives PCIe devices the page of its registers as their MSI
 * address (through the root complex's inbound window for it) and the
 * vector number as their MSI data, so an MSI is a write of n to
 * INT_RAISE, which latches bit n of the status. A vector configured as
 * an edge (a set bit in INT_CFGL/H_HOST, as Linux configures all of
 * them) pulses its output when raised, unless masked for the host; a
 * vector configured as a level drives its output while it is raised and
 * unmasked. Writing n to INT_CLEAR clears bit n. Linux never clears the
 * status, as its interrupts are edges.
 *
 * TODO(WS0.4): check on hardware that INT_CLEAR takes a vector number
 * like INT_RAISE, not a mask; which values the masks and configuration
 * reset to (masked and level here); and whether the VPU's status differs
 * from the host's (both read the one status here).
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/registerfields.h"
#include "hw/intc/bcm2712_mip.h"
#include "migration/vmstate.h"
#include "trace.h"

REG32(INT_RAISE,        0x00)
REG32(INT_CLEAR,        0x10)
REG32(INT_CFGL_HOST,    0x20)
REG32(INT_CFGH_HOST,    0x30)
REG32(INT_MASKL_HOST,   0x40)
REG32(INT_MASKH_HOST,   0x50)
REG32(INT_MASKL_VPU,    0x60)
REG32(INT_MASKH_VPU,    0x70)
REG32(INT_STATUSL_HOST, 0x80)
REG32(INT_STATUSH_HOST, 0x90)
REG32(INT_STATUSL_VPU,  0xa0)
REG32(INT_STATUSH_VPU,  0xb0)

/* Drive the outputs: those of edge vectors are low between pulses */
static void bcm2712_mip_update(BCM2712MIPState *s)
{
    uint64_t level = s->status & ~s->mask & ~s->cfg;

    for (int n = 0; n < BCM2712_MIP_NUM_IRQS; n++) {
        qemu_set_irq(s->irq[n], extract64(level, n, 1));
    }
}

static void bcm2712_mip_raise(BCM2712MIPState *s, uint32_t n)
{
    if (n >= BCM2712_MIP_NUM_IRQS) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: no vector %u\n",
                      TYPE_BCM2712_MIP, n);
        return;
    }
    trace_bcm2712_mip_raise(n);
    s->status |= BIT_ULL(n);
    if (extract64(s->cfg, n, 1)) {
        if (!extract64(s->mask, n, 1)) {
            qemu_irq_pulse(s->irq[n]);
        }
    } else {
        bcm2712_mip_update(s);
    }
}

static void bcm2712_mip_clear(BCM2712MIPState *s, uint32_t n)
{
    if (n >= BCM2712_MIP_NUM_IRQS) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: no vector %u\n",
                      TYPE_BCM2712_MIP, n);
        return;
    }
    s->status &= ~BIT_ULL(n);
    bcm2712_mip_update(s);
}

/* The 64-bit register a low or high word belongs to, or NULL */
static uint64_t *bcm2712_mip_reg(BCM2712MIPState *s, hwaddr addr)
{
    switch (addr & ~0x10) {
    case A_INT_CFGL_HOST:
        return &s->cfg;
    case A_INT_MASKL_HOST:
        return &s->mask;
    case A_INT_MASKL_VPU:
        return &s->vpu_mask;
    case A_INT_STATUSL_HOST:
    case A_INT_STATUSL_VPU:
        return &s->status;
    }
    return NULL;
}

static uint64_t bcm2712_mip_read(void *opaque, hwaddr addr, unsigned size)
{
    BCM2712MIPState *s = BCM2712_MIP(opaque);
    uint64_t *reg = bcm2712_mip_reg(s, addr);
    uint32_t value = 0;

    if (reg && !(addr & 0xf)) {
        value = extract64(*reg, addr & 0x10 ? 32 : 0, 32);
    } else if (addr != A_INT_RAISE && addr != A_INT_CLEAR) {
        qemu_log_mask(LOG_UNIMP, "%s: read of unknown register 0x%02"
                      HWADDR_PRIx "\n", TYPE_BCM2712_MIP, addr);
    }
    trace_bcm2712_mip_read(addr, value);
    return value;
}

static void bcm2712_mip_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    BCM2712MIPState *s = BCM2712_MIP(opaque);
    uint64_t *reg = bcm2712_mip_reg(s, addr);

    trace_bcm2712_mip_write(addr, value);
    switch (addr) {
    case A_INT_RAISE:
        bcm2712_mip_raise(s, value);
        return;
    case A_INT_CLEAR:
        bcm2712_mip_clear(s, value);
        return;
    case A_INT_STATUSL_HOST:
    case A_INT_STATUSH_HOST:
    case A_INT_STATUSL_VPU:
    case A_INT_STATUSH_VPU:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write to read-only register "
                      "0x%02" HWADDR_PRIx "\n", TYPE_BCM2712_MIP, addr);
        return;
    }
    if (!reg || (addr & 0xf)) {
        qemu_log_mask(LOG_UNIMP, "%s: write to unknown register 0x%02"
                      HWADDR_PRIx "\n", TYPE_BCM2712_MIP, addr);
        return;
    }
    *reg = deposit64(*reg, addr & 0x10 ? 32 : 0, 32, value);
    bcm2712_mip_update(s);
}

static const MemoryRegionOps bcm2712_mip_ops = {
    .read = bcm2712_mip_read,
    .write = bcm2712_mip_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void bcm2712_mip_reset_enter(Object *obj, ResetType type)
{
    BCM2712MIPState *s = BCM2712_MIP(obj);

    s->status = 0;
    s->cfg = 0;
    s->mask = UINT64_MAX;
    s->vpu_mask = UINT64_MAX;
}

static void bcm2712_mip_reset_hold(Object *obj, ResetType type)
{
    bcm2712_mip_update(BCM2712_MIP(obj));
}

static void bcm2712_mip_init(Object *obj)
{
    BCM2712MIPState *s = BCM2712_MIP(obj);

    memory_region_init_io(&s->iomem, obj, &bcm2712_mip_ops, s,
                          TYPE_BCM2712_MIP, BCM2712_MIP_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    qdev_init_gpio_out(DEVICE(obj), s->irq, BCM2712_MIP_NUM_IRQS);
}

static const VMStateDescription vmstate_bcm2712_mip = {
    .name = TYPE_BCM2712_MIP,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(status, BCM2712MIPState),
        VMSTATE_UINT64(cfg, BCM2712MIPState),
        VMSTATE_UINT64(mask, BCM2712MIPState),
        VMSTATE_UINT64(vpu_mask, BCM2712MIPState),
        VMSTATE_END_OF_LIST()
    }
};

static void bcm2712_mip_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.enter = bcm2712_mip_reset_enter;
    rc->phases.hold = bcm2712_mip_reset_hold;
    dc->vmsd = &vmstate_bcm2712_mip;
    dc->desc = "Broadcom BCM2712 MIP MSI interrupt controller";
}

static const TypeInfo bcm2712_mip_types[] = {
    {
        .name           = TYPE_BCM2712_MIP,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(BCM2712MIPState),
        .instance_init  = bcm2712_mip_init,
        .class_init     = bcm2712_mip_class_init,
    },
};

DEFINE_TYPES(bcm2712_mip_types)
