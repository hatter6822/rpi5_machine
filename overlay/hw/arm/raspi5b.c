/*
 * Raspberry Pi 5 Model B
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Boot model: on real hardware the VideoCore firmware loads TF-A BL31 at
 * physical address 0 and enters the OS at EL2, with BL31 providing PSCI
 * over SMC. By default this machine reproduces that contract with QEMU's
 * built-in PSCI emulation and hides EL3 from the guest; "secure=on"
 * exposes EL3 (and the GIC Security Extensions) so the guest may bring
 * its own secure firmware instead.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/host-utils.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/arm/bcm2712.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/registerfields.h"
#include "system/address-spaces.h"
#include "system/device_tree.h"
#include <libfdt.h>

#define TYPE_RASPI5B_MACHINE MACHINE_TYPE_NAME("raspi5b")
OBJECT_DECLARE_SIMPLE_TYPE(Raspi5bMachineState, RASPI5B_MACHINE)

struct Raspi5bMachineState {
    /*< private >*/
    MachineState parent_obj;

    /*< public >*/
    BCM2712State soc;
    struct arm_boot_info binfo;
    uint32_t board_rev;
    bool secure;
};

/*
 * "New-style" board revision code, see
 * https://www.raspberrypi.com/documentation/computers/raspberry-pi.html
 */
FIELD(REV_CODE, REVISION,       0, 4)
FIELD(REV_CODE, TYPE,           4, 8)
FIELD(REV_CODE, PROCESSOR,     12, 4)
FIELD(REV_CODE, MANUFACTURER,  16, 4)
FIELD(REV_CODE, MEMORY_SIZE,   20, 3)
FIELD(REV_CODE, STYLE,         23, 1)

#define REV_TYPE_5B             0x17
#define REV_PROCESSOR_BCM2712   4
#define REV_MANUFACTURER_SONY   0

static uint32_t raspi5b_board_rev(uint64_t ram_size)
{
    uint32_t rev = 0;

    rev = FIELD_DP32(rev, REV_CODE, REVISION, 0);        /* 1.0 */
    rev = FIELD_DP32(rev, REV_CODE, TYPE, REV_TYPE_5B);
    rev = FIELD_DP32(rev, REV_CODE, PROCESSOR, REV_PROCESSOR_BCM2712);
    rev = FIELD_DP32(rev, REV_CODE, MANUFACTURER, REV_MANUFACTURER_SONY);
    /* Encoded as log2(size / 256 MiB) */
    rev = FIELD_DP32(rev, REV_CODE, MEMORY_SIZE, ctz64(ram_size / (256 * MiB)));
    rev = FIELD_DP32(rev, REV_CODE, STYLE, 1);
    return rev;
}

/*
 * Device tree nodes for hardware that is not modelled yet. They are marked
 * disabled rather than deleted so that phandle references stay valid. This
 * list should only ever shrink; see docs/PLAN.md.
 */
static const char *const raspi5b_unmodelled_compatibles[] = {
    "brcm,bcm2835-system-timer",
    "brcm,bcm2712-pcie",
    "brcm,bcm2712-mip",
    "brcm,bcm2712-sdhci",
    "brcm,bcm2835-mbox",
    "raspberrypi,bcm2835-firmware",
    "brcm,bcm2712-pm",
    "brcm,bcm2711-rng200",
    "brcm,bcm7271-uart",
    "brcm,brcmstb-i2c",
    "brcm,2712-v3d",
    "brcm,bcm2712-vc6",
    "brcm,bcm2712-hvs",
    "brcm,bcm2712-hdmi0",
    "brcm,bcm2712-hdmi1",
    "brcm,bcm2712-pixelvalve0",
    "brcm,bcm2712-pixelvalve1",
    "brcm,bcm2712-mop",
    "brcm,bcm2712-moplet",
    "brcm,bcm2712-pispbe",
    "brcm,bcm2712c0-pinctrl",
    "brcm,bcm2712c0-aon-pinctrl",
    "brcm,brcmstb-gpio",
    "brcm,l2-intc",             /* also matches brcm,bcm2711-l2-intc nodes */
    "brcm,bcm7271-l2-intc",
    "brcm,brcmstb-reset",
    "brcm,bcm7216-pcie-sata-rescal",
    /* Nodes only present in the Raspberry Pi downstream device tree */
    "brcm,bcm2712-iommu",
    "brcm,bcm2712-iommuc",
    "brcm,bcm2711-avs-monitor",
    "brcm,bcm2712-dma",
    "brcm,bcm2712-hevc-dec",
    "raspberrypi,pispbe",
    "brcm,syscon-piarbctl",
    "brcm,brcm2711-dvp",
    "brcm,bcm2835-spi",
    "raspberrypi,gpiomem",
    /* Clients of the VideoCore firmware (mailbox) or of RP1's */
    "brcm,bcm2708-fb",
    "raspberrypi,rpi-otp",
    "raspberrypi,bcm2835-power",
    "raspberrypi,rpi-rtc",
    "raspberrypi,rp1-firmware",
};

/*
 * With -smp below 4, mark the CPU nodes of absent cores "fail": unlike
 * "disabled", which means "can be brought online", Linux skips failed CPU
 * nodes entirely instead of trying to start them with PSCI CPU_ON.
 */
static void raspi5b_fdt_fail_absent_cpus(void *fdt, unsigned int num_cpus)
{
    g_autoptr(GPtrArray) absent = g_ptr_array_new_with_free_func(g_free);
    int cpus = fdt_path_offset(fdt, "/cpus");
    int node;

    if (cpus < 0) {
        return;
    }
    /* Collect paths first: setting a property moves node offsets */
    fdt_for_each_subnode(node, fdt, cpus) {
        const char *type = fdt_getprop(fdt, node, "device_type", NULL);
        int len;
        const fdt32_t *reg = fdt_getprop(fdt, node, "reg", &len);
        char path[128];

        /* The low cell holds Aff0..Aff2 whatever /cpus #address-cells is */
        if (!type || strcmp(type, "cpu") || !reg || len < sizeof(*reg) ||
            ((fdt32_to_cpu(reg[len / sizeof(*reg) - 1]) >> ARM_AFF1_SHIFT)
             & 0xff) < num_cpus ||
            fdt_get_path(fdt, node, path, sizeof(path))) {
            continue;
        }
        g_ptr_array_add(absent, g_strdup(path));
    }
    for (unsigned int i = 0; i < absent->len; i++) {
        qemu_fdt_setprop_string(fdt, g_ptr_array_index(absent, i),
                                "status", "fail");
    }
}

static void raspi5b_modify_dtb(const struct arm_boot_info *info, void *fdt)
{
    const Raspi5bMachineState *s =
        container_of(info, Raspi5bMachineState, binfo);

    /* The VideoCore firmware publishes the board revision here */
    qemu_fdt_add_path(fdt, "/system");
    qemu_fdt_setprop_cell(fdt, "/system", "linux,revision", s->board_rev);

    raspi5b_fdt_fail_absent_cpus(fdt, s->parent_obj.smp.cpus);

    /* No match yields an empty list; errors mean a corrupt blob */
    for (int i = 0; i < ARRAY_SIZE(raspi5b_unmodelled_compatibles); i++) {
        const char *compat = raspi5b_unmodelled_compatibles[i];
        g_auto(GStrv) paths = qemu_fdt_node_path(fdt, NULL, compat,
                                                 &error_fatal);

        for (char **path = paths; *path; path++) {
            qemu_fdt_setprop_string(fdt, *path, "status", "disabled");
        }
    }
}

static void raspi5b_machine_init(MachineState *machine)
{
    Raspi5bMachineState *s = RASPI5B_MACHINE(machine);
    DeviceState *soc;

    if (!is_power_of_2(machine->ram_size) ||
        machine->ram_size < BCM2712_RAM_SIZE_MIN ||
        machine->ram_size > BCM2712_RAM_SIZE_MAX) {
        error_report("Raspberry Pi 5 boards have 1, 2, 4, 8 or 16 GiB of RAM");
        exit(EXIT_FAILURE);
    }
    if (machine->firmware) {
        /* TODO(WS3.3): load an armstub/BL31 image at 0x0 like the VPU does */
        error_report("-bios is not supported yet; use -kernel");
        exit(EXIT_FAILURE);
    }
    s->board_rev = raspi5b_board_rev(machine->ram_size);

    memory_region_add_subregion(get_system_memory(), BCM2712_RAM_BASE,
                                machine->ram);

    object_initialize_child(OBJECT(machine), "soc", &s->soc, TYPE_BCM2712);
    soc = DEVICE(&s->soc);
    qdev_prop_set_uint32(soc, "num-cpus", machine->smp.cpus);
    qdev_prop_set_bit(soc, "has-el3", s->secure);
    qdev_realize(soc, NULL, &error_fatal);

    s->binfo = (struct arm_boot_info) {
        .ram_size = machine->ram_size,
        .loader_start = BCM2712_RAM_BASE,
        /* Disabled by arm_load_kernel() if the guest itself starts in EL3 */
        .psci_conduit = QEMU_PSCI_CONDUIT_SMC,
        .modify_dtb = raspi5b_modify_dtb,
    };
    arm_load_kernel(&s->soc.cpu[0], machine, &s->binfo);
}

static bool raspi5b_get_secure(Object *obj, Error **errp)
{
    return RASPI5B_MACHINE(obj)->secure;
}

static void raspi5b_set_secure(Object *obj, bool value, Error **errp)
{
    RASPI5B_MACHINE(obj)->secure = value;
}

static void raspi5b_machine_class_init(ObjectClass *oc, const void *data)
{
    static const char *const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-a76"),
        NULL
    };
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Raspberry Pi 5 Model B (BCM2712)";
    mc->init = raspi5b_machine_init;
    mc->default_cpu_type = valid_cpu_types[0];
    mc->valid_cpu_types = valid_cpu_types;
    mc->min_cpus = 1;
    mc->max_cpus = BCM2712_NUM_CPUS;
    mc->default_cpus = BCM2712_NUM_CPUS;
    mc->default_ram_size = 2 * GiB;
    mc->default_ram_id = "ram";
    mc->no_parallel = 1;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;

    object_class_property_add_bool(oc, "secure", raspi5b_get_secure,
                                   raspi5b_set_secure);
    object_class_property_set_description(oc, "secure",
        "Expose EL3 and the GIC Security Extensions to the guest. "
        "When off (the default), QEMU provides PSCI in place of the "
        "firmware's TF-A BL31");
}

static const TypeInfo raspi5b_machine_types[] = {
    {
        .name           = TYPE_RASPI5B_MACHINE,
        .parent         = TYPE_MACHINE,
        .instance_size  = sizeof(Raspi5bMachineState),
        .class_init     = raspi5b_machine_class_init,
        .interfaces     = aarch64_machine_interfaces,
    },
};

DEFINE_TYPES(raspi5b_machine_types)
