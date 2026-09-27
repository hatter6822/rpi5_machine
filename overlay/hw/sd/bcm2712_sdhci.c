/*
 * Broadcom BCM2712 SD/eMMC host controller
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * No public datasheet: as Linux drivers/mmc/host/sdhci-brcmstb.c
 * (match_priv_2712) and U-Boot's drivers/mmc/bcmstb_sdhci.c use it. The
 * BCM2712 has two: SDIO1 for the SD card, or a CM5's eMMC, and SDIO2 for
 * the Wi-Fi radio. Each is an SD Host Controller 3.00 with a command
 * queueing engine (CQHCI) at 0x200, followed at 0x400 by Broadcom
 * configuration registers:
 *
 *   0x000  SDIO_CFG_CTRL       bit 31 overrides card detect with bit 30,
 *                              the level of SDCD_N (0: a card)
 *   0x034  OP_DLY
 *   0x044  SD_PIN_SEL          bits 1:0, SD (2) or eMMC (1) timing
 *   0x04c  CQ_CAPABILITY       the command queueing engine's timer clock
 *   0x07c  PHY_SW_MODE_0_RX_CTRL
 *   0x1ac  MAX_50MHZ_MODE      bit 31 overrides the strap, bit 0 caps the
 *                              clock at 50 MHz
 *
 * The host is QEMU's SDHCI, with the capabilities below. The
 * configuration registers are storage: software reads back what it
 * writes, to no effect. The clock runs at any rate, and card detect
 * follows the card whatever SDIO_CFG_CTRL says: Linux and U-Boot override
 * it only to report a card in a slot that cannot lose it, which they never
 * check for one. The command queueing engine is not modelled: Linux uses
 * it only with cards that queue commands, which QEMU's do not.
 */

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/sd/bcm2712_sdhci.h"
#include "migration/vmstate.h"
#include "trace.h"

/*
 * The capabilities: SDMA, ADMA2 with 64-bit addresses (Linux reports
 * "using ADMA 64-bit" on a Pi 5), high speed, 8-bit buses, 3.3 V and
 * 1.8 V, and the UHS-I modes SDR50 (with tuning), SDR104 and DDR50; a
 * 200 MHz base clock, clk_emmc2 in bcm2712.dtsi; a 50 MHz timeout clock
 * and 512-byte blocks. No re-tuning mode, which would need a re-tuning
 * timer the model does not have: bcm2712.dtsi masks the silicon's out
 * of SDIO2's capabilities.
 * TODO(WS0.4): read the capabilities on hardware.
 */
#define BCM2712_SDHCI_CAPAREG   0x00002007156cc8b2ULL

static uint64_t bcm2712_sdhci_cfg_read(void *opaque, hwaddr offset,
                                       unsigned size)
{
    BCM2712SDHCIState *s = BCM2712_SDHCI(opaque);
    uint32_t value = s->cfg[offset / 4];

    trace_bcm2712_sdhci_cfg_read(DEVICE(s)->canonical_path, offset, value);
    return value;
}

static void bcm2712_sdhci_cfg_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size)
{
    BCM2712SDHCIState *s = BCM2712_SDHCI(opaque);

    trace_bcm2712_sdhci_cfg_write(DEVICE(s)->canonical_path, offset, value);
    s->cfg[offset / 4] = value;
}

static const MemoryRegionOps bcm2712_sdhci_cfg_ops = {
    .read = bcm2712_sdhci_cfg_read,
    .write = bcm2712_sdhci_cfg_write,
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

/* TODO(WS0.4): check the reset values of the configuration registers */
static void bcm2712_sdhci_reset_enter(Object *obj, ResetType type)
{
    BCM2712SDHCIState *s = BCM2712_SDHCI(obj);

    memset(s->cfg, 0, sizeof(s->cfg));
}

static void bcm2712_sdhci_init(Object *obj)
{
    BCM2712SDHCIState *s = BCM2712_SDHCI(obj);
    DeviceState *sdhci;

    object_initialize_child(obj, "sdhci", &s->sdhci, TYPE_SYSBUS_SDHCI);
    sdhci = DEVICE(&s->sdhci);
    qdev_prop_set_uint8(sdhci, "sd-spec-version", 3);
    qdev_prop_set_uint8(sdhci, "uhs", UHS_I);
    qdev_prop_set_uint64(sdhci, "capareg", BCM2712_SDHCI_CAPAREG);
    qdev_pass_gpios(sdhci, DEVICE(obj), "card-inserted");
}

static void bcm2712_sdhci_realize(DeviceState *dev, Error **errp)
{
    BCM2712SDHCIState *s = BCM2712_SDHCI(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    SysBusDevice *sdhci = SYS_BUS_DEVICE(&s->sdhci);

    if (!sysbus_realize(sdhci, errp)) {
        return;
    }
    /* The rest of the region, from the host's 0x100 bytes on, is a gap */
    memory_region_init(&s->container, OBJECT(s), TYPE_BCM2712_SDHCI,
                       BCM2712_SDHCI_SIZE);
    memory_region_add_subregion(&s->container, 0,
                                sysbus_mmio_get_region(sdhci, 0));
    memory_region_init_io(&s->cfg_iomem, OBJECT(s), &bcm2712_sdhci_cfg_ops,
                          s, TYPE_BCM2712_SDHCI "-cfg",
                          BCM2712_SDHCI_CFG_SIZE);
    memory_region_add_subregion(&s->container, BCM2712_SDHCI_CFG_OFFSET,
                                &s->cfg_iomem);
    sysbus_init_mmio(sbd, &s->container);
    sysbus_pass_irq(sbd, sdhci);
}

static const VMStateDescription vmstate_bcm2712_sdhci = {
    .name = TYPE_BCM2712_SDHCI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(cfg, BCM2712SDHCIState,
                             BCM2712_SDHCI_CFG_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void bcm2712_sdhci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.enter = bcm2712_sdhci_reset_enter;
    dc->realize = bcm2712_sdhci_realize;
    dc->vmsd = &vmstate_bcm2712_sdhci;
    dc->desc = "Broadcom BCM2712 SD/eMMC host controller";
}

static const TypeInfo bcm2712_sdhci_types[] = {
    {
        .name           = TYPE_BCM2712_SDHCI,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(BCM2712SDHCIState),
        .instance_init  = bcm2712_sdhci_init,
        .class_init     = bcm2712_sdhci_class_init,
    },
};

DEFINE_TYPES(bcm2712_sdhci_types)
