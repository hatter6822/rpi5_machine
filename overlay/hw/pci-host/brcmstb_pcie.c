/*
 * Broadcom set-top-box (brcmstb) PCIe root complex, BCM7712 layout
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * No public datasheet: registers and semantics as Linux
 * drivers/pci/controller/pcie-brcmstb.c uses them with the register
 * offsets of the BCM7712 (pcie_offsets_bcm7712), which the BCM2712 of the
 * Raspberry Pi 5 shares, and as the Pi 5's device trees describe it.
 *
 * The first 4 KiB of the register window are the root port's own
 * configuration space, which the root bus shows at devfn 0. A few words
 * past its capabilities are private registers of the controller: the
 * class code (PRIV1_ID_VAL3) and the link capabilities
 * (PRIV1_LINK_CAPABILITY) that the configuration space shows, and ones
 * kept without effect. Other devices' configuration spaces are reached
 * through the 4 KiB EXT_CFG_DATA window, at the bus and devfn that
 * EXT_CFG_INDEX selects, while the link is up; the root port is the only
 * device on the root bus.
 *
 * The link is up while PERST# is released (MISC_PCIE_CTRL.PERSTB), the
 * bridge is out of reset (RGR1_SW_INIT_1.INIT or the reset controller's
 * line), the SerDes is powered (HARD_DEBUG.SERDES_IDDQ clear), no
 * L2/L3 entry is requested and a device is on the secondary bus. It
 * trains at once. Asserting PERST# resets the devices on the bus, and
 * asserting the bridge reset resets the root port.
 *
 * Up to four outbound windows map CPU addresses, in MiB units, onto PCI
 * memory; the CPU reaches them in the outbound aperture. Up to ten
 * inbound windows (RC_BAR1..10, each with its UBUS remap) map PCI
 * addresses onto the CPU's, and a write to the MSI target address
 * (MSI_BAR_CONFIG) whose data matches MSI_DATA_CONFIG raises one of 32
 * vectors in MSI_INTR2. INTR2_CPU is there, without inputs. Where
 * nothing answers a request, the UBUS reply-error controls choose
 * between a bus error and AXI_READ_ERROR_DATA.
 *
 * The remaining registers (the MDIO bus to the PHY, the PHY and link
 * controls, statistics, QoS maps, timeouts) keep what is written to
 * them. The MDIO bus completes each access at once and reads as zero.
 * Reset clears every register but those noted.
 * TODO(WS0.4): take the reset values, the PHY's MDIO registers and
 * MISC_REVISION from hardware.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/registerfields.h"
#include "hw/pci/msi.h"
#include "hw/pci/pci_bridge.h"
#include "hw/pci/pcie.h"
#include "hw/pci-host/brcmstb_pcie.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "trace.h"

/* The BCM2712's root port, C1 stepping (lspci: "BCM2712 PCIe Bridge") */
#define BRCMSTB_PCIE_VENDOR_ID      0x14e4      /* Broadcom */
#define BRCMSTB_PCIE_DEVICE_ID      0x2712
#define BRCMSTB_PCIE_REVISION       0x21

/* The root port's configuration space and its private registers */
#define BRCMSTB_PCIE_CFG_SIZE       0x1000
#define BRCMSTB_PCIE_EXP_OFFSET     0xac
#define BRCMSTB_PCIE_PM_OFFSET      0x48
#define BRCMSTB_PCIE_AER_OFFSET     0x100
#define BRCMSTB_PCIE_VC_OFFSET      0x160
#define BRCMSTB_PCIE_VC_SIZE        0x20
#define BRCMSTB_PCIE_VSEC_OFFSET    0x180
#define BRCMSTB_PCIE_VSEC_SIZE      0x28
#define BRCMSTB_PCIE_L1SS_OFFSET    0x240
#define BRCMSTB_PCIE_L1SS_SIZE      0x10
#define BRCMSTB_PCIE_SPCIE_OFFSET   0x300

REG32(RC_CFG_VENDOR_SPECIFIC_REG1,  0x0188)
REG32(RC_CFG_PRIV1_ID_VAL3,         0x043c)
    FIELD(RC_CFG_PRIV1_ID_VAL3, CLASS_CODE, 0, 24)
REG32(RC_CFG_PRIV1_LINK_CAPABILITY, 0x04dc)
REG32(RC_CFG_PRIV1_ROOT_CAP,        0x04f8)
REG32(RC_TL_VDM_CTL1,               0x0a0c)
REG32(RC_TL_VDM_CTL0,               0x0a20)

REG32(RC_DL_MDIO_ADDR,              0x1100)
REG32(RC_DL_MDIO_WR_DATA,           0x1104)
REG32(RC_DL_MDIO_RD_DATA,           0x1108)
    FIELD(RC_DL_MDIO_DATA, DONE, 31, 1)

REG32(MISC_MISC_CTRL,               0x4008)
    FIELD(MISC_MISC_CTRL, CFG_READ_UR_MODE, 13, 1)
REG32(MISC_CPU_2_PCIE_MEM_WIN0_LO,  0x400c)
REG32(MISC_CPU_2_PCIE_MEM_WIN0_HI,  0x4010)
REG32(MISC_RC_BAR1_CONFIG_LO,       0x402c)
    FIELD(MISC_RC_BAR_CONFIG_LO, SIZE, 0, 5)
REG32(MISC_MSI_BAR_CONFIG_LO,       0x4044)
    FIELD(MISC_MSI_BAR_CONFIG_LO, ENABLE, 0, 1)
REG32(MISC_MSI_BAR_CONFIG_HI,       0x4048)
REG32(MISC_MSI_DATA_CONFIG,         0x404c)
    FIELD(MISC_MSI_DATA_CONFIG, MATCH, 0, 16)
    FIELD(MISC_MSI_DATA_CONFIG, MASK, 16, 16)
REG32(MISC_PCIE_CTRL,               0x4064)
    FIELD(MISC_PCIE_CTRL, L23_REQUEST, 0, 1)
    FIELD(MISC_PCIE_CTRL, PERSTB, 2, 1)
REG32(MISC_PCIE_STATUS,             0x4068)
    FIELD(MISC_PCIE_STATUS, PHYLINKUP, 4, 1)
    FIELD(MISC_PCIE_STATUS, DL_ACTIVE, 5, 1)
    FIELD(MISC_PCIE_STATUS, LINK_IN_L23, 6, 1)
    FIELD(MISC_PCIE_STATUS, PORT, 7, 1)
REG32(MISC_REVISION,                0x406c)
REG32(MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT, 0x4070)
    FIELD(MISC_MEM_WIN_BASE_LIMIT, BASE, 4, 12)
    FIELD(MISC_MEM_WIN_BASE_LIMIT, LIMIT, 20, 12)
REG32(MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI, 0x4080)
REG32(MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI, 0x4084)
    FIELD(MISC_MEM_WIN_HI, MB, 0, 8)
REG32(MISC_UBUS_CTRL,               0x40a4)
    FIELD(MISC_UBUS_CTRL, REPLY_ERR_DIS, 13, 1)
    FIELD(MISC_UBUS_CTRL, REPLY_DECERR_DIS, 19, 1)
REG32(MISC_UBUS_BAR1_CONFIG_REMAP,  0x40ac)
    FIELD(MISC_UBUS_BAR_CONFIG_REMAP, ACCESS_EN, 0, 1)
REG32(MISC_RC_BAR4_CONFIG_LO,       0x40d4)
REG32(MISC_UBUS_BAR4_CONFIG_REMAP,  0x410c)
REG32(MISC_AXI_INTF_CTRL,           0x416c)
    FIELD(MISC_AXI_INTF_CTRL, EN_QOS_UPDATE_TIMING_FIX, 12, 1)
REG32(MISC_AXI_READ_ERROR_DATA,     0x4170)
REG32(MISC_HARD_PCIE_HARD_DEBUG,    0x4304)
    FIELD(MISC_HARD_DEBUG, PERST_ASSERT, 3, 1)
    FIELD(MISC_HARD_DEBUG, SERDES_IDDQ, 27, 1)
#define BRCMSTB_PCIE_INTR2_CPU      0x4400
#define BRCMSTB_PCIE_MSI_INTR2      0x4500
#define BRCMSTB_PCIE_EXT_CFG_DATA   0x8000
REG32(EXT_CFG_INDEX,                0x9000)
    FIELD(EXT_CFG_INDEX, DEVFN, 12, 8)
    FIELD(EXT_CFG_INDEX, BUS, 20, 8)
REG32(RGR1_SW_INIT_1,               0x9210)
    FIELD(RGR1_SW_INIT_1, INIT, 1, 1)

#define R(s, name)      ((s)->reg[R_ ## name])

/* The windows' registers, by window: outbound 0-3, inbound (RC_BAR) 1-10 */
static hwaddr brcmstb_pcie_out_lo(unsigned n)
{
    return A_MISC_CPU_2_PCIE_MEM_WIN0_LO + 8 * n;
}

static hwaddr brcmstb_pcie_out_base_limit(unsigned n)
{
    return A_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT + 4 * n;
}

static hwaddr brcmstb_pcie_out_base_hi(unsigned n)
{
    return A_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI + 8 * n;
}

static hwaddr brcmstb_pcie_in_bar(unsigned bar)
{
    return bar <= 3 ? A_MISC_RC_BAR1_CONFIG_LO + 8 * (bar - 1)
                    : A_MISC_RC_BAR4_CONFIG_LO + 8 * (bar - 4);
}

static hwaddr brcmstb_pcie_in_remap(unsigned bar)
{
    return bar <= 3 ? A_MISC_UBUS_BAR1_CONFIG_REMAP + 8 * (bar - 1)
                    : A_MISC_UBUS_BAR4_CONFIG_REMAP + 8 * (bar - 4);
}

static uint32_t brcmstb_pcie_reg(BrcmstbPCIeHostState *s, hwaddr offset)
{
    return s->reg[offset / 4];
}

static uint64_t brcmstb_pcie_reg64(BrcmstbPCIeHostState *s, hwaddr offset)
{
    return deposit64(s->reg[offset / 4], 32, 32, s->reg[offset / 4 + 1]);
}

static PCIDevice *brcmstb_pcie_root(BrcmstbPCIeHostState *s)
{
    return PCI_DEVICE(&s->root);
}

static PCIBus *brcmstb_pcie_sec_bus(BrcmstbPCIeHostState *s)
{
    return pci_bridge_get_sec_bus(PCI_BRIDGE(&s->root));
}

/* PERST#, as the controller drives it to the slot */
static bool brcmstb_pcie_perst(BrcmstbPCIeHostState *s)
{
    return !FIELD_EX32(R(s, MISC_PCIE_CTRL), MISC_PCIE_CTRL, PERSTB) ||
           FIELD_EX32(R(s, MISC_HARD_PCIE_HARD_DEBUG), MISC_HARD_DEBUG,
                      PERST_ASSERT);
}

static bool brcmstb_pcie_bridge_in_reset(BrcmstbPCIeHostState *s)
{
    return s->bridge_reset ||
           FIELD_EX32(R(s, RGR1_SW_INIT_1), RGR1_SW_INIT_1, INIT);
}

/* The link trains, with a device at the other end */
static bool brcmstb_pcie_link_trained(BrcmstbPCIeHostState *s)
{
    return !brcmstb_pcie_perst(s) && !brcmstb_pcie_bridge_in_reset(s) &&
           !FIELD_EX32(R(s, MISC_HARD_PCIE_HARD_DEBUG), MISC_HARD_DEBUG,
                       SERDES_IDDQ) &&
           !QTAILQ_EMPTY(&BUS(brcmstb_pcie_sec_bus(s))->children);
}

static bool brcmstb_pcie_link_in_l23(BrcmstbPCIeHostState *s)
{
    return brcmstb_pcie_link_trained(s) &&
           FIELD_EX32(R(s, MISC_PCIE_CTRL), MISC_PCIE_CTRL, L23_REQUEST);
}

static bool brcmstb_pcie_link_up(BrcmstbPCIeHostState *s)
{
    return brcmstb_pcie_link_trained(s) && !brcmstb_pcie_link_in_l23(s);
}

static uint32_t brcmstb_pcie_status(BrcmstbPCIeHostState *s)
{
    bool up = brcmstb_pcie_link_up(s);
    uint32_t status = R_MISC_PCIE_STATUS_PORT_MASK;

    status = FIELD_DP32(status, MISC_PCIE_STATUS, PHYLINKUP, up);
    status = FIELD_DP32(status, MISC_PCIE_STATUS, DL_ACTIVE, up);
    return FIELD_DP32(status, MISC_PCIE_STATUS, LINK_IN_L23,
                      brcmstb_pcie_link_in_l23(s));
}

/*
 * A request that no device or window answers: the controller replies
 * with an error, which the UBUS reply-error control @dis_mask of
 * MISC_UBUS_CTRL turns into AXI_READ_ERROR_DATA for reads and nothing
 * for writes.
 */
static MemTxResult brcmstb_pcie_no_reply(BrcmstbPCIeHostState *s,
                                         uint32_t dis_mask, uint64_t *data)
{
    if (!(R(s, MISC_UBUS_CTRL) & dis_mask)) {
        return MEMTX_ERROR;
    }
    if (data) {
        *data = R(s, MISC_AXI_READ_ERROR_DATA);
    }
    return MEMTX_OK;
}

/* A PCI device, but not the root port, in @s's hierarchy, by bus number */
static PCIDevice *brcmstb_pcie_find_device(BrcmstbPCIeHostState *s,
                                           uint32_t index)
{
    PCIBus *root_bus = PCI_HOST_BRIDGE(s)->bus;
    unsigned bus = FIELD_EX32(index, EXT_CFG_INDEX, BUS);

    if (bus == pci_bus_num(root_bus)) {
        return NULL;
    }
    return pci_find_device(root_bus, bus, FIELD_EX32(index, EXT_CFG_INDEX,
                                                     DEVFN));
}

static void brcmstb_pcie_update_outbound(BrcmstbPCIeHostState *s)
{
    uint64_t aperture = memory_region_size(&s->outbound);
    unsigned n;

    memory_region_transaction_begin();
    for (n = 0; n < BRCMSTB_PCIE_NUM_OUTBOUND; n++) {
        MemoryRegion *mr = &s->out_win[n];
        uint32_t bl = brcmstb_pcie_reg(s, brcmstb_pcie_out_base_limit(n));
        uint64_t base_mb, limit_mb, base, size;

        base_mb = deposit64(FIELD_EX32(bl, MISC_MEM_WIN_BASE_LIMIT, BASE),
                            12, 8,
                            FIELD_EX32(brcmstb_pcie_reg(
                                           s, brcmstb_pcie_out_base_hi(n)),
                                       MISC_MEM_WIN_HI, MB));
        limit_mb = deposit64(FIELD_EX32(bl, MISC_MEM_WIN_BASE_LIMIT, LIMIT),
                             12, 8,
                             FIELD_EX32(brcmstb_pcie_reg(
                                            s,
                                            brcmstb_pcie_out_base_hi(n) + 4),
                                        MISC_MEM_WIN_HI, MB));
        base = base_mb * MiB;
        size = (limit_mb + 1) * MiB - base;

        if (memory_region_is_mapped(mr)) {
            memory_region_del_subregion(&s->outbound, mr);
        }
        /* The CPU reaches the windows within the aperture only */
        if (limit_mb < base_mb || base < s->outbound_base ||
            base + size > s->outbound_base + aperture) {
            continue;
        }
        base -= s->outbound_base;
        memory_region_set_size(mr, size);
        memory_region_set_alias_offset(
            mr, brcmstb_pcie_reg64(s, brcmstb_pcie_out_lo(n)));
        memory_region_add_subregion(&s->outbound, base, mr);
        trace_brcmstb_pcie_outbound(s->domain, n, base,
                                    brcmstb_pcie_reg64(
                                        s, brcmstb_pcie_out_lo(n)), size);
    }
    memory_region_transaction_commit();
}

/* The size an RC_BAR size code gives, or 0 if it disables the window */
static uint64_t brcmstb_pcie_in_size(uint32_t code)
{
    if (code >= 1 && code <= 21) {
        return 1ULL << (code + 15);
    }
    if (code >= 0x1c) {
        return 4 * KiB << (code - 0x1c);
    }
    return 0;
}

static void brcmstb_pcie_update_inbound(BrcmstbPCIeHostState *s)
{
    unsigned bar;

    memory_region_transaction_begin();
    for (bar = 1; bar <= BRCMSTB_PCIE_NUM_INBOUND; bar++) {
        MemoryRegion *mr = &s->in_win[bar - 1];
        uint64_t cfg = brcmstb_pcie_reg64(s, brcmstb_pcie_in_bar(bar));
        uint64_t remap = brcmstb_pcie_reg64(s, brcmstb_pcie_in_remap(bar));
        uint64_t size = brcmstb_pcie_in_size(
            FIELD_EX32(cfg, MISC_RC_BAR_CONFIG_LO, SIZE));
        uint64_t pci, cpu;

        if (memory_region_is_mapped(mr)) {
            memory_region_del_subregion(&s->dma_root, mr);
        }
        if (!size ||
            !FIELD_EX32(remap, MISC_UBUS_BAR_CONFIG_REMAP, ACCESS_EN)) {
            continue;
        }
        pci = cfg & ~(size - 1);
        cpu = remap & ~(uint64_t)(4 * KiB - 1);
        memory_region_set_size(mr, size);
        memory_region_set_alias_offset(mr, cpu);
        memory_region_add_subregion(&s->dma_root, pci, mr);
        trace_brcmstb_pcie_inbound(s->domain, bar, pci, cpu, size);
    }
    memory_region_transaction_commit();
}

static void brcmstb_pcie_update_msi(BrcmstbPCIeHostState *s)
{
    uint32_t lo = R(s, MISC_MSI_BAR_CONFIG_LO);

    memory_region_transaction_begin();
    if (memory_region_is_mapped(&s->msi_win)) {
        memory_region_del_subregion(&s->dma_root, &s->msi_win);
    }
    if (FIELD_EX32(lo, MISC_MSI_BAR_CONFIG_LO, ENABLE)) {
        memory_region_add_subregion_overlap(
            &s->dma_root,
            deposit64(lo & ~3u, 32, 32, R(s, MISC_MSI_BAR_CONFIG_HI)),
            &s->msi_win, 1);
    }
    memory_region_transaction_commit();
}

/*
 * PERST# follows the registers that drive it: asserting it resets the
 * devices on the secondary bus.
 */
static void brcmstb_pcie_update_perst(BrcmstbPCIeHostState *s)
{
    bool perst = brcmstb_pcie_perst(s);

    if (perst != s->perst) {
        s->perst = perst;
        trace_brcmstb_pcie_perst(s->domain, perst);
        if (perst) {
            bus_cold_reset(BUS(brcmstb_pcie_sec_bus(s)));
        }
    }
}

/*
 * The bridge reset resets the root port as it is asserted, its command
 * register and BARs as well as its bridge registers and capabilities
 */
static void brcmstb_pcie_set_bridge_reset(BrcmstbPCIeHostState *s,
                                          bool was_in_reset)
{
    if (!was_in_reset && brcmstb_pcie_bridge_in_reset(s)) {
        trace_brcmstb_pcie_bridge_reset(s->domain);
        pci_device_reset(brcmstb_pcie_root(s));
    }
}

static void brcmstb_pcie_bridge_reset_in(void *opaque, int n, int level)
{
    BrcmstbPCIeHostState *s = BRCMSTB_PCIE_HOST(opaque);
    bool was_in_reset = brcmstb_pcie_bridge_in_reset(s);

    s->bridge_reset = level;
    brcmstb_pcie_set_bridge_reset(s, was_in_reset);
}

/* The root port's private registers in its configuration space */
static bool brcmstb_pcie_is_private(hwaddr offset)
{
    switch (offset & ~3) {
    case A_RC_CFG_VENDOR_SPECIFIC_REG1:
    case A_RC_CFG_PRIV1_ID_VAL3:
    case A_RC_CFG_PRIV1_LINK_CAPABILITY:
    case A_RC_CFG_PRIV1_ROOT_CAP:
    case A_RC_TL_VDM_CTL1:
    case A_RC_TL_VDM_CTL0:
        return true;
    }
    return false;
}

static uint32_t brcmstb_pcie_private_read(BrcmstbPCIeHostState *s,
                                          hwaddr offset)
{
    PCIDevice *d = brcmstb_pcie_root(s);

    switch (offset) {
    case A_RC_CFG_PRIV1_ID_VAL3:
        return FIELD_DP32(s->reg[offset / 4], RC_CFG_PRIV1_ID_VAL3,
                          CLASS_CODE,
                          pci_get_long(d->config + PCI_CLASS_REVISION) >> 8);
    case A_RC_CFG_PRIV1_LINK_CAPABILITY:
        return pci_get_long(d->config + d->exp.exp_cap + PCI_EXP_LNKCAP);
    }
    return s->reg[offset / 4];
}

static void brcmstb_pcie_private_write(BrcmstbPCIeHostState *s,
                                       hwaddr offset, uint32_t value)
{
    PCIDevice *d = brcmstb_pcie_root(s);
    uint32_t lnkcap_mask = PCI_EXP_LNKCAP_SLS | PCI_EXP_LNKCAP_MLW |
                           PCI_EXP_LNKCAP_ASPMS;

    switch (offset) {
    case A_RC_CFG_PRIV1_ID_VAL3:
        pci_set_long(d->config + PCI_CLASS_REVISION,
                     deposit32(pci_get_long(d->config + PCI_CLASS_REVISION),
                               8, 24, FIELD_EX32(value, RC_CFG_PRIV1_ID_VAL3,
                                                 CLASS_CODE)));
        value &= ~R_RC_CFG_PRIV1_ID_VAL3_CLASS_CODE_MASK;
        break;
    case A_RC_CFG_PRIV1_LINK_CAPABILITY:
        pci_long_test_and_clear_mask(d->config + d->exp.exp_cap +
                                     PCI_EXP_LNKCAP, lnkcap_mask);
        pci_long_test_and_set_mask(d->config + d->exp.exp_cap +
                                   PCI_EXP_LNKCAP, value & lnkcap_mask);
        return;
    }
    s->reg[offset / 4] = value;
}

static MemTxResult brcmstb_pcie_cfg_read(BrcmstbPCIeHostState *s,
                                         hwaddr offset, uint64_t *data,
                                         unsigned size)
{
    PCIDevice *d = brcmstb_pcie_root(s);

    if (brcmstb_pcie_is_private(offset)) {
        if (size != 4) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %u-byte access to private "
                          "register 0x%04"HWADDR_PRIx"\n", __func__, size,
                          offset);
        }
        *data = extract32(brcmstb_pcie_private_read(s, offset & ~3),
                          (offset & 3) * 8, size * 8);
        return MEMTX_OK;
    }
    *data = pci_host_config_read_common(d, offset, pci_config_size(d), size);
    return MEMTX_OK;
}

static MemTxResult brcmstb_pcie_cfg_write(BrcmstbPCIeHostState *s,
                                          hwaddr offset, uint64_t data,
                                          unsigned size)
{
    PCIDevice *d = brcmstb_pcie_root(s);

    if (brcmstb_pcie_is_private(offset)) {
        if (size != 4) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %u-byte access to private "
                          "register 0x%04"HWADDR_PRIx" ignored\n", __func__,
                          size, offset);
            return MEMTX_OK;
        }
        brcmstb_pcie_private_write(s, offset, data);
        return MEMTX_OK;
    }
    pci_host_config_write_common(d, offset, pci_config_size(d), data, size);
    return MEMTX_OK;
}

/* EXT_CFG_DATA: the selected device's configuration space, link up */
static MemTxResult brcmstb_pcie_ext_cfg_read(BrcmstbPCIeHostState *s,
                                             hwaddr offset, uint64_t *data,
                                             unsigned size)
{
    uint32_t index = R(s, EXT_CFG_INDEX);
    PCIDevice *d;

    if (!brcmstb_pcie_link_up(s)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: configuration read with the "
                      "link down\n", __func__);
        return brcmstb_pcie_no_reply(s, R_MISC_UBUS_CTRL_REPLY_ERR_DIS_MASK,
                                     data);
    }
    d = brcmstb_pcie_find_device(s, index);
    if (!d) {
        /* Unsupported Request: all ones, in the mode Linux selects */
        if (FIELD_EX32(R(s, MISC_MISC_CTRL), MISC_MISC_CTRL,
                       CFG_READ_UR_MODE)) {
            *data = MAKE_64BIT_MASK(0, size * 8);
            return MEMTX_OK;
        }
        return brcmstb_pcie_no_reply(s, R_MISC_UBUS_CTRL_REPLY_ERR_DIS_MASK,
                                     data);
    }
    *data = pci_host_config_read_common(d, offset, pci_config_size(d), size);
    return MEMTX_OK;
}

static MemTxResult brcmstb_pcie_ext_cfg_write(BrcmstbPCIeHostState *s,
                                              hwaddr offset, uint64_t data,
                                              unsigned size)
{
    uint32_t index = R(s, EXT_CFG_INDEX);
    PCIDevice *d;

    if (!brcmstb_pcie_link_up(s)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: configuration write with the "
                      "link down\n", __func__);
        return brcmstb_pcie_no_reply(s, R_MISC_UBUS_CTRL_REPLY_ERR_DIS_MASK,
                                     NULL);
    }
    d = brcmstb_pcie_find_device(s, index);
    if (d) {
        pci_host_config_write_common(d, offset, pci_config_size(d), data,
                                     size);
    }
    return MEMTX_OK;
}

static uint32_t brcmstb_pcie_misc_read(BrcmstbPCIeHostState *s,
                                       hwaddr offset)
{
    switch (offset) {
    case A_RC_DL_MDIO_RD_DATA:
        return R_RC_DL_MDIO_DATA_DONE_MASK;
    case A_MISC_PCIE_STATUS:
        return brcmstb_pcie_status(s);
    case A_MISC_REVISION:
        return s->hw_revision;
    }
    return s->reg[offset / 4];
}

static bool brcmstb_pcie_in_window_regs(hwaddr offset)
{
    unsigned bar;

    for (bar = 1; bar <= BRCMSTB_PCIE_NUM_INBOUND; bar++) {
        if (offset - brcmstb_pcie_in_bar(bar) < 8 ||
            offset - brcmstb_pcie_in_remap(bar) < 8) {
            return true;
        }
    }
    return false;
}

static bool brcmstb_pcie_out_window_regs(hwaddr offset)
{
    return (offset >= A_MISC_CPU_2_PCIE_MEM_WIN0_LO &&
            offset < A_MISC_RC_BAR1_CONFIG_LO) ||
           (offset >= A_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT &&
            offset < brcmstb_pcie_out_base_hi(BRCMSTB_PCIE_NUM_OUTBOUND));
}

static void brcmstb_pcie_misc_write(BrcmstbPCIeHostState *s, hwaddr offset,
                                    uint32_t value)
{
    bool was_in_reset = brcmstb_pcie_bridge_in_reset(s);

    switch (offset) {
    case A_RC_DL_MDIO_WR_DATA:
        /* The write completes at once */
        value &= ~R_RC_DL_MDIO_DATA_DONE_MASK;
        break;
    case A_MISC_PCIE_STATUS:
    case A_MISC_REVISION:
    case A_RC_DL_MDIO_RD_DATA:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: register 0x%04"HWADDR_PRIx
                      " is read-only\n", __func__, offset);
        return;
    case A_MISC_AXI_INTF_CTRL:
        /* Reserved on the C1 stepping, where Linux looks for it clear */
        value &= ~R_MISC_AXI_INTF_CTRL_EN_QOS_UPDATE_TIMING_FIX_MASK;
        break;
    }
    s->reg[offset / 4] = value;

    switch (offset) {
    case A_MISC_PCIE_CTRL:
    case A_MISC_HARD_PCIE_HARD_DEBUG:
        brcmstb_pcie_update_perst(s);
        break;
    case A_RGR1_SW_INIT_1:
        brcmstb_pcie_set_bridge_reset(s, was_in_reset);
        break;
    case A_MISC_MSI_BAR_CONFIG_LO:
    case A_MISC_MSI_BAR_CONFIG_HI:
        brcmstb_pcie_update_msi(s);
        break;
    default:
        if (brcmstb_pcie_out_window_regs(offset)) {
            brcmstb_pcie_update_outbound(s);
        } else if (brcmstb_pcie_in_window_regs(offset)) {
            brcmstb_pcie_update_inbound(s);
        }
        break;
    }
}

static MemTxResult brcmstb_pcie_regs_read(void *opaque, hwaddr offset,
                                          uint64_t *data, unsigned size,
                                          MemTxAttrs attrs)
{
    BrcmstbPCIeHostState *s = BRCMSTB_PCIE_HOST(opaque);
    MemTxResult res = MEMTX_OK;

    *data = 0;
    if (offset < BRCMSTB_PCIE_CFG_SIZE) {
        res = brcmstb_pcie_cfg_read(s, offset, data, size);
    } else if (offset >= BRCMSTB_PCIE_EXT_CFG_DATA &&
               offset < BRCMSTB_PCIE_EXT_CFG_DATA + BRCMSTB_PCIE_CFG_SIZE) {
        res = brcmstb_pcie_ext_cfg_read(s,
                                        offset - BRCMSTB_PCIE_EXT_CFG_DATA,
                                        data, size);
    } else if (size != 4 || (offset & 3)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %u-byte read at 0x%04"
                      HWADDR_PRIx"\n", __func__, size, offset);
    } else {
        *data = brcmstb_pcie_misc_read(s, offset);
    }

    trace_brcmstb_pcie_read(s->domain, offset, size, *data);
    return res;
}

static MemTxResult brcmstb_pcie_regs_write(void *opaque, hwaddr offset,
                                           uint64_t data, unsigned size,
                                           MemTxAttrs attrs)
{
    BrcmstbPCIeHostState *s = BRCMSTB_PCIE_HOST(opaque);

    trace_brcmstb_pcie_write(s->domain, offset, size, data);

    if (offset < BRCMSTB_PCIE_CFG_SIZE) {
        return brcmstb_pcie_cfg_write(s, offset, data, size);
    }
    if (offset >= BRCMSTB_PCIE_EXT_CFG_DATA &&
        offset < BRCMSTB_PCIE_EXT_CFG_DATA + BRCMSTB_PCIE_CFG_SIZE) {
        return brcmstb_pcie_ext_cfg_write(s,
                                          offset - BRCMSTB_PCIE_EXT_CFG_DATA,
                                          data, size);
    }
    if (size != 4 || (offset & 3)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %u-byte write at 0x%04"
                      HWADDR_PRIx" ignored\n", __func__, size, offset);
        return MEMTX_OK;
    }
    brcmstb_pcie_misc_write(s, offset, data);
    return MEMTX_OK;
}

static const MemoryRegionOps brcmstb_pcie_regs_ops = {
    .read_with_attrs = brcmstb_pcie_regs_read,
    .write_with_attrs = brcmstb_pcie_regs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* PCI memory that no device claims */
static MemTxResult brcmstb_pcie_unclaimed_read(void *opaque, hwaddr offset,
                                               uint64_t *data, unsigned size,
                                               MemTxAttrs attrs)
{
    BrcmstbPCIeHostState *s = BRCMSTB_PCIE_HOST(opaque);

    return brcmstb_pcie_no_reply(s, R_MISC_UBUS_CTRL_REPLY_ERR_DIS_MASK,
                                 data);
}

static MemTxResult brcmstb_pcie_unclaimed_write(void *opaque, hwaddr offset,
                                                uint64_t data, unsigned size,
                                                MemTxAttrs attrs)
{
    BrcmstbPCIeHostState *s = BRCMSTB_PCIE_HOST(opaque);

    return brcmstb_pcie_no_reply(s, R_MISC_UBUS_CTRL_REPLY_ERR_DIS_MASK,
                                 NULL);
}

static const MemoryRegionOps brcmstb_pcie_unclaimed_ops = {
    .read_with_attrs = brcmstb_pcie_unclaimed_read,
    .write_with_attrs = brcmstb_pcie_unclaimed_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

/* Aperture addresses that no outbound window decodes */
static MemTxResult brcmstb_pcie_undecoded_read(void *opaque, hwaddr offset,
                                               uint64_t *data, unsigned size,
                                               MemTxAttrs attrs)
{
    BrcmstbPCIeHostState *s = BRCMSTB_PCIE_HOST(opaque);

    return brcmstb_pcie_no_reply(s, R_MISC_UBUS_CTRL_REPLY_DECERR_DIS_MASK,
                                 data);
}

static MemTxResult brcmstb_pcie_undecoded_write(void *opaque, hwaddr offset,
                                                uint64_t data, unsigned size,
                                                MemTxAttrs attrs)
{
    BrcmstbPCIeHostState *s = BRCMSTB_PCIE_HOST(opaque);

    return brcmstb_pcie_no_reply(s, R_MISC_UBUS_CTRL_REPLY_DECERR_DIS_MASK,
                                 NULL);
}

static const MemoryRegionOps brcmstb_pcie_undecoded_ops = {
    .read_with_attrs = brcmstb_pcie_undecoded_read,
    .write_with_attrs = brcmstb_pcie_undecoded_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

/* A write to the MSI target: one of MSI_INTR2's 32 vectors, by its data */
static MemTxResult brcmstb_pcie_msi_write(void *opaque, hwaddr offset,
                                          uint64_t data, unsigned size,
                                          MemTxAttrs attrs)
{
    BrcmstbPCIeHostState *s = BRCMSTB_PCIE_HOST(opaque);
    uint32_t cfg = R(s, MISC_MSI_DATA_CONFIG);
    uint32_t mask = FIELD_EX32(cfg, MISC_MSI_DATA_CONFIG, MASK);
    uint32_t match = FIELD_EX32(cfg, MISC_MSI_DATA_CONFIG, MATCH);
    uint32_t vector = data & ~mask & 0xffff;

    if (((data ^ match) & mask & 0xffff) || vector >= 32) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: MSI data 0x%04"PRIx64" does not"
                      " match MSI_DATA_CONFIG 0x%08"PRIx32"\n", __func__,
                      data, cfg);
        return MEMTX_OK;
    }
    trace_brcmstb_pcie_msi(s->domain, vector);
    qemu_irq_pulse(qdev_get_gpio_in(DEVICE(&s->msi_intr2), vector));
    return MEMTX_OK;
}

static MemTxResult brcmstb_pcie_msi_read(void *opaque, hwaddr offset,
                                         uint64_t *data, unsigned size,
                                         MemTxAttrs attrs)
{
    *data = 0;
    return MEMTX_OK;
}

static const MemoryRegionOps brcmstb_pcie_msi_ops = {
    .read_with_attrs = brcmstb_pcie_msi_read,
    .write_with_attrs = brcmstb_pcie_msi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 2,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void brcmstb_pcie_set_irq(void *opaque, int irq_num, int level)
{
    BrcmstbPCIeHostState *s = BRCMSTB_PCIE_HOST(opaque);

    qemu_set_irq(s->irq[BRCMSTB_PCIE_IRQ_INTA + irq_num], level);
}

static int brcmstb_pcie_map_irq(PCIDevice *pci_dev, int pin)
{
    return pin;
}

static void brcmstb_pcie_intr2_out(void *opaque, int n, int level)
{
    BrcmstbPCIeHostState *s = BRCMSTB_PCIE_HOST(opaque);

    qemu_set_irq(s->irq[BRCMSTB_PCIE_IRQ_PCIE + n], level);
}

static AddressSpace *brcmstb_pcie_dma_as(PCIBus *bus, void *opaque,
                                         int devfn)
{
    return &BRCMSTB_PCIE_HOST(opaque)->dma_as;
}

static const PCIIOMMUOps brcmstb_pcie_iommu_ops = {
    .get_address_space = brcmstb_pcie_dma_as,
};

static const char *brcmstb_pcie_root_bus_path(PCIHostState *host_bridge,
                                              PCIBus *rootbus)
{
    BrcmstbPCIeHostState *s = BRCMSTB_PCIE_HOST(host_bridge);

    snprintf(s->root_bus_path, sizeof(s->root_bus_path), "%04x:00",
             s->domain);
    return s->root_bus_path;
}

static void brcmstb_pcie_host_init(Object *obj)
{
    BrcmstbPCIeHostState *s = BRCMSTB_PCIE_HOST(obj);

    object_initialize_child(obj, "intr2-cpu", &s->intr2_cpu,
                            TYPE_BRCMSTB_L2_INTC);
    object_initialize_child(obj, "msi-intr2", &s->msi_intr2,
                            TYPE_BRCMSTB_L2_INTC);
    object_initialize_child(obj, "root-port", &s->root,
                            TYPE_BRCMSTB_PCIE_ROOT_PORT);
    qdev_init_gpio_in_named(DEVICE(obj), brcmstb_pcie_bridge_reset_in,
                            "bridge-reset", 1);
    qdev_init_gpio_in_named(DEVICE(obj), brcmstb_pcie_intr2_out,
                            "intr2-out", 2);
}

static void brcmstb_pcie_host_realize(DeviceState *dev, Error **errp)
{
    BrcmstbPCIeHostState *s = BRCMSTB_PCIE_HOST(dev);
    PCIHostState *pci = PCI_HOST_BRIDGE(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    PCIESlot *slot = PCIE_SLOT(&s->root);
    g_autofree char *root_bus_name = NULL;
    BrcmstbL2IntcState *intr2[] = { &s->intr2_cpu, &s->msi_intr2 };
    hwaddr intr2_offset[] = { BRCMSTB_PCIE_INTR2_CPU,
                              BRCMSTB_PCIE_MSI_INTR2 };
    unsigned i;

    if (!s->dma_mr) {
        error_setg(errp, "%s: the dma-memory link must be set",
                   TYPE_BRCMSTB_PCIE_HOST);
        return;
    }
    if (!s->bus_name) {
        error_setg(errp, "%s: bus-name must be set", TYPE_BRCMSTB_PCIE_HOST);
        return;
    }
    if ((s->outbound_base | s->outbound_size) & (MiB - 1) ||
        !s->outbound_size) {
        error_setg(errp, "%s: the outbound aperture must be a non-empty "
                   "multiple of 1 MiB", TYPE_BRCMSTB_PCIE_HOST);
        return;
    }
    if (s->num_lanes != 1 && s->num_lanes != 2 && s->num_lanes != 4) {
        error_setg(errp, "%s: num-lanes must be 1, 2 or 4",
                   TYPE_BRCMSTB_PCIE_HOST);
        return;
    }
    if (s->max_link_speed < 1 || s->max_link_speed > 3) {
        error_setg(errp, "%s: max-link-speed must be 1 to 3",
                   TYPE_BRCMSTB_PCIE_HOST);
        return;
    }

    /* The register window, with the two L2 controllers in it */
    memory_region_init(&s->regs, OBJECT(s), "brcmstb-pcie",
                       BRCMSTB_PCIE_REGS_SIZE);
    memory_region_init_io(&s->regs_io, OBJECT(s), &brcmstb_pcie_regs_ops, s,
                          "brcmstb-pcie.regs", BRCMSTB_PCIE_REGS_SIZE);
    memory_region_add_subregion(&s->regs, 0, &s->regs_io);
    for (i = 0; i < ARRAY_SIZE(intr2); i++) {
        if (!object_property_set_bool(OBJECT(intr2[i]), "edge", true, errp) ||
            !sysbus_realize(SYS_BUS_DEVICE(intr2[i]), errp)) {
            return;
        }
        memory_region_add_subregion_overlap(
            &s->regs, intr2_offset[i],
            sysbus_mmio_get_region(SYS_BUS_DEVICE(intr2[i]), 0), 1);
        sysbus_connect_irq(SYS_BUS_DEVICE(intr2[i]), 0,
                           qdev_get_gpio_in_named(dev, "intr2-out", i));
    }
    sysbus_init_mmio(sbd, &s->regs);

    /* PCI memory, and the aperture through which the CPU reaches it */
    memory_region_init(&s->pci_mem, OBJECT(s), "brcmstb-pcie.pci-mem",
                       UINT64_MAX);
    memory_region_init_io(&s->pci_mem_err, OBJECT(s),
                          &brcmstb_pcie_unclaimed_ops, s,
                          "brcmstb-pcie.unclaimed", UINT64_MAX);
    memory_region_add_subregion_overlap(&s->pci_mem, 0, &s->pci_mem_err,
                                        INT_MIN);
    memory_region_init(&s->pci_io, OBJECT(s), "brcmstb-pcie.pci-io",
                       64 * KiB);
    memory_region_init(&s->outbound, OBJECT(s), "brcmstb-pcie.outbound",
                       s->outbound_size);
    memory_region_init_io(&s->outbound_err, OBJECT(s),
                          &brcmstb_pcie_undecoded_ops, s,
                          "brcmstb-pcie.undecoded", s->outbound_size);
    memory_region_add_subregion_overlap(&s->outbound, 0, &s->outbound_err,
                                        -1);
    for (i = 0; i < BRCMSTB_PCIE_NUM_OUTBOUND; i++) {
        g_autofree char *name = g_strdup_printf("brcmstb-pcie.outbound%u",
                                                i);

        memory_region_init_alias(&s->out_win[i], OBJECT(s), name,
                                 &s->pci_mem, 0, MiB);
    }
    sysbus_init_mmio(sbd, &s->outbound);

    /* The requests of the PCI devices, as the RC decodes them */
    memory_region_init(&s->dma_root, OBJECT(s), "brcmstb-pcie.dma",
                       UINT64_MAX);
    for (i = 0; i < BRCMSTB_PCIE_NUM_INBOUND; i++) {
        g_autofree char *name = g_strdup_printf("brcmstb-pcie.inbound%u",
                                                i + 1);

        memory_region_init_alias(&s->in_win[i], OBJECT(s), name, s->dma_mr,
                                 0, 4 * KiB);
    }
    memory_region_init_io(&s->msi_win, OBJECT(s), &brcmstb_pcie_msi_ops, s,
                          "brcmstb-pcie.msi", 4);
    address_space_init(&s->dma_as, &s->dma_root, "brcmstb-pcie.dma");

    for (i = 0; i < ARRAY_SIZE(s->irq); i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
    }

    root_bus_name = g_strdup_printf("pcie%u.root", s->domain);
    pci->bus = pci_register_root_bus(dev, root_bus_name,
                                     brcmstb_pcie_set_irq,
                                     brcmstb_pcie_map_irq, s, &s->pci_mem,
                                     &s->pci_io, 0, BRCMSTB_PCIE_NUM_INTX,
                                     TYPE_PCIE_BUS);
    pci->bus->flags |= PCI_BUS_EXTENDED_CONFIG_SPACE;
    pci_setup_iommu(pci->bus, &brcmstb_pcie_iommu_ops, s);

    /* The root port, whose link is what the PHY's settings make it */
    slot->speed = (PCIExpLinkSpeed[]){ QEMU_PCI_EXP_LNK_2_5GT,
                                       QEMU_PCI_EXP_LNK_5GT,
                                       QEMU_PCI_EXP_LNK_8GT }
                  [s->max_link_speed - 1];
    slot->width = (PCIExpLinkWidth)s->num_lanes;
    slot->chassis = s->domain;
    PCI_BRIDGE(&s->root)->bus_name = s->bus_name;
    s->root.l1ss = s->l1ss;
    if (!qdev_realize(DEVICE(&s->root), BUS(pci->bus), errp)) {
        return;
    }
    if (s->aspm_l0s) {
        PCIDevice *d = brcmstb_pcie_root(s);

        pci_long_test_and_set_mask(d->config + d->exp.exp_cap +
                                   PCI_EXP_LNKCAP, PCI_EXP_LNKCAP_ASPM_L0S);
    }
    /* Nothing else plugs into the root bus */
    qbus_mark_full(BUS(pci->bus));
}

/*
 * What the boot firmware programs when it keeps the link for the OS.
 * TODO(WS0.4): compare with a register dump taken with pciex4_reset=0.
 */
#define BRCMSTB_PCIE_PREINIT_OUT_SIZE   (4 * GiB)
#define BRCMSTB_PCIE_PREINIT_IN_BAR     2
#define BRCMSTB_PCIE_PREINIT_IN_PCI     0x1000000000ULL
#define BRCMSTB_PCIE_PREINIT_IN_SIZE    21          /* 64 GiB */

static void brcmstb_pcie_preinit(BrcmstbPCIeHostState *s)
{
    PCIDevice *d = brcmstb_pcie_root(s);
    uint64_t cpu = s->outbound_base + s->outbound_size -
                   BRCMSTB_PCIE_PREINIT_OUT_SIZE;
    uint64_t base_mb = cpu / MiB;
    uint64_t limit_mb = (cpu + BRCMSTB_PCIE_PREINIT_OUT_SIZE) / MiB - 1;
    hwaddr bar = brcmstb_pcie_in_bar(BRCMSTB_PCIE_PREINIT_IN_BAR);
    hwaddr remap = brcmstb_pcie_in_remap(BRCMSTB_PCIE_PREINIT_IN_BAR);
    uint32_t bl = 0;

    R(s, MISC_PCIE_CTRL) = R_MISC_PCIE_CTRL_PERSTB_MASK;

    /* Window 0 starts at PCI 0: WIN0_LO and WIN0_HI stay zero */
    bl = FIELD_DP32(bl, MISC_MEM_WIN_BASE_LIMIT, BASE, base_mb);
    bl = FIELD_DP32(bl, MISC_MEM_WIN_BASE_LIMIT, LIMIT, limit_mb);
    s->reg[brcmstb_pcie_out_base_limit(0) / 4] = bl;
    s->reg[brcmstb_pcie_out_base_hi(0) / 4] = base_mb >> 12;
    s->reg[brcmstb_pcie_out_base_hi(0) / 4 + 1] = limit_mb >> 12;

    s->reg[bar / 4] = (uint32_t)BRCMSTB_PCIE_PREINIT_IN_PCI |
                      BRCMSTB_PCIE_PREINIT_IN_SIZE;
    s->reg[bar / 4 + 1] = BRCMSTB_PCIE_PREINIT_IN_PCI >> 32;
    s->reg[remap / 4] = R_MISC_UBUS_BAR_CONFIG_REMAP_ACCESS_EN_MASK;

    /*
     * The root port: bus 1 behind it, PCI 0 to 4 GiB forwarded, memory
     * and bus mastering on. TODO(WS0.4): the firmware's window.
     */
    pci_host_config_write_common(d, PCI_PRIMARY_BUS, pci_config_size(d),
                                 0x010100, 4);
    pci_host_config_write_common(d, PCI_MEMORY_BASE, pci_config_size(d),
                                 0xfff00000, 4);
    pci_host_config_write_common(d, PCI_COMMAND, pci_config_size(d),
                                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER, 2);
}

static void brcmstb_pcie_host_reset_enter(Object *obj, ResetType type)
{
    BrcmstbPCIeHostState *s = BRCMSTB_PCIE_HOST(obj);

    memset(s->reg, 0, sizeof(s->reg));
    s->perst = true;
}

/* The root port and the devices behind it have been reset already */
static void brcmstb_pcie_host_reset_hold(Object *obj, ResetType type)
{
    BrcmstbPCIeHostState *s = BRCMSTB_PCIE_HOST(obj);

    if (s->preinit) {
        brcmstb_pcie_preinit(s);
    }
    brcmstb_pcie_update_outbound(s);
    brcmstb_pcie_update_inbound(s);
    brcmstb_pcie_update_msi(s);
    brcmstb_pcie_update_perst(s);
}

static int brcmstb_pcie_host_post_load(void *opaque, int version_id)
{
    BrcmstbPCIeHostState *s = BRCMSTB_PCIE_HOST(opaque);

    brcmstb_pcie_update_outbound(s);
    brcmstb_pcie_update_inbound(s);
    brcmstb_pcie_update_msi(s);
    return 0;
}

static const VMStateDescription vmstate_brcmstb_pcie_host = {
    .name = TYPE_BRCMSTB_PCIE_HOST,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = brcmstb_pcie_host_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(reg, BrcmstbPCIeHostState,
                             BRCMSTB_PCIE_REGS_SIZE / 4),
        VMSTATE_BOOL(bridge_reset, BrcmstbPCIeHostState),
        VMSTATE_BOOL(perst, BrcmstbPCIeHostState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property brcmstb_pcie_host_properties[] = {
    DEFINE_PROP_UINT32("domain", BrcmstbPCIeHostState, domain, 0),
    DEFINE_PROP_STRING("bus-name", BrcmstbPCIeHostState, bus_name),
    DEFINE_PROP_UINT64("outbound-base", BrcmstbPCIeHostState, outbound_base,
                       0),
    DEFINE_PROP_UINT64("outbound-size", BrcmstbPCIeHostState, outbound_size,
                       16 * GiB),
    DEFINE_PROP_UINT32("num-lanes", BrcmstbPCIeHostState, num_lanes, 1),
    DEFINE_PROP_UINT32("max-link-speed", BrcmstbPCIeHostState,
                       max_link_speed, 2),
    DEFINE_PROP_BOOL("aspm-l0s", BrcmstbPCIeHostState, aspm_l0s, false),
    DEFINE_PROP_BOOL("l1ss", BrcmstbPCIeHostState, l1ss, false),
    DEFINE_PROP_BOOL("preinit", BrcmstbPCIeHostState, preinit, false),
    DEFINE_PROP_UINT32("hw-revision", BrcmstbPCIeHostState, hw_revision,
                       0x0304),
    DEFINE_PROP_LINK("dma-memory", BrcmstbPCIeHostState, dma_mr,
                     TYPE_MEMORY_REGION, MemoryRegion *),
};

static void brcmstb_pcie_host_class_init(ObjectClass *klass,
                                         const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIHostBridgeClass *hc = PCI_HOST_BRIDGE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    hc->root_bus_path = brcmstb_pcie_root_bus_path;
    rc->phases.enter = brcmstb_pcie_host_reset_enter;
    rc->phases.hold = brcmstb_pcie_host_reset_hold;
    dc->realize = brcmstb_pcie_host_realize;
    dc->vmsd = &vmstate_brcmstb_pcie_host;
    device_class_set_props(dc, brcmstb_pcie_host_properties);
    dc->desc = "Broadcom STB PCIe root complex";
    dc->fw_name = "pci";
    dc->user_creatable = false;

    msi_nonbroken = true;
}

/*
 * The root port: a type-1 header with the capabilities of the BCM2712's,
 * without a slot, and a link whose speed and width the host sets
 */
static void brcmstb_pcie_root_port_realize(PCIDevice *d, Error **errp)
{
    BrcmstbPCIeRootPortClass *rpc = BRCMSTB_PCIE_ROOT_PORT_GET_CLASS(d);
    BrcmstbPCIeRootPortState *rp = BRCMSTB_PCIE_ROOT_PORT(d);
    ERRP_GUARD();
    uint8_t *exp_cap;

    PCIE_SLOT(d)->hotplug = false;
    d->cap_present &= ~QEMU_PCIE_SLTCAP_PCP;
    rpc->parent_realize(d, errp);
    if (*errp) {
        return;
    }

    /* No slot: the link goes to a connector or a soldered-down device */
    exp_cap = d->config + d->exp.exp_cap;
    pci_word_test_and_clear_mask(exp_cap + PCI_EXP_FLAGS,
                                 PCI_EXP_FLAGS_SLOT);
    pci_set_long(exp_cap + PCI_EXP_SLTCAP, 0);
    pci_set_word(exp_cap + PCI_EXP_SLTCTL, 0);
    pci_set_word(d->wmask + d->exp.exp_cap + PCI_EXP_SLTCTL, 0);
    pci_set_word(d->w1cmask + d->exp.exp_cap + PCI_EXP_SLTSTA, 0);

    /*
     * ASPM L1 (L0s is the host's to add), exits in under 1us and 2us,
     * clock PM and bandwidth notification; no link active reporting,
     * which only faster links must have
     */
    pci_long_test_and_clear_mask(exp_cap + PCI_EXP_LNKCAP,
                                 PCI_EXP_LNKCAP_ASPMS | PCI_EXP_LNKCAP_L0SEL |
                                 PCI_EXP_LNKCAP_L1EL);
    if (PCIE_SLOT(d)->speed <= QEMU_PCI_EXP_LNK_5GT) {
        pci_long_test_and_clear_mask(exp_cap + PCI_EXP_LNKCAP,
                                     PCI_EXP_LNKCAP_DLLLARC);
    }
    pci_long_test_and_set_mask(exp_cap + PCI_EXP_LNKCAP,
                               PCI_EXP_LNKCAP_ASPM_L1 |
                               5 << ctz32(PCI_EXP_LNKCAP_L0SEL) |
                               1 << ctz32(PCI_EXP_LNKCAP_L1EL) |
                               PCI_EXP_LNKCAP_CLKPM | PCI_EXP_LNKCAP_LBNC |
                               BIT(22) /* ASPM optionality compliance */);

    /* AER as version 1 */
    pci_long_test_and_clear_mask(d->config + BRCMSTB_PCIE_AER_OFFSET,
                                 0xf << 16);
    pci_long_test_and_set_mask(d->config + BRCMSTB_PCIE_AER_OFFSET, 1 << 16);

    /* No subsystem IDs, which the parent always adds */
    pci_del_capability(d, PCI_CAP_ID_SSVID, 8);

    if (pci_pm_init(d, BRCMSTB_PCIE_PM_OFFSET, errp) < 0) {
        return;
    }
    pci_set_word(d->config + d->pm_cap + PCI_PM_PMC,
                 3 | PCI_PM_CAP_PME_D0 | PCI_PM_CAP_PME_D3hot);
    pci_set_word(d->config + d->pm_cap + PCI_PM_CTRL,
                 PCI_PM_CTRL_NO_SOFT_RESET);
    pci_set_word(d->wmask + d->pm_cap + PCI_PM_CTRL,
                 PCI_PM_CTRL_STATE_MASK | PCI_PM_CTRL_PME_ENABLE);
    pci_set_word(d->w1cmask + d->pm_cap + PCI_PM_CTRL,
                 PCI_PM_CTRL_PME_STATUS);

    /* 512-byte payloads, and no extended tags */
    pci_long_test_and_clear_mask(exp_cap + PCI_EXP_DEVCAP,
                                 PCI_EXP_DEVCAP_PAYLOAD |
                                 PCI_EXP_DEVCAP_EXT_TAG);
    pci_long_test_and_set_mask(exp_cap + PCI_EXP_DEVCAP,
                               2 /* 512 bytes */);

    pcie_add_capability(d, PCI_EXT_CAP_ID_VC, 1, BRCMSTB_PCIE_VC_OFFSET,
                        BRCMSTB_PCIE_VC_SIZE);
    pcie_add_capability(d, PCI_EXT_CAP_ID_VNDR, 1, BRCMSTB_PCIE_VSEC_OFFSET,
                        BRCMSTB_PCIE_VSEC_SIZE);
    pci_set_long(d->config + BRCMSTB_PCIE_VSEC_OFFSET + PCI_VNDR_HEADER,
                 BRCMSTB_PCIE_VSEC_SIZE << 20);
    if (rp->l1ss) {
        /*
         * L1.1 and L1.2, 8us to restore common mode and 10us to power on;
         * nothing downstream does anything with them
         */
        pcie_add_capability(d, PCI_EXT_CAP_ID_L1SS, 1,
                            BRCMSTB_PCIE_L1SS_OFFSET,
                            BRCMSTB_PCIE_L1SS_SIZE);
        pci_set_long(d->config + BRCMSTB_PCIE_L1SS_OFFSET + PCI_L1SS_CAP,
                     PCI_L1SS_CAP_PCIPM_L1_2 | PCI_L1SS_CAP_PCIPM_L1_1 |
                     PCI_L1SS_CAP_ASPM_L1_2 | PCI_L1SS_CAP_ASPM_L1_1 |
                     PCI_L1SS_CAP_L1_PM_SS |
                     8 << 8 |           /* Common_Mode_Restore_Time */
                     1 << 16 |          /* T_POWER_ON: 1 x 10us */
                     1 << 19);
        pci_set_long(d->wmask + BRCMSTB_PCIE_L1SS_OFFSET + PCI_L1SS_CTL1,
                     PCI_L1SS_CTL1_L1SS_MASK |
                     PCI_L1SS_CTL1_CM_RESTORE_TIME |
                     PCI_L1SS_CTL1_LTR_L12_TH_VALUE |
                     PCI_L1SS_CTL1_LTR_L12_TH_SCALE);
        pci_set_long(d->wmask + BRCMSTB_PCIE_L1SS_OFFSET + PCI_L1SS_CTL2,
                     PCI_L1SS_CTL2_T_PWR_ON_SCALE |
                     PCI_L1SS_CTL2_T_PWR_ON_VALUE);
    }
    pcie_add_capability(d, PCI_EXT_CAP_ID_SECPCI, 1,
                        BRCMSTB_PCIE_SPCIE_OFFSET,
                        PCI_SECPCI_LE_CTRL + 2 * 16);
}

static const VMStateDescription vmstate_brcmstb_pcie_root_port = {
    .name = TYPE_BRCMSTB_PCIE_ROOT_PORT,
    .priority = MIG_PRI_PCI_BUS,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = pcie_cap_slot_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj.parent_obj.parent_obj.parent_obj,
                           BrcmstbPCIeRootPortState),
        VMSTATE_STRUCT(parent_obj.parent_obj.parent_obj.parent_obj.exp.aer_log,
                       BrcmstbPCIeRootPortState, 0, vmstate_pcie_aer_log,
                       PCIEAERLog),
        VMSTATE_END_OF_LIST()
    }
};

static void brcmstb_pcie_root_port_class_init(ObjectClass *klass,
                                              const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    PCIERootPortClass *rpc = PCIE_ROOT_PORT_CLASS(klass);
    BrcmstbPCIeRootPortClass *brpc = BRCMSTB_PCIE_ROOT_PORT_CLASS(klass);

    brpc->parent_realize = k->realize;
    k->realize = brcmstb_pcie_root_port_realize;
    k->vendor_id = BRCMSTB_PCIE_VENDOR_ID;
    k->device_id = BRCMSTB_PCIE_DEVICE_ID;
    k->revision = BRCMSTB_PCIE_REVISION;
    rpc->exp_offset = BRCMSTB_PCIE_EXP_OFFSET;
    rpc->aer_offset = BRCMSTB_PCIE_AER_OFFSET;
    dc->vmsd = &vmstate_brcmstb_pcie_root_port;
    dc->desc = "Broadcom STB PCIe root port";
    dc->user_creatable = false;
}

static const TypeInfo brcmstb_pcie_types[] = {
    {
        .name           = TYPE_BRCMSTB_PCIE_HOST,
        .parent         = TYPE_PCI_HOST_BRIDGE,
        .instance_size  = sizeof(BrcmstbPCIeHostState),
        .instance_init  = brcmstb_pcie_host_init,
        .class_init     = brcmstb_pcie_host_class_init,
    },
    {
        .name           = TYPE_BRCMSTB_PCIE_ROOT_PORT,
        .parent         = TYPE_PCIE_ROOT_PORT,
        .instance_size  = sizeof(BrcmstbPCIeRootPortState),
        .class_size     = sizeof(BrcmstbPCIeRootPortClass),
        .class_init     = brcmstb_pcie_root_port_class_init,
    },
};

DEFINE_TYPES(brcmstb_pcie_types)
