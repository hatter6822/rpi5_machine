/*
 * Broadcom set-top-box (brcmstb) GPIO controller
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * No public datasheet: registers and semantics as used by Linux
 * drivers/gpio/gpio-brcmstb.c, the "UPG GIO" block of Broadcom STB SoCs.
 * Lines come in banks of up to 32, each with eight registers at a 0x20
 * stride:
 *
 *   ODEN   open-drain enable
 *   DATA   the output latch when written, the level of the lines when read
 *   IODIR  direction, 1 = input
 *   EC     1 = rising edge or high level, 0 = falling edge or low level
 *   EI     1 = both edges, whatever EC says
 *   MASK   1 = interrupt enabled
 *   LEVEL  1 = level-sensitive, 0 = edge-sensitive
 *   STAT   interrupt status, write 1 to clear
 *
 * A line an output drives low, or high unless it is open-drain, is at
 * that level; any other line is at the level driven into it from outside
 * (the GPIO input of the same number). Detection watches the level of the
 * line whatever its direction, so an output interrupts on its own edges.
 * An edge sets its STAT bit until the bit is cleared; a level-sensitive
 * line sets its STAT bit for as long as it is at its active level, so
 * clearing the bit then has no effect. The one interrupt output is the OR
 * over the banks of STAT & MASK. Reset makes every line an input, with
 * every interrupt disabled and falling-edge detection; STAT is cleared.
 * Bits beyond a bank's width read as zero and ignore writes.
 * TODO(WS0.4): check the reset values and the level of DATA for an
 * open-drain line on hardware.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/gpio/brcmstb_gpio.h"
#include "migration/vmstate.h"
#include "trace.h"

/* Registers of a bank, in words (gio_reg_index in gpio-brcmstb.c) */
enum {
    GIO_ODEN,
    GIO_DATA,
    GIO_IODIR,
    GIO_EC,
    GIO_EI,
    GIO_MASK,
    GIO_LEVEL,
    GIO_STAT,
};

static uint32_t brcmstb_gpio_levels(const BrcmstbGpioBank *b)
{
    uint32_t out = ~b->iodir;
    uint32_t low = out & ~b->data;
    uint32_t high = out & ~b->oden & b->data;

    return ((b->input & ~low) | high) & b->valid;
}

static void brcmstb_gpio_update_irq(BrcmstbGpioState *s)
{
    bool level = false;

    for (unsigned i = 0; i < s->num_banks; i++) {
        level |= (s->bank[i].stat & s->bank[i].mask) != 0;
    }
    trace_brcmstb_gpio_irq(DEVICE(s)->canonical_path, level);
    qemu_set_irq(s->irq, level);
}

/*
 * Bring bank @n's lines to the levels its registers and inputs now give,
 * latching the edges that detection watches for when @edges is set, and
 * the lines at their active level; drive the changed levels out
 */
static void brcmstb_gpio_update(BrcmstbGpioState *s, unsigned n, bool edges)
{
    BrcmstbGpioBank *b = &s->bank[n];
    uint32_t levels = brcmstb_gpio_levels(b);
    uint32_t changed = levels ^ b->pads;
    uint32_t edge = changed & (b->ei | ~(levels ^ b->ec));

    if (edges) {
        b->stat |= edge & ~b->level;
    }
    b->stat |= b->level & ~(levels ^ b->ec) & b->valid;
    b->pads = levels;

    for (unsigned bit = 0; bit < BRCMSTB_GPIO_BANK_LINES; bit++) {
        if (changed & BIT(bit)) {
            unsigned line = n * BRCMSTB_GPIO_BANK_LINES + bit;
            int level = extract32(levels, bit, 1);

            trace_brcmstb_gpio_output(DEVICE(s)->canonical_path, line, level);
            qemu_set_irq(s->out[line], level);
        }
    }
    brcmstb_gpio_update_irq(s);
}

static void brcmstb_gpio_set_input(void *opaque, int line, int level)
{
    BrcmstbGpioState *s = BRCMSTB_GPIO(opaque);
    unsigned n = line / BRCMSTB_GPIO_BANK_LINES;
    uint32_t bit = BIT(line % BRCMSTB_GPIO_BANK_LINES);

    trace_brcmstb_gpio_set_input(DEVICE(s)->canonical_path, line, level);
    if (level) {
        s->bank[n].input |= bit;
    } else {
        s->bank[n].input &= ~bit;
    }
    brcmstb_gpio_update(s, n, true);
}

static uint64_t brcmstb_gpio_read(void *opaque, hwaddr offset, unsigned size)
{
    BrcmstbGpioState *s = BRCMSTB_GPIO(opaque);
    BrcmstbGpioBank *b = &s->bank[offset / BRCMSTB_GPIO_BANK_SIZE];
    uint32_t value = 0;

    switch ((offset % BRCMSTB_GPIO_BANK_SIZE) / 4) {
    case GIO_ODEN:
        value = b->oden;
        break;
    case GIO_DATA:
        value = b->pads;
        break;
    case GIO_IODIR:
        value = b->iodir;
        break;
    case GIO_EC:
        value = b->ec;
        break;
    case GIO_EI:
        value = b->ei;
        break;
    case GIO_MASK:
        value = b->mask;
        break;
    case GIO_LEVEL:
        value = b->level;
        break;
    case GIO_STAT:
        value = b->stat;
        break;
    }

    trace_brcmstb_gpio_read(DEVICE(s)->canonical_path, offset, value);
    return value;
}

static void brcmstb_gpio_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    BrcmstbGpioState *s = BRCMSTB_GPIO(opaque);
    unsigned n = offset / BRCMSTB_GPIO_BANK_SIZE;
    BrcmstbGpioBank *b = &s->bank[n];

    trace_brcmstb_gpio_write(DEVICE(s)->canonical_path, offset, value);
    value &= b->valid;

    switch ((offset % BRCMSTB_GPIO_BANK_SIZE) / 4) {
    case GIO_ODEN:
        b->oden = value;
        break;
    case GIO_DATA:
        b->data = value;
        break;
    case GIO_IODIR:
        b->iodir = value;
        break;
    case GIO_EC:
        b->ec = value;
        break;
    case GIO_EI:
        b->ei = value;
        break;
    case GIO_MASK:
        b->mask = value;
        break;
    case GIO_LEVEL:
        b->level = value;
        break;
    case GIO_STAT:
        b->stat &= ~value;
        break;
    }
    brcmstb_gpio_update(s, n, true);
}

static const MemoryRegionOps brcmstb_gpio_ops = {
    .read = brcmstb_gpio_read,
    .write = brcmstb_gpio_write,
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
static void brcmstb_gpio_reset_registers(BrcmstbGpioState *s)
{
    for (unsigned i = 0; i < s->num_banks; i++) {
        BrcmstbGpioBank *b = &s->bank[i];

        b->oden = 0;
        b->data = 0;
        b->iodir = b->valid;
        b->ec = 0;
        b->ei = 0;
        b->mask = 0;
        b->level = 0;
        b->stat = 0;
    }
}

static void brcmstb_gpio_reset_enter(Object *obj, ResetType type)
{
    brcmstb_gpio_reset_registers(BRCMSTB_GPIO(obj));
}

/* No edges: the lines a reset releases take their input levels quietly */
static void brcmstb_gpio_reset_hold(Object *obj, ResetType type)
{
    BrcmstbGpioState *s = BRCMSTB_GPIO(obj);

    for (unsigned i = 0; i < s->num_banks; i++) {
        brcmstb_gpio_update(s, i, false);
    }
}

static void brcmstb_gpio_init(Object *obj)
{
    BrcmstbGpioState *s = BRCMSTB_GPIO(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void brcmstb_gpio_realize(DeviceState *dev, Error **errp)
{
    BrcmstbGpioState *s = BRCMSTB_GPIO(dev);
    unsigned lines = s->num_banks * BRCMSTB_GPIO_BANK_LINES;

    if (s->num_banks == 0 || s->num_banks > BRCMSTB_GPIO_MAX_BANKS) {
        error_setg(errp, "bank-widths must give 1 to %d banks",
                   BRCMSTB_GPIO_MAX_BANKS);
        return;
    }
    for (unsigned i = 0; i < s->num_banks; i++) {
        if (s->bank_widths[i] > BRCMSTB_GPIO_BANK_LINES) {
            error_setg(errp, "bank %u has %" PRIu32 " lines; a bank has at "
                       "most %d", i, s->bank_widths[i],
                       BRCMSTB_GPIO_BANK_LINES);
            return;
        }
        /* Linux takes a bank of no lines as a gap in the block */
        s->bank[i].valid = (1ULL << s->bank_widths[i]) - 1;
    }

    memory_region_init_io(&s->iomem, OBJECT(s), &brcmstb_gpio_ops, s,
                          TYPE_BRCMSTB_GPIO,
                          s->num_banks * BRCMSTB_GPIO_BANK_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    qdev_init_gpio_in(dev, brcmstb_gpio_set_input, lines);
    qdev_init_gpio_out(dev, s->out, lines);

    /* Inputs from the start, for levels driven in before the first reset */
    brcmstb_gpio_reset_registers(s);
    for (unsigned i = 0; i < s->num_banks; i++) {
        s->bank[i].pads = brcmstb_gpio_levels(&s->bank[i]);
    }
}

static int brcmstb_gpio_post_load(void *opaque, int version_id)
{
    BrcmstbGpioState *s = BRCMSTB_GPIO(opaque);

    for (unsigned i = 0; i < s->num_banks; i++) {
        s->bank[i].pads = brcmstb_gpio_levels(&s->bank[i]);
    }
    return 0;
}

static const VMStateDescription vmstate_brcmstb_gpio_bank = {
    .name = TYPE_BRCMSTB_GPIO "/bank",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(oden, BrcmstbGpioBank),
        VMSTATE_UINT32(data, BrcmstbGpioBank),
        VMSTATE_UINT32(iodir, BrcmstbGpioBank),
        VMSTATE_UINT32(ec, BrcmstbGpioBank),
        VMSTATE_UINT32(ei, BrcmstbGpioBank),
        VMSTATE_UINT32(mask, BrcmstbGpioBank),
        VMSTATE_UINT32(level, BrcmstbGpioBank),
        VMSTATE_UINT32(stat, BrcmstbGpioBank),
        VMSTATE_UINT32(input, BrcmstbGpioBank),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_brcmstb_gpio = {
    .name = TYPE_BRCMSTB_GPIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = brcmstb_gpio_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(bank, BrcmstbGpioState, BRCMSTB_GPIO_MAX_BANKS,
                             1, vmstate_brcmstb_gpio_bank, BrcmstbGpioBank),
        VMSTATE_END_OF_LIST()
    }
};

static const Property brcmstb_gpio_properties[] = {
    DEFINE_PROP_ARRAY("bank-widths", BrcmstbGpioState, num_banks,
                      bank_widths, qdev_prop_uint32, uint32_t),
};

static void brcmstb_gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.enter = brcmstb_gpio_reset_enter;
    rc->phases.hold = brcmstb_gpio_reset_hold;
    dc->realize = brcmstb_gpio_realize;
    dc->vmsd = &vmstate_brcmstb_gpio;
    device_class_set_props(dc, brcmstb_gpio_properties);
    dc->desc = "Broadcom STB GPIO controller";
}

static const TypeInfo brcmstb_gpio_types[] = {
    {
        .name           = TYPE_BRCMSTB_GPIO,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(BrcmstbGpioState),
        .instance_init  = brcmstb_gpio_init,
        .class_init     = brcmstb_gpio_class_init,
    },
};

DEFINE_TYPES(brcmstb_gpio_types)
