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
 * its own secure firmware instead: an image that owns EL3 from reset
 * (-kernel), or the BL31 the firmware would load (-bios), which the
 * machine hands the kernel and device tree the way the firmware does.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/datadir.h"
#include "qemu/error-report.h"
#include "qemu/host-utils.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/arm/bcm2712.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/registerfields.h"
#include "system/address-spaces.h"
#include "system/device_tree.h"
#include "system/reset.h"
#include "elf.h"
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
    uint64_t serial;
    uint64_t dtb_addr;
    bool secure;
    bool builtin_dtb;
};

/* An obviously made-up serial number, overridden with "serial=" */
#define RASPI5B_DEFAULT_SERIAL  0x0123456789abcdefULL

/*
 * With -bios the machine loads what the firmware runs at EL3 (config.txt's
 * "armstub=", TF-A BL31 on a stock Pi 5) at address 0, and lays out the
 * rest as the firmware does: the kernel at the firmware's default
 * kernel_address for 64-bit kernels, and above it the initrd and the
 * device tree, unless "dtb-address" (device_tree_address=) places the
 * tree. Everything stays in the first GiB, below the VideoCore's memory.
 */
#define RASPI5B_KERNEL_ADDR     0x200000

/* The lowest address for the initrd and the device tree, as for -kernel */
#define RASPI5B_INITRD_ADDR     (128 * MiB)

/*
 * The header the firmware looks for in an armstub (TF-A's
 * plat/rpi/common/aarch64/armstub8_header.S): when the magic is there, it
 * clears it and fills in the device tree and kernel addresses.
 */
#define RASPI5B_ARMSTUB_MAGIC           0x5afe570b
#define RASPI5B_ARMSTUB_MAGIC_OFFSET    0xf0
#define RASPI5B_ARMSTUB_DTB_OFFSET      0xf8
#define RASPI5B_ARMSTUB_KERNEL_OFFSET   0xfc

/* The memory the firmware's device tree reserves for BL31 (atf@0) */
#define RASPI5B_ARMSTUB_RESERVED        0x80000

/* The arm64 Linux Image header: text_offset, image_size and magic */
#define RASPI5B_IMAGE_TEXT_OFFSET       8
#define RASPI5B_IMAGE_SIZE              16
#define RASPI5B_IMAGE_MAGIC             56

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
    "brcm,bcm2712-pcie",
    "brcm,bcm2712-mip",
    "brcm,bcm2712-sdhci",
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

/*
 * Like the firmware, leave the VideoCore's memory at the top of the first
 * GiB out of the memory node that arm_load_dtb() wrote: it replaces every
 * /memory node of the tree with one "/memory@0", which libfdt also finds
 * by the unit-address-less "/memory".
 */
static void raspi5b_fdt_memory(void *fdt, uint64_t ram_size)
{
    uint32_t acells = qemu_fdt_getprop_cell(fdt, "/", "#address-cells",
                                            NULL, &error_fatal);
    uint32_t scells = qemu_fdt_getprop_cell(fdt, "/", "#size-cells",
                                            NULL, &error_fatal);
    int rc;

    if (ram_size > BCM2712_VC_RAM_WINDOW) {
        rc = qemu_fdt_setprop_sized_cells(fdt, "/memory", "reg",
                acells, BCM2712_RAM_BASE, scells, BCM2712_VC_RAM_BASE,
                acells, BCM2712_RAM_BASE + BCM2712_VC_RAM_WINDOW,
                scells, ram_size - BCM2712_VC_RAM_WINDOW);
    } else {
        rc = qemu_fdt_setprop_sized_cells(fdt, "/memory", "reg",
                acells, BCM2712_RAM_BASE, scells, BCM2712_VC_RAM_BASE);
    }
    if (rc < 0) {
        error_report("raspi5b: cannot set the device tree memory node");
        exit(EXIT_FAILURE);
    }
}

/*
 * Room left in the built-in tree for what arm_load_dtb() and
 * raspi5b_modify_dtb() add: memory, PSCI, /chosen with -append, /system.
 * load_device_tree() leaves the same for a -dtb blob.
 */
#define RASPI5B_FDT_SLACK       10000

/*
 * The device tree given without -dtb: the board's own nodes, the SoC's,
 * and the console on UART10 as on the firmware's tree. arm_load_dtb()
 * adds the memory, PSCI and /chosen properties, then the same fix-ups as
 * for a -dtb blob apply.
 */
static void *raspi5b_get_dtb(const struct arm_boot_info *info, int *size)
{
    static const char compat[] = "raspberrypi,5-model-b\0brcm,bcm2712";
    Raspi5bMachineState *s = container_of(info, Raspi5bMachineState, binfo);
    void *fdt = create_device_tree(size);

    if (!fdt) {
        return NULL;
    }
    qemu_fdt_setprop(fdt, "/", "compatible", compat, sizeof(compat));
    qemu_fdt_setprop_string(fdt, "/", "model", "Raspberry Pi 5 Model B");
    qemu_fdt_setprop_cell(fdt, "/", "#address-cells", 2);
    qemu_fdt_setprop_cell(fdt, "/", "#size-cells", 2);

    bcm2712_fdt_populate(&s->soc, fdt);

    qemu_fdt_add_subnode(fdt, "/chosen");
    qemu_fdt_setprop_string(fdt, "/chosen", "stdout-path",
                            "serial10:115200n8");

    if (MACHINE(s)->firmware) {
        /* BL31, as the firmware's tree describes it */
        static const char psci_compat[] = "arm,psci-1.0\0arm,psci-0.2";

        qemu_fdt_add_subnode(fdt, "/psci");
        qemu_fdt_setprop(fdt, "/psci", "compatible", psci_compat,
                         sizeof(psci_compat));
        qemu_fdt_setprop_string(fdt, "/psci", "method", "smc");

        qemu_fdt_add_subnode(fdt, "/reserved-memory/atf@0");
        qemu_fdt_setprop_sized_cells(fdt, "/reserved-memory/atf@0", "reg",
                                     2, BCM2712_RAM_BASE,
                                     2, RASPI5B_ARMSTUB_RESERVED);
        qemu_fdt_setprop(fdt, "/reserved-memory/atf@0", "no-map", NULL, 0);
    }

    if (fdt_pack(fdt) < 0) {
        g_free(fdt);
        return NULL;
    }
    *size = fdt_totalsize(fdt) + RASPI5B_FDT_SLACK;
    if (fdt_open_into(fdt, fdt, *size) < 0) {
        g_free(fdt);
        return NULL;
    }
    return g_realloc(fdt, *size);
}

static void raspi5b_modify_dtb(const struct arm_boot_info *info, void *fdt)
{
    const Raspi5bMachineState *s =
        container_of(info, Raspi5bMachineState, binfo);

    /* The VideoCore firmware publishes the board revision here */
    qemu_fdt_add_path(fdt, "/system");
    qemu_fdt_setprop_cell(fdt, "/system", "linux,revision", s->board_rev);

    raspi5b_fdt_memory(fdt, info->ram_size);
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

static void raspi5b_cpu_reset(void *opaque)
{
    cpu_reset(CPU(opaque));
}

/*
 * Load a 64-bit kernel where the firmware does: an ELF at its own
 * addresses, anything else, such as a Linux Image (gzipped or not), at
 * RASPI5B_KERNEL_ADDR plus the Image's text_offset. Returns the entry
 * point, and in *end the end of the memory the kernel uses, which for an
 * Image includes its BSS.
 */
static hwaddr raspi5b_load_kernel(const char *filename, AddressSpace *as,
                                  hwaddr *end)
{
    g_autofree uint8_t *buffer = NULL;
    uint64_t entry, high, used;
    hwaddr addr = RASPI5B_KERNEL_ADDR;
    ssize_t size;

    size = load_elf_as(filename, NULL, NULL, NULL, &entry, NULL, &high, NULL,
                       ELFDATA2LSB, EM_AARCH64, 1, 0, as);
    if (size > 0) {
        *end = high;
        return entry;
    }
    if (size != ELF_LOAD_NOT_ELF) {
        error_report("could not load kernel '%s': %s", filename,
                     load_elf_strerror(size));
        exit(EXIT_FAILURE);
    }

    size = load_image_gzipped_buffer(filename,
                                     LOAD_IMAGE_MAX_DECOMPRESSED_BYTES,
                                     &buffer);
    if (size < 0) {
        gsize len;

        if (!g_file_get_contents(filename, (char **)&buffer, &len, NULL)) {
            error_report("could not load kernel '%s'", filename);
            exit(EXIT_FAILURE);
        }
        size = len;
    }

    used = size;
    /* image_size is 0 in the headers of kernels before Linux 3.17 */
    if (size >= RASPI5B_IMAGE_MAGIC + 4 &&
        !memcmp(buffer + RASPI5B_IMAGE_MAGIC, "ARM\x64", 4) &&
        ldq_le_p(buffer + RASPI5B_IMAGE_SIZE)) {
        addr += ldq_le_p(buffer + RASPI5B_IMAGE_TEXT_OFFSET);
        used = MAX(ldq_le_p(buffer + RASPI5B_IMAGE_SIZE), size);
    }
    rom_add_blob_fixed_as(filename, buffer, size, addr, as);
    *end = addr + used;
    return addr;
}

/*
 * -bios: load the armstub at address 0, the kernel, initrd and device
 * tree where the firmware puts them, and tell the armstub where they are
 * through its header. Every core starts in the armstub at EL3, as when
 * the firmware releases them.
 */
static void raspi5b_boot_armstub(Raspi5bMachineState *s,
                                 MachineState *machine)
{
    ARMCPU *cpu = &s->soc.cpu[0];
    AddressSpace *as = arm_boot_address_space(cpu, &s->binfo);
    g_autofree char *filename = qemu_find_file(QEMU_FILE_TYPE_BIOS,
                                               machine->firmware);
    g_autofree uint8_t *stub = NULL;
    hwaddr kernel = RASPI5B_KERNEL_ADDR, next = RASPI5B_INITRD_ADDR;
    hwaddr dtb = 0, end;
    gsize size;

    if (!filename ||
        !g_file_get_contents(filename, (char **)&stub, &size, NULL)) {
        error_report("could not load the armstub '%s'", machine->firmware);
        exit(EXIT_FAILURE);
    }

    if (machine->kernel_filename) {
        kernel = raspi5b_load_kernel(machine->kernel_filename, as, &end);
        next = MAX(next, end);
    }
    if (machine->initrd_filename) {
        ssize_t initrd_size;

        next = QEMU_ALIGN_UP(next, 4 * KiB);
        initrd_size = load_ramdisk_as(machine->initrd_filename, next,
                                      BCM2712_VC_RAM_BASE - next, as);
        if (initrd_size < 0) {
            initrd_size = load_image_targphys_as(machine->initrd_filename,
                                                 next,
                                                 BCM2712_VC_RAM_BASE - next,
                                                 as, &error_fatal);
        }
        s->binfo.initrd_start = next;
        s->binfo.initrd_size = initrd_size;
        next += initrd_size;
    }
    if (s->binfo.dtb_filename || s->binfo.get_dtb) {
        int dtb_size;

        /* As for -kernel: the kernel maps the tree's 2 MiB block early */
        dtb = s->dtb_addr ? s->dtb_addr : QEMU_ALIGN_UP(next, 2 * MiB);
        dtb_size = arm_load_dtb(dtb, &s->binfo, 0, as, machine, cpu);
        if (dtb_size < 0) {
            exit(EXIT_FAILURE);
        }
        next = MAX(next, dtb + dtb_size);
    }
    if (next > BCM2712_VC_RAM_BASE || kernel > UINT32_MAX) {
        error_report("the kernel, initrd and device tree must fit below "
                     "0x%x, where the VideoCore's memory starts",
                     BCM2712_VC_RAM_BASE);
        exit(EXIT_FAILURE);
    }

    if (size >= RASPI5B_ARMSTUB_KERNEL_OFFSET + 4 &&
        ldl_le_p(stub + RASPI5B_ARMSTUB_MAGIC_OFFSET) ==
        RASPI5B_ARMSTUB_MAGIC) {
        stl_le_p(stub + RASPI5B_ARMSTUB_MAGIC_OFFSET, 0);
        stl_le_p(stub + RASPI5B_ARMSTUB_DTB_OFFSET, dtb);
        stl_le_p(stub + RASPI5B_ARMSTUB_KERNEL_OFFSET, kernel);
    }
    rom_add_blob_fixed_as(filename, stub, size, BCM2712_RAM_BASE, as);

    /* No PSCI here: the cores run from their reset vector, address 0 */
    for (unsigned int i = 0; i < machine->smp.cpus; i++) {
        qemu_register_reset(raspi5b_cpu_reset, &s->soc.cpu[i]);
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
    if (machine->firmware && !s->secure) {
        error_report("-bios loads firmware that runs at EL3; "
                     "use -M raspi5b,secure=on");
        exit(EXIT_FAILURE);
    }
    if (s->dtb_addr && !machine->firmware) {
        error_report("dtb-address places the device tree for -bios only");
        exit(EXIT_FAILURE);
    }
    if (s->dtb_addr % 8 || s->dtb_addr >= BCM2712_VC_RAM_BASE) {
        error_report("dtb-address must be a multiple of 8 below 0x%x",
                     BCM2712_VC_RAM_BASE);
        exit(EXIT_FAILURE);
    }
    s->board_rev = raspi5b_board_rev(machine->ram_size);

    memory_region_add_subregion(get_system_memory(), BCM2712_RAM_BASE,
                                machine->ram);

    object_initialize_child(OBJECT(machine), "soc", &s->soc, TYPE_BCM2712);
    soc = DEVICE(&s->soc);
    qdev_prop_set_uint32(soc, "num-cpus", machine->smp.cpus);
    qdev_prop_set_bit(soc, "has-el3", s->secure);
    object_property_set_link(OBJECT(soc), "ram", OBJECT(machine->ram),
                             &error_abort);
    qdev_prop_set_uint32(soc, "board-rev", s->board_rev);
    qdev_prop_set_uint64(soc, "board-serial", s->serial);
    /* The firmware passes on the command line it gives the kernel */
    qdev_prop_set_string(soc, "command-line", machine->kernel_cmdline);
    qdev_realize(soc, NULL, &error_fatal);

    s->binfo = (struct arm_boot_info) {
        .ram_size = machine->ram_size,
        .loader_start = BCM2712_RAM_BASE,
        /* Disabled by arm_load_kernel() if the guest itself starts in EL3 */
        .psci_conduit = QEMU_PSCI_CONDUIT_SMC,
        .modify_dtb = raspi5b_modify_dtb,
        .get_dtb = s->builtin_dtb ? raspi5b_get_dtb : NULL,
    };
    if (machine->firmware) {
        s->binfo.dtb_filename = machine->dtb;
        raspi5b_boot_armstub(s, machine);
    } else {
        arm_load_kernel(&s->soc.cpu[0], machine, &s->binfo);
    }
}

static bool raspi5b_get_secure(Object *obj, Error **errp)
{
    return RASPI5B_MACHINE(obj)->secure;
}

static void raspi5b_set_secure(Object *obj, bool value, Error **errp)
{
    RASPI5B_MACHINE(obj)->secure = value;
}

static void raspi5b_get_serial(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    visit_type_uint64(v, name, &RASPI5B_MACHINE(obj)->serial, errp);
}

static void raspi5b_set_serial(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    visit_type_uint64(v, name, &RASPI5B_MACHINE(obj)->serial, errp);
}

static void raspi5b_get_dtb_addr(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    visit_type_uint64(v, name, &RASPI5B_MACHINE(obj)->dtb_addr, errp);
}

static void raspi5b_set_dtb_addr(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    visit_type_uint64(v, name, &RASPI5B_MACHINE(obj)->dtb_addr, errp);
}

static bool raspi5b_get_builtin_dtb(Object *obj, Error **errp)
{
    return RASPI5B_MACHINE(obj)->builtin_dtb;
}

static void raspi5b_set_builtin_dtb(Object *obj, bool value, Error **errp)
{
    RASPI5B_MACHINE(obj)->builtin_dtb = value;
}

static void raspi5b_machine_instance_init(Object *obj)
{
    Raspi5bMachineState *s = RASPI5B_MACHINE(obj);

    s->serial = RASPI5B_DEFAULT_SERIAL;
    s->builtin_dtb = true;
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
        "firmware's TF-A BL31, which -bios loads when on");

    object_class_property_add_bool(oc, "builtin-dtb",
                                   raspi5b_get_builtin_dtb,
                                   raspi5b_set_builtin_dtb);
    object_class_property_set_description(oc, "builtin-dtb",
        "Without -dtb, give the guest a device tree generated from the "
        "model (the default); when off, give it none, like an empty "
        "device_tree= line in the firmware's config.txt");

    object_class_property_add(oc, "dtb-address", "uint64",
                              raspi5b_get_dtb_addr, raspi5b_set_dtb_addr,
                              NULL, NULL);
    object_class_property_set_description(oc, "dtb-address",
        "Where -bios places the device tree, like device_tree_address= "
        "in the firmware's config.txt; by default, above the kernel and "
        "initrd");

    object_class_property_add(oc, "serial", "uint64", raspi5b_get_serial,
                              raspi5b_set_serial, NULL, NULL);
    object_class_property_set_description(oc, "serial",
        "The board serial number the firmware reports (GET_BOARD_SERIAL)");
}

static const TypeInfo raspi5b_machine_types[] = {
    {
        .name           = TYPE_RASPI5B_MACHINE,
        .parent         = TYPE_MACHINE,
        .instance_size  = sizeof(Raspi5bMachineState),
        .class_init     = raspi5b_machine_class_init,
        .instance_init  = raspi5b_machine_instance_init,
        .interfaces     = aarch64_machine_interfaces,
    },
};

DEFINE_TYPES(raspi5b_machine_types)
