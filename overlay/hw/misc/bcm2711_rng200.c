/*
 * Broadcom RNG200 random number generator (BCM2711, BCM2712)
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * No public datasheet: registers and fields as used by Linux
 * drivers/char/hw_random/iproc-rng200.c, whose "brcm,bcm2711-rng200"
 * path is the one both Raspberry Pi 4 and 5 device trees select.
 *
 * The generator is modelled as infinitely fast: while it runs, the FIFO
 * is full, every word read from it is replaced at once, and the bit
 * counter advances by the bits that took. The warm-up bits the guest
 * asks to discard (TOTAL_BIT_COUNT_THRESHOLD) are counted, never output.
 * Data comes from qemu_guest_getrandom_nofail(), so -seed makes it
 * reproducible.
 */

#include "qemu/osdep.h"
#include "qemu/guest-random.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/registerfields.h"
#include "hw/misc/bcm2711_rng200.h"
#include "migration/vmstate.h"
#include "trace.h"

REG32(CTRL,                         0x00)
    FIELD(CTRL, RBGEN,               0, 13)  /* bit 0 enables */
    FIELD(CTRL, DIV_CTRL,           13, 8)
REG32(RNG_SOFT_RESET,               0x04)
    FIELD(RNG_SOFT_RESET, RESET,     0, 1)
REG32(RBG_SOFT_RESET,               0x08)
    FIELD(RBG_SOFT_RESET, RESET,     0, 1)
REG32(TOTAL_BIT_COUNT,              0x0c)
REG32(TOTAL_BIT_COUNT_THRESHOLD,    0x10)
REG32(INT_STATUS,                   0x18)
    FIELD(INT_STATUS, TOTAL_BITS_COUNT,         0, 1)
    FIELD(INT_STATUS, NIST_FAIL,                5, 1)
    FIELD(INT_STATUS, STARTUP_TRANSITIONS_MET, 17, 1)
    FIELD(INT_STATUS, MASTER_FAIL_LOCKOUT,     31, 1)
REG32(INT_ENABLE,                   0x1c)
REG32(FIFO_DATA,                    0x20)
REG32(FIFO_COUNT,                   0x24)
    FIELD(FIFO_COUNT, COUNT,         0, 8)
    FIELD(FIFO_COUNT, THRESHOLD,     8, 8)

#define CTRL_RBGEN_ENABLE   1

static bool rng200_running(BCM2711Rng200State *s)
{
    return (FIELD_EX32(s->ctrl, CTRL, RBGEN) & CTRL_RBGEN_ENABLE) &&
           !s->rng_soft_reset && !s->rbg_soft_reset;
}

static void rng200_update_irq(BCM2711Rng200State *s)
{
    qemu_set_irq(s->irq, !!(s->int_status & s->int_enable));
}

/* Count @bits more generated bits, flagging the threshold when crossed */
static void rng200_count(BCM2711Rng200State *s, uint32_t bits)
{
    uint32_t old = s->total_bit_count;

    s->total_bit_count = old > UINT32_MAX - bits ? UINT32_MAX : old + bits;
    if (old < s->total_bit_count_threshold &&
        s->total_bit_count >= s->total_bit_count_threshold) {
        s->int_status |= R_INT_STATUS_TOTAL_BITS_COUNT_MASK;
    }
}

/* Refill the FIFO if the generator runs; @started: it has just started */
static void rng200_update(BCM2711Rng200State *s, bool started)
{
    if (rng200_running(s)) {
        if (started) {
            s->int_status |= R_INT_STATUS_STARTUP_TRANSITIONS_MET_MASK;
        }
        if (s->total_bit_count < s->total_bit_count_threshold) {
            /* The warm-up bits, discarded */
            rng200_count(s, s->total_bit_count_threshold -
                            s->total_bit_count);
        }
        rng200_count(s, (BCM2711_RNG200_FIFO_DEPTH - s->fifo_count) * 32);
        s->fifo_count = BCM2711_RNG200_FIFO_DEPTH;
    }
    rng200_update_irq(s);
}

/* The soft resets empty the FIFO and restart the bit count */
static void rng200_soft_reset(BCM2711Rng200State *s)
{
    s->fifo_count = 0;
    s->total_bit_count = 0;
}

static uint64_t bcm2711_rng200_read(void *opaque, hwaddr offset,
                                    unsigned size)
{
    BCM2711Rng200State *s = BCM2711_RNG200(opaque);
    uint32_t value = 0;

    switch (offset) {
    case A_CTRL:
        value = s->ctrl;
        break;
    case A_RNG_SOFT_RESET:
        value = s->rng_soft_reset;
        break;
    case A_RBG_SOFT_RESET:
        value = s->rbg_soft_reset;
        break;
    case A_TOTAL_BIT_COUNT:
        value = s->total_bit_count;
        break;
    case A_TOTAL_BIT_COUNT_THRESHOLD:
        value = s->total_bit_count_threshold;
        break;
    case A_INT_STATUS:
        value = s->int_status;
        break;
    case A_INT_ENABLE:
        value = s->int_enable;
        break;
    case A_FIFO_DATA:
        if (!s->fifo_count) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: FIFO_DATA read while the "
                          "FIFO is empty\n", __func__);
            break;
        }
        qemu_guest_getrandom_nofail(&value, sizeof(value));
        s->fifo_count--;
        rng200_update(s, false);
        break;
    case A_FIFO_COUNT:
        value = FIELD_DP32(s->fifo_count, FIFO_COUNT, THRESHOLD,
                           s->fifo_threshold);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unknown register 0x%02"HWADDR_PRIx"\n",
                      __func__, offset);
        break;
    }

    trace_bcm2711_rng200_read(offset, value);
    return value;
}

static void bcm2711_rng200_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size)
{
    BCM2711Rng200State *s = BCM2711_RNG200(opaque);
    bool was_running = rng200_running(s);

    trace_bcm2711_rng200_write(offset, value);

    switch (offset) {
    case A_CTRL:
        s->ctrl = value;
        break;
    case A_RNG_SOFT_RESET:
        s->rng_soft_reset = value & R_RNG_SOFT_RESET_RESET_MASK;
        if (s->rng_soft_reset) {
            rng200_soft_reset(s);
        }
        break;
    case A_RBG_SOFT_RESET:
        s->rbg_soft_reset = value & R_RBG_SOFT_RESET_RESET_MASK;
        if (s->rbg_soft_reset) {
            rng200_soft_reset(s);
        }
        break;
    case A_TOTAL_BIT_COUNT:
    case A_FIFO_DATA:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: register 0x%02"HWADDR_PRIx
                      " is read-only\n", __func__, offset);
        return;
    case A_TOTAL_BIT_COUNT_THRESHOLD:
        s->total_bit_count_threshold = value;
        break;
    case A_INT_STATUS:
        s->int_status &= ~value;
        break;
    case A_INT_ENABLE:
        s->int_enable = value;
        break;
    case A_FIFO_COUNT:
        s->fifo_threshold = FIELD_EX32(value, FIFO_COUNT, THRESHOLD);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unknown register 0x%02"HWADDR_PRIx"\n",
                      __func__, offset);
        return;
    }

    rng200_update(s, !was_running && rng200_running(s));
}

static const MemoryRegionOps bcm2711_rng200_ops = {
    .read = bcm2711_rng200_read,
    .write = bcm2711_rng200_write,
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

static void bcm2711_rng200_reset_enter(Object *obj, ResetType type)
{
    BCM2711Rng200State *s = BCM2711_RNG200(obj);

    s->ctrl = 0;
    s->rng_soft_reset = 0;
    s->rbg_soft_reset = 0;
    s->total_bit_count = 0;
    s->total_bit_count_threshold = 0;
    s->int_status = 0;
    s->int_enable = 0;
    s->fifo_threshold = 0;
    s->fifo_count = 0;
}

static void bcm2711_rng200_reset_hold(Object *obj, ResetType type)
{
    rng200_update_irq(BCM2711_RNG200(obj));
}

static void bcm2711_rng200_init(Object *obj)
{
    BCM2711Rng200State *s = BCM2711_RNG200(obj);

    memory_region_init_io(&s->iomem, obj, &bcm2711_rng200_ops, s,
                          TYPE_BCM2711_RNG200, BCM2711_RNG200_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const VMStateDescription vmstate_bcm2711_rng200 = {
    .name = TYPE_BCM2711_RNG200,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, BCM2711Rng200State),
        VMSTATE_UINT32(rng_soft_reset, BCM2711Rng200State),
        VMSTATE_UINT32(rbg_soft_reset, BCM2711Rng200State),
        VMSTATE_UINT32(total_bit_count, BCM2711Rng200State),
        VMSTATE_UINT32(total_bit_count_threshold, BCM2711Rng200State),
        VMSTATE_UINT32(int_status, BCM2711Rng200State),
        VMSTATE_UINT32(int_enable, BCM2711Rng200State),
        VMSTATE_UINT32(fifo_threshold, BCM2711Rng200State),
        VMSTATE_UINT32(fifo_count, BCM2711Rng200State),
        VMSTATE_END_OF_LIST()
    }
};

static void bcm2711_rng200_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.enter = bcm2711_rng200_reset_enter;
    rc->phases.hold = bcm2711_rng200_reset_hold;
    dc->vmsd = &vmstate_bcm2711_rng200;
    dc->desc = "Broadcom RNG200 random number generator";
}

static const TypeInfo bcm2711_rng200_types[] = {
    {
        .name           = TYPE_BCM2711_RNG200,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(BCM2711Rng200State),
        .instance_init  = bcm2711_rng200_init,
        .class_init     = bcm2711_rng200_class_init,
    },
};

DEFINE_TYPES(bcm2711_rng200_types)
