/*
 * Broadcom BCM2712 SoC (Raspberry Pi 5)
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_BCM2712_H
#define HW_ARM_BCM2712_H

#include "exec/hwaddr.h"
#include "hw/char/pl011.h"
#include "hw/char/serial-mm.h"
#include "hw/display/bcm2835_fb.h"
#include "hw/gpio/brcmstb_gpio.h"
#include "hw/gpio/brcmstb_pinctrl.h"
#include "hw/i2c/brcmstb_i2c.h"
#include "hw/intc/arm_gic.h"
#include "hw/intc/brcmstb_l2_intc.h"
#include "hw/misc/bcm2711_rng200.h"
#include "hw/misc/bcm2835_mbox.h"
#include "hw/misc/bcm2835_powermgt.h"
#include "hw/misc/bcm2835_property.h"
#include "hw/nvram/bcm2835_otp.h"
#include "hw/sd/bcm2712_sdhci.h"
#include "hw/timer/bcm2835_systmr.h"
#include "qemu/units.h"
#include "qom/object.h"
#include "target/arm/cpu.h"

#define TYPE_BCM2712 "bcm2712"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2712State, BCM2712)

/* Quad Cortex-A76, one cluster, MPIDR.Aff1 = core number */
#define BCM2712_NUM_CPUS            4

/* ARM generic timer (CNTFRQ_EL0) runs from the 54 MHz crystal */
#define BCM2712_CNTFRQ_HZ           54000000

/*
 * GIC-400 shared peripheral interrupts. The highest SPI referenced by the
 * device tree is 276 (UARTA), so round up to the next multiple of 32.
 * TODO(WS1.3): confirm GICD_TYPER.ITLinesNumber against real silicon.
 */
#define BCM2712_NUM_SPIS            288

/* DRAM is contiguous from physical address 0 */
#define BCM2712_RAM_BASE            0x0
#define BCM2712_RAM_SIZE_MIN        (1 * GiB)
#define BCM2712_RAM_SIZE_MAX        (16 * GiB)

/*
 * The VideoCore reaches the first GiB of RAM only, at bus address 0x0
 * and at 0xc000_0000 (the alias older Pis use), and keeps the top of it
 * for itself; the firmware leaves that out of the ARM memory node.
 * TODO(WS0.4): check GET_VC_MEMORY on hardware.
 */
#define BCM2712_VC_RAM_WINDOW       (1 * GiB)
#define BCM2712_VC_RAM_BUS_BASE     0xc0000000
#define BCM2712_VC_RAM_SIZE         (4 * MiB)
#define BCM2712_VC_RAM_BASE         0x3fc00000     /* top of the window */

/*
 * The DMA channels the ARM may use (GET_DMA_CHANNELS): those of the
 * "dma32" (0-5) and "dma40" (6-10) nodes of the firmware's device tree.
 * TODO(WS0.4): check the firmware's answer on hardware.
 */
#define BCM2712_DMA_CHANNEL_MASK    0x07ff

/*
 * Physical memory map. Addresses are 40-bit CPU physical addresses; the
 * device tree describes most of them relative to the "soc" simple-bus,
 * which maps bus address 0x0 to CPU address 0x10_0000_0000.
 */
typedef enum BCM2712Device {
    /* Catch-all windows, mapped below every specific region */
    BCM2712_AXI,
    BCM2712_SOC,

    /* AXI peripherals */
    BCM2712_PCIE0,
    BCM2712_PCIE1,
    BCM2712_PCIE_RESCAL,
    BCM2712_PCIE2,
    BCM2712_MIP0,
    BCM2712_MIP1,
    BCM2712_ISP,
    BCM2712_SDIO1,
    BCM2712_SDIO2,
    BCM2712_RESET,
    BCM2712_V3D,

    /* "soc" peripherals */
    BCM2712_SYSTIMER,
    BCM2712_MBOX,
    BCM2712_PIXELVALVE0,
    BCM2712_PIXELVALVE1,
    BCM2712_MOP,
    BCM2712_MOPLET,
    BCM2712_DISP_INTR,
    BCM2712_HVS,
    BCM2712_HDMI,
    BCM2712_UART10,
    BCM2712_PM,
    BCM2712_RNG,
    BCM2712_CPU_L2_IRQ,
    BCM2712_PINCTRL,
    BCM2712_DDC0,
    BCM2712_DDC1,
    BCM2712_BSC_IRQ,
    BCM2712_MAIN_IRQ,
    BCM2712_GIO,
    BCM2712_UARTA,
    BCM2712_AON_INTR,
    BCM2712_PINCTRL_AON,
    BCM2712_L2_INTC,
    BCM2712_MAIN_AON_IRQ,
    BCM2712_GIO_AON,
    BCM2712_GIC,

    BCM2712_NUM_DEVICES
} BCM2712Device;

extern const MemMapEntry bcm2712_memmap[BCM2712_NUM_DEVICES];

/* GIC-400 SPI numbers, i.e. the N in "<GIC_SPI N ...>" in bcm2712.dtsi */
enum {
    BCM2712_SPI_PMU0            = 16,   /* 16..19: one per core */
    BCM2712_SPI_MBOX            = 33,
    BCM2712_SPI_SYSTIMER0       = 64,   /* 64..67: one per comparator */
    BCM2712_SPI_ISP             = 72,
    BCM2712_SPI_DISP_INTR       = 97,
    BCM2712_SPI_PIXELVALVE0     = 101,
    BCM2712_SPI_PIXELVALVE1     = 110,
    BCM2712_SPI_UART10          = 121,
    BCM2712_SPI_MIP0_BASE       = 128,  /* 128..191: MSIs from PCIe2 */
    BCM2712_SPI_PCIE0_INTA      = 209,  /* 209..212: INTA..INTD */
    BCM2712_SPI_PCIE0           = 213,
    BCM2712_SPI_PCIE0_MSI       = 214,
    BCM2712_SPI_PCIE1_INTA      = 219,
    BCM2712_SPI_PCIE1           = 223,
    BCM2712_SPI_PCIE1_MSI       = 224,
    BCM2712_SPI_PCIE2_INTA      = 229,
    BCM2712_SPI_PCIE2           = 233,
    BCM2712_SPI_PCIE2_MSI       = 234,
    BCM2712_SPI_CPU_L2_IRQ      = 238,
    BCM2712_SPI_AON_INTR        = 239,
    BCM2712_SPI_BSC             = 242,
    BCM2712_SPI_MAIN_IRQ        = 244,
    BCM2712_SPI_MAIN_AON_IRQ    = 245,
    BCM2712_SPI_L2_INTC         = 247,
    BCM2712_SPI_V3D_HUB         = 249,
    BCM2712_SPI_V3D_CORE0       = 250,
    BCM2712_SPI_MIP1_BASE       = 255,  /* 255..262: MSIs from PCIe1 */
    BCM2712_SPI_SDIO1           = 273,
    BCM2712_SPI_SDIO2           = 274,
    BCM2712_SPI_UARTA           = 276,
};

/*
 * The brcmstb level 2 interrupt controllers, each in front of one SPI,
 * named after their labels in the firmware's device tree; the comments
 * give the nodes that use them there.
 */
typedef enum BCM2712L2Intc {
    BCM2712_L2_DISP_INTR,       /* display: HVS, MOP, MOPLET */
    BCM2712_L2_CPU_L2_IRQ,      /* the firmware's KMS doorbell */
    BCM2712_L2_BSC_IRQ,         /* the HDMI DDC I2C controllers */
    BCM2712_L2_MAIN_IRQ,        /* GIO, the main GPIO block */
    BCM2712_L2_AON_INTR,        /* HDMI0 and HDMI1 */
    BCM2712_L2_7D517000,        /* unlabelled, unused */
    BCM2712_L2_MAIN_AON_IRQ,    /* unused */
    BCM2712_NUM_L2_INTCS
} BCM2712L2Intc;

/* Inputs of the level 2 controllers, i.e. the N in "interrupts = <N>" */
enum {
    BCM2712_BSC_IRQ_DDC0        = 1,    /* of BCM2712_L2_BSC_IRQ */
    BCM2712_BSC_IRQ_DDC1        = 2,    /* of BCM2712_L2_BSC_IRQ */
    BCM2712_MAIN_IRQ_GIO        = 0,    /* of BCM2712_L2_MAIN_IRQ */
};

/* The HDMI ports, each with the I2C bus that reads its monitor's EDID */
#define BCM2712_NUM_HDMI            2

/* UARTA's baud clock, sw_baud in bcm2712.dtsi, in Hz */
#define BCM2712_UARTA_CLK_HZ        96000000

/* The SD/eMMC host controllers: SDIO1 for the SD card, SDIO2 for Wi-Fi */
#define BCM2712_NUM_SDIO            2

struct BCM2712State {
    /*< private >*/
    DeviceState parent_obj;

    /*< public >*/
    uint32_t num_cpus;
    bool has_el3;
    MemoryRegion *ram;

    ARMCPU cpu[BCM2712_NUM_CPUS];
    GICState gic;
    BrcmstbL2IntcState l2_intc[BCM2712_NUM_L2_INTCS];
    BrcmstbGpioState gio;       /* line n: BCM2712 GPIO n */
    BrcmstbGpioState gio_aon;   /* line n: always-on GPIO n */
    BrcmstbPinctrlState pinctrl;
    BrcmstbPinctrlState pinctrl_aon;
    BrcmstbI2cState ddc[BCM2712_NUM_HDMI];  /* HDMI n's DDC bus */
    BCM2712SDHCIState sdio[BCM2712_NUM_SDIO];   /* SDIO1, SDIO2 */
    BCM2835SystemTimerState systimer;
    BCM2835PowerMgtState pm;
    BCM2711Rng200State rng;
    PL011State uart10;
    SerialMM uarta;

    /* The VideoCore firmware interface, behind the mailbox */
    BCM2835MboxState mbox;
    MemoryRegion mbox_regs;
    MemoryRegion mbox_chans;
    MemoryRegion vc_bus;        /* the VideoCore's view of memory */
    MemoryRegion vc_ram[2];     /* aliases of the first GiB of RAM in it */
    BCM2835PropertyState property;
    BCM2835FBState fb;
    BCM2835OTPState otp;
};

/*
 * Add the SoC's nodes to the device tree @fdt: CPUs, timer, PMU, fixed
 * clocks, the "soc" bus with every modelled device, the firmware
 * interface and the serial10 alias, and point the root at the GIC.
 */
void bcm2712_fdt_populate(BCM2712State *s, void *fdt);

/*
 * The path of the node at @dev's base address on the "soc" bus of a tree
 * that bcm2712_fdt_populate() wrote, or NULL if there is none; to be
 * freed. The nodes of the GPIO blocks and pin controllers have phandles.
 */
char *bcm2712_fdt_node_path(void *fdt, BCM2712Device dev);

#endif /* HW_ARM_BCM2712_H */
