/*
 * Broadcom set-top-box (brcmstb) PCIe root complex, BCM7712 layout
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PCI_HOST_BRCMSTB_PCIE_H
#define HW_PCI_HOST_BRCMSTB_PCIE_H

#include "hw/core/sysbus.h"
#include "hw/intc/brcmstb_l2_intc.h"
#include "hw/pci/pci_host.h"
#include "hw/pci/pcie_port.h"
#include "qom/object.h"

#define TYPE_BRCMSTB_PCIE_HOST "brcmstb-pcie-host"
OBJECT_DECLARE_SIMPLE_TYPE(BrcmstbPCIeHostState, BRCMSTB_PCIE_HOST)

#define TYPE_BRCMSTB_PCIE_ROOT_PORT "brcmstb-pcie-root-port"
OBJECT_DECLARE_TYPE(BrcmstbPCIeRootPortState, BrcmstbPCIeRootPortClass,
                    BRCMSTB_PCIE_ROOT_PORT)

/* The register window: root port config space, controller, config window */
#define BRCMSTB_PCIE_REGS_SIZE      0x9310

#define BRCMSTB_PCIE_NUM_OUTBOUND   4
#define BRCMSTB_PCIE_NUM_INBOUND    10  /* RC_BAR1..RC_BAR10 */
#define BRCMSTB_PCIE_NUM_INTX       4

/* Sysbus IRQs: INTA..INTD, then the two interrupts of the device tree */
#define BRCMSTB_PCIE_IRQ_INTA       0
#define BRCMSTB_PCIE_IRQ_PCIE       4   /* INTR2_CPU, "pcie" */
#define BRCMSTB_PCIE_IRQ_MSI        5   /* MSI_INTR2, "msi" */

struct BrcmstbPCIeRootPortState {
    /*< private >*/
    PCIESlot parent_obj;

    bool l1ss;                  /* has the L1 PM Substates capability */
};

struct BrcmstbPCIeRootPortClass {
    /*< private >*/
    PCIERootPortClass parent_class;

    /*< public >*/
    void (*parent_realize)(PCIDevice *dev, Error **errp);
};

/*
 * A root complex with its root port at devfn 0 of its root bus, named
 * "pcie<domain>.root"; devices plug into the port's secondary bus, named
 * by the "bus-name" property.
 *
 * Sysbus MMIO 0 is the register window, 1 the outbound aperture: the
 * CPU addresses from "outbound-base" that the outbound windows map onto
 * PCI memory, where the SoC is to map it. The
 * device's DMA reaches "dma-memory" through the inbound windows.
 * The named GPIO input "bridge-reset" is the bridge's software-init reset
 * line, as the SoC's reset controller drives it.
 */
struct BrcmstbPCIeHostState {
    /*< private >*/
    PCIHostState parent_obj;

    /*< public >*/
    MemoryRegion regs;          /* the register window */
    MemoryRegion regs_io;       /* all of it but the L2 controllers */
    MemoryRegion outbound;      /* the outbound aperture */
    MemoryRegion outbound_err;  /* its background: no window decodes */
    MemoryRegion out_win[BRCMSTB_PCIE_NUM_OUTBOUND];
    MemoryRegion pci_mem;       /* PCI memory space */
    MemoryRegion pci_mem_err;   /* its background: no device claims */
    MemoryRegion pci_io;        /* PCI I/O space, not reachable */
    MemoryRegion dma_root;      /* PCI memory space as the RC decodes it */
    MemoryRegion in_win[BRCMSTB_PCIE_NUM_INBOUND];
    MemoryRegion msi_win;
    AddressSpace dma_as;

    BrcmstbL2IntcState intr2_cpu;
    BrcmstbL2IntcState msi_intr2;
    BrcmstbPCIeRootPortState root;
    qemu_irq irq[BRCMSTB_PCIE_IRQ_MSI + 1];

    /* Properties */
    uint32_t domain;            /* the PCI domain, for unique bus paths */
    char *bus_name;             /* of the root port's secondary bus */
    uint64_t outbound_base;
    uint64_t outbound_size;
    uint32_t num_lanes;
    uint32_t max_link_speed;    /* PCIe generation */
    bool aspm_l0s;
    bool l1ss;                  /* L1 PM Substates */
    uint32_t hw_revision;       /* MISC_REVISION */
    MemoryRegion *dma_mr;

    char root_bus_path[16];     /* "dddd:00" */
    uint32_t reg[BRCMSTB_PCIE_REGS_SIZE / 4];
    bool bridge_reset;          /* the "bridge-reset" input */
    bool perst;                 /* PERST# as last driven */
};

#endif /* HW_PCI_HOST_BRCMSTB_PCIE_H */
