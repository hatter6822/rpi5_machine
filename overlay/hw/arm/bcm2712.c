/*
 * Broadcom BCM2712 SoC (Raspberry Pi 5)
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The BCM2712 couples four Cortex-A76 cores and a GIC-400 with a set of
 * Broadcom set-top-box (brcmstb) and legacy VideoCore peripherals. Most
 * board I/O (Ethernet, USB, GPIO, the 40-pin header) lives on the RP1
 * south bridge, which hangs off PCIe2 and is modelled separately.
 *
 * Sources: arch/arm64/boot/dts/broadcom/bcm2712.dtsi in Linux, and
 * https://www.raspberrypi.com/documentation/computers/processors.html
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/arm/bcm2712.h"
#include "hw/arm/bsa.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "hw/misc/bcm2835_mbox_defs.h"
#include "hw/misc/unimp.h"
#include "system/address-spaces.h"
#include "system/system.h"

/* Sizes follow the device tree "reg" properties (spanning multi-reg nodes) */
const MemMapEntry bcm2712_memmap[BCM2712_NUM_DEVICES] = {
    [BCM2712_AXI]           = { 0x1000000000, 0x7c000000 },
    [BCM2712_SOC]           = { 0x107c000000, 0x04000000 },

    [BCM2712_PCIE0]         = { 0x1000100000, 0x9310 },
    [BCM2712_PCIE1]         = { 0x1000110000, 0x9310 },
    [BCM2712_PCIE_RESCAL]   = { 0x1000119500, 0x10 },
    [BCM2712_PCIE2]         = { 0x1000120000, 0x9310 },
    [BCM2712_MIP0]          = { 0x1000130000, 0xc0 },
    [BCM2712_MIP1]          = { 0x1000131000, 0xc0 },
    [BCM2712_ISP]           = { 0x1000880000, 0x4000 },
    [BCM2712_SDIO1]         = { 0x1000fff000, 0x600 },
    [BCM2712_SDIO2]         = { 0x1001100000, 0x600 },
    [BCM2712_RESET]         = { 0x1001504318, 0x30 },
    [BCM2712_V3D]           = { 0x1002000000, 0x30f00 },

    [BCM2712_SYSTIMER]      = { 0x107c003000, 0x1000 },
    [BCM2712_MBOX]          = { 0x107c013880, 0x40 },
    [BCM2712_PIXELVALVE0]   = { 0x107c410000, 0x100 },
    [BCM2712_PIXELVALVE1]   = { 0x107c411000, 0x100 },
    [BCM2712_MOP]           = { 0x107c500000, 0x28 },
    [BCM2712_MOPLET]        = { 0x107c501000, 0x20 },
    [BCM2712_DISP_INTR]     = { 0x107c502000, 0x30 },
    [BCM2712_HVS]           = { 0x107c580000, 0x1a000 },
    [BCM2712_HDMI]          = { 0x107c700000, 0x20100 },
    [BCM2712_UART10]        = { 0x107d001000, 0x200 },
    [BCM2712_PM]            = { 0x107d200000, 0x604 },
    [BCM2712_RNG]           = { 0x107d208000, 0x28 },
    [BCM2712_PINCTRL]       = { 0x107d504100, 0x30 },
    [BCM2712_BSC]           = { 0x107d508200, 0x190 },
    [BCM2712_MAIN_IRQ]      = { 0x107d508400, 0x10 },
    [BCM2712_GIO]           = { 0x107d508500, 0x40 },
    [BCM2712_UARTA]         = { 0x107d50c000, 0x20 },
    [BCM2712_AON_INTR]      = { 0x107d510600, 0x30 },
    [BCM2712_PINCTRL_AON]   = { 0x107d510700, 0x20 },
    [BCM2712_L2_INTC]       = { 0x107d517000, 0x10 },
    [BCM2712_GIO_AON]       = { 0x107d517c00, 0x40 },
    [BCM2712_GIC]           = { 0x107fff8000, 0x8000 },
};

static const char *const bcm2712_device_names[BCM2712_NUM_DEVICES] = {
    [BCM2712_AXI]           = "bcm2712.axi",
    [BCM2712_SOC]           = "bcm2712.soc",
    [BCM2712_PCIE0]         = "bcm2712.pcie0",
    [BCM2712_PCIE1]         = "bcm2712.pcie1",
    [BCM2712_PCIE_RESCAL]   = "bcm2712.pcie-rescal",
    [BCM2712_PCIE2]         = "bcm2712.pcie2",
    [BCM2712_MIP0]          = "bcm2712.mip0",
    [BCM2712_MIP1]          = "bcm2712.mip1",
    [BCM2712_ISP]           = "bcm2712.isp",
    [BCM2712_SDIO1]         = "bcm2712.sdio1",
    [BCM2712_SDIO2]         = "bcm2712.sdio2",
    [BCM2712_RESET]         = "bcm2712.reset",
    [BCM2712_V3D]           = "bcm2712.v3d",
    [BCM2712_SYSTIMER]      = "bcm2712.systimer",
    [BCM2712_MBOX]          = "bcm2712.mbox",
    [BCM2712_PIXELVALVE0]   = "bcm2712.pixelvalve0",
    [BCM2712_PIXELVALVE1]   = "bcm2712.pixelvalve1",
    [BCM2712_MOP]           = "bcm2712.mop",
    [BCM2712_MOPLET]        = "bcm2712.moplet",
    [BCM2712_DISP_INTR]     = "bcm2712.disp-intr",
    [BCM2712_HVS]           = "bcm2712.hvs",
    [BCM2712_HDMI]          = "bcm2712.hdmi",
    [BCM2712_UART10]        = "bcm2712.uart10",
    [BCM2712_PM]            = "bcm2712.pm",
    [BCM2712_RNG]           = "bcm2712.rng",
    [BCM2712_PINCTRL]       = "bcm2712.pinctrl",
    [BCM2712_BSC]           = "bcm2712.bsc",
    [BCM2712_MAIN_IRQ]      = "bcm2712.main-irq",
    [BCM2712_GIO]           = "bcm2712.gio",
    [BCM2712_UARTA]         = "bcm2712.uarta",
    [BCM2712_AON_INTR]      = "bcm2712.aon-intr",
    [BCM2712_PINCTRL_AON]   = "bcm2712.pinctrl-aon",
    [BCM2712_L2_INTC]       = "bcm2712.l2-intc",
    [BCM2712_GIO_AON]       = "bcm2712.gio-aon",
    [BCM2712_GIC]           = "bcm2712.gic",
};

/* GIC-400 register frames, relative to bcm2712_memmap[BCM2712_GIC] */
#define GIC400_DIST_OFS             0x1000
#define GIC400_CPU_OFS              0x2000
#define GIC400_VIFACE_THIS_OFS      0x4000
#define GIC400_VIFACE_CPU_OFS(cpu)  (0x5000 + (cpu) * 0x200)
#define GIC400_VCPU_OFS             0x6000
#define GIC400_PRIORITY_BITS        5

/*
 * Catch-all windows sit below the named unimplemented regions (which
 * create_unimplemented_device() maps at -1000) so that stray accesses are
 * still logged under -d unimp rather than raising an external abort.
 */
#define BCM2712_CATCHALL_PRIORITY   -2000

static qemu_irq bcm2712_spi(BCM2712State *s, int spi)
{
    return qdev_get_gpio_in(DEVICE(&s->gic), spi);
}

static qemu_irq bcm2712_ppi(BCM2712State *s, int cpu, int intid)
{
    return qdev_get_gpio_in(DEVICE(&s->gic),
                            BCM2712_NUM_SPIS + cpu * GIC_INTERNAL + intid);
}

static void bcm2712_map(SysBusDevice *sbd, int n, BCM2712Device dev)
{
    sysbus_mmio_map(sbd, n, bcm2712_memmap[dev].base);
}

static void bcm2712_create_catchall(BCM2712Device dev)
{
    DeviceState *d = qdev_new(TYPE_UNIMPLEMENTED_DEVICE);

    qdev_prop_set_string(d, "name", bcm2712_device_names[dev]);
    qdev_prop_set_uint64(d, "size", bcm2712_memmap[dev].size);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(d), &error_fatal);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(d), 0, bcm2712_memmap[dev].base,
                            BCM2712_CATCHALL_PRIORITY);
}

static bool bcm2712_realize_cpus(BCM2712State *s, Error **errp)
{
    for (unsigned i = 0; i < s->num_cpus; i++) {
        Object *cpu;

        object_initialize_child(OBJECT(s), "cpu[*]", &s->cpu[i],
                                ARM_CPU_TYPE_NAME("cortex-a76"));
        cpu = OBJECT(&s->cpu[i]);

        /* Cortex-A76 reports its core number in MPIDR_EL1.Aff1 */
        object_property_set_uint(cpu, "mp-affinity", i << ARM_AFF1_SHIFT,
                                 &error_abort);
        object_property_set_uint(cpu, "cntfrq", BCM2712_CNTFRQ_HZ,
                                 &error_abort);
        /*
         * Without EL3 the guest starts in EL2, as it does when the VideoCore
         * firmware hands over through its resident TF-A BL31.
         */
        object_property_set_bool(cpu, "has_el3", s->has_el3, &error_abort);

        if (!qdev_realize(DEVICE(cpu), NULL, errp)) {
            return false;
        }
    }
    return true;
}

static bool bcm2712_realize_gic(BCM2712State *s, Error **errp)
{
    /*
     * Fixed GIC-400 PPI assignments. INTID 28 is the legacy nFIQ input on
     * the GIC-400, so the EL2 virtual timer has no interrupt line.
     */
    static const int timer_intid[] = {
        [GTIMER_PHYS] = ARCH_TIMER_NS_EL1_IRQ,
        [GTIMER_VIRT] = ARCH_TIMER_VIRT_IRQ,
        [GTIMER_HYP]  = ARCH_TIMER_NS_EL2_IRQ,
        [GTIMER_SEC]  = ARCH_TIMER_S_EL1_IRQ,
    };
    DeviceState *gicdev = DEVICE(&s->gic);
    SysBusDevice *gicsbd = SYS_BUS_DEVICE(&s->gic);
    hwaddr base = bcm2712_memmap[BCM2712_GIC].base;
    unsigned n = s->num_cpus;

    qdev_prop_set_uint32(gicdev, "revision", 2);
    qdev_prop_set_uint32(gicdev, "num-cpu", n);
    qdev_prop_set_uint32(gicdev, "num-irq", BCM2712_NUM_SPIS + GIC_INTERNAL);
    /* GIC-400 implements 32 priority levels (GIC-400 TRM, section 3.1) */
    qdev_prop_set_uint32(gicdev, "num-priority-bits", GIC400_PRIORITY_BITS);
    qdev_prop_set_bit(gicdev, "has-security-extensions", s->has_el3);
    qdev_prop_set_bit(gicdev, "has-virtualization-extensions", true);
    if (!sysbus_realize(gicsbd, errp)) {
        return false;
    }

    sysbus_mmio_map(gicsbd, 0, base + GIC400_DIST_OFS);
    sysbus_mmio_map(gicsbd, 1, base + GIC400_CPU_OFS);
    sysbus_mmio_map(gicsbd, 2, base + GIC400_VIFACE_THIS_OFS);
    sysbus_mmio_map(gicsbd, 3, base + GIC400_VCPU_OFS);
    for (unsigned i = 0; i < n; i++) {
        sysbus_mmio_map(gicsbd, 4 + i, base + GIC400_VIFACE_CPU_OFS(i));
    }

    for (unsigned i = 0; i < n; i++) {
        DeviceState *cpudev = DEVICE(&s->cpu[i]);

        for (int t = 0; t < ARRAY_SIZE(timer_intid); t++) {
            qdev_connect_gpio_out(cpudev, t, bcm2712_ppi(s, i, timer_intid[t]));
        }
        /*
         * Per-core SPIs, from the arm-pmu node of the Raspberry Pi firmware's
         * bcm2712-rpi-5-b.dtb. TODO(WS1.4): confirm on hardware.
         */
        qdev_connect_gpio_out_named(cpudev, "pmu-interrupt", 0,
                                    bcm2712_spi(s, BCM2712_SPI_PMU0 + i));

        sysbus_connect_irq(gicsbd, i,
                           qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
        sysbus_connect_irq(gicsbd, i + n,
                           qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
        sysbus_connect_irq(gicsbd, i + 2 * n,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VIRQ));
        sysbus_connect_irq(gicsbd, i + 3 * n,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VFIQ));
        sysbus_connect_irq(gicsbd, i + 4 * n,
                           bcm2712_ppi(s, i, ARCH_GIC_MAINT_IRQ));
    }
    return true;
}

static void bcm2712_init(Object *obj)
{
    BCM2712State *s = BCM2712(obj);

    object_initialize_child(obj, "gic", &s->gic, TYPE_ARM_GIC);
    object_initialize_child(obj, "systimer", &s->systimer,
                            TYPE_BCM2835_SYSTIMER);
    object_initialize_child(obj, "pm", &s->pm, TYPE_BCM2835_POWERMGT);

    memory_region_init(&s->mbox_chans, obj, "bcm2712.mbox-channels",
                       MBOX_CHAN_COUNT << MBOX_AS_CHAN_SHIFT);
    memory_region_init(&s->vc_bus, obj, "bcm2712.vc-bus", 4 * GiB);
    object_initialize_child(obj, "mbox", &s->mbox, TYPE_BCM2835_MBOX);
    object_property_add_const_link(OBJECT(&s->mbox), "mbox-mr",
                                   OBJECT(&s->mbox_chans));
    object_initialize_child(obj, "fb", &s->fb, TYPE_BCM2835_FB);
    object_property_add_const_link(OBJECT(&s->fb), "dma-mr",
                                   OBJECT(&s->vc_bus));
    object_initialize_child(obj, "otp", &s->otp, TYPE_BCM2835_OTP);
    object_initialize_child(obj, "property", &s->property,
                            TYPE_BCM2835_PROPERTY);
    object_property_add_alias(obj, "board-rev", OBJECT(&s->property),
                              "board-rev");
    object_property_add_alias(obj, "command-line", OBJECT(&s->property),
                              "command-line");
    object_property_add_const_link(OBJECT(&s->property), "fb",
                                   OBJECT(&s->fb));
    object_property_add_const_link(OBJECT(&s->property), "otp",
                                   OBJECT(&s->otp));
    object_property_add_const_link(OBJECT(&s->property), "dma-mr",
                                   OBJECT(&s->vc_bus));
    object_initialize_child(obj, "uart10", &s->uart10, TYPE_PL011);
}

/*
 * The VideoCore system timer: a 1 MHz free-running counter and four
 * comparators, the same block as on BCM2835. The device tree node spans
 * 0x1000 bytes; the registers occupy the first 0x20 and the rest stays
 * with the placeholder mapped beneath.
 */
static bool bcm2712_realize_systimer(BCM2712State *s, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(&s->systimer);

    if (!sysbus_realize(sbd, errp)) {
        return false;
    }
    bcm2712_map(sbd, 0, BCM2712_SYSTIMER);
    for (int i = 0; i < BCM2835_SYSTIMER_COUNT; i++) {
        sysbus_connect_irq(sbd, i, bcm2712_spi(s, BCM2712_SPI_SYSTIMER0 + i));
    }
    return true;
}

/*
 * Power management: the watchdog (with reboot and halt through it) and the
 * reset status, the same registers as on BCM2835. The power domain
 * registers after them stay with the placeholder mapped beneath; on
 * BCM2712 Linux only drives them for V3D, which is not modelled.
 */
static bool bcm2712_realize_pm(BCM2712State *s, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(&s->pm);

    if (!sysbus_realize(sbd, errp)) {
        return false;
    }
    bcm2712_map(sbd, 0, BCM2712_PM);
    return true;
}

/* Offset of MAIL0_READ in the bcm2835-mbox MMIO region */
#define BCM2712_MBOX_REGS_OFFSET    0x80

/* A firmware device answering on mailbox channel @chan */
static bool bcm2712_realize_mbox_client(BCM2712State *s, SysBusDevice *sbd,
                                        int chan, Error **errp)
{
    if (!sysbus_realize(sbd, errp)) {
        return false;
    }
    memory_region_add_subregion(&s->mbox_chans, chan << MBOX_AS_CHAN_SHIFT,
                                sysbus_mmio_get_region(sbd, 0));
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(DEVICE(&s->mbox), chan));
    return true;
}

/*
 * The VideoCore firmware interface: the ARM mailbox, and behind it the
 * property and framebuffer channels of the BCM283x models. Buffers are
 * read through the VideoCore's view of memory: the first GiB of RAM at
 * bus address 0xc000_0000, as Linux addresses it through the "soc"
 * node's dma-ranges, and at 0x0 as well, which firmware accepts from
 * code written for older Pis. Anything else goes unanswered.
 */
static bool bcm2712_realize_vc(BCM2712State *s, Error **errp)
{
    uint64_t window;
    SysBusDevice *sbd = SYS_BUS_DEVICE(&s->mbox);

    QEMU_BUILD_BUG_ON(BCM2712_VC_RAM_BASE + BCM2712_VC_RAM_SIZE !=
                      BCM2712_VC_RAM_WINDOW);

    if (!s->ram) {
        error_setg(errp, "%s: the 'ram' link is not set", TYPE_BCM2712);
        return false;
    }
    window = MIN(memory_region_size(s->ram), BCM2712_VC_RAM_WINDOW);
    for (int i = 0; i < ARRAY_SIZE(s->vc_ram); i++) {
        memory_region_init_alias(&s->vc_ram[i], OBJECT(s), "bcm2712.vc-ram",
                                 s->ram, 0, window);
    }
    memory_region_add_subregion(&s->vc_bus, 0, &s->vc_ram[0]);
    memory_region_add_subregion(&s->vc_bus, BCM2712_VC_RAM_BUS_BASE,
                                &s->vc_ram[1]);

    if (!sysbus_realize(sbd, errp)) {
        return false;
    }
    /*
     * The model's registers start 0x80 before MAIL0_READ, where the
     * BCM2835's ARM control block puts them; map just the node's window.
     */
    memory_region_init_alias(&s->mbox_regs, OBJECT(s), "bcm2712.mbox",
                             sysbus_mmio_get_region(sbd, 0),
                             BCM2712_MBOX_REGS_OFFSET,
                             bcm2712_memmap[BCM2712_MBOX].size);
    memory_region_add_subregion(get_system_memory(),
                                bcm2712_memmap[BCM2712_MBOX].base,
                                &s->mbox_regs);
    sysbus_connect_irq(sbd, 0, bcm2712_spi(s, BCM2712_SPI_MBOX));

    if (!object_property_set_uint(OBJECT(&s->fb), "vcram-base",
                                  BCM2712_VC_RAM_BASE, errp) ||
        !object_property_set_uint(OBJECT(&s->fb), "vcram-size",
                                  BCM2712_VC_RAM_SIZE, errp) ||
        !bcm2712_realize_mbox_client(s, SYS_BUS_DEVICE(&s->fb), MBOX_CHAN_FB,
                                     errp) ||
        !sysbus_realize(SYS_BUS_DEVICE(&s->otp), errp)) {
        return false;
    }
    return bcm2712_realize_mbox_client(s, SYS_BUS_DEVICE(&s->property),
                                       MBOX_CHAN_PROPERTY, errp);
}

static void bcm2712_realize(DeviceState *dev, Error **errp)
{
    BCM2712State *s = BCM2712(dev);

    if (s->num_cpus < 1 || s->num_cpus > BCM2712_NUM_CPUS) {
        error_setg(errp, "%s: num-cpus must be between 1 and %d (got %u)",
                   TYPE_BCM2712, BCM2712_NUM_CPUS, s->num_cpus);
        return;
    }

    if (!bcm2712_realize_cpus(s, errp) || !bcm2712_realize_gic(s, errp) ||
        !bcm2712_realize_systimer(s, errp) || !bcm2712_realize_pm(s, errp) ||
        !bcm2712_realize_vc(s, errp)) {
        return;
    }

    /* UART10: the PL011 debug UART on the 3-pin JST header */
    qdev_prop_set_chr(DEVICE(&s->uart10), "chardev", serial_hd(0));
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->uart10), errp)) {
        return;
    }
    bcm2712_map(SYS_BUS_DEVICE(&s->uart10), 0, BCM2712_UART10);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->uart10), 0,
                       bcm2712_spi(s, BCM2712_SPI_UART10));

    /*
     * Everything not yet modelled logs its accesses under -d unimp. The
     * placeholders sit below the models, so a block whose model is smaller
     * than its device tree node (the system timer, PM) keeps one for the rest.
     */
    for (BCM2712Device d = 0; d < BCM2712_NUM_DEVICES; d++) {
        switch (d) {
        case BCM2712_AXI:
        case BCM2712_SOC:
            bcm2712_create_catchall(d);
            break;
        case BCM2712_GIC:
        case BCM2712_UART10:
            break;
        default:
            create_unimplemented_device(bcm2712_device_names[d],
                                        bcm2712_memmap[d].base,
                                        bcm2712_memmap[d].size);
            break;
        }
    }
}

static const Property bcm2712_properties[] = {
    DEFINE_PROP_UINT32("num-cpus", BCM2712State, num_cpus, BCM2712_NUM_CPUS),
    DEFINE_PROP_BOOL("has-el3", BCM2712State, has_el3, false),
    DEFINE_PROP_LINK("ram", BCM2712State, ram, TYPE_MEMORY_REGION,
                     MemoryRegion *),
};

static void bcm2712_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_props(dc, bcm2712_properties);
    dc->realize = bcm2712_realize;
    /* Embedded in the raspi5b machine; reads serial_hd() directly */
    dc->user_creatable = false;
}

static const TypeInfo bcm2712_types[] = {
    {
        .name           = TYPE_BCM2712,
        .parent         = TYPE_DEVICE,
        .instance_size  = sizeof(BCM2712State),
        .instance_init  = bcm2712_init,
        .class_init     = bcm2712_class_init,
    },
};

DEFINE_TYPES(bcm2712_types)
