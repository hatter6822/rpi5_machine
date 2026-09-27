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
#include "qemu/guest-random.h"
#include "qemu/host-utils.h"
#include "qemu/range.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/arm/bcm2712.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/registerfields.h"
#include "hw/display/i2c-ddc.h"
#include "hw/misc/led.h"
#include "hw/sd/sd.h"
#include "migration/vmstate.h"
#include "standard-headers/linux/input.h"
#include "system/address-spaces.h"
#include "system/blockdev.h"
#include "system/device_tree.h"
#include "system/reset.h"
#include "system/runstate.h"
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
    /* Whether dtb-address was given at all: 0 is an address like any other */
    bool dtb_addr_set;
    /* The size of the image -bios loaded, which the built-in tree reserves */
    uint64_t armstub_size;
    bool secure;
    bool builtin_dtb;
    /*
     * The firmware's boot count: boots since power-on, in 8 bits, which it
     * keeps in a register a reset leaves alone
     */
    uint8_t boot_count;
    /*
     * The rest of what a reset leaves for the boot it starts, when given
     * for the first boot (see raspi5b_carry_reset()): the reset status
     * and the firmware's reboot flags
     */
    uint32_t reset_status;
    bool reset_status_set;
    uint32_t reboot_flags;
    /* The partition the boot's files come from, when given */
    uint32_t boot_partition;
    bool boot_partition_set;

    /*
     * The power button, which system_powerdown presses for a moment: its
     * line, and the timer that releases it
     */
    Notifier powerdown;
    qemu_irq pwr_button;
    QEMUTimer *pwr_button_release;
};

/* An obviously made-up serial number, overridden with "serial=" */
#define RASPI5B_DEFAULT_SERIAL  0x0123456789abcdefULL

/*
 * The board's use of the SoC's GPIO lines, from the firmware's device
 * tree. The power button pulls GIO 20 (PWR_GPIO) low while pressed; AON
 * GPIO 9 lights the green activity LED while driven low. Both lines are
 * pulled up. So is AON GPIO 5 (SD_CDET_N), which the SD card slot's
 * switch pulls low while a card is in; AON GPIO 4 switches the card's
 * supply on, and AON GPIO 3 its signalling from 3.3 V to 1.8 V.
 */
#define RASPI5B_GIO_PWR_BUTTON          20
#define RASPI5B_AON_GPIO_SD_IO_1V8      3
#define RASPI5B_AON_GPIO_SD_VCC         4
#define RASPI5B_AON_GPIO_SD_CDET_N      5
#define RASPI5B_AON_GPIO_ACT_LED        9

/*
 * system_powerdown presses the power button for a moment. Linux reports
 * KEY_POWER once the line has stayed low for the debounce interval, and
 * systemd-logind then powers off; the press outlasts the interval with
 * room to spare for a busy guest.
 */
#define RASPI5B_PWR_BUTTON_DEBOUNCE_MS  50
#define RASPI5B_PWR_BUTTON_PRESS_MS     200

/* A monitor's EDID answers at this address on its HDMI port's DDC bus */
#define RASPI5B_DDC_EDID_ADDR           0x50

/* include/dt-bindings/gpio/gpio.h */
#define RASPI5B_FDT_GPIO_ACTIVE_HIGH    0
#define RASPI5B_FDT_GPIO_ACTIVE_LOW     1

/* The SD card's supply and the signalling voltages, in microvolts */
#define RASPI5B_FDT_SD_3V3              3300000
#define RASPI5B_FDT_SD_1V8              1800000

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

/*
 * The memory the firmware's device tree reserves for BL31 (atf@0), which
 * the built-in tree extends to cover a larger armstub, in steps of 64 KiB,
 * the largest page size of arm64 Linux
 */
#define RASPI5B_ARMSTUB_RESERVED        0x80000
#define RASPI5B_ARMSTUB_RESERVED_ALIGN  (64 * KiB)

/*
 * The arm64 Linux Image header: text_offset, image_size and magic. Before
 * Linux 3.17, image_size is 0 and text_offset 0x80000, in the kernel's
 * byte order (Documentation/arch/arm64/booting.rst).
 */
#define RASPI5B_IMAGE_TEXT_OFFSET       8
#define RASPI5B_IMAGE_SIZE              16
#define RASPI5B_IMAGE_MAGIC             56
#define RASPI5B_IMAGE_OLD_TEXT_OFFSET   0x80000

/*
 * The bootloader configuration the firmware copies into its own memory
 * for the OS, which finds it through /reserved-memory/nvram@0 (alias
 * blconfig): a Pi 5's default, rated for the bench supply the machine
 * reports in /chosen/power, which the firmware takes from
 * PSU_MAX_CURRENT (in mA) without USB-PD negotiation.
 */
#define RASPI5B_PSU_MAX_CURRENT         5000
static const char raspi5b_blconfig[] =
    "[all]\n"
    "BOOT_UART=1\n"
    "POWER_OFF_ON_HALT=0\n"
    "BOOT_ORDER=0xf461\n"
    "PSU_MAX_CURRENT=" stringify(RASPI5B_PSU_MAX_CURRENT) "\n";

/* Where the copy goes: VideoCore memory the model leaves unused */
#define RASPI5B_BLCONFIG_ADDR           (BCM2712_VC_RAM_BASE + 512 * KiB)

/*
 * /chosen/bootloader/boot-mode, as BOOT_ORDER numbers them: RPIBOOT, in
 * which the host supplies the boot files, as QEMU's -kernel, -dtb and
 * -initrd do
 */
#define RASPI5B_BOOT_MODE_RPIBOOT       3

/* PM_RSTS carries the partition the OS asked for in bits 0, 2, .. 10 */
#define RASPI5B_RSTS_PARTITION_BITS     6

/* Size of the rng-seed the machine gives, as QEMU's virt machine does */
#define RASPI5B_RNG_SEED_SIZE           32

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
 * disabled rather than deleted so that phandle references stay valid. The
 * list shrinks as models land; a device behind a newly modelled one, such
 * as the Bluetooth radio on UARTA, joins it while it stays unmodelled, as
 * does a device whose driver needs one that is not. See docs/PLAN.md.
 */
static const char *const raspi5b_unmodelled_compatibles[] = {
    "brcm,bcm2712-pcie",
    "brcm,bcm2712-mip",
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
    "brcm,brcmstb-reset",
    "brcm,bcm7216-pcie-sata-rescal",
    /* Nodes only present in the Raspberry Pi downstream device tree */
    "brcm,bcm2712-iommu",
    "brcm,bcm2712-iommuc",
    "brcm,bcm2712-dma",
    "brcm,bcm2712-hevc-dec",
    "raspberrypi,pispbe",
    "brcm,syscon-piarbctl",
    "brcm,brcm2711-dvp",
    "brcm,bcm2835-spi",
    /* Clients of the VideoCore firmware (mailbox) or of RP1's */
    "raspberrypi,rpi-otp",
    "raspberrypi,rp1-firmware",
    /*
     * The firmware's framebuffer, whose Linux driver (bcm2708_fb) takes a
     * channel of the DMA controller above
     */
    "brcm,bcm2708-fb",
    /*
     * Behind modelled devices: the Bluetooth radio on UARTA, the Wi-Fi
     * radio on SDIO2
     */
    "brcm,bcm43438-bt",
    "brcm,bcm4329-fmac",
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
 * The power button, with the state of its pin, and the activity LED, as
 * the firmware's tree has them but under node names their bindings
 * accept. The power LED hangs off RP1, which is not modelled.
 */
static void raspi5b_fdt_gpio_users(void *fdt)
{
    g_autofree char *gio = bcm2712_fdt_node_path(fdt, BCM2712_GIO);
    g_autofree char *gio_aon = bcm2712_fdt_node_path(fdt, BCM2712_GIO_AON);
    g_autofree char *pinctrl = bcm2712_fdt_node_path(fdt, BCM2712_PINCTRL);
    g_autofree char *button_pin = g_strdup_printf(
        "%s/pwr-button-default-state", pinctrl);
    g_autofree char *button_gpio = g_strdup_printf(
        "gpio%d", RASPI5B_GIO_PWR_BUTTON);
    uint32_t button_pin_phandle = qemu_fdt_alloc_phandle(fdt);
    const char *button = "/gpio-keys/power-button";
    const char *led = "/leds/led-act";

    qemu_fdt_add_subnode(fdt, button_pin);
    qemu_fdt_setprop_string(fdt, button_pin, "function", "gpio");
    qemu_fdt_setprop_string(fdt, button_pin, "pins", button_gpio);
    qemu_fdt_setprop(fdt, button_pin, "bias-pull-up", NULL, 0);
    qemu_fdt_setprop_cell(fdt, button_pin, "phandle", button_pin_phandle);

    qemu_fdt_add_subnode(fdt, "/gpio-keys");
    qemu_fdt_setprop_string(fdt, "/gpio-keys", "compatible", "gpio-keys");
    qemu_fdt_setprop_string(fdt, "/gpio-keys", "pinctrl-names", "default");
    qemu_fdt_setprop_cell(fdt, "/gpio-keys", "pinctrl-0", button_pin_phandle);
    qemu_fdt_add_subnode(fdt, button);
    qemu_fdt_setprop_string(fdt, button, "label", "pwr_button");
    qemu_fdt_setprop_cell(fdt, button, "linux,code", KEY_POWER);
    qemu_fdt_setprop_cells(fdt, button, "gpios",
                           qemu_fdt_get_phandle(fdt, gio),
                           RASPI5B_GIO_PWR_BUTTON,
                           RASPI5B_FDT_GPIO_ACTIVE_LOW);
    qemu_fdt_setprop_cell(fdt, button, "debounce-interval",
                          RASPI5B_PWR_BUTTON_DEBOUNCE_MS);

    qemu_fdt_add_subnode(fdt, "/leds");
    qemu_fdt_setprop_string(fdt, "/leds", "compatible", "gpio-leds");
    qemu_fdt_add_subnode(fdt, led);
    qemu_fdt_setprop_string(fdt, led, "label", "ACT");
    qemu_fdt_setprop_cells(fdt, led, "gpios",
                           qemu_fdt_get_phandle(fdt, gio_aon),
                           RASPI5B_AON_GPIO_ACT_LED,
                           RASPI5B_FDT_GPIO_ACTIVE_LOW);
    qemu_fdt_setprop_string(fdt, led, "default-state", "off");
    qemu_fdt_setprop_string(fdt, led, "linux,default-trigger", "mmc0");
}

/*
 * The SD card slot on SDIO1, as the firmware's tree has it: the pull-ups
 * of the card's lines and its card detect switch, the regulators of its
 * supply and signalling voltage, and the UHS-I modes, which need 1.8 V
 * signalling that QEMU's cards do not offer.
 */
static void raspi5b_fdt_sd_slot(void *fdt)
{
    static const char sd_pins[] =
        "emmc_cmd\0emmc_dat0\0emmc_dat1\0emmc_dat2\0emmc_dat3";
    g_autofree char *gio_aon = bcm2712_fdt_node_path(fdt, BCM2712_GIO_AON);
    g_autofree char *pinctrl = bcm2712_fdt_node_path(fdt, BCM2712_PINCTRL);
    g_autofree char *pinctrl_aon = bcm2712_fdt_node_path(fdt,
                                                         BCM2712_PINCTRL_AON);
    g_autofree char *sdio1 = bcm2712_fdt_node_path(fdt, BCM2712_SDIO1);
    g_autofree char *sd_state = g_strdup_printf(
        "%s/emmc-sd-default-state", pinctrl);
    g_autofree char *cd_state = g_strdup_printf(
        "%s/emmc-aon-cd-default-state", pinctrl_aon);
    g_autofree char *cd_pin = g_strdup_printf(
        "aon_gpio%d", RASPI5B_AON_GPIO_SD_CDET_N);
    uint32_t gio_aon_phandle = qemu_fdt_get_phandle(fdt, gio_aon);
    uint32_t sd_state_phandle = qemu_fdt_alloc_phandle(fdt);
    uint32_t cd_state_phandle = qemu_fdt_alloc_phandle(fdt);
    uint32_t io_reg_phandle = qemu_fdt_alloc_phandle(fdt);
    uint32_t vcc_reg_phandle = qemu_fdt_alloc_phandle(fdt);
    const char *io_reg = "/sd-io-1v8-reg";
    const char *vcc_reg = "/sd-vcc-reg";

    qemu_fdt_add_subnode(fdt, sd_state);
    qemu_fdt_setprop(fdt, sd_state, "pins", sd_pins, sizeof(sd_pins));
    qemu_fdt_setprop(fdt, sd_state, "bias-pull-up", NULL, 0);
    qemu_fdt_setprop_cell(fdt, sd_state, "phandle", sd_state_phandle);

    qemu_fdt_add_subnode(fdt, cd_state);
    qemu_fdt_setprop_string(fdt, cd_state, "function", "sd_card_g");
    qemu_fdt_setprop_string(fdt, cd_state, "pins", cd_pin);
    qemu_fdt_setprop(fdt, cd_state, "bias-pull-up", NULL, 0);
    qemu_fdt_setprop_cell(fdt, cd_state, "phandle", cd_state_phandle);

    /* In reverse, since libfdt adds each subnode first */
    qemu_fdt_add_subnode(fdt, vcc_reg);
    qemu_fdt_setprop_string(fdt, vcc_reg, "compatible", "regulator-fixed");
    qemu_fdt_setprop_string(fdt, vcc_reg, "regulator-name", "vcc-sd");
    qemu_fdt_setprop_cell(fdt, vcc_reg, "regulator-min-microvolt",
                          RASPI5B_FDT_SD_3V3);
    qemu_fdt_setprop_cell(fdt, vcc_reg, "regulator-max-microvolt",
                          RASPI5B_FDT_SD_3V3);
    qemu_fdt_setprop(fdt, vcc_reg, "regulator-boot-on", NULL, 0);
    qemu_fdt_setprop(fdt, vcc_reg, "enable-active-high", NULL, 0);
    qemu_fdt_setprop_cells(fdt, vcc_reg, "gpios", gio_aon_phandle,
                           RASPI5B_AON_GPIO_SD_VCC,
                           RASPI5B_FDT_GPIO_ACTIVE_HIGH);
    qemu_fdt_setprop_cell(fdt, vcc_reg, "phandle", vcc_reg_phandle);

    qemu_fdt_add_subnode(fdt, io_reg);
    qemu_fdt_setprop_string(fdt, io_reg, "compatible", "regulator-gpio");
    qemu_fdt_setprop_string(fdt, io_reg, "regulator-name", "vdd-sd-io");
    qemu_fdt_setprop_cell(fdt, io_reg, "regulator-min-microvolt",
                          RASPI5B_FDT_SD_1V8);
    qemu_fdt_setprop_cell(fdt, io_reg, "regulator-max-microvolt",
                          RASPI5B_FDT_SD_3V3);
    qemu_fdt_setprop(fdt, io_reg, "regulator-boot-on", NULL, 0);
    qemu_fdt_setprop(fdt, io_reg, "regulator-always-on", NULL, 0);
    qemu_fdt_setprop_cell(fdt, io_reg, "regulator-settling-time-us", 5000);
    qemu_fdt_setprop_cells(fdt, io_reg, "gpios", gio_aon_phandle,
                           RASPI5B_AON_GPIO_SD_IO_1V8,
                           RASPI5B_FDT_GPIO_ACTIVE_HIGH);
    qemu_fdt_setprop_cells(fdt, io_reg, "states", RASPI5B_FDT_SD_1V8, 1,
                           RASPI5B_FDT_SD_3V3, 0);
    qemu_fdt_setprop_cell(fdt, io_reg, "phandle", io_reg_phandle);

    qemu_fdt_setprop_cells(fdt, sdio1, "pinctrl-0", sd_state_phandle,
                           cd_state_phandle);
    qemu_fdt_setprop_string(fdt, sdio1, "pinctrl-names", "default");
    qemu_fdt_setprop_cell(fdt, sdio1, "vqmmc-supply", io_reg_phandle);
    qemu_fdt_setprop_cell(fdt, sdio1, "vmmc-supply", vcc_reg_phandle);
    qemu_fdt_setprop_cell(fdt, sdio1, "bus-width", 4);
    qemu_fdt_setprop(fdt, sdio1, "sd-uhs-sdr50", NULL, 0);
    qemu_fdt_setprop(fdt, sdio1, "sd-uhs-ddr50", NULL, 0);
    qemu_fdt_setprop(fdt, sdio1, "sd-uhs-sdr104", NULL, 0);
    qemu_fdt_setprop_cells(fdt, sdio1, "cd-gpios", gio_aon_phandle,
                           RASPI5B_AON_GPIO_SD_CDET_N,
                           RASPI5B_FDT_GPIO_ACTIVE_LOW);

    qemu_fdt_setprop_string(fdt, "/aliases", "mmc0", sdio1);
}

/*
 * Room left in the built-in tree for what arm_load_dtb() and
 * raspi5b_modify_dtb() add: memory, PSCI, /chosen with the command line,
 * /system. load_device_tree() leaves at least as much for a -dtb blob.
 */
#define RASPI5B_FDT_SLACK       10000

/*
 * The device tree given without -dtb: the board's own nodes, the SoC's,
 * the board's users of GPIO lines, and the console on UART10 as on the
 * firmware's tree. arm_load_dtb() adds the memory, PSCI and /chosen
 * properties, then the same fix-ups as for a -dtb blob apply.
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
    raspi5b_fdt_gpio_users(fdt);
    raspi5b_fdt_sd_slot(fdt);

    qemu_fdt_add_subnode(fdt, "/chosen");
    qemu_fdt_setprop_string(fdt, "/chosen", "stdout-path",
                            "serial10:115200n8");

    if (MACHINE(s)->firmware) {
        /* BL31, as the firmware's tree describes it, or a larger armstub */
        static const char psci_compat[] = "arm,psci-1.0\0arm,psci-0.2";
        uint64_t reserved = MAX(RASPI5B_ARMSTUB_RESERVED,
                                QEMU_ALIGN_UP(s->armstub_size,
                                              RASPI5B_ARMSTUB_RESERVED_ALIGN));

        qemu_fdt_add_subnode(fdt, "/psci");
        qemu_fdt_setprop(fdt, "/psci", "compatible", psci_compat,
                         sizeof(psci_compat));
        qemu_fdt_setprop_string(fdt, "/psci", "method", "smc");

        qemu_fdt_add_subnode(fdt, "/reserved-memory/atf@0");
        qemu_fdt_setprop_sized_cells(fdt, "/reserved-memory/atf@0", "reg",
                                     2, BCM2712_RAM_BASE, 2, reserved);
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

/* The partition the OS asked the next boot for, which PM_RSTS carries */
static uint32_t raspi5b_rsts_partition(uint32_t rsts)
{
    uint32_t partition = 0;

    for (int i = 0; i < RASPI5B_RSTS_PARTITION_BITS; i++) {
        partition = deposit32(partition, i, 1, extract32(rsts, 2 * i, 1));
    }
    return partition;
}

/*
 * What the firmware reports about the boot it is making, which it writes
 * afresh each time: the reset status it found, the partition it boots
 * from, and the boot's number since power-on. The files QEMU supplies
 * come from the partition boot-partition names, if it names one, and
 * otherwise stand for the one the OS asked for (0 at power-on).
 */
static void raspi5b_boot_values(const Raspi5bMachineState *s,
                                uint32_t *rsts, uint32_t *partition,
                                uint8_t *count)
{
    *rsts = s->soc.pm.rsts;
    *partition = s->boot_partition_set ? s->boot_partition
                                       : raspi5b_rsts_partition(*rsts);
    *count = s->boot_count + 1;
}

/*
 * The board's identity as the firmware writes it: the model name with
 * the revision of the board revision code, the serial number, and
 * /system, from which the Raspberry Pi kernel reads the revision code
 * (the serial it reads from the root's serial-number)
 */
static void raspi5b_fdt_identity(const Raspi5bMachineState *s, void *fdt)
{
    g_autofree char *model = g_strdup_printf("Raspberry Pi 5 Model B Rev 1.%u",
        FIELD_EX32(s->board_rev, REV_CODE, REVISION));
    g_autofree char *serial = g_strdup_printf("%016" PRIx64, s->serial);

    qemu_fdt_setprop_string(fdt, "/", "model", model);
    qemu_fdt_setprop_string(fdt, "/", "serial-number", serial);
    qemu_fdt_add_path(fdt, "/system");
    qemu_fdt_setprop_cell(fdt, "/system", "linux,revision", s->board_rev);
    qemu_fdt_setprop_u64(fdt, "/system", "linux,serial", s->serial);
    qemu_fdt_setprop_string(fdt, "/chosen", "rpi-serial64", serial);
}

/*
 * The tree's own bootargs, which arm_load_dtb() has replaced with -append
 * if there is one: none for the built-in tree
 */
static char *raspi5b_dtb_bootargs(const char *filename)
{
    g_autofree void *fdt = NULL;
    const char *args;
    int size, len;

    fdt = filename ? load_device_tree(filename, &size) : NULL;
    if (!fdt) {
        return NULL;
    }
    args = fdt_getprop(fdt, fdt_path_offset(fdt, "/chosen"), "bootargs",
                       &len);
    if (!args || len < 2 || args[len - 1]) {
        return NULL;
    }
    return g_strdup(args);
}

/*
 * The kernel command line as the firmware builds it: the tree's bootargs,
 * the arguments the firmware adds for the board (the Ethernet address it
 * reports, and where the VideoCore's memory lies), then cmdline.txt,
 * whose part -append plays, each part set off by two spaces as the
 * firmware sets them. It adds no NUMA arguments, which follow how the
 * SDRAM's banks are mapped (see docs/system/arm/raspi5b.rst).
 */
static void raspi5b_fdt_bootargs(const Raspi5bMachineState *s, void *fdt,
                                 const char *dtb_filename)
{
    g_autofree char *base = raspi5b_dtb_bootargs(dtb_filename);
    const char *append = MACHINE(s)->kernel_cmdline;
    const uint8_t *mac = s->soc.property.parent_obj.macaddr.a;
    g_autoptr(GString) args = g_string_new(base);

    if (args->len) {
        g_string_append(args, "  ");
    }
    g_string_append_printf(args,
        "smsc95xx.macaddr=%02X:%02X:%02X:%02X:%02X:%02X "
        "vc_mem.mem_base=0x%x vc_mem.mem_size=0x%x",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
        BCM2712_VC_RAM_BASE, (unsigned)BCM2712_VC_RAM_WINDOW);
    if (append && *append) {
        g_string_append_printf(args, "  %s", append);
    }
    qemu_fdt_setprop_string(fdt, "/chosen", "bootargs", args->str);
}

/* A string property of /chosen, unless the tree has one of that name */
static void raspi5b_fdt_chosen_default(void *fdt, const char *name,
                                       const char *value)
{
    if (!fdt_getprop(fdt, fdt_path_offset(fdt, "/chosen"), name, NULL)) {
        qemu_fdt_setprop_string(fdt, "/chosen", name, value);
    }
}

/*
 * /chosen as the firmware fills it in, beyond bootargs: entropy for the
 * kernel, the boot the bootloader made, the power supply, and the
 * config.txt prefixes, at their defaults unless the tree has them (as a
 * host that evaluated config.txt for the boot writes them)
 */
static void raspi5b_fdt_chosen(const Raspi5bMachineState *s, void *fdt,
                               uint64_t ram_size)
{
    uint8_t rng_seed[RASPI5B_RNG_SEED_SIZE];
    uint64_t kaslr_seed;
    uint32_t rsts, partition;
    uint8_t count;

    qemu_guest_getrandom_nofail(&kaslr_seed, sizeof(kaslr_seed));
    qemu_guest_getrandom_nofail(rng_seed, sizeof(rng_seed));
    qemu_fdt_setprop_u64(fdt, "/chosen", "kaslr-seed", kaslr_seed);
    qemu_fdt_setprop(fdt, "/chosen", "rng-seed", rng_seed, sizeof(rng_seed));

    raspi5b_boot_values(s, &rsts, &partition, &count);
    qemu_fdt_add_path(fdt, "/chosen/bootloader");
    qemu_fdt_setprop_cell(fdt, "/chosen/bootloader", "boot-mode",
                          RASPI5B_BOOT_MODE_RPIBOOT);
    qemu_fdt_setprop_cell(fdt, "/chosen/bootloader", "partition", partition);
    qemu_fdt_setprop_cell(fdt, "/chosen/bootloader", "rsts", rsts);
    /* The reboot flags a reset left, which raspi5b_bootloader() takes */
    qemu_fdt_setprop_cell(fdt, "/chosen/bootloader", "tryboot",
        !!(s->soc.property.reboot_flags & BCM2712_REBOOT_FLAG_TRYBOOT));
    /* No USB, network, tryboot, RAM disk, NVMe or secure boot */
    qemu_fdt_setprop_cell(fdt, "/chosen/bootloader", "capabilities", 0);
    qemu_fdt_setprop_cell(fdt, "/chosen/bootloader", "arg1", 0);
    qemu_fdt_setprop_cell(fdt, "/chosen/bootloader", "count", count);

    qemu_fdt_add_path(fdt, "/chosen/power");
    qemu_fdt_setprop_cell(fdt, "/chosen/power", "max_current",
                          RASPI5B_PSU_MAX_CURRENT);
    /* Enabled for a supply that claims 5 A */
    qemu_fdt_setprop_cell(fdt, "/chosen/power", "usb_max_current_enable",
                          RASPI5B_PSU_MAX_CURRENT >= 5000);
    qemu_fdt_setprop_cell(fdt, "/chosen/power", "usb_over_current_detected",
                          0);
    qemu_fdt_setprop_cell(fdt, "/chosen/power", "power_reset", 0);

    raspi5b_fdt_chosen_default(fdt, "os_prefix", "");
    raspi5b_fdt_chosen_default(fdt, "overlay_prefix", "overlays/");
    qemu_fdt_setprop_cell(fdt, "/chosen", "rpi-sdram-size-gbit",
                          ram_size / (GiB / 8));
}

/*
 * A CMA pool sized in one cell, as the firmware's tree has it, gets the
 * parent's two, as the firmware writes it: Linux warns that the firmware
 * is out of date otherwise
 */
static void raspi5b_fdt_cma(void *fdt)
{
    const char *path = "/reserved-memory/linux,cma";
    int node = fdt_path_offset(fdt, path);
    const fdt32_t *size;
    int len, cells;

    if (node < 0) {
        return;
    }
    size = fdt_getprop(fdt, node, "size", &len);
    cells = fdt_size_cells(fdt, fdt_parent_offset(fdt, node));
    if (size && len == sizeof(*size) && cells == 2) {
        qemu_fdt_setprop_u64(fdt, path, "size", fdt32_to_cpu(*size));
    }
}

/*
 * Point the tree's bootloader-config node at the copy of the configuration
 * and enable it, as the firmware does once it has made the copy
 */
static void raspi5b_fdt_blconfig(void *fdt)
{
    g_auto(GStrv) paths = qemu_fdt_node_path(fdt, NULL,
                                             "raspberrypi,bootloader-config",
                                             &error_fatal);
    size_t size = sizeof(raspi5b_blconfig) - 1;

    if (!paths[0]) {
        return;
    }
    rom_add_blob_fixed("blconfig", raspi5b_blconfig, size,
                       RASPI5B_BLCONFIG_ADDR);
    for (char **path = paths; *path; path++) {
        int parent = fdt_parent_offset(fdt, fdt_path_offset(fdt, *path));

        qemu_fdt_setprop_sized_cells(fdt, *path, "reg",
                                     fdt_address_cells(fdt, parent),
                                     RASPI5B_BLCONFIG_ADDR,
                                     fdt_size_cells(fdt, parent), size);
        qemu_fdt_setprop_string(fdt, *path, "status", "okay");
    }
}

/* The board's Ethernet address, which the command line carries too */
static void raspi5b_fdt_mac(const Raspi5bMachineState *s, void *fdt)
{
    const char *path = fdt_get_alias(fdt, "ethernet0");

    if (path && fdt_path_offset(fdt, path) >= 0) {
        g_autofree char *node = g_strdup(path);

        qemu_fdt_setprop(fdt, node, "local-mac-address",
                         s->soc.property.parent_obj.macaddr.a,
                         sizeof(s->soc.property.parent_obj.macaddr.a));
    }
}

/*
 * What the firmware changes in the tree before it starts the OS, for any
 * tree: the built-in one (see raspi5b_get_dtb()) or a -dtb blob. The rest
 * of the model's own changes follow: RAM, CPUs and unmodelled devices.
 */
static void raspi5b_modify_dtb(const struct arm_boot_info *info, void *fdt)
{
    const Raspi5bMachineState *s =
        container_of(info, Raspi5bMachineState, binfo);

    raspi5b_fdt_identity(s, fdt);
    raspi5b_fdt_bootargs(s, fdt, info->dtb_filename);
    raspi5b_fdt_chosen(s, fdt, info->ram_size);
    raspi5b_fdt_cma(fdt);
    raspi5b_fdt_blconfig(fdt);
    raspi5b_fdt_mac(s, fdt);

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

/*
 * What the bootloader does for each boot. It counts the boot, with a
 * device tree or without, and takes the reboot flags the last boot left
 * in the firmware, which are for this boot only. QEMU copies the same
 * tree back into memory at every reset, where the firmware writes a new
 * one for each boot: bring the values that change from boot to boot up
 * to date first, including a new KASLR seed (QEMU renews rng-seed
 * itself). Registered before the ROMs' own reset, so the boot the reset
 * starts sees them.
 */
static void raspi5b_bootloader(void *opaque)
{
    Raspi5bMachineState *s = opaque;
    AddressSpace *as = arm_boot_address_space(&s->soc.cpu[0], &s->binfo);
    uint32_t reboot_flags = s->soc.property.reboot_flags;
    void *fdt;
    int node;
    uint64_t kaslr_seed;
    uint32_t rsts, partition;
    uint8_t count;

    raspi5b_boot_values(s, &rsts, &partition, &count);
    s->boot_count = count;
    s->soc.property.reboot_flags = 0;
    /* arm_load_dtb() has left the tree here if it loaded one */
    if (!MACHINE(s)->fdt) {
        return;
    }
    fdt = rom_ptr_for_as(as, s->binfo.dtb_start, sizeof(struct fdt_header));
    if (!fdt || fdt_check_header(fdt) ||
        !rom_ptr_for_as(as, s->binfo.dtb_start, fdt_totalsize(fdt))) {
        return;
    }
    node = fdt_path_offset(fdt, "/chosen/bootloader");
    if (node >= 0) {
        fdt_setprop_inplace_u32(fdt, node, "rsts", rsts);
        fdt_setprop_inplace_u32(fdt, node, "partition", partition);
        fdt_setprop_inplace_u32(fdt, node, "count", count);
        fdt_setprop_inplace_u32(fdt, node, "tryboot",
                                !!(reboot_flags & BCM2712_REBOOT_FLAG_TRYBOOT));
    }
    qemu_guest_getrandom_nofail(&kaslr_seed, sizeof(kaslr_seed));
    fdt_setprop_inplace_u64(fdt, fdt_path_offset(fdt, "/chosen"),
                            "kaslr-seed", kaslr_seed);
}

static bool raspi5b_pwr_button_needed(void *opaque)
{
    Raspi5bMachineState *s = opaque;

    return timer_pending(s->pwr_button_release);
}

/* A press in progress: when the button comes back up */
static const VMStateDescription vmstate_raspi5b_pwr_button = {
    .name = "raspi5b/pwr-button",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = raspi5b_pwr_button_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER_PTR(pwr_button_release, Raspi5bMachineState),
        VMSTATE_END_OF_LIST()
    },
};

/* Without the subsection, no press is in progress */
static int raspi5b_pre_load(void *opaque)
{
    Raspi5bMachineState *s = opaque;

    timer_del(s->pwr_button_release);
    return 0;
}

/*
 * The boot count outlives resets, so it moves with the machine, and so
 * does a press of the power button; the level of its line moves with
 * GIO
 */
static const VMStateDescription vmstate_raspi5b = {
    .name = "raspi5b",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_load = raspi5b_pre_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(boot_count, Raspi5bMachineState),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_raspi5b_pwr_button,
        NULL
    },
};

static void raspi5b_cpu_reset(void *opaque)
{
    cpu_reset(CPU(opaque));
}

/*
 * Load a 64-bit kernel where the firmware does: an ELF at its own
 * addresses, anything else, such as a Linux Image (gzipped or not), at
 * RASPI5B_KERNEL_ADDR plus the Image's text_offset. Returns the entry
 * point, and in *start and *end the memory the kernel uses, which for an
 * Image includes the BSS its header declares. An Image whose header takes
 * it past the VideoCore's memory is refused, and so is an ELF whose entry
 * point, where the armstub jumps, lies outside every segment it loads.
 */
static hwaddr raspi5b_load_kernel(const char *filename, AddressSpace *as,
                                  hwaddr *start, hwaddr *end)
{
    g_autofree uint8_t *buffer = NULL;
    uint64_t entry, low, high, used;
    hwaddr addr = RASPI5B_KERNEL_ADDR;
    ssize_t size;

    size = load_elf_as(filename, NULL, NULL, NULL, &entry, &low, &high, NULL,
                       ELFDATA2LSB, EM_AARCH64, 1, 0, as);
    if (size > 0) {
        /*
         * The segments may leave gaps between low and high; they are the
         * only ROMs yet, so a ROM at the entry point is one of them
         */
        if (!rom_ptr_for_as(as, entry, 4)) {
            error_report("could not load kernel '%s': its entry point "
                         "0x%" PRIx64 " lies outside every segment it loads",
                         filename, entry);
            exit(EXIT_FAILURE);
        }
        *start = low;
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
    if (size >= RASPI5B_IMAGE_MAGIC + 4 &&
        !memcmp(buffer + RASPI5B_IMAGE_MAGIC, "ARM\x64", 4)) {
        uint64_t text_offset = ldq_le_p(buffer + RASPI5B_IMAGE_TEXT_OFFSET);
        uint64_t image_size = ldq_le_p(buffer + RASPI5B_IMAGE_SIZE);

        if (!image_size) {
            /* Linux before 3.17, whatever byte order the field is in */
            text_offset = RASPI5B_IMAGE_OLD_TEXT_OFFSET;
        }
        used = MAX(image_size, size);
        /* Subtracting, as a header's values could wrap a sum around */
        if (text_offset > BCM2712_VC_RAM_BASE - addr ||
            used > BCM2712_VC_RAM_BASE - addr - text_offset) {
            error_report("could not load kernel '%s': its text_offset "
                         "0x%" PRIx64 " and size 0x%" PRIx64 " take it past "
                         "0x%x, where the VideoCore's memory starts",
                         filename, text_offset, used, BCM2712_VC_RAM_BASE);
            exit(EXIT_FAILURE);
        }
        addr += text_offset;
    }
    rom_add_blob_fixed_as(filename, buffer, size, addr, as);
    *start = addr;
    *end = addr + used;
    return addr;
}

/*
 * -bios lays out what it loads as the firmware does, but the armstub's
 * size, a kernel's addresses and dtb-address (device_tree_address=) can
 * still make two of them meet: refuse that, rather than load one over the
 * other or let a kernel clear the device tree with its BSS. @a lies from
 * @a_start to @a_end, @b from @b_start to @b_end.
 */
static void raspi5b_check_overlap(const char *a, hwaddr a_start, hwaddr a_end,
                                  const char *b, hwaddr b_start, hwaddr b_end)
{
    if (a_end > a_start && b_end > b_start &&
        ranges_overlap(a_start, a_end - a_start, b_start, b_end - b_start)) {
        error_report("the %s at 0x%" HWADDR_PRIx "-0x%" HWADDR_PRIx
                     " overlaps the %s at 0x%" HWADDR_PRIx "-0x%" HWADDR_PRIx,
                     a, a_start, a_end - 1, b, b_start, b_end - 1);
        exit(EXIT_FAILURE);
    }
}

/*
 * Everything -bios loads stays below the VideoCore's memory, as the
 * firmware keeps it; this also keeps the addresses written into the
 * armstub in 32 bits. @end is where the last of it ends.
 */
static void raspi5b_check_fits(hwaddr end)
{
    if (end > BCM2712_VC_RAM_BASE) {
        error_report("the armstub, kernel, initrd and device tree must fit "
                     "below 0x%x, where the VideoCore's memory starts",
                     BCM2712_VC_RAM_BASE);
        exit(EXIT_FAILURE);
    }
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
    hwaddr kernel_start = 0, kernel_end = 0, dtb = 0, stub_end;
    gsize size;

    if (!filename ||
        !g_file_get_contents(filename, (char **)&stub, &size, NULL)) {
        error_report("could not load the armstub '%s'", machine->firmware);
        exit(EXIT_FAILURE);
    }
    stub_end = BCM2712_RAM_BASE + size;
    raspi5b_check_fits(stub_end);
    s->armstub_size = size;

    if (machine->kernel_filename) {
        kernel = raspi5b_load_kernel(machine->kernel_filename, as,
                                     &kernel_start, &kernel_end);
        raspi5b_check_fits(kernel_end);
        raspi5b_check_overlap("kernel", kernel_start, kernel_end,
                              "armstub", BCM2712_RAM_BASE, stub_end);
        next = MAX(next, kernel_end);
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
        raspi5b_check_overlap("initrd", s->binfo.initrd_start, next,
                              "armstub", BCM2712_RAM_BASE, stub_end);
    }
    if (s->binfo.dtb_filename || s->binfo.get_dtb) {
        int dtb_size;

        /* As for -kernel: the kernel maps the tree's 2 MiB block early */
        dtb = s->dtb_addr_set ? s->dtb_addr : QEMU_ALIGN_UP(next, 2 * MiB);
        s->binfo.dtb_start = dtb;
        dtb_size = arm_load_dtb(dtb, &s->binfo, 0, as, machine, cpu);
        if (dtb_size < 0) {
            exit(EXIT_FAILURE);
        }
        raspi5b_check_overlap("device tree", dtb, dtb + dtb_size,
                              "armstub", BCM2712_RAM_BASE, stub_end);
        raspi5b_check_overlap("device tree", dtb, dtb + dtb_size,
                              "kernel", kernel_start, kernel_end);
        raspi5b_check_overlap("device tree", dtb, dtb + dtb_size,
                              "initrd", s->binfo.initrd_start,
                              s->binfo.initrd_start + s->binfo.initrd_size);
        next = MAX(next, dtb + dtb_size);
    }
    raspi5b_check_fits(next);

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

static void raspi5b_pwr_button_release(void *opaque)
{
    Raspi5bMachineState *s = opaque;

    qemu_set_irq(s->pwr_button, 1);
}

/*
 * A reset leaves a press in progress alone, as it would a finger on the
 * button: the line stays low through it and comes back up on time
 */
static void raspi5b_powerdown_req(Notifier *n, void *opaque)
{
    Raspi5bMachineState *s = container_of(n, Raspi5bMachineState, powerdown);

    qemu_set_irq(s->pwr_button, 0);
    timer_mod(s->pwr_button_release,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              RASPI5B_PWR_BUTTON_PRESS_MS * SCALE_MS);
}

/*
 * Connect the power button, the activity LED and the SD card slot's card
 * detect switch to their GPIO lines, and hold the first two at the level
 * of their pull-ups: the button released, the LED dark. SDIO1 drives the
 * switch's line from its reset on, low while a card is in the slot.
 */
static void raspi5b_wire_gpio(Raspi5bMachineState *s)
{
    DeviceState *gio = DEVICE(&s->soc.gio);
    DeviceState *gio_aon = DEVICE(&s->soc.gio_aon);
    LEDState *act_led;

    s->pwr_button = qdev_get_gpio_in(gio, RASPI5B_GIO_PWR_BUTTON);
    qemu_set_irq(s->pwr_button, 1);
    s->pwr_button_release = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                         raspi5b_pwr_button_release, s);
    s->powerdown.notify = raspi5b_powerdown_req;
    qemu_register_powerdown_notifier(&s->powerdown);

    act_led = led_create_simple(OBJECT(s), GPIO_POLARITY_ACTIVE_LOW,
                                LED_COLOR_GREEN, "ACT");
    qdev_connect_gpio_out(gio_aon, RASPI5B_AON_GPIO_ACT_LED,
                          qdev_get_gpio_in(DEVICE(act_led), 0));
    qemu_set_irq(qdev_get_gpio_in(gio_aon, RASPI5B_AON_GPIO_ACT_LED), 1);

    qdev_connect_gpio_out_named(DEVICE(&s->soc.sdio[0]), "card-inserted", 0,
        qemu_irq_invert(qdev_get_gpio_in(gio_aon,
                                          RASPI5B_AON_GPIO_SD_CDET_N)));
}

/*
 * The card in the SD card slot, from -drive if=sd. Without one, QEMU's
 * default drive stands in, a slot without a card until the monitor's
 * "change sd0" inserts one; with -nodefaults, the slot stays empty.
 */
static void raspi5b_sd_card(Raspi5bMachineState *s)
{
    DriveInfo *di = drive_get(IF_SD, 0, 0);
    BusState *bus = qdev_get_child_bus(DEVICE(&s->soc), "sd-bus");
    DeviceState *card;

    if (!di) {
        return;
    }
    card = qdev_new(TYPE_SD_CARD);
    /* For the monitor: the card is /machine/sd-card */
    object_property_add_child(OBJECT(s), "sd-card", OBJECT(card));
    qdev_prop_set_drive_err(card, "drive", blk_by_legacy_dinfo(di),
                            &error_fatal);
    qdev_realize_and_unref(card, bus, &error_fatal);
}

/*
 * What a reset leaves for the boot it starts (the reset status, the
 * firmware's reboot flags and the boot count) may be given for the first
 * boot, as the properties of those names read it from a machine whose
 * guest has asked for a reset that QEMU has not made yet: the first boot
 * is then the one that reset would have started, for a host that runs
 * QEMU afresh for each boot.
 */
static void raspi5b_carry_reset(Raspi5bMachineState *s)
{
    if (s->reset_status_set) {
        s->soc.pm.rsts = s->reset_status;
    }
    s->soc.property.reboot_flags = s->reboot_flags;
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
    if (s->dtb_addr_set && !machine->firmware) {
        error_report("dtb-address places the device tree for -bios only");
        exit(EXIT_FAILURE);
    }
    if (s->dtb_addr_set &&
        (s->dtb_addr % 8 || s->dtb_addr >= BCM2712_VC_RAM_BASE)) {
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
    /* The command line tag answers with -append: cmdline.txt's part */
    qdev_prop_set_string(soc, "command-line", machine->kernel_cmdline);
    qdev_realize(soc, NULL, &error_fatal);
    raspi5b_carry_reset(s);
    raspi5b_wire_gpio(s);
    raspi5b_sd_card(s);
    /* A monitor on HDMI0 */
    i2c_slave_create_simple(s->soc.ddc[0].bus, TYPE_I2CDDC,
                            RASPI5B_DDC_EDID_ADDR);

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
    qemu_register_reset_nosnapshotload(raspi5b_bootloader, s);
    vmstate_register(NULL, 0, &vmstate_raspi5b, s);
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
    Raspi5bMachineState *s = RASPI5B_MACHINE(obj);

    if (visit_type_uint64(v, name, &s->dtb_addr, errp)) {
        s->dtb_addr_set = true;
    }
}

static bool raspi5b_get_builtin_dtb(Object *obj, Error **errp)
{
    return RASPI5B_MACHINE(obj)->builtin_dtb;
}

static void raspi5b_set_builtin_dtb(Object *obj, bool value, Error **errp)
{
    RASPI5B_MACHINE(obj)->builtin_dtb = value;
}

/*
 * The state a reset leaves is given for the first boot only, and reads as
 * it stands once the machine exists
 */
static bool raspi5b_settable(const char *name, Error **errp)
{
    if (phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp, "'%s' can only be set when the machine is created",
                   name);
        return false;
    }
    return true;
}

static void raspi5b_get_reset_status(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    Raspi5bMachineState *s = RASPI5B_MACHINE(obj);
    uint32_t value = phase_check(PHASE_MACHINE_INITIALIZED) ?
                     s->soc.pm.rsts : s->reset_status;

    visit_type_uint32(v, name, &value, errp);
}

static void raspi5b_set_reset_status(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    Raspi5bMachineState *s = RASPI5B_MACHINE(obj);

    if (raspi5b_settable(name, errp) &&
        visit_type_uint32(v, name, &s->reset_status, errp)) {
        s->reset_status_set = true;
    }
}

static void raspi5b_get_reboot_flags(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    Raspi5bMachineState *s = RASPI5B_MACHINE(obj);
    uint32_t value = phase_check(PHASE_MACHINE_INITIALIZED) ?
                     s->soc.property.reboot_flags : s->reboot_flags;

    visit_type_uint32(v, name, &value, errp);
}

static void raspi5b_set_reboot_flags(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    Raspi5bMachineState *s = RASPI5B_MACHINE(obj);

    if (raspi5b_settable(name, errp)) {
        visit_type_uint32(v, name, &s->reboot_flags, errp);
    }
}

static void raspi5b_get_boot_count(Object *obj, Visitor *v,
                                   const char *name, void *opaque,
                                   Error **errp)
{
    visit_type_uint8(v, name, &RASPI5B_MACHINE(obj)->boot_count, errp);
}

static void raspi5b_set_boot_count(Object *obj, Visitor *v,
                                   const char *name, void *opaque,
                                   Error **errp)
{
    Raspi5bMachineState *s = RASPI5B_MACHINE(obj);

    if (raspi5b_settable(name, errp)) {
        visit_type_uint8(v, name, &s->boot_count, errp);
    }
}

static void raspi5b_get_boot_partition(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    visit_type_uint32(v, name, &RASPI5B_MACHINE(obj)->boot_partition, errp);
}

static void raspi5b_set_boot_partition(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    Raspi5bMachineState *s = RASPI5B_MACHINE(obj);

    if (raspi5b_settable(name, errp) &&
        visit_type_uint32(v, name, &s->boot_partition, errp)) {
        s->boot_partition_set = true;
    }
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
    /* -drive goes in the SD card slot, which is empty by default */
    mc->block_default_type = IF_SD;
    mc->auto_create_sdcard = true;

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

    object_class_property_add(oc, "boot-partition", "uint32",
                              raspi5b_get_boot_partition,
                              raspi5b_set_boot_partition, NULL, NULL);
    object_class_property_set_description(oc, "boot-partition",
        "The partition of the SD card the boot files come from, which the "
        "firmware reports; by default, the one the reset status asks for");

    object_class_property_add(oc, "reset-status", "uint32",
                              raspi5b_get_reset_status,
                              raspi5b_set_reset_status, NULL, NULL);
    object_class_property_set_description(oc, "reset-status",
        "PM_RSTS for the first boot, with the partition the OS asked for "
        "(by default, a power-on's); a running machine reads it as it is");

    object_class_property_add(oc, "reboot-flags", "uint32",
                              raspi5b_get_reboot_flags,
                              raspi5b_set_reboot_flags, NULL, NULL);
    object_class_property_set_description(oc, "reboot-flags",
        "The firmware's reboot flags for the first boot (1: tryboot); a "
        "running machine reads those the OS has set for the next");

    object_class_property_add(oc, "boot-count", "uint8",
                              raspi5b_get_boot_count, raspi5b_set_boot_count,
                              NULL, NULL);
    object_class_property_set_description(oc, "boot-count",
        "The boots before the first one, as the firmware counts them in 8 "
        "bits; a running machine reads its boots so far");
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
