/*
 * Broadcom set-top-box (brcmstb) pin controller
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * No public datasheet: registers as used by Linux
 * drivers/pinctrl/bcm/pinctrl-brcmstb.c, the pin controllers beside the
 * "UPG GIO" GPIO blocks of Broadcom STB SoCs such as the BCM2712. A
 * block is a run of 32-bit registers: the first ones select the function
 * of each pin, 4 bits per pin (0 = GPIO), the others its pull, 2 bits
 * per pin (0 = none, 1 = down, 2 = up). Which fields belong to which pin
 * is up to the SoC.
 *
 * The model stores what software writes, so drivers read their settings
 * back, but the settings have no effect: a GPIO line behaves the same
 * whatever its pin's function and pull. Reset clears every register.
 * TODO(WS0.4): check the reset values on hardware.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/gpio/brcmstb_pinctrl.h"
#include "migration/vmstate.h"
#include "trace.h"

static uint64_t brcmstb_pinctrl_read(void *opaque, hwaddr offset,
                                     unsigned size)
{
    BrcmstbPinctrlState *s = BRCMSTB_PINCTRL(opaque);
    uint32_t value = s->regs[offset / 4];

    trace_brcmstb_pinctrl_read(DEVICE(s)->canonical_path, offset, value);
    return value;
}

static void brcmstb_pinctrl_write(void *opaque, hwaddr offset, uint64_t value,
                                  unsigned size)
{
    BrcmstbPinctrlState *s = BRCMSTB_PINCTRL(opaque);

    trace_brcmstb_pinctrl_write(DEVICE(s)->canonical_path, offset, value);
    s->regs[offset / 4] = value;
}

static const MemoryRegionOps brcmstb_pinctrl_ops = {
    .read = brcmstb_pinctrl_read,
    .write = brcmstb_pinctrl_write,
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

static void brcmstb_pinctrl_reset_enter(Object *obj, ResetType type)
{
    BrcmstbPinctrlState *s = BRCMSTB_PINCTRL(obj);

    memset(s->regs, 0, sizeof(s->regs));
}

static void brcmstb_pinctrl_realize(DeviceState *dev, Error **errp)
{
    BrcmstbPinctrlState *s = BRCMSTB_PINCTRL(dev);

    if (s->num_regs == 0 || s->num_regs > BRCMSTB_PINCTRL_MAX_REGS) {
        error_setg(errp, "num-regs must be 1 to %d", BRCMSTB_PINCTRL_MAX_REGS);
        return;
    }
    memory_region_init_io(&s->iomem, OBJECT(s), &brcmstb_pinctrl_ops, s,
                          TYPE_BRCMSTB_PINCTRL, s->num_regs * 4);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static const VMStateDescription vmstate_brcmstb_pinctrl = {
    .name = TYPE_BRCMSTB_PINCTRL,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, BrcmstbPinctrlState,
                             BRCMSTB_PINCTRL_MAX_REGS),
        VMSTATE_END_OF_LIST()
    }
};

static const Property brcmstb_pinctrl_properties[] = {
    DEFINE_PROP_UINT32("num-regs", BrcmstbPinctrlState, num_regs, 0),
};

static void brcmstb_pinctrl_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.enter = brcmstb_pinctrl_reset_enter;
    dc->realize = brcmstb_pinctrl_realize;
    dc->vmsd = &vmstate_brcmstb_pinctrl;
    device_class_set_props(dc, brcmstb_pinctrl_properties);
    dc->desc = "Broadcom STB pin controller";
}

static const TypeInfo brcmstb_pinctrl_types[] = {
    {
        .name           = TYPE_BRCMSTB_PINCTRL,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(BrcmstbPinctrlState),
        .class_init     = brcmstb_pinctrl_class_init,
    },
};

DEFINE_TYPES(brcmstb_pinctrl_types)
