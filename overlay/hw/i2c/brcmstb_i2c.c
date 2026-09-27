/*
 * Broadcom set-top-box (brcmstb) BSC I2C controller
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * No public datasheet: registers and semantics as used by Linux
 * drivers/i2c/busses/i2c-brcmstb.c, the BSC (Broadcom serial control) I2C
 * master of Broadcom STB SoCs. On the BCM2712, two of them read the EDID
 * of the monitors on the HDMI ports.
 *
 *   0x00       CHIP_ADDRESS    the target's address, in bits 7:1
 *   0x04-0x20  DATA_IN0-7      the bytes to write
 *   0x24       CNT_REG         the number of bytes to transfer
 *   0x28       CTL_REG         DTF (bits 1:0): 0 = write, 1 = read;
 *                              SCL_SEL (5:4), INT_EN (6), DIV_CLK (7)
 *   0x2c       IIC_ENABLE      ENABLE (0), INTRP (1), NOACK (2),
 *                              NOSTOP (4), NOSTART (5), RESTART (6)
 *   0x30-0x4c  DATA_OUT0-7     the bytes read
 *   0x50       CTLHI_REG       WAIT_DIS (0), IGNORE_ACK (1),
 *                              DATAREG_SIZE (6), INPUT_SWITCHING (7)
 *   0x54       SCL_PARAM       reserved
 *
 * The data registers hold 4 bytes each when DATAREG_SIZE is set and one
 * byte otherwise, least significant first, so a transfer moves up to 32
 * or 8 bytes, and the count is bits 5:0 or 3:0 of CNT_REG.
 *
 * Setting ENABLE runs a transfer: a start condition and the address,
 * unless NOSTART is set; the bytes, in the direction DTF gives; and a
 * stop condition, unless NOSTOP is set. A start while an earlier transfer
 * holds the bus is a repeated start, whether or not RESTART is set. The
 * direction comes from DTF; bit 0 of CHIP_ADDRESS, which Linux sets to
 * match, is ignored. When no target acknowledges the address or a byte
 * written, NOACK is set and, unless IGNORE_ACK is set, the transfer stops
 * there with a stop condition. A read first clears DATA_OUT. The model
 * moves the bytes at once and sets INTRP; the interrupt output is
 * INTRP && INT_EN. Clearing ENABLE clears INTRP and NOACK.
 *
 * Not modelled: the combined formats (DTF 2 and 3, a write and a read
 * joined by a repeated start), which Linux does not use and which end at
 * once with NOACK set, and the bus speed: SCL_SEL, DIV_CLK, WAIT_DIS and
 * INPUT_SWITCHING are only stored. SCL_PARAM reads as zero, as do the
 * bits Linux does not name. Reset clears every register and ends any
 * transfer the controller left open.
 * TODO(WS0.4): check the reset values and the unnamed bits on hardware.
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/i2c/brcmstb_i2c.h"
#include "migration/vmstate.h"
#include "trace.h"

#define BSC_CHIP_ADDRESS        0x00
#define BSC_DATA_IN(n)          (0x04 + (n) * 4)
#define BSC_CNT_REG             0x24
#define BSC_CTL_REG             0x28
#define BSC_IIC_ENABLE          0x2c
#define BSC_DATA_OUT(n)         (0x30 + (n) * 4)
#define BSC_CTLHI_REG           0x50
#define BSC_SCL_PARAM           0x54

#define BSC_CHIP_ADDRESS_MASK   0xff
#define BSC_CNT_MASK            0x3f

#define BSC_CTL_DTF             0x03
#define BSC_CTL_DTF_WRITE       0
#define BSC_CTL_DTF_READ        1
#define BSC_CTL_INT_EN          BIT(6)
#define BSC_CTL_MASK            0xf3

#define BSC_IIC_EN_ENABLE       BIT(0)
#define BSC_IIC_EN_INTRP        BIT(1)
#define BSC_IIC_EN_NOACK        BIT(2)
#define BSC_IIC_EN_NOSTOP       BIT(4)
#define BSC_IIC_EN_NOSTART      BIT(5)
#define BSC_IIC_EN_RESTART      BIT(6)
#define BSC_IIC_EN_STATUS       (BSC_IIC_EN_INTRP | BSC_IIC_EN_NOACK)
#define BSC_IIC_EN_WRITABLE     (BSC_IIC_EN_ENABLE | BSC_IIC_EN_NOSTOP | \
                                 BSC_IIC_EN_NOSTART | BSC_IIC_EN_RESTART)

#define BSC_CTLHI_IGNORE_ACK    BIT(1)
#define BSC_CTLHI_DATAREG_SIZE  BIT(6)
#define BSC_CTLHI_MASK          0xc3

static void brcmstb_i2c_update_irq(BrcmstbI2cState *s)
{
    qemu_set_irq(s->irq, (s->iic_enable & BSC_IIC_EN_INTRP) &&
                         (s->ctl & BSC_CTL_INT_EN));
}

/* Run the transfer the registers describe; returns true on a NACK */
static bool brcmstb_i2c_transfer(BrcmstbI2cState *s)
{
    unsigned regsz = s->ctlhi & BSC_CTLHI_DATAREG_SIZE ? 4 : 1;
    unsigned count = s->cnt & (regsz == 4 ? 0x3f : 0x0f);
    unsigned max = BRCMSTB_I2C_NUM_DATA_REGS * regsz;
    bool ignore_ack = s->ctlhi & BSC_CTLHI_IGNORE_ACK;
    uint8_t address = extract32(s->chip_address, 1, 7);
    bool nack = false;
    bool read;

    switch (s->ctl & BSC_CTL_DTF) {
    case BSC_CTL_DTF_WRITE:
        read = false;
        break;
    case BSC_CTL_DTF_READ:
        read = true;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: combined transfer formats are not "
                      "modelled\n", __func__);
        return true;
    }
    if (count > max) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: a count of %u exceeds the %u "
                      "bytes the data registers hold\n", __func__, count, max);
        count = max;
    }
    trace_brcmstb_i2c_transfer(DEVICE(s)->canonical_path, address, read,
                               count, s->iic_enable);

    if (!(s->iic_enable & BSC_IIC_EN_NOSTART) &&
        i2c_start_transfer(s->bus, address, read)) {
        /* No target acknowledged: none listens to what follows */
        i2c_end_transfer(s->bus);
        nack = true;
    }
    if (read) {
        memset(s->data_out, 0, sizeof(s->data_out));
    }
    for (unsigned i = 0; i < count && (!nack || ignore_ack); i++) {
        uint32_t *reg = read ? &s->data_out[i / regsz] : &s->data_in[i / regsz];
        unsigned shift = 8 * (i % regsz);

        if (read) {
            /* With no target the bus reads as its pull-ups */
            *reg = deposit32(*reg, shift, 8, i2c_recv(s->bus));
        } else if (!i2c_bus_busy(s->bus) ||
                   i2c_send(s->bus, extract32(*reg, shift, 8))) {
            nack = true;
        }
    }

    if (nack && !ignore_ack) {
        i2c_end_transfer(s->bus);
    } else if (!(s->iic_enable & BSC_IIC_EN_NOSTOP)) {
        /* The master does not acknowledge the last byte it reads */
        if (read && count && i2c_bus_busy(s->bus)) {
            i2c_nack(s->bus);
        }
        i2c_end_transfer(s->bus);
    }
    trace_brcmstb_i2c_done(DEVICE(s)->canonical_path, nack);
    return nack;
}

static void brcmstb_i2c_write_enable(BrcmstbI2cState *s, uint32_t value)
{
    bool start = (value & BSC_IIC_EN_ENABLE) &&
                 !(s->iic_enable & BSC_IIC_EN_ENABLE);

    s->iic_enable = (s->iic_enable & BSC_IIC_EN_STATUS) |
                    (value & BSC_IIC_EN_WRITABLE);
    if (!(value & BSC_IIC_EN_ENABLE)) {
        s->iic_enable &= ~BSC_IIC_EN_STATUS;
    } else if (start) {
        if (brcmstb_i2c_transfer(s)) {
            s->iic_enable |= BSC_IIC_EN_NOACK;
        }
        s->iic_enable |= BSC_IIC_EN_INTRP;
    }
}

static uint64_t brcmstb_i2c_read(void *opaque, hwaddr offset, unsigned size)
{
    BrcmstbI2cState *s = BRCMSTB_I2C(opaque);
    uint32_t value = 0;

    switch (offset) {
    case BSC_CHIP_ADDRESS:
        value = s->chip_address;
        break;
    case BSC_DATA_IN(0) ... BSC_DATA_IN(BRCMSTB_I2C_NUM_DATA_REGS - 1):
        value = s->data_in[(offset - BSC_DATA_IN(0)) / 4];
        break;
    case BSC_CNT_REG:
        value = s->cnt;
        break;
    case BSC_CTL_REG:
        value = s->ctl;
        break;
    case BSC_IIC_ENABLE:
        value = s->iic_enable;
        break;
    case BSC_DATA_OUT(0) ... BSC_DATA_OUT(BRCMSTB_I2C_NUM_DATA_REGS - 1):
        value = s->data_out[(offset - BSC_DATA_OUT(0)) / 4];
        break;
    case BSC_CTLHI_REG:
        value = s->ctlhi;
        break;
    case BSC_SCL_PARAM:
        qemu_log_mask(LOG_UNIMP, "%s: SCL_PARAM is not modelled\n", __func__);
        break;
    }
    trace_brcmstb_i2c_read(DEVICE(s)->canonical_path, offset, value);
    return value;
}

static void brcmstb_i2c_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    BrcmstbI2cState *s = BRCMSTB_I2C(opaque);

    trace_brcmstb_i2c_write(DEVICE(s)->canonical_path, offset, value);
    switch (offset) {
    case BSC_CHIP_ADDRESS:
        s->chip_address = value & BSC_CHIP_ADDRESS_MASK;
        break;
    case BSC_DATA_IN(0) ... BSC_DATA_IN(BRCMSTB_I2C_NUM_DATA_REGS - 1):
        s->data_in[(offset - BSC_DATA_IN(0)) / 4] = value;
        break;
    case BSC_CNT_REG:
        s->cnt = value & BSC_CNT_MASK;
        break;
    case BSC_CTL_REG:
        s->ctl = value & BSC_CTL_MASK;
        break;
    case BSC_IIC_ENABLE:
        brcmstb_i2c_write_enable(s, value);
        break;
    case BSC_DATA_OUT(0) ... BSC_DATA_OUT(BRCMSTB_I2C_NUM_DATA_REGS - 1):
        qemu_log_mask(LOG_GUEST_ERROR, "%s: DATA_OUT%d is read-only\n",
                      __func__, (int)(offset - BSC_DATA_OUT(0)) / 4);
        break;
    case BSC_CTLHI_REG:
        s->ctlhi = value & BSC_CTLHI_MASK;
        break;
    case BSC_SCL_PARAM:
        qemu_log_mask(LOG_UNIMP, "%s: SCL_PARAM is not modelled\n", __func__);
        break;
    }
    brcmstb_i2c_update_irq(s);
}

static const MemoryRegionOps brcmstb_i2c_ops = {
    .read = brcmstb_i2c_read,
    .write = brcmstb_i2c_write,
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

static void brcmstb_i2c_reset_enter(Object *obj, ResetType type)
{
    BrcmstbI2cState *s = BRCMSTB_I2C(obj);

    s->chip_address = 0;
    memset(s->data_in, 0, sizeof(s->data_in));
    s->cnt = 0;
    s->ctl = 0;
    s->iic_enable = 0;
    memset(s->data_out, 0, sizeof(s->data_out));
    s->ctlhi = 0;
}

static void brcmstb_i2c_reset_hold(Object *obj, ResetType type)
{
    BrcmstbI2cState *s = BRCMSTB_I2C(obj);

    if (i2c_bus_busy(s->bus)) {
        i2c_end_transfer(s->bus);
    }
    brcmstb_i2c_update_irq(s);
}

static void brcmstb_i2c_init(Object *obj)
{
    BrcmstbI2cState *s = BRCMSTB_I2C(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &brcmstb_i2c_ops, s,
                          TYPE_BRCMSTB_I2C, BRCMSTB_I2C_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static void brcmstb_i2c_realize(DeviceState *dev, Error **errp)
{
    BrcmstbI2cState *s = BRCMSTB_I2C(dev);

    s->bus = i2c_init_bus(dev, NULL);
}

static const VMStateDescription vmstate_brcmstb_i2c = {
    .name = TYPE_BRCMSTB_I2C,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(chip_address, BrcmstbI2cState),
        VMSTATE_UINT32_ARRAY(data_in, BrcmstbI2cState,
                             BRCMSTB_I2C_NUM_DATA_REGS),
        VMSTATE_UINT32(cnt, BrcmstbI2cState),
        VMSTATE_UINT32(ctl, BrcmstbI2cState),
        VMSTATE_UINT32(iic_enable, BrcmstbI2cState),
        VMSTATE_UINT32_ARRAY(data_out, BrcmstbI2cState,
                             BRCMSTB_I2C_NUM_DATA_REGS),
        VMSTATE_UINT32(ctlhi, BrcmstbI2cState),
        VMSTATE_END_OF_LIST()
    }
};

static void brcmstb_i2c_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.enter = brcmstb_i2c_reset_enter;
    rc->phases.hold = brcmstb_i2c_reset_hold;
    dc->realize = brcmstb_i2c_realize;
    dc->vmsd = &vmstate_brcmstb_i2c;
    dc->desc = "Broadcom STB BSC I2C controller";
}

static const TypeInfo brcmstb_i2c_types[] = {
    {
        .name           = TYPE_BRCMSTB_I2C,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(BrcmstbI2cState),
        .instance_init  = brcmstb_i2c_init,
        .class_init     = brcmstb_i2c_class_init,
    },
};

DEFINE_TYPES(brcmstb_i2c_types)
