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
#include "hw/arm/fdt.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "hw/misc/bcm2835_mbox_defs.h"
#include "hw/misc/unimp.h"
#include "qobject/qlist.h"
#include "system/address-spaces.h"
#include "system/device_tree.h"
#include "system/system.h"
#include <libfdt.h>

/* Sizes follow the device tree "reg" properties (spanning multi-reg nodes) */
const MemMapEntry bcm2712_memmap[BCM2712_NUM_DEVICES] = {
    [BCM2712_AXI]           = { 0x1000000000, 0x7c000000 },
    [BCM2712_SOC]           = { 0x107c000000, 0x04000000 },

    [BCM2712_PCIE0_MEM]     = { 0x1400000000, 16 * GiB },
    [BCM2712_PCIE1_MEM]     = { 0x1800000000, 16 * GiB },
    [BCM2712_PCIE2_MEM]     = { 0x1c00000000, 16 * GiB },

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
    [BCM2712_PM]            = { 0x107d200000, 0x308 },
    [BCM2712_RNG]           = { 0x107d208000, 0x28 },
    [BCM2712_CPU_L2_IRQ]    = { 0x107d503000, 0x18 },
    [BCM2712_PINCTRL]       = { 0x107d504100, 0x30 },
    [BCM2712_DDC0]          = { 0x107d508200, 0x58 },
    [BCM2712_DDC1]          = { 0x107d508280, 0x58 },
    [BCM2712_BSC_IRQ]       = { 0x107d508380, 0x10 },
    [BCM2712_MAIN_IRQ]      = { 0x107d508400, 0x10 },
    [BCM2712_GIO]           = { 0x107d508500, 0x40 },
    [BCM2712_UARTA]         = { 0x107d50c000, 0x20 },
    [BCM2712_AON_INTR]      = { 0x107d510600, 0x30 },
    [BCM2712_PINCTRL_AON]   = { 0x107d510700, 0x20 },
    [BCM2712_L2_INTC]       = { 0x107d517000, 0x10 },
    [BCM2712_MAIN_AON_IRQ]  = { 0x107d517ac0, 0x10 },
    [BCM2712_GIO_AON]       = { 0x107d517c00, 0x40 },
    [BCM2712_AVS]           = { 0x107d542000, 0xf00 },
    [BCM2712_GIC]           = { 0x107fff8000, 0x8000 },
};

static const char *const bcm2712_device_names[BCM2712_NUM_DEVICES] = {
    [BCM2712_AXI]           = "bcm2712.axi",
    [BCM2712_SOC]           = "bcm2712.soc",
    [BCM2712_PCIE0_MEM]     = "bcm2712.pcie0-mem",
    [BCM2712_PCIE1_MEM]     = "bcm2712.pcie1-mem",
    [BCM2712_PCIE2_MEM]     = "bcm2712.pcie2-mem",
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
    [BCM2712_CPU_L2_IRQ]    = "bcm2712.cpu-l2-irq",
    [BCM2712_PINCTRL]       = "bcm2712.pinctrl",
    [BCM2712_DDC0]          = "bcm2712.ddc0",
    [BCM2712_DDC1]          = "bcm2712.ddc1",
    [BCM2712_BSC_IRQ]       = "bcm2712.bsc-irq",
    [BCM2712_MAIN_IRQ]      = "bcm2712.main-irq",
    [BCM2712_GIO]           = "bcm2712.gio",
    [BCM2712_UARTA]         = "bcm2712.uarta",
    [BCM2712_AON_INTR]      = "bcm2712.aon-intr",
    [BCM2712_PINCTRL_AON]   = "bcm2712.pinctrl-aon",
    [BCM2712_L2_INTC]       = "bcm2712.l2-intc",
    [BCM2712_MAIN_AON_IRQ]  = "bcm2712.main-aon-irq",
    [BCM2712_GIO_AON]       = "bcm2712.gio-aon",
    [BCM2712_AVS]           = "bcm2712.avs-monitor",
    [BCM2712_GIC]           = "bcm2712.gic",
};

#define L2_COMPAT(s)    .compat = s, .compat_len = sizeof(s)
#define L2_EDGE_COMPAT  L2_COMPAT("brcm,l2-intc")
#define L2_2711_COMPAT  L2_COMPAT("brcm,bcm2711-l2-intc\0brcm,l2-intc")
#define L2_LEVEL_COMPAT L2_COMPAT("brcm,bcm7271-l2-intc")
typedef enum { L2_LEVEL, L2_EDGE, L2_2711 } BCM2712L2Variant;

/*
 * The level 2 interrupt controllers, as in the firmware's device tree,
 * which leaves four of them disabled, as the Pi 5's sources do: the
 * display's, the always-on block's two (the main one "will clash with the
 * firmware monitoring the PMIC interrupt via the VPU") and the one at
 * 0x7d517000, whose SPI PCIe1's MSIs use.
 */
static const struct {
    const char *name;
    BCM2712Device dev;
    int spi;
    BCM2712L2Variant variant;
    bool disabled;
    const char *compat;
    size_t compat_len;
} bcm2712_l2_intcs[BCM2712_NUM_L2_INTCS] = {
    [BCM2712_L2_DISP_INTR] = {
        "disp-intr", BCM2712_DISP_INTR, BCM2712_SPI_DISP_INTR, L2_2711, true,
        L2_2711_COMPAT,
    },
    [BCM2712_L2_CPU_L2_IRQ] = {
        "cpu-l2-irq", BCM2712_CPU_L2_IRQ, BCM2712_SPI_CPU_L2_IRQ, L2_EDGE,
        false, L2_EDGE_COMPAT,
    },
    [BCM2712_L2_BSC_IRQ] = {
        "bsc-irq", BCM2712_BSC_IRQ, BCM2712_SPI_BSC, L2_LEVEL, false,
        L2_LEVEL_COMPAT,
    },
    [BCM2712_L2_MAIN_IRQ] = {
        "main-irq", BCM2712_MAIN_IRQ, BCM2712_SPI_MAIN_IRQ, L2_LEVEL, false,
        L2_LEVEL_COMPAT,
    },
    [BCM2712_L2_AON_INTR] = {
        "aon-intr", BCM2712_AON_INTR, BCM2712_SPI_AON_INTR, L2_2711, true,
        L2_2711_COMPAT,
    },
    [BCM2712_L2_7D517000] = {
        "l2-intc", BCM2712_L2_INTC, BCM2712_SPI_L2_INTC, L2_LEVEL, true,
        L2_LEVEL_COMPAT,
    },
    [BCM2712_L2_MAIN_AON_IRQ] = {
        "main-aon-irq", BCM2712_MAIN_AON_IRQ, BCM2712_SPI_MAIN_AON_IRQ,
        L2_LEVEL, true, L2_LEVEL_COMPAT,
    },
};

/*
 * The lines in each bank of the two GPIO blocks, as bcm2712.dtsi has
 * them. The Pi 5's own tree trims GIO's second bank to the 4 lines the
 * board uses, which raspi5b.c does to its node of the tree.
 */
static const uint32_t bcm2712_gio_widths[] = { 32, 22 };
static const uint32_t bcm2712_gio_aon_widths[] = { 17, 6 };

/* The DDC I2C controllers, by HDMI port, and their bsc_irq inputs */
static const struct {
    BCM2712Device dev;
    int irq;
} bcm2712_ddcs[BCM2712_NUM_HDMI] = {
    { BCM2712_DDC0, BCM2712_BSC_IRQ_DDC0 },
    { BCM2712_DDC1, BCM2712_BSC_IRQ_DDC1 },
};

/* The SD/eMMC host controllers and their interrupts */
static const struct {
    const char *name;
    BCM2712Device dev;
    int spi;
} bcm2712_sdios[BCM2712_NUM_SDIO] = {
    { "sdio1", BCM2712_SDIO1, BCM2712_SPI_SDIO1 },
    { "sdio2", BCM2712_SDIO2, BCM2712_SPI_SDIO2 },
};

/*
 * The PCIe root complexes: registers, outbound aperture, interrupts
 * (INTA's, then "pcie" and "msi"), and the root ports' links. PCIe1's
 * and PCIe2's are those lspci shows on a Pi 5; PCIe0's are taken to be
 * PCIe1's. TODO(WS0.4): check PCIe0's link capabilities on hardware.
 */
static const struct {
    const char *name;
    BCM2712Device dev;
    BCM2712Device mem;
    int spi_inta;
    int spi;
    int spi_msi;
    uint32_t num_lanes;
    uint32_t max_link_speed;
    bool aspm_l0s;
    bool l1ss;
} bcm2712_pcies[BCM2712_NUM_PCIE] = {
    { "pcie0", BCM2712_PCIE0, BCM2712_PCIE0_MEM, BCM2712_SPI_PCIE0_INTA,
      BCM2712_SPI_PCIE0, BCM2712_SPI_PCIE0_MSI, 1, 2, true, false },
    { "pcie1", BCM2712_PCIE1, BCM2712_PCIE1_MEM, BCM2712_SPI_PCIE1_INTA,
      BCM2712_SPI_PCIE1, BCM2712_SPI_PCIE1_MSI, 1, 2, true, false },
    { "pcie2", BCM2712_PCIE2, BCM2712_PCIE2_MEM, BCM2712_SPI_PCIE2_INTA,
      BCM2712_SPI_PCIE2, BCM2712_SPI_PCIE2_MSI, 4, 2, false, true },
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

        /*
         * Cortex-A76 reports its core number in MPIDR_EL1.Aff1, with the
         * MT bit set and thread 0 in Aff0, as a DynamIQ core does.
         * Firmware numbers the cores by the MT bit: TF-A shifts the
         * affinity down a level when it is set.
         */
        object_property_set_uint(cpu, "mp-affinity", i << ARM_AFF1_SHIFT,
                                 &error_abort);
        object_property_set_bool(cpu, "mpidr-mt", true, &error_abort);
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
    /*
     * The GIC has a region for each CPU's own CPU interface (4 .. 4 + n
     * - 1), which the GIC-400 lacks, and then one for each CPU's virtual
     * interface control block (4 + n ..), which it aliases at 0x5000.
     */
    for (unsigned i = 0; i < n; i++) {
        sysbus_mmio_map(gicsbd, 4 + n + i, base + GIC400_VIFACE_CPU_OFS(i));
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
    for (int i = 0; i < BCM2712_NUM_L2_INTCS; i++) {
        object_initialize_child(obj, bcm2712_l2_intcs[i].name, &s->l2_intc[i],
                                TYPE_BRCMSTB_L2_INTC);
    }
    object_initialize_child(obj, "gio", &s->gio, TYPE_BRCMSTB_GPIO);
    object_initialize_child(obj, "gio-aon", &s->gio_aon, TYPE_BRCMSTB_GPIO);
    object_initialize_child(obj, "pinctrl", &s->pinctrl, TYPE_BRCMSTB_PINCTRL);
    object_initialize_child(obj, "pinctrl-aon", &s->pinctrl_aon,
                            TYPE_BRCMSTB_PINCTRL);
    for (int i = 0; i < BCM2712_NUM_HDMI; i++) {
        g_autofree char *name = g_strdup_printf("ddc%d", i);

        object_initialize_child(obj, name, &s->ddc[i], TYPE_BRCMSTB_I2C);
    }
    for (int i = 0; i < BCM2712_NUM_SDIO; i++) {
        object_initialize_child(obj, bcm2712_sdios[i].name, &s->sdio[i],
                                TYPE_BCM2712_SDHCI);
    }
    /* The SD card slot */
    object_property_add_alias(obj, "sd-bus", OBJECT(&s->sdio[0].sdhci),
                              "sd-bus");
    object_initialize_child(obj, "reset", &s->reset, TYPE_BRCMSTB_RESET);
    object_initialize_child(obj, "pcie-rescal", &s->rescal,
                            TYPE_BRCMSTB_RESCAL);
    for (int i = 0; i < BCM2712_NUM_PCIE; i++) {
        object_initialize_child(obj, bcm2712_pcies[i].name, &s->pcie[i],
                                TYPE_BRCMSTB_PCIE_HOST);
    }
    /* PCIe2 as the boot firmware leaves it with pciex4_reset=0 */
    object_property_add_alias(obj, "pcie2-preinit", OBJECT(&s->pcie[2]),
                              "preinit");
    object_initialize_child(obj, "mip0", &s->mip[0], TYPE_BCM2712_MIP);
    object_initialize_child(obj, "mip1", &s->mip[1], TYPE_BCM2712_MIP);
    object_initialize_child(obj, "systimer", &s->systimer,
                            TYPE_BCM2835_SYSTIMER);
    object_initialize_child(obj, "pm", &s->pm, TYPE_BCM2835_POWERMGT);
    object_initialize_child(obj, "rng", &s->rng, TYPE_BCM2711_RNG200);
    object_initialize_child(obj, "avs-monitor", &s->avs,
                            TYPE_BCM2711_AVS_MONITOR);

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
                            TYPE_BCM2712_PROPERTY);
    object_property_add_alias(obj, "board-rev", OBJECT(&s->property),
                              "board-rev");
    object_property_add_alias(obj, "command-line", OBJECT(&s->property),
                              "command-line");
    object_property_add_alias(obj, "board-serial", OBJECT(&s->property),
                              "board-serial");
    object_property_add_const_link(OBJECT(&s->property), "fb",
                                   OBJECT(&s->fb));
    object_property_add_const_link(OBJECT(&s->property), "otp",
                                   OBJECT(&s->otp));
    object_property_add_const_link(OBJECT(&s->property), "dma-mr",
                                   OBJECT(&s->vc_bus));
    object_initialize_child(obj, "uart10", &s->uart10, TYPE_PL011);
    object_initialize_child(obj, "uarta", &s->uarta, TYPE_SERIAL_MM);
}

/*
 * The level 2 interrupt controllers. The display and always-on blocks
 * have more registers after theirs, which stay with the placeholders
 * mapped beneath.
 */
static bool bcm2712_realize_l2_intcs(BCM2712State *s, Error **errp)
{
    for (int i = 0; i < BCM2712_NUM_L2_INTCS; i++) {
        SysBusDevice *sbd = SYS_BUS_DEVICE(&s->l2_intc[i]);

        qdev_prop_set_bit(DEVICE(sbd), "edge",
                          bcm2712_l2_intcs[i].variant == L2_EDGE);
        qdev_prop_set_bit(DEVICE(sbd), "bcm2711",
                          bcm2712_l2_intcs[i].variant == L2_2711);
        if (!sysbus_realize(sbd, errp)) {
            return false;
        }
        bcm2712_map(sbd, 0, bcm2712_l2_intcs[i].dev);
        sysbus_connect_irq(sbd, 0, bcm2712_spi(s, bcm2712_l2_intcs[i].spi));
    }
    return true;
}

static bool bcm2712_realize_gpio(BrcmstbGpioState *gpio, BCM2712Device dev,
                                 const uint32_t *widths, size_t banks,
                                 Error **errp)
{
    QList *list = qlist_new();

    for (size_t i = 0; i < banks; i++) {
        qlist_append_int(list, widths[i]);
    }
    qdev_prop_set_array(DEVICE(gpio), "bank-widths", list);
    if (!sysbus_realize(SYS_BUS_DEVICE(gpio), errp)) {
        return false;
    }
    bcm2712_map(SYS_BUS_DEVICE(gpio), 0, dev);
    return true;
}

/*
 * The two GPIO blocks. GIO interrupts through the main level 2
 * controller. GIO AON's interrupt output stays unconnected: no tree says
 * where it goes, and bcm2712.dtsi deliberately leaves the block without
 * interrupt-controller, as the firmware watches the PMIC's interrupt
 * line through it.
 */
static bool bcm2712_realize_gpios(BCM2712State *s, Error **errp)
{
    DeviceState *main_irq = DEVICE(&s->l2_intc[BCM2712_L2_MAIN_IRQ]);

    if (!bcm2712_realize_gpio(&s->gio, BCM2712_GIO, bcm2712_gio_widths,
                              ARRAY_SIZE(bcm2712_gio_widths), errp) ||
        !bcm2712_realize_gpio(&s->gio_aon, BCM2712_GIO_AON,
                              bcm2712_gio_aon_widths,
                              ARRAY_SIZE(bcm2712_gio_aon_widths), errp)) {
        return false;
    }
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->gio), 0,
                       qdev_get_gpio_in(main_irq, BCM2712_MAIN_IRQ_GIO));
    return true;
}

/* A pin controller, with as many registers as its device tree node spans */
static bool bcm2712_realize_pinctrl(BrcmstbPinctrlState *pinctrl,
                                    BCM2712Device dev, Error **errp)
{
    qdev_prop_set_uint32(DEVICE(pinctrl), "num-regs",
                         bcm2712_memmap[dev].size / 4);
    if (!sysbus_realize(SYS_BUS_DEVICE(pinctrl), errp)) {
        return false;
    }
    bcm2712_map(SYS_BUS_DEVICE(pinctrl), 0, dev);
    return true;
}

/* The DDC I2C controllers, which interrupt through bsc_irq */
static bool bcm2712_realize_ddcs(BCM2712State *s, Error **errp)
{
    DeviceState *bsc_irq = DEVICE(&s->l2_intc[BCM2712_L2_BSC_IRQ]);

    for (int i = 0; i < BCM2712_NUM_HDMI; i++) {
        SysBusDevice *sbd = SYS_BUS_DEVICE(&s->ddc[i]);

        if (!sysbus_realize(sbd, errp)) {
            return false;
        }
        bcm2712_map(sbd, 0, bcm2712_ddcs[i].dev);
        sysbus_connect_irq(sbd, 0,
                           qdev_get_gpio_in(bsc_irq, bcm2712_ddcs[i].irq));
    }
    return true;
}

/*
 * The SD/eMMC host controllers. Their MMIO regions leave gaps, the
 * command queueing engines among them, to the placeholders beneath.
 */
static bool bcm2712_realize_sdios(BCM2712State *s, Error **errp)
{
    for (int i = 0; i < BCM2712_NUM_SDIO; i++) {
        SysBusDevice *sbd = SYS_BUS_DEVICE(&s->sdio[i]);

        if (!sysbus_realize(sbd, errp)) {
            return false;
        }
        bcm2712_map(sbd, 0, bcm2712_sdios[i].dev);
        sysbus_connect_irq(sbd, 0, bcm2712_spi(s, bcm2712_sdios[i].spi));
    }
    return true;
}

/*
 * The MIPs: an MSI from PCIe2 (through MIP0) or PCIe1 (MIP1) raises an
 * SPI of its own. MIP1's first vectors are not connected, as the SPIs
 * they would raise belong to other blocks.
 */
static bool bcm2712_realize_mips(BCM2712State *s, Error **errp)
{
    for (int i = 0; i < BCM2712_NUM_MIP; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->mip[i]), errp)) {
            return false;
        }
        bcm2712_map(SYS_BUS_DEVICE(&s->mip[i]), 0, BCM2712_MIP0 + i);
    }
    for (int n = 0; n < BCM2712_MIP_NUM_IRQS; n++) {
        qdev_connect_gpio_out(DEVICE(&s->mip[0]), n,
                              bcm2712_spi(s, BCM2712_SPI_MIP0_BASE + n));
    }
    for (int n = BCM2712_MIP1_FIRST_VECTOR; n < 16; n++) {
        qdev_connect_gpio_out(DEVICE(&s->mip[1]), n,
                              bcm2712_spi(s, BCM2712_SPI_MIP1_BASE + n));
    }
    return true;
}

/*
 * The PCIe root complexes, with the reset controller that drives their
 * bridge resets and the calibration of their PHYs. Each root port's
 * secondary bus is named "pcie<n>.0", after the controller's PCI domain
 * in the device tree. Their DMA reaches the whole physical address space
 * through their inbound windows.
 */
static bool bcm2712_realize_pcie(BCM2712State *s, Error **errp)
{
    SysBusDevice *reset = SYS_BUS_DEVICE(&s->reset);

    qdev_prop_set_uint32(DEVICE(reset), "num-banks",
                         bcm2712_memmap[BCM2712_RESET].size /
                         BRCMSTB_RESET_BANK_SIZE);
    if (!sysbus_realize(reset, errp) ||
        !sysbus_realize(SYS_BUS_DEVICE(&s->rescal), errp)) {
        return false;
    }
    bcm2712_map(reset, 0, BCM2712_RESET);
    bcm2712_map(SYS_BUS_DEVICE(&s->rescal), 0, BCM2712_PCIE_RESCAL);

    for (int i = 0; i < BCM2712_NUM_PCIE; i++) {
        SysBusDevice *sbd = SYS_BUS_DEVICE(&s->pcie[i]);
        DeviceState *dev = DEVICE(sbd);
        g_autofree char *bus_name = g_strdup_printf("pcie%d.0", i);

        qdev_prop_set_uint32(dev, "domain", i);
        qdev_prop_set_string(dev, "bus-name", bus_name);
        qdev_prop_set_uint64(dev, "outbound-base",
                             bcm2712_memmap[bcm2712_pcies[i].mem].base);
        qdev_prop_set_uint64(dev, "outbound-size",
                             bcm2712_memmap[bcm2712_pcies[i].mem].size);
        qdev_prop_set_uint32(dev, "num-lanes", bcm2712_pcies[i].num_lanes);
        qdev_prop_set_uint32(dev, "max-link-speed",
                             bcm2712_pcies[i].max_link_speed);
        qdev_prop_set_bit(dev, "aspm-l0s", bcm2712_pcies[i].aspm_l0s);
        qdev_prop_set_bit(dev, "l1ss", bcm2712_pcies[i].l1ss);
        object_property_set_link(OBJECT(dev), "dma-memory",
                                 OBJECT(get_system_memory()), &error_abort);
        if (!sysbus_realize(sbd, errp)) {
            return false;
        }
        bcm2712_map(sbd, 0, bcm2712_pcies[i].dev);
        bcm2712_map(sbd, 1, bcm2712_pcies[i].mem);
        for (int n = 0; n < BRCMSTB_PCIE_NUM_INTX; n++) {
            sysbus_connect_irq(sbd, BRCMSTB_PCIE_IRQ_INTA + n,
                               bcm2712_spi(s, bcm2712_pcies[i].spi_inta + n));
        }
        sysbus_connect_irq(sbd, BRCMSTB_PCIE_IRQ_PCIE,
                           bcm2712_spi(s, bcm2712_pcies[i].spi));
        sysbus_connect_irq(sbd, BRCMSTB_PCIE_IRQ_MSI,
                           bcm2712_spi(s, bcm2712_pcies[i].spi_msi));
        qdev_connect_gpio_out_named(DEVICE(reset), "reset",
                                    BCM2712_RESET_PCIE0_BRIDGE + i,
                                    qdev_get_gpio_in_named(dev,
                                                           "bridge-reset",
                                                           0));
    }
    return true;
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

/*
 * The RNG200, as on BCM2711. The device tree gives it no interrupt, so
 * its output stays unconnected.
 */
static bool bcm2712_realize_rng(BCM2712State *s, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(&s->rng);

    if (!sysbus_realize(sbd, errp)) {
        return false;
    }
    bcm2712_map(sbd, 0, BCM2712_RNG);
    return true;
}

/* The AVS monitor, whose temperature code converts as the Pi 5's trees say */
static bool bcm2712_realize_avs(BCM2712State *s, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(&s->avs);

    qdev_prop_set_int32(DEVICE(sbd), "slope", BCM2712_AVS_TEMP_SLOPE);
    qdev_prop_set_int32(DEVICE(sbd), "offset", BCM2712_AVS_TEMP_OFFSET);
    if (!sysbus_realize(sbd, errp)) {
        return false;
    }
    bcm2712_map(sbd, 0, BCM2712_AVS);
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
 * bus address 0x0, where Linux addresses it (the firmware's device tree
 * gives the "soc" node no dma-ranges), and at 0xc000_0000, the alias
 * code written for older Pis uses. A property request whose buffer lies
 * anywhere else is not answered; the framebuffer channel, which does not
 * check, reads zeros there.
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
        g_autofree char *name = g_strdup_printf("bcm2712.vc-ram%d", i);

        memory_region_init_alias(&s->vc_ram[i], OBJECT(s), name, s->ram, 0,
                                 window);
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
        !sysbus_realize(SYS_BUS_DEVICE(&s->otp), errp) ||
        !object_property_set_uint(OBJECT(&s->property), "dma-channel-mask",
                                  BCM2712_DMA_CHANNEL_MASK, errp) ||
        !object_property_set_link(OBJECT(&s->property), "avs-monitor",
                                  OBJECT(&s->avs), errp)) {
        return false;
    }
    return bcm2712_realize_mbox_client(s, SYS_BUS_DEVICE(&s->property),
                                       MBOX_CHAN_PROPERTY, errp);
}

/* UARTA's FIFOs, as Linux's 8250 driver has them for a BCM7271 UART */
#define BCM2712_UARTA_FIFO_SIZE     32

/*
 * The serial ports, in an order that stays as more are modelled: UART10,
 * the PL011 debug UART on the 3-pin JST header, is serial_hd(0). UARTA, a
 * 16550 with 32-bit registers wired to the Bluetooth radio on the Pi 5,
 * is serial_hd(1); its baud rate divides the 96 MHz sw_baud clock by 16
 * and by the divisor.
 */
static bool bcm2712_realize_uarts(BCM2712State *s, Error **errp)
{
    DeviceState *uarta = DEVICE(&s->uarta);

    qdev_prop_set_chr(DEVICE(&s->uart10), "chardev", serial_hd(0));
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->uart10), errp)) {
        return false;
    }
    bcm2712_map(SYS_BUS_DEVICE(&s->uart10), 0, BCM2712_UART10);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->uart10), 0,
                       bcm2712_spi(s, BCM2712_SPI_UART10));

    qdev_prop_set_uint8(uarta, "regshift", 2);
    qdev_prop_set_uint32(uarta, "baudbase", BCM2712_UARTA_CLK_HZ / 16);
    qdev_prop_set_uint8(uarta, "endianness", DEVICE_LITTLE_ENDIAN);
    qdev_prop_set_uint32(uarta, "fifo-size", BCM2712_UARTA_FIFO_SIZE);
    qdev_prop_set_chr(uarta, "chardev", serial_hd(1));
    if (!sysbus_realize(SYS_BUS_DEVICE(uarta), errp)) {
        return false;
    }
    bcm2712_map(SYS_BUS_DEVICE(uarta), 0, BCM2712_UARTA);
    sysbus_connect_irq(SYS_BUS_DEVICE(uarta), 0,
                       bcm2712_spi(s, BCM2712_SPI_UARTA));
    return true;
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
        !bcm2712_realize_l2_intcs(s, errp) ||
        !bcm2712_realize_gpios(s, errp) ||
        !bcm2712_realize_pinctrl(&s->pinctrl, BCM2712_PINCTRL, errp) ||
        !bcm2712_realize_pinctrl(&s->pinctrl_aon, BCM2712_PINCTRL_AON, errp) ||
        !bcm2712_realize_ddcs(s, errp) || !bcm2712_realize_sdios(s, errp) ||
        !bcm2712_realize_mips(s, errp) || !bcm2712_realize_pcie(s, errp) ||
        !bcm2712_realize_systimer(s, errp) || !bcm2712_realize_pm(s, errp) ||
        !bcm2712_realize_rng(s, errp) || !bcm2712_realize_avs(s, errp) ||
        !bcm2712_realize_vc(s, errp) || !bcm2712_realize_uarts(s, errp)) {
        return;
    }

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
        case BCM2712_PCIE0_MEM:
        case BCM2712_PCIE1_MEM:
        case BCM2712_PCIE2_MEM:
            break;
        default:
            create_unimplemented_device(bcm2712_device_names[d],
                                        bcm2712_memmap[d].base,
                                        bcm2712_memmap[d].size);
            break;
        }
    }
}

/*
 * Device tree
 *
 * Node names, compatibles and properties follow bcm2712.dtsi in Linux and
 * the firmware's bcm2712-rpi-5-b.dtb; every address comes from
 * bcm2712_memmap. Only modelled devices get a node.
 */

/* The "soc" bus: child address 0 is CPU address 0x10_0000_0000 */
#define BCM2712_FDT_SOC_PATH        "/soc@107c000000"
#define BCM2712_FDT_SOC_BUS_BASE    0x1000000000ULL
#define BCM2712_FDT_SOC_BUS_SIZE    0x80000000U

/* The "axi" bus, whose addresses are CPU addresses */
#define BCM2712_FDT_AXI_PATH        "/axi"

/* The DDC buses' speed in bcm2712.dtsi, in Hz; the model has none */
#define BCM2712_FDT_DDC_HZ          97500

/* Fixed clocks of the firmware's tree, in Hz */
#define BCM2712_FDT_CLK_OSC         54000000
#define BCM2712_FDT_CLK_VPU         750000000
#define BCM2712_FDT_CLK_UART        9216000
#define BCM2712_FDT_CLK_EMMC2       200000000   /* the SD hosts' base clock */

/* The SD hosts' "host" registers in the tree: SDHCI and command queueing */
#define BCM2712_FDT_SDHCI_HOST_SIZE 0x260

/* The firmware's default CMA pool */
#define BCM2712_FDT_CMA_SIZE        (64 * MiB)

static uint32_t bcm2712_fdt_bus_addr(hwaddr addr)
{
    assert(addr >= BCM2712_FDT_SOC_BUS_BASE &&
           addr - BCM2712_FDT_SOC_BUS_BASE < BCM2712_FDT_SOC_BUS_SIZE);
    return addr - BCM2712_FDT_SOC_BUS_BASE;
}

/*
 * Add "<name>@<bus address>" for @dev under the "soc" bus, with a "reg"
 * covering its memory map entry and @compat (NUL-separated, @compat_len
 * bytes with the last NUL). Returns the node's path, to be freed.
 */
static char *bcm2712_fdt_soc_node(void *fdt, const char *name,
                                  BCM2712Device dev, const char *compat,
                                  size_t compat_len)
{
    uint32_t addr = bcm2712_fdt_bus_addr(bcm2712_memmap[dev].base);
    char *path = g_strdup_printf(BCM2712_FDT_SOC_PATH "/%s@%x", name, addr);

    qemu_fdt_add_subnode(fdt, path);
    qemu_fdt_setprop(fdt, path, "compatible", compat, compat_len);
    qemu_fdt_setprop_cells(fdt, path, "reg", addr,
                           (uint32_t)bcm2712_memmap[dev].size);
    return path;
}

static uint32_t bcm2712_fdt_clock(void *fdt, const char *name,
                                  const char *output, uint32_t hz)
{
    g_autofree char *path = g_strdup_printf("/clocks/%s", name);
    uint32_t phandle = qemu_fdt_alloc_phandle(fdt);

    qemu_fdt_add_subnode(fdt, path);
    qemu_fdt_setprop_string(fdt, path, "compatible", "fixed-clock");
    qemu_fdt_setprop_cell(fdt, path, "#clock-cells", 0);
    qemu_fdt_setprop_cell(fdt, path, "clock-frequency", hz);
    qemu_fdt_setprop_string(fdt, path, "clock-output-names", output);
    qemu_fdt_setprop_cell(fdt, path, "phandle", phandle);
    return phandle;
}

static void bcm2712_fdt_cpus(BCM2712State *s, void *fdt, uint32_t *phandles)
{
    qemu_fdt_add_subnode(fdt, "/cpus");
    qemu_fdt_setprop_cell(fdt, "/cpus", "#address-cells", 1);
    qemu_fdt_setprop_cell(fdt, "/cpus", "#size-cells", 0);

    /* In reverse, since libfdt adds each subnode first */
    for (int i = s->num_cpus - 1; i >= 0; i--) {
        uint32_t mpidr = i << ARM_AFF1_SHIFT;
        g_autofree char *path = g_strdup_printf("/cpus/cpu@%x", mpidr);

        phandles[i] = qemu_fdt_alloc_phandle(fdt);
        qemu_fdt_add_subnode(fdt, path);
        qemu_fdt_setprop_string(fdt, path, "device_type", "cpu");
        qemu_fdt_setprop_string(fdt, path, "compatible", "arm,cortex-a76");
        qemu_fdt_setprop_cell(fdt, path, "reg", mpidr);
        qemu_fdt_setprop_string(fdt, path, "enable-method", "psci");
        qemu_fdt_setprop_cell(fdt, path, "phandle", phandles[i]);
    }
}

/* The generic timer and PMU interrupts, and the PMU's affinity */
static void bcm2712_fdt_cpu_irqs(BCM2712State *s, void *fdt,
                                 const uint32_t *cpu_phandles)
{
    static const int timer_ppis[] = {
        ARCH_TIMER_S_EL1_IRQ, ARCH_TIMER_NS_EL1_IRQ, ARCH_TIMER_VIRT_IRQ,
        ARCH_TIMER_NS_EL2_IRQ,
    };
    uint32_t ppi_flags = (MAKE_64BIT_MASK(0, s->num_cpus) <<
                          GIC_FDT_IRQ_PPI_CPU_START) |
                         GIC_FDT_IRQ_FLAGS_LEVEL_LO;
    uint32_t timer[ARRAY_SIZE(timer_ppis) * 3];
    uint32_t pmu[BCM2712_NUM_CPUS * 3], affinity[BCM2712_NUM_CPUS];

    for (int i = 0; i < ARRAY_SIZE(timer_ppis); i++) {
        timer[3 * i] = cpu_to_be32(GIC_FDT_IRQ_TYPE_PPI);
        timer[3 * i + 1] = cpu_to_be32(timer_ppis[i] - GIC_NR_SGIS);
        timer[3 * i + 2] = cpu_to_be32(ppi_flags);
    }
    qemu_fdt_add_subnode(fdt, "/timer");
    qemu_fdt_setprop_string(fdt, "/timer", "compatible", "arm,armv8-timer");
    qemu_fdt_setprop(fdt, "/timer", "interrupts", timer, sizeof(timer));

    for (int i = 0; i < s->num_cpus; i++) {
        pmu[3 * i] = cpu_to_be32(GIC_FDT_IRQ_TYPE_SPI);
        pmu[3 * i + 1] = cpu_to_be32(BCM2712_SPI_PMU0 + i);
        pmu[3 * i + 2] = cpu_to_be32(GIC_FDT_IRQ_FLAGS_LEVEL_HI);
        affinity[i] = cpu_to_be32(cpu_phandles[i]);
    }
    qemu_fdt_add_subnode(fdt, "/arm-pmu");
    qemu_fdt_setprop_string(fdt, "/arm-pmu", "compatible",
                            "arm,cortex-a76-pmu");
    qemu_fdt_setprop(fdt, "/arm-pmu", "interrupts", pmu,
                     s->num_cpus * 3 * sizeof(uint32_t));
    qemu_fdt_setprop(fdt, "/arm-pmu", "interrupt-affinity", affinity,
                     s->num_cpus * sizeof(uint32_t));
}

static uint32_t bcm2712_fdt_gic(BCM2712State *s, void *fdt)
{
    static const char compat[] = "arm,gic-400";
    hwaddr base = bcm2712_memmap[BCM2712_GIC].base;
    uint32_t phandle = qemu_fdt_alloc_phandle(fdt);
    g_autofree char *path = g_strdup_printf(
        BCM2712_FDT_SOC_PATH "/interrupt-controller@%x",
        bcm2712_fdt_bus_addr(base + GIC400_DIST_OFS));

    qemu_fdt_add_subnode(fdt, path);
    qemu_fdt_setprop(fdt, path, "compatible", compat, sizeof(compat));
    qemu_fdt_setprop_cells(fdt, path, "reg",
        bcm2712_fdt_bus_addr(base + GIC400_DIST_OFS), 0x1000,
        bcm2712_fdt_bus_addr(base + GIC400_CPU_OFS), 0x2000,
        bcm2712_fdt_bus_addr(base + GIC400_VIFACE_THIS_OFS), 0x2000,
        bcm2712_fdt_bus_addr(base + GIC400_VCPU_OFS), 0x2000);
    qemu_fdt_setprop(fdt, path, "interrupt-controller", NULL, 0);
    qemu_fdt_setprop_cell(fdt, path, "#interrupt-cells", 3);
    qemu_fdt_setprop_cell(fdt, path, "#address-cells", 0);
    qemu_fdt_setprop_cells(fdt, path, "interrupts", GIC_FDT_IRQ_TYPE_PPI,
                           ARCH_GIC_MAINT_IRQ - GIC_NR_SGIS,
                           (MAKE_64BIT_MASK(0, s->num_cpus) <<
                            GIC_FDT_IRQ_PPI_CPU_START) |
                           GIC_FDT_IRQ_FLAGS_LEVEL_HI);
    qemu_fdt_setprop_cell(fdt, path, "phandle", phandle);
    return phandle;
}

/* In reverse, since libfdt adds each subnode first */
static void bcm2712_fdt_l2_intcs(void *fdt, uint32_t *phandles)
{
    for (int i = BCM2712_NUM_L2_INTCS - 1; i >= 0; i--) {
        g_autofree char *path = bcm2712_fdt_soc_node(fdt,
            "interrupt-controller", bcm2712_l2_intcs[i].dev,
            bcm2712_l2_intcs[i].compat, bcm2712_l2_intcs[i].compat_len);

        phandles[i] = qemu_fdt_alloc_phandle(fdt);
        qemu_fdt_setprop_cells(fdt, path, "interrupts", GIC_FDT_IRQ_TYPE_SPI,
                               bcm2712_l2_intcs[i].spi,
                               GIC_FDT_IRQ_FLAGS_LEVEL_HI);
        qemu_fdt_setprop(fdt, path, "interrupt-controller", NULL, 0);
        qemu_fdt_setprop_cell(fdt, path, "#interrupt-cells", 1);
        if (bcm2712_l2_intcs[i].disabled) {
            qemu_fdt_setprop_string(fdt, path, "status", "disabled");
        }
        qemu_fdt_setprop_cell(fdt, path, "phandle", phandles[i]);
    }
}

/*
 * A GPIO block, as bcm2712.dtsi has it; the binding takes no
 * brcm,gpio-direct, which the firmware's tree adds. The phandle is for
 * the board's nodes that use its lines.
 */
static char *bcm2712_fdt_gpio(void *fdt, BCM2712Device dev,
                              const uint32_t *widths, size_t banks)
{
    static const char compat[] = "brcm,bcm7445-gpio\0brcm,brcmstb-gpio";
    g_autofree uint32_t *cells = g_new(uint32_t, banks);
    char *path = bcm2712_fdt_soc_node(fdt, "gpio", dev, compat,
                                      sizeof(compat));

    for (size_t i = 0; i < banks; i++) {
        cells[i] = cpu_to_be32(widths[i]);
    }
    qemu_fdt_setprop(fdt, path, "gpio-controller", NULL, 0);
    qemu_fdt_setprop_cell(fdt, path, "#gpio-cells", 2);
    qemu_fdt_setprop(fdt, path, "brcm,gpio-bank-widths", cells,
                     banks * sizeof(uint32_t));
    qemu_fdt_setprop_cell(fdt, path, "phandle", qemu_fdt_alloc_phandle(fdt));
    return path;
}

/* In reverse, since libfdt adds each subnode first */
static void bcm2712_fdt_gpios(void *fdt, const uint32_t *l2_phandles)
{
    g_autofree char *gio_aon = NULL, *gio = NULL;

    gio_aon = bcm2712_fdt_gpio(fdt, BCM2712_GIO_AON, bcm2712_gio_aon_widths,
                               ARRAY_SIZE(bcm2712_gio_aon_widths));

    gio = bcm2712_fdt_gpio(fdt, BCM2712_GIO, bcm2712_gio_widths,
                           ARRAY_SIZE(bcm2712_gio_widths));
    qemu_fdt_setprop_cell(fdt, gio, "interrupt-parent",
                          l2_phandles[BCM2712_L2_MAIN_IRQ]);
    qemu_fdt_setprop_cell(fdt, gio, "interrupts", BCM2712_MAIN_IRQ_GIO);
    qemu_fdt_setprop(fdt, gio, "interrupt-controller", NULL, 0);
    qemu_fdt_setprop_cell(fdt, gio, "#interrupt-cells", 2);
}

/*
 * The pin controllers, as bcm2712.dtsi has them, in reverse since libfdt
 * adds each subnode first. The phandles are for the board's pin states.
 */
static void bcm2712_fdt_pinctrls(void *fdt)
{
    static const char aon_compat[] = "brcm,bcm2712c0-aon-pinctrl";
    static const char compat[] = "brcm,bcm2712c0-pinctrl";
    g_autofree char *aon = NULL, *pinctrl = NULL;

    aon = bcm2712_fdt_soc_node(fdt, "pinctrl", BCM2712_PINCTRL_AON,
                               aon_compat, sizeof(aon_compat));
    qemu_fdt_setprop_cell(fdt, aon, "phandle", qemu_fdt_alloc_phandle(fdt));

    pinctrl = bcm2712_fdt_soc_node(fdt, "pinctrl", BCM2712_PINCTRL, compat,
                                   sizeof(compat));
    qemu_fdt_setprop_cell(fdt, pinctrl, "phandle",
                          qemu_fdt_alloc_phandle(fdt));
}

/*
 * The DDC I2C controllers, as bcm2712.dtsi has them, in reverse since
 * libfdt adds each subnode first
 */
static void bcm2712_fdt_ddcs(void *fdt, const uint32_t *l2_phandles)
{
    static const char compat[] = "brcm,brcmstb-i2c";

    for (int i = BCM2712_NUM_HDMI - 1; i >= 0; i--) {
        g_autofree char *path = bcm2712_fdt_soc_node(fdt, "i2c",
            bcm2712_ddcs[i].dev, compat, sizeof(compat));

        qemu_fdt_setprop_cell(fdt, path, "interrupt-parent",
                              l2_phandles[BCM2712_L2_BSC_IRQ]);
        qemu_fdt_setprop_cell(fdt, path, "interrupts", bcm2712_ddcs[i].irq);
        qemu_fdt_setprop_cell(fdt, path, "clock-frequency",
                              BCM2712_FDT_DDC_HZ);
        qemu_fdt_setprop_cell(fdt, path, "#address-cells", 1);
        qemu_fdt_setprop_cell(fdt, path, "#size-cells", 0);
    }
}

/*
 * The SD/eMMC host controllers, SDIO1 as bcm2712.dtsi has it and SDIO2 as
 * bcm2712-ds.dtsi has it, in reverse since libfdt adds each subnode first.
 * SDIO2 serves the board's Wi-Fi radio, so it is the board's to enable.
 */
static void bcm2712_fdt_sdios(void *fdt, uint32_t clk_emmc2)
{
    static const char compat[] = "brcm,bcm2712-sdhci\0brcm,sdhci-brcmstb";
    static const char reg_names[] = "host\0cfg";

    for (int i = BCM2712_NUM_SDIO - 1; i >= 0; i--) {
        uint32_t addr = bcm2712_fdt_bus_addr(
            bcm2712_memmap[bcm2712_sdios[i].dev].base);
        g_autofree char *path = bcm2712_fdt_soc_node(fdt, "mmc",
            bcm2712_sdios[i].dev, compat, sizeof(compat));

        qemu_fdt_setprop_cells(fdt, path, "reg",
                               addr, BCM2712_FDT_SDHCI_HOST_SIZE,
                               addr + BCM2712_SDHCI_CFG_OFFSET,
                               BCM2712_SDHCI_CFG_SIZE);
        qemu_fdt_setprop(fdt, path, "reg-names", reg_names,
                         sizeof(reg_names));
        qemu_fdt_setprop_cells(fdt, path, "interrupts", GIC_FDT_IRQ_TYPE_SPI,
                               bcm2712_sdios[i].spi,
                               GIC_FDT_IRQ_FLAGS_LEVEL_HI);
        qemu_fdt_setprop_cell(fdt, path, "clocks", clk_emmc2);
        qemu_fdt_setprop_string(fdt, path, "clock-names", "sw_sdio");
        qemu_fdt_setprop(fdt, path, "mmc-ddr-3_3v", NULL, 0);
        if (bcm2712_sdios[i].dev == BCM2712_SDIO2) {
            /* The silicon's re-tuning modes, which the model lacks */
            qemu_fdt_setprop_cells(fdt, path, "sdhci-caps-mask", 0xc000, 0);
            qemu_fdt_setprop_cells(fdt, path, "sdhci-caps", 0, 0);
            qemu_fdt_setprop_string(fdt, path, "status", "disabled");
        }
    }
}

/*
 * The "axi" bus of the firmware's tree, which reaches the PCIe root
 * complexes' 64-bit address ranges: the first 64 GiB (RAM), the AXI
 * peripherals and the outbound apertures, at their CPU addresses.
 */
static void bcm2712_fdt_axi(void *fdt)
{
    static const BCM2712Device windows[] = {
        BCM2712_AXI, BCM2712_PCIE0_MEM, BCM2712_PCIE1_MEM, BCM2712_PCIE2_MEM,
    };
    uint32_t ranges[(ARRAY_SIZE(windows) + 1) * 6];
    uint32_t *r = ranges;

    /* RAM, then each window, mapped 1:1 */
    for (int i = -1; i < (int)ARRAY_SIZE(windows); i++) {
        uint64_t base = i < 0 ? BCM2712_RAM_BASE
                              : bcm2712_memmap[windows[i]].base;
        uint64_t size = i < 0 ? 64 * GiB
                              : windows[i] == BCM2712_AXI
                                ? 4 * GiB : bcm2712_memmap[windows[i]].size;

        *r++ = cpu_to_be32(base >> 32);
        *r++ = cpu_to_be32(base);
        *r++ = cpu_to_be32(base >> 32);
        *r++ = cpu_to_be32(base);
        *r++ = cpu_to_be32(size >> 32);
        *r++ = cpu_to_be32(size);
    }
    qemu_fdt_add_subnode(fdt, BCM2712_FDT_AXI_PATH);
    qemu_fdt_setprop_string(fdt, BCM2712_FDT_AXI_PATH, "compatible",
                            "simple-bus");
    qemu_fdt_setprop_cell(fdt, BCM2712_FDT_AXI_PATH, "#address-cells", 2);
    qemu_fdt_setprop_cell(fdt, BCM2712_FDT_AXI_PATH, "#size-cells", 2);
    qemu_fdt_setprop(fdt, BCM2712_FDT_AXI_PATH, "ranges", ranges,
                     sizeof(ranges));
    qemu_fdt_setprop(fdt, BCM2712_FDT_AXI_PATH, "dma-ranges", ranges,
                     sizeof(ranges));
}

/*
 * The PCIe root complexes as the firmware's tree has them, each with an
 * outbound window of 32-bit and one of 64-bit PCI memory, and an inbound
 * window onto RAM, and using its own MSI controller. PCIe0 is disabled,
 * as there; PCIe1, the connector, is enabled, as dtparam=pciex1 has the
 * firmware do, and Linux leaves it when it finds no link; PCIe2 is
 * disabled while RP1 is not modelled.
 */
static void bcm2712_fdt_pcie(void *fdt, uint32_t gic)
{
    static const char irq_names[] = "pcie\0msi";
    static const char reset_names[] = "rescal\0bridge";
    /* PCI address, CPU address and size of the 32-bit and 64-bit windows */
    static const uint64_t outbound[BCM2712_NUM_PCIE][2][3] = {
        { { 0x0, 0x1700000000, 0xfffffffc },
          { 0x400000000, 0x1400000000, 0x300000000 } },
        { { 0x80000000, 0x1b80000000, 0x80000000 },
          { 0x400000000, 0x1800000000, 0x380000000 } },
        { { 0x0, 0x1f00000000, 0xfffffffc },
          { 0x400000000, 0x1c00000000, 0x300000000 } },
    };
    /*
     * The inbound windows beyond RAM at PCI 0x10_0000_0000: PCIe1's and
     * PCIe2's to their MIP's registers, at the MSI address the MIP gives,
     * and PCIe2's to RP1's shared SRAM
     */
    static const uint32_t dma_ranges[BCM2712_NUM_PCIE][21] = {
        { 0x43000000, 0x10, 0x0, 0x0, 0x0, 0x10, 0x0 },
        { 0x03000000, 0x10, 0x0, 0x0, 0x0, 0x10, 0x0,
          0x03000000, 0xff, 0xfffff000, 0x10, 0x131000, 0x0, 0x1000 },
        { 0x02000000, 0x0, 0x0, 0x1f, 0x0, 0x0, 0x400000,
          0x43000000, 0x10, 0x0, 0x0, 0x0, 0x10, 0x0,
          0x03000000, 0xff, 0xfffff000, 0x10, 0x130000, 0x0, 0x1000 },
    };
    static const int dma_ranges_len[BCM2712_NUM_PCIE] = { 7, 14, 21 };
    /* The MSI controller of each: its own (below), MIP1 and MIP0 */
    uint32_t msi_parent[BCM2712_NUM_PCIE] = { 0 };
    uint32_t rescal, reset;
    g_autofree char *rescal_path = NULL, *reset_path = NULL;

    rescal = qemu_fdt_alloc_phandle(fdt);
    rescal_path = bcm2712_fdt_soc_node(fdt, "reset-controller",
        BCM2712_PCIE_RESCAL, "brcm,bcm7216-pcie-sata-rescal",
        sizeof("brcm,bcm7216-pcie-sata-rescal"));
    qemu_fdt_setprop_cell(fdt, rescal_path, "#reset-cells", 0);
    qemu_fdt_setprop_cell(fdt, rescal_path, "phandle", rescal);
    reset = qemu_fdt_alloc_phandle(fdt);
    reset_path = bcm2712_fdt_soc_node(fdt, "reset-controller", BCM2712_RESET,
                                      "brcm,brcmstb-reset",
                                      sizeof("brcm,brcmstb-reset"));
    qemu_fdt_setprop_cell(fdt, reset_path, "#reset-cells", 1);
    qemu_fdt_setprop_cell(fdt, reset_path, "phandle", reset);

    /* The MIPs, after the root complexes on the bus, as in the firmware's */
    for (int i = BCM2712_NUM_MIP - 1; i >= 0; i--) {
        hwaddr base = bcm2712_memmap[BCM2712_MIP0 + i].base;
        g_autofree char *path = g_strdup_printf(BCM2712_FDT_AXI_PATH
            "/msi-controller@%" HWADDR_PRIx, base);
        uint32_t phandle = qemu_fdt_alloc_phandle(fdt);

        qemu_fdt_add_subnode(fdt, path);
        qemu_fdt_setprop_string(fdt, path, "compatible", "brcm,bcm2712-mip");
        qemu_fdt_setprop_sized_cells(fdt, path, "reg",
            2, base, 2, bcm2712_memmap[BCM2712_MIP0 + i].size,
            2, BCM2712_MIP_MSI_ADDR, 2, 4 * KiB);
        qemu_fdt_setprop(fdt, path, "msi-controller", NULL, 0);
        qemu_fdt_setprop_cells(fdt, path, "msi-ranges",
            gic, GIC_FDT_IRQ_TYPE_SPI,
            i ? BCM2712_SPI_MIP1_BASE : BCM2712_SPI_MIP0_BASE,
            GIC_FDT_IRQ_FLAGS_EDGE_LO_HI,
            i ? 16 - BCM2712_MIP1_FIRST_VECTOR : BCM2712_MIP_NUM_IRQS);
        qemu_fdt_setprop_cell(fdt, path, "brcm,msi-offset",
                              i ? BCM2712_MIP1_FIRST_VECTOR : 0);
        qemu_fdt_setprop_cell(fdt, path, "phandle", phandle);
        msi_parent[i ? 1 : 2] = phandle;
    }

    for (int i = BCM2712_NUM_PCIE - 1; i >= 0; i--) {
        hwaddr base = bcm2712_memmap[bcm2712_pcies[i].dev].base;
        g_autofree char *path = g_strdup_printf(BCM2712_FDT_AXI_PATH
                                                "/pcie@%" HWADDR_PRIx, base);
        uint32_t phandle = qemu_fdt_alloc_phandle(fdt);
        uint32_t map[BRCMSTB_PCIE_NUM_INTX * 8];
        uint32_t dma[ARRAY_SIZE(dma_ranges[0])];

        for (int n = 0; n < BRCMSTB_PCIE_NUM_INTX; n++) {
            uint32_t entry[8] = { 0, 0, 0, n + 1, gic, GIC_FDT_IRQ_TYPE_SPI,
                                  bcm2712_pcies[i].spi_inta + n,
                                  GIC_FDT_IRQ_FLAGS_LEVEL_HI };

            for (int k = 0; k < 8; k++) {
                map[n * 8 + k] = cpu_to_be32(entry[k]);
            }
        }
        qemu_fdt_add_subnode(fdt, path);
        qemu_fdt_setprop_string(fdt, path, "compatible", "brcm,bcm2712-pcie");
        qemu_fdt_setprop_sized_cells(fdt, path, "reg", 2, base, 2,
            bcm2712_memmap[bcm2712_pcies[i].dev].size);
        qemu_fdt_setprop_string(fdt, path, "device_type", "pci");
        qemu_fdt_setprop_cell(fdt, path, "linux,pci-domain", i);
        qemu_fdt_setprop_cell(fdt, path, "max-link-speed", 2);
        qemu_fdt_setprop_cell(fdt, path, "num-lanes",
                              bcm2712_pcies[i].num_lanes);
        qemu_fdt_setprop_cell(fdt, path, "#address-cells", 3);
        qemu_fdt_setprop_cell(fdt, path, "#interrupt-cells", 1);
        qemu_fdt_setprop_cell(fdt, path, "#size-cells", 2);
        qemu_fdt_setprop_cells(fdt, path, "interrupts",
                               GIC_FDT_IRQ_TYPE_SPI, bcm2712_pcies[i].spi,
                               GIC_FDT_IRQ_FLAGS_LEVEL_HI,
                               GIC_FDT_IRQ_TYPE_SPI, bcm2712_pcies[i].spi_msi,
                               GIC_FDT_IRQ_FLAGS_LEVEL_HI);
        qemu_fdt_setprop(fdt, path, "interrupt-names", irq_names,
                         sizeof(irq_names));
        qemu_fdt_setprop_cells(fdt, path, "interrupt-map-mask", 0, 0, 0, 7);
        qemu_fdt_setprop(fdt, path, "interrupt-map", map, sizeof(map));
        qemu_fdt_setprop_cells(fdt, path, "resets", rescal, reset,
                               BCM2712_RESET_PCIE0_BRIDGE + i);
        qemu_fdt_setprop(fdt, path, "reset-names", reset_names,
                         sizeof(reset_names));
        qemu_fdt_setprop(fdt, path, "msi-controller", NULL, 0);
        qemu_fdt_setprop_cell(fdt, path, "msi-parent",
                              msi_parent[i] ? msi_parent[i] : phandle);
        qemu_fdt_setprop_cells(fdt, path, "ranges",
            0x02000000, outbound[i][0][0] >> 32, outbound[i][0][0],
            outbound[i][0][1] >> 32, outbound[i][0][1],
            outbound[i][0][2] >> 32, outbound[i][0][2],
            0x43000000, outbound[i][1][0] >> 32, outbound[i][1][0],
            outbound[i][1][1] >> 32, outbound[i][1][1],
            outbound[i][1][2] >> 32, outbound[i][1][2]);
        for (int k = 0; k < dma_ranges_len[i]; k++) {
            dma[k] = cpu_to_be32(dma_ranges[i][k]);
        }
        qemu_fdt_setprop(fdt, path, "dma-ranges", dma,
                         dma_ranges_len[i] * sizeof(dma[0]));
        if (bcm2712_pcies[i].dev == BCM2712_PCIE1) {
            qemu_fdt_setprop_cell(fdt, path, "brcm,fifo-qos-map", 0x3030303);
            qemu_fdt_setprop_string(fdt, path, "brcm,clkreq-mode", "safe");
        } else if (bcm2712_pcies[i].dev == BCM2712_PCIE2) {
            qemu_fdt_setprop_cells(fdt, path, "brcm,vdm-qos-map",
                                   0x8080809, 0xa0a0b0b);
            qemu_fdt_setprop(fdt, path, "aspm-no-l0s", NULL, 0);
        }
        qemu_fdt_setprop_string(fdt, path, "status",
            bcm2712_pcies[i].dev == BCM2712_PCIE1 ? "okay" : "disabled");
        qemu_fdt_setprop_cell(fdt, path, "phandle", phandle);
    }
}

/*
 * The AVS monitor and its temperature sensor, as the firmware's tree has
 * them, and the thermal zone of the Pi 5's trees, which converts the
 * sensor's code and has Linux shut down at the critical temperature. Their
 * other trips switch the fan, which is behind RP1.
 */
static void bcm2712_fdt_avs(void *fdt)
{
    static const char compat[] =
        "brcm,bcm2711-avs-monitor\0syscon\0simple-mfd";
    const char *zone = "/thermal-zones/cpu-thermal";
    const char *crit = "/thermal-zones/cpu-thermal/trips/cpu-crit";
    g_autofree char *avs = bcm2712_fdt_soc_node(fdt, "avs-monitor",
                                                BCM2712_AVS, compat,
                                                sizeof(compat));
    g_autofree char *sensor = g_strdup_printf("%s/thermal", avs);
    uint32_t phandle = qemu_fdt_alloc_phandle(fdt);

    qemu_fdt_add_subnode(fdt, sensor);
    qemu_fdt_setprop_string(fdt, sensor, "compatible", "brcm,bcm2711-thermal");
    qemu_fdt_setprop_cell(fdt, sensor, "#thermal-sensor-cells", 0);
    qemu_fdt_setprop_cell(fdt, sensor, "phandle", phandle);

    qemu_fdt_add_path(fdt, crit);
    qemu_fdt_setprop_cell(fdt, zone, "polling-delay-passive", 1000);
    qemu_fdt_setprop_cell(fdt, zone, "polling-delay", 1000);
    qemu_fdt_setprop_cells(fdt, zone, "coefficients",
                           (uint32_t)BCM2712_AVS_TEMP_SLOPE,
                           BCM2712_AVS_TEMP_OFFSET);
    qemu_fdt_setprop_cell(fdt, zone, "thermal-sensors", phandle);
    qemu_fdt_setprop_cell(fdt, crit, "temperature", BCM2712_TEMP_CRITICAL);
    qemu_fdt_setprop_cell(fdt, crit, "hysteresis", 0);
    qemu_fdt_setprop_string(fdt, crit, "type", "critical");
}

char *bcm2712_fdt_node_path(void *fdt, BCM2712Device dev)
{
    g_autofree char *unit = g_strdup_printf("@%x",
        bcm2712_fdt_bus_addr(bcm2712_memmap[dev].base));
    int soc = fdt_path_offset(fdt, BCM2712_FDT_SOC_PATH);
    int node;

    if (soc < 0) {
        return NULL;
    }
    fdt_for_each_subnode(node, fdt, soc) {
        const char *name = fdt_get_name(fdt, node, NULL);

        if (name && g_str_has_suffix(name, unit)) {
            return g_strdup_printf(BCM2712_FDT_SOC_PATH "/%s", name);
        }
    }
    return NULL;
}

void bcm2712_fdt_populate(BCM2712State *s, void *fdt)
{
    static const char uart_compat[] = "arm,pl011\0arm,primecell";
    static const char uart_clock_names[] = "uartclk\0apb_pclk";
    static const char firmware_compat[] =
        "raspberrypi,bcm2835-firmware\0simple-mfd";
    uint32_t cpu_phandles[BCM2712_NUM_CPUS];
    uint32_t l2_phandles[BCM2712_NUM_L2_INTCS];
    uint32_t gic, clk_sw_baud, clk_emmc2, clk_uart, clk_vpu, mbox;
    g_autofree char *systimer = NULL, *mailbox = NULL, *uart = NULL;
    g_autofree char *uarta = NULL, *pm = NULL, *rng = NULL;
    const char *firmware = BCM2712_FDT_SOC_PATH "/firmware";
    const char *fw_clocks = BCM2712_FDT_SOC_PATH "/firmware/clocks";
    const char *fw_reset = BCM2712_FDT_SOC_PATH "/firmware/reset";
    const char *fw_vcio = BCM2712_FDT_SOC_PATH "/firmware/vcio";
    const char *power = BCM2712_FDT_SOC_PATH "/power";
    const char *rtc = BCM2712_FDT_SOC_PATH "/rpi_rtc";
    uint32_t spi, fw;

    /* libfdt adds each subnode first: create them in reverse order */
    bcm2712_fdt_axi(fdt);
    qemu_fdt_add_subnode(fdt, BCM2712_FDT_SOC_PATH);
    qemu_fdt_setprop_string(fdt, BCM2712_FDT_SOC_PATH, "compatible",
                            "simple-bus");
    qemu_fdt_setprop_cell(fdt, BCM2712_FDT_SOC_PATH, "#address-cells", 1);
    qemu_fdt_setprop_cell(fdt, BCM2712_FDT_SOC_PATH, "#size-cells", 1);
    qemu_fdt_setprop_cells(fdt, BCM2712_FDT_SOC_PATH, "ranges", 0,
                           BCM2712_FDT_SOC_BUS_BASE >> 32,
                           (uint32_t)BCM2712_FDT_SOC_BUS_BASE,
                           BCM2712_FDT_SOC_BUS_SIZE);

    qemu_fdt_add_subnode(fdt, "/clocks");
    clk_sw_baud = bcm2712_fdt_clock(fdt, "clk-sw-baud", "sw-baud",
                                    BCM2712_UARTA_CLK_HZ);
    clk_emmc2 = bcm2712_fdt_clock(fdt, "clk-emmc2", "emmc2-clock",
                                  BCM2712_FDT_CLK_EMMC2);
    clk_uart = bcm2712_fdt_clock(fdt, "clk-uart", "uart-clock",
                                 BCM2712_FDT_CLK_UART);
    clk_vpu = bcm2712_fdt_clock(fdt, "clk-vpu", "vpu-clock",
                                BCM2712_FDT_CLK_VPU);
    bcm2712_fdt_clock(fdt, "clk-osc", "osc", BCM2712_FDT_CLK_OSC);

    bcm2712_fdt_cpus(s, fdt, cpu_phandles);
    gic = bcm2712_fdt_gic(s, fdt);
    qemu_fdt_setprop_cell(fdt, "/", "interrupt-parent", gic);
    bcm2712_fdt_cpu_irqs(s, fdt, cpu_phandles);

    bcm2712_fdt_avs(fdt);
    bcm2712_fdt_l2_intcs(fdt, l2_phandles);
    bcm2712_fdt_gpios(fdt, l2_phandles);
    bcm2712_fdt_pinctrls(fdt);
    bcm2712_fdt_ddcs(fdt, l2_phandles);

    rng = bcm2712_fdt_soc_node(fdt, "rng", BCM2712_RNG,
                               "brcm,bcm2711-rng200",
                               sizeof("brcm,bcm2711-rng200"));

    pm = bcm2712_fdt_soc_node(fdt, "watchdog", BCM2712_PM, "brcm,bcm2712-pm",
                              sizeof("brcm,bcm2712-pm"));
    qemu_fdt_setprop_string(fdt, pm, "reg-names", "pm");
    qemu_fdt_setprop_cell(fdt, pm, "#power-domain-cells", 1);
    qemu_fdt_setprop_cell(fdt, pm, "#reset-cells", 1);
    qemu_fdt_setprop(fdt, pm, "system-power-controller", NULL, 0);

    /*
     * UARTA as bcm2712.dtsi has it; the firmware's tree gives it a
     * clock-frequency instead of the clock, which the binding does not take
     */
    uarta = bcm2712_fdt_soc_node(fdt, "serial", BCM2712_UARTA,
                                 "brcm,bcm7271-uart",
                                 sizeof("brcm,bcm7271-uart"));
    qemu_fdt_setprop_string(fdt, uarta, "reg-names", "uart");
    qemu_fdt_setprop_cell(fdt, uarta, "clocks", clk_sw_baud);
    qemu_fdt_setprop_string(fdt, uarta, "clock-names", "sw_baud");
    qemu_fdt_setprop_cells(fdt, uarta, "interrupts", GIC_FDT_IRQ_TYPE_SPI,
                           BCM2712_SPI_UARTA, GIC_FDT_IRQ_FLAGS_LEVEL_HI);
    qemu_fdt_setprop_string(fdt, uarta, "interrupt-names", "uart");

    uart = bcm2712_fdt_soc_node(fdt, "serial", BCM2712_UART10, uart_compat,
                                sizeof(uart_compat));
    qemu_fdt_setprop_cells(fdt, uart, "interrupts", GIC_FDT_IRQ_TYPE_SPI,
                           BCM2712_SPI_UART10, GIC_FDT_IRQ_FLAGS_LEVEL_HI);
    qemu_fdt_setprop_cells(fdt, uart, "clocks", clk_uart, clk_vpu);
    qemu_fdt_setprop(fdt, uart, "clock-names", uart_clock_names,
                     sizeof(uart_clock_names));
    /*
     * The node covers 0x200 bytes, so Linux cannot find the ID registers
     * at the end of it and needs them here. The firmware's tree gives
     * the silicon's r1p5 (0x00341011); these are QEMU's PL011 ID bytes.
     */
    qemu_fdt_setprop_cell(fdt, uart, "arm,primecell-periphid", 0x00141011);

    mbox = qemu_fdt_alloc_phandle(fdt);
    mailbox = bcm2712_fdt_soc_node(fdt, "mailbox", BCM2712_MBOX,
                                   "brcm,bcm2835-mbox",
                                   sizeof("brcm,bcm2835-mbox"));
    qemu_fdt_setprop_cells(fdt, mailbox, "interrupts", GIC_FDT_IRQ_TYPE_SPI,
                           BCM2712_SPI_MBOX, GIC_FDT_IRQ_FLAGS_LEVEL_HI);
    qemu_fdt_setprop_cell(fdt, mailbox, "#mbox-cells", 0);
    qemu_fdt_setprop_cell(fdt, mailbox, "phandle", mbox);

    spi = BCM2712_SPI_SYSTIMER0;
    systimer = bcm2712_fdt_soc_node(fdt, "timer", BCM2712_SYSTIMER,
                                    "brcm,bcm2835-system-timer",
                                    sizeof("brcm,bcm2835-system-timer"));
    qemu_fdt_setprop_cells(fdt, systimer, "interrupts",
                           GIC_FDT_IRQ_TYPE_SPI, spi,
                           GIC_FDT_IRQ_FLAGS_LEVEL_HI,
                           GIC_FDT_IRQ_TYPE_SPI, spi + 1,
                           GIC_FDT_IRQ_FLAGS_LEVEL_HI,
                           GIC_FDT_IRQ_TYPE_SPI, spi + 2,
                           GIC_FDT_IRQ_FLAGS_LEVEL_HI,
                           GIC_FDT_IRQ_TYPE_SPI, spi + 3,
                           GIC_FDT_IRQ_FLAGS_LEVEL_HI);
    qemu_fdt_setprop_cell(fdt, systimer, "clock-frequency", 1000000);

    bcm2712_fdt_sdios(fdt, clk_emmc2);
    bcm2712_fdt_pcie(fdt, gic);

    /*
     * The firmware interface, behind the mailbox, as in the firmware's
     * tree: Linux passes it buffers by their "soc" bus address. Its clocks
     * and reset controller, and beside it the power domains it switches,
     * as mainline's tree has them; then its real-time clock and the vcio
     * device through which user space reaches the interface, which only
     * the firmware's tree has, the clock with the battery's charger off.
     */
    fw = qemu_fdt_alloc_phandle(fdt);
    qemu_fdt_add_subnode(fdt, rtc);
    qemu_fdt_setprop_string(fdt, rtc, "compatible", "raspberrypi,rpi-rtc");
    qemu_fdt_setprop_cell(fdt, rtc, "firmware", fw);
    qemu_fdt_setprop_cell(fdt, rtc, "trickle-charge-microvolt", 0);

    qemu_fdt_add_subnode(fdt, power);
    qemu_fdt_setprop_string(fdt, power, "compatible",
                            "raspberrypi,bcm2835-power");
    qemu_fdt_setprop_cell(fdt, power, "firmware", fw);
    qemu_fdt_setprop_cell(fdt, power, "#power-domain-cells", 1);

    qemu_fdt_add_subnode(fdt, firmware);
    qemu_fdt_setprop(fdt, firmware, "compatible", firmware_compat,
                     sizeof(firmware_compat));
    qemu_fdt_setprop_cell(fdt, firmware, "#address-cells", 1);
    qemu_fdt_setprop_cell(fdt, firmware, "#size-cells", 1);
    qemu_fdt_setprop(fdt, firmware, "dma-ranges", NULL, 0);
    qemu_fdt_setprop_cell(fdt, firmware, "mboxes", mbox);
    qemu_fdt_setprop_cell(fdt, firmware, "phandle", fw);
    qemu_fdt_add_subnode(fdt, fw_reset);
    qemu_fdt_setprop_string(fdt, fw_reset, "compatible",
                            "raspberrypi,firmware-reset");
    qemu_fdt_setprop_cell(fdt, fw_reset, "#reset-cells", 1);
    qemu_fdt_add_subnode(fdt, fw_clocks);
    qemu_fdt_setprop_string(fdt, fw_clocks, "compatible",
                            "raspberrypi,firmware-clocks");
    qemu_fdt_setprop_cell(fdt, fw_clocks, "#clock-cells", 1);
    qemu_fdt_add_subnode(fdt, fw_vcio);
    qemu_fdt_setprop_string(fdt, fw_vcio, "compatible", "raspberrypi,vcio");

    /*
     * Keep the default CMA pool, where Linux allocates the buffers it
     * hands the firmware, in the part of RAM the VideoCore reaches
     */
    qemu_fdt_add_subnode(fdt, "/reserved-memory");
    qemu_fdt_setprop_cell(fdt, "/reserved-memory", "#address-cells", 2);
    qemu_fdt_setprop_cell(fdt, "/reserved-memory", "#size-cells", 2);
    qemu_fdt_setprop(fdt, "/reserved-memory", "ranges", NULL, 0);
    qemu_fdt_add_subnode(fdt, "/reserved-memory/linux,cma");
    qemu_fdt_setprop_string(fdt, "/reserved-memory/linux,cma", "compatible",
                            "shared-dma-pool");
    qemu_fdt_setprop_sized_cells(fdt, "/reserved-memory/linux,cma", "size",
                                 2, BCM2712_FDT_CMA_SIZE);
    qemu_fdt_setprop(fdt, "/reserved-memory/linux,cma", "reusable", NULL, 0);
    qemu_fdt_setprop(fdt, "/reserved-memory/linux,cma", "linux,cma-default",
                     NULL, 0);
    qemu_fdt_setprop_sized_cells(fdt, "/reserved-memory/linux,cma",
                                 "alloc-ranges", 2, BCM2712_RAM_BASE,
                                 2, BCM2712_VC_RAM_WINDOW);

    qemu_fdt_add_subnode(fdt, "/aliases");
    qemu_fdt_setprop_string(fdt, "/aliases", "serial10", uart);
    qemu_fdt_setprop_string(fdt, "/aliases", "console", uart);
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
