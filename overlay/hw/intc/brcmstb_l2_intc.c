/*
 * Broadcom set-top-box (brcmstb) level 2 interrupt controller
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * No public datasheet: registers and semantics as used by Linux
 * drivers/irqchip/irq-brcmstb-l2.c, which knows two layouts:
 *
 * - edge ("brcm,l2-intc", "brcm,bcm2711-l2-intc"): STATUS latches a rising
 *   input and holds it until written to CLEAR; SET raises status bits from
 *   software. The driver acks through CLEAR.
 * - level ("brcm,bcm7271-l2-intc"): STATUS follows the inputs, and there
 *   is nothing to ack.
 *
 * Both have a mask with write-one-to-set and write-one-to-clear views,
 * and one output: the OR of STATUS & ~MASK. Reset masks every input.
 * The write-only registers (SET, CLEAR, MASK_SET, MASK_CLEAR) read as
 * zero; Linux never reads them. TODO(WS0.4): check on hardware.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/registerfields.h"
#include "hw/intc/brcmstb_l2_intc.h"
#include "migration/vmstate.h"
#include "trace.h"

/* Edge layout (brcmstb_l2_edge_intc_of_init) */
REG32(EDGE_STATUS,          0x00)
REG32(EDGE_SET,             0x04)
REG32(EDGE_CLEAR,           0x08)
REG32(EDGE_MASK_STATUS,     0x0c)
REG32(EDGE_MASK_SET,        0x10)
REG32(EDGE_MASK_CLEAR,      0x14)

/* Level layout (brcmstb_l2_lvl_intc_of_init) */
REG32(LEVEL_STATUS,         0x00)
REG32(LEVEL_MASK_STATUS,    0x04)
REG32(LEVEL_MASK_SET,       0x08)
REG32(LEVEL_MASK_CLEAR,     0x0c)

/* The registers common to both layouts */
typedef enum {
    L2_STATUS,
    L2_SET,
    L2_CLEAR,
    L2_MASK_STATUS,
    L2_MASK_SET,
    L2_MASK_CLEAR,
    L2_INVALID,
} BrcmstbL2Reg;

static BrcmstbL2Reg brcmstb_l2_decode(BrcmstbL2IntcState *s, hwaddr offset)
{
    if (s->edge) {
        switch (offset) {
        case A_EDGE_STATUS:         return L2_STATUS;
        case A_EDGE_SET:            return L2_SET;
        case A_EDGE_CLEAR:          return L2_CLEAR;
        case A_EDGE_MASK_STATUS:    return L2_MASK_STATUS;
        case A_EDGE_MASK_SET:       return L2_MASK_SET;
        case A_EDGE_MASK_CLEAR:     return L2_MASK_CLEAR;
        }
    } else {
        switch (offset) {
        case A_LEVEL_STATUS:        return L2_STATUS;
        case A_LEVEL_MASK_STATUS:   return L2_MASK_STATUS;
        case A_LEVEL_MASK_SET:      return L2_MASK_SET;
        case A_LEVEL_MASK_CLEAR:    return L2_MASK_CLEAR;
        }
    }
    return L2_INVALID;
}

static uint32_t brcmstb_l2_status(BrcmstbL2IntcState *s)
{
    return s->edge ? s->status : s->input;
}

static void brcmstb_l2_update(BrcmstbL2IntcState *s)
{
    uint32_t pending = brcmstb_l2_status(s) & ~s->mask;

    trace_brcmstb_l2_intc_update(DEVICE(s)->canonical_path, pending);
    qemu_set_irq(s->irq, pending != 0);
}

static void brcmstb_l2_set_irq(void *opaque, int n, int level)
{
    BrcmstbL2IntcState *s = BRCMSTB_L2_INTC(opaque);
    uint32_t bit = BIT(n);

    trace_brcmstb_l2_intc_set_irq(DEVICE(s)->canonical_path, n, level);
    if (level) {
        if (s->edge && !(s->input & bit)) {
            s->status |= bit;
        }
        s->input |= bit;
    } else {
        s->input &= ~bit;
    }
    brcmstb_l2_update(s);
}

static uint64_t brcmstb_l2_read(void *opaque, hwaddr offset, unsigned size)
{
    BrcmstbL2IntcState *s = BRCMSTB_L2_INTC(opaque);
    uint32_t value = 0;

    switch (brcmstb_l2_decode(s, offset)) {
    case L2_STATUS:
        value = brcmstb_l2_status(s);
        break;
    case L2_MASK_STATUS:
        value = s->mask;
        break;
    case L2_SET:
    case L2_CLEAR:
    case L2_MASK_SET:
    case L2_MASK_CLEAR:
        break;
    case L2_INVALID:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%02"HWADDR_PRIx"\n",
                      __func__, offset);
        break;
    }

    trace_brcmstb_l2_intc_read(DEVICE(s)->canonical_path, offset, value);
    return value;
}

static void brcmstb_l2_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    BrcmstbL2IntcState *s = BRCMSTB_L2_INTC(opaque);

    trace_brcmstb_l2_intc_write(DEVICE(s)->canonical_path, offset, value);

    switch (brcmstb_l2_decode(s, offset)) {
    case L2_SET:
        s->status |= value;
        break;
    case L2_CLEAR:
        s->status &= ~value;
        break;
    case L2_MASK_SET:
        s->mask |= value;
        break;
    case L2_MASK_CLEAR:
        s->mask &= ~value;
        break;
    case L2_STATUS:
    case L2_MASK_STATUS:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: register 0x%02"HWADDR_PRIx
                      " is read-only\n", __func__, offset);
        return;
    case L2_INVALID:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%02"HWADDR_PRIx"\n",
                      __func__, offset);
        return;
    }
    brcmstb_l2_update(s);
}

static const MemoryRegionOps brcmstb_l2_ops = {
    .read = brcmstb_l2_read,
    .write = brcmstb_l2_write,
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

/* The inputs are driven from outside and keep their levels across reset */
static void brcmstb_l2_reset_enter(Object *obj, ResetType type)
{
    BrcmstbL2IntcState *s = BRCMSTB_L2_INTC(obj);

    s->status = 0;
    s->mask = UINT32_MAX;
}

static void brcmstb_l2_reset_hold(Object *obj, ResetType type)
{
    brcmstb_l2_update(BRCMSTB_L2_INTC(obj));
}

static void brcmstb_l2_init(Object *obj)
{
    BrcmstbL2IntcState *s = BRCMSTB_L2_INTC(obj);

    qdev_init_gpio_in(DEVICE(obj), brcmstb_l2_set_irq,
                      BRCMSTB_L2_INTC_NUM_IRQS);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void brcmstb_l2_realize(DeviceState *dev, Error **errp)
{
    BrcmstbL2IntcState *s = BRCMSTB_L2_INTC(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &brcmstb_l2_ops, s,
                          TYPE_BRCMSTB_L2_INTC,
                          s->edge ? BRCMSTB_L2_INTC_EDGE_SIZE
                                  : BRCMSTB_L2_INTC_LEVEL_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static const VMStateDescription vmstate_brcmstb_l2 = {
    .name = TYPE_BRCMSTB_L2_INTC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(input, BrcmstbL2IntcState),
        VMSTATE_UINT32(status, BrcmstbL2IntcState),
        VMSTATE_UINT32(mask, BrcmstbL2IntcState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property brcmstb_l2_properties[] = {
    DEFINE_PROP_BOOL("edge", BrcmstbL2IntcState, edge, false),
};

static void brcmstb_l2_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.enter = brcmstb_l2_reset_enter;
    rc->phases.hold = brcmstb_l2_reset_hold;
    dc->realize = brcmstb_l2_realize;
    dc->vmsd = &vmstate_brcmstb_l2;
    device_class_set_props(dc, brcmstb_l2_properties);
    dc->desc = "Broadcom STB level 2 interrupt controller";
}

static const TypeInfo brcmstb_l2_types[] = {
    {
        .name           = TYPE_BRCMSTB_L2_INTC,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(BrcmstbL2IntcState),
        .instance_init  = brcmstb_l2_init,
        .class_init     = brcmstb_l2_class_init,
    },
};

DEFINE_TYPES(brcmstb_l2_types)
