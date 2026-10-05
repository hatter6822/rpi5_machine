/*
 * Broadcom set-top-box (brcmstb) reset controllers
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * No public datasheet: registers and semantics as used by Linux
 * drivers/reset/reset-brcmstb.c and reset-brcmstb-rescal.c.
 *
 * brcmstb-reset (brcm,brcmstb-reset) holds blocks of the SoC in software
 * init. Each bank of 0x18 bytes drives 32 reset lines: a write of ones to
 * SET asserts lines, to CLEAR deasserts them, and STATUS reads which are
 * asserted. The bank's other three words are unknown and read as zero;
 * Linux never touches them. Every line is deasserted at reset.
 * Unverified (PLAN.md P16): the lines the boot firmware leaves asserted.
 *
 * brcmstb-rescal (brcm,bcm7216-pcie-sata-rescal) calibrates the PHYs'
 * termination resistors. Setting START runs the calibration, which in the
 * model is done at once: STATUS then reads as done until reset. Linux
 * clears START again afterwards. CTRL is kept but has no effect.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/registerfields.h"
#include "hw/misc/brcmstb_reset.h"
#include "migration/vmstate.h"
#include "trace.h"

/* One bank of brcmstb-reset */
REG32(SW_INIT_SET,          0x00)
REG32(SW_INIT_CLEAR,        0x04)
REG32(SW_INIT_STATUS,       0x08)

REG32(RESCAL_START,         0x00)
    FIELD(RESCAL_START, START, 0, 1)
REG32(RESCAL_CTRL,          0x04)
REG32(RESCAL_STATUS,        0x08)
    FIELD(RESCAL_STATUS, DONE, 0, 1)

static void brcmstb_reset_update(BrcmstbResetState *s, unsigned bank)
{
    unsigned i;

    for (i = 0; i < 32; i++) {
        qemu_set_irq(s->lines[bank * 32 + i], extract32(s->asserted[bank],
                                                        i, 1));
    }
}

static uint64_t brcmstb_reset_read(void *opaque, hwaddr offset,
                                   unsigned size)
{
    BrcmstbResetState *s = BRCMSTB_RESET(opaque);
    unsigned bank = offset / BRCMSTB_RESET_BANK_SIZE;
    uint32_t value = 0;

    switch (offset % BRCMSTB_RESET_BANK_SIZE) {
    case A_SW_INIT_STATUS:
        value = s->asserted[bank];
        break;
    case A_SW_INIT_SET:
    case A_SW_INIT_CLEAR:
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unknown register 0x%02"HWADDR_PRIx
                      "\n", __func__, offset);
        break;
    }

    trace_brcmstb_reset_read(offset, value);
    return value;
}

static void brcmstb_reset_write(void *opaque, hwaddr offset, uint64_t value,
                                unsigned size)
{
    BrcmstbResetState *s = BRCMSTB_RESET(opaque);
    unsigned bank = offset / BRCMSTB_RESET_BANK_SIZE;

    trace_brcmstb_reset_write(offset, value);

    switch (offset % BRCMSTB_RESET_BANK_SIZE) {
    case A_SW_INIT_SET:
        s->asserted[bank] |= value;
        break;
    case A_SW_INIT_CLEAR:
        s->asserted[bank] &= ~value;
        break;
    case A_SW_INIT_STATUS:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: register 0x%02"HWADDR_PRIx
                      " is read-only\n", __func__, offset);
        return;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unknown register 0x%02"HWADDR_PRIx
                      "\n", __func__, offset);
        return;
    }
    brcmstb_reset_update(s, bank);
}

static const MemoryRegionOps brcmstb_reset_ops = {
    .read = brcmstb_reset_read,
    .write = brcmstb_reset_write,
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

static void brcmstb_reset_reset_enter(Object *obj, ResetType type)
{
    BrcmstbResetState *s = BRCMSTB_RESET(obj);

    memset(s->asserted, 0, sizeof(s->asserted));
}

static void brcmstb_reset_reset_hold(Object *obj, ResetType type)
{
    BrcmstbResetState *s = BRCMSTB_RESET(obj);
    unsigned bank;

    for (bank = 0; bank < s->num_banks; bank++) {
        brcmstb_reset_update(s, bank);
    }
}

static void brcmstb_reset_realize(DeviceState *dev, Error **errp)
{
    BrcmstbResetState *s = BRCMSTB_RESET(dev);

    if (s->num_banks < 1 || s->num_banks > BRCMSTB_RESET_MAX_BANKS) {
        error_setg(errp, "%s: num-banks must be 1 to %d", TYPE_BRCMSTB_RESET,
                   BRCMSTB_RESET_MAX_BANKS);
        return;
    }
    memory_region_init_io(&s->iomem, OBJECT(s), &brcmstb_reset_ops, s,
                          TYPE_BRCMSTB_RESET,
                          s->num_banks * BRCMSTB_RESET_BANK_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    qdev_init_gpio_out_named(dev, s->lines, "reset", s->num_banks * 32);
}

static const VMStateDescription vmstate_brcmstb_reset = {
    .name = TYPE_BRCMSTB_RESET,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(asserted, BrcmstbResetState,
                             BRCMSTB_RESET_MAX_BANKS),
        VMSTATE_END_OF_LIST()
    }
};

static const Property brcmstb_reset_properties[] = {
    DEFINE_PROP_UINT32("num-banks", BrcmstbResetState, num_banks, 1),
};

static void brcmstb_reset_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.enter = brcmstb_reset_reset_enter;
    rc->phases.hold = brcmstb_reset_reset_hold;
    dc->realize = brcmstb_reset_realize;
    dc->vmsd = &vmstate_brcmstb_reset;
    device_class_set_props(dc, brcmstb_reset_properties);
    dc->desc = "Broadcom STB software-init reset controller";
}

static uint64_t brcmstb_rescal_read(void *opaque, hwaddr offset,
                                    unsigned size)
{
    BrcmstbRescalState *s = BRCMSTB_RESCAL(opaque);
    uint32_t value = 0;

    switch (offset) {
    case A_RESCAL_START:
        value = s->start;
        break;
    case A_RESCAL_CTRL:
        value = s->ctrl;
        break;
    case A_RESCAL_STATUS:
        value = FIELD_DP32(0, RESCAL_STATUS, DONE, s->done);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unknown register 0x%02"HWADDR_PRIx
                      "\n", __func__, offset);
        break;
    }

    trace_brcmstb_rescal_read(offset, value);
    return value;
}

static void brcmstb_rescal_write(void *opaque, hwaddr offset, uint64_t value,
                                 unsigned size)
{
    BrcmstbRescalState *s = BRCMSTB_RESCAL(opaque);

    trace_brcmstb_rescal_write(offset, value);

    switch (offset) {
    case A_RESCAL_START:
        if (FIELD_EX32(value, RESCAL_START, START)) {
            s->done = true;
        }
        s->start = value;
        break;
    case A_RESCAL_CTRL:
        s->ctrl = value;
        break;
    case A_RESCAL_STATUS:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: STATUS is read-only\n",
                      __func__);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unknown register 0x%02"HWADDR_PRIx
                      "\n", __func__, offset);
        break;
    }
}

static const MemoryRegionOps brcmstb_rescal_ops = {
    .read = brcmstb_rescal_read,
    .write = brcmstb_rescal_write,
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

static void brcmstb_rescal_reset_enter(Object *obj, ResetType type)
{
    BrcmstbRescalState *s = BRCMSTB_RESCAL(obj);

    s->start = 0;
    s->ctrl = 0;
    s->done = false;
}

static void brcmstb_rescal_init(Object *obj)
{
    BrcmstbRescalState *s = BRCMSTB_RESCAL(obj);

    memory_region_init_io(&s->iomem, obj, &brcmstb_rescal_ops, s,
                          TYPE_BRCMSTB_RESCAL, BRCMSTB_RESCAL_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription vmstate_brcmstb_rescal = {
    .name = TYPE_BRCMSTB_RESCAL,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(start, BrcmstbRescalState),
        VMSTATE_UINT32(ctrl, BrcmstbRescalState),
        VMSTATE_BOOL(done, BrcmstbRescalState),
        VMSTATE_END_OF_LIST()
    }
};

static void brcmstb_rescal_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.enter = brcmstb_rescal_reset_enter;
    dc->vmsd = &vmstate_brcmstb_rescal;
    dc->desc = "Broadcom STB SATA/PCIe PHY resistor calibration";
}

static const TypeInfo brcmstb_reset_types[] = {
    {
        .name           = TYPE_BRCMSTB_RESET,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(BrcmstbResetState),
        .class_init     = brcmstb_reset_class_init,
    },
    {
        .name           = TYPE_BRCMSTB_RESCAL,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(BrcmstbRescalState),
        .instance_init  = brcmstb_rescal_init,
        .class_init     = brcmstb_rescal_class_init,
    },
};

DEFINE_TYPES(brcmstb_reset_types)
