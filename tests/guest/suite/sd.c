/*
 * SD host controller tests (WS5.1).
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The card in the SD card slot, on SDIO1 (Linux sdhci-brcmstb.c), read
 * as a bootloader reads it: polled, a word at a time through the buffer
 * data port. The card has to answer at 3.3 V, as it does from power-on.
 * Unverified (PLAN.md P21): how the firmware leaves the card.
 */

#include <bm/fdt.h>
#include <bm/io.h>
#include <bm/test.h>
#include <bm/timer.h>

/* Without a device tree: SDIO1 in both raspi5b trees */
#define DEFAULT_SDIO1           0x1000fff000ull

/* Linux drivers/mmc/host/sdhci.h */
#define SDHCI_BLOCK_SIZE        0x04    /* BLOCK_COUNT in the upper half */
#define SDHCI_ARGUMENT          0x08
#define SDHCI_TRANSFER_MODE     0x0c    /* COMMAND in the upper half */
#define SDHCI_RESPONSE          0x10
#define SDHCI_BUFFER            0x20
#define SDHCI_PRESENT_STATE     0x24
#define SDHCI_POWER_CONTROL     0x29
#define SDHCI_CLOCK_CONTROL     0x2c
#define SDHCI_SOFTWARE_RESET    0x2f
#define SDHCI_INT_STATUS        0x30
#define SDHCI_INT_ENABLE        0x34
#define SDHCI_SIGNAL_ENABLE     0x38
#define SDHCI_CAPABILITIES      0x40
#define SDHCI_TRNS_READ         BIT(4)
#define SDHCI_CMD_RESP_LONG     0x01
#define SDHCI_CMD_RESP_SHORT    0x02
#define SDHCI_CMD_RESP_SHORT_BUSY 0x03
#define SDHCI_CMD_CRC           BIT(3)
#define SDHCI_CMD_INDEX         BIT(4)
#define SDHCI_CMD_DATA          BIT(5)
#define SDHCI_CMD_R1            (SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC | \
                                 SDHCI_CMD_INDEX)
#define SDHCI_CMD_R1B           (SDHCI_CMD_RESP_SHORT_BUSY | SDHCI_CMD_CRC | \
                                 SDHCI_CMD_INDEX)
#define SDHCI_CMD_R2            (SDHCI_CMD_RESP_LONG | SDHCI_CMD_CRC)
#define SDHCI_CMD_R3            SDHCI_CMD_RESP_SHORT
#define SDHCI_CMD_INHIBIT       BIT(0)
#define SDHCI_DATA_INHIBIT      BIT(1)
#define SDHCI_CARD_PRESENT      BIT(16)
#define SDHCI_POWER_ON          0x01
#define SDHCI_POWER_330         0x0e
#define SDHCI_CLOCK_INT_EN      BIT(0)
#define SDHCI_CLOCK_INT_STABLE  BIT(1)
#define SDHCI_CLOCK_CARD_EN     BIT(2)
#define SDHCI_DIVIDER_SHIFT     8       /* the divisor's low 8 bits */
#define SDHCI_DIVIDER_HI_SHIFT  6       /* and its high 2 */
#define SDHCI_MAX_DIV           0x3ff   /* SD Host Controller 3.00 */
#define SDHCI_RESET_ALL         BIT(0)
#define SDHCI_INT_RESPONSE      BIT(0)
#define SDHCI_INT_DATA_END      BIT(1)
#define SDHCI_INT_DATA_AVAIL    BIT(5)
#define SDHCI_INT_ERROR         BIT(15)
#define SDHCI_INT_ERROR_MASK    0xffff0000u

/* SD Physical Layer Simplified Specification */
#define SD_GO_IDLE_STATE        0
#define SD_ALL_SEND_CID         2
#define SD_SEND_RELATIVE_ADDR   3
#define SD_SELECT_CARD          7
#define SD_SEND_IF_COND         8
#define SD_SET_BLOCKLEN         16
#define SD_READ_SINGLE_BLOCK    17
#define SD_APP_OP_COND          41      /* after SD_APP_CMD */
#define SD_APP_CMD              55
#define SD_IF_COND_CHECK        0x1aa   /* 2.7-3.6 V, check pattern */
#define SD_OCR_VDD_32_34        (BIT(20) | BIT(21))
#define SD_OCR_CCS              BIT(30) /* HCS in the argument */
#define SD_OCR_BUSY             BIT(31) /* set once powered up */
#define SD_BLOCK_SIZE           512
#define SD_ID_CLOCK_HZ          400000
#define SD_CLOCK_HZ             25000000
#define SD_WAIT_US              100000
#define SD_POWER_UP_US          1000000 /* for ACMD41, per the spec */

/* The partition table of a master boot record */
#define MBR_PARTITIONS          446
#define MBR_ENTRY_SIZE          16
#define MBR_ENTRY_TYPE          4
#define MBR_ENTRY_START         8
#define MBR_ENTRY_SECTORS       12
#define MBR_SIGNATURE           510

/* INT_STATUS when the last command or transfer ended */
static uint32_t sd_status;

/* The SD host of the card slot: mmc0, as the firmware's tree has it */
static bool find_sdio1(uintptr_t *base, const char **source)
{
    uint64_t addr, size;
    int node;

    if (!fdt_present()) {
        *base = DEFAULT_SDIO1;
        *source = "default";
        return true;
    }
    node = fdt_alias_offset("mmc0");
    if (node < 0 || !fdt_node_is_compatible(node, "brcm,bcm2712-sdhci") ||
        !fdt_node_is_enabled(node) || !fdt_reg(node, 0, &addr, &size)) {
        return false;
    }
    *base = addr;
    *source = "dt";
    return true;
}

/* Clock the card at @hz or the rate below that the divider gives */
static bool sd_set_clock(uintptr_t base, uint32_t hz)
{
    uint32_t base_mhz = (mmio_read32(base + SDHCI_CAPABILITIES) >> 8) & 0xff;
    uint32_t div = (base_mhz * 1000000 + 2 * hz - 1) / (2 * hz);
    uint16_t clk;

    if (div > SDHCI_MAX_DIV) {
        div = SDHCI_MAX_DIV;
    }
    clk = (div & 0xff) << SDHCI_DIVIDER_SHIFT |
          (div >> 8) << SDHCI_DIVIDER_HI_SHIFT | SDHCI_CLOCK_INT_EN;
    mmio_write16(base + SDHCI_CLOCK_CONTROL, 0);
    mmio_write16(base + SDHCI_CLOCK_CONTROL, clk);
    if (!wait_until(mmio_read16(base + SDHCI_CLOCK_CONTROL) &
                    SDHCI_CLOCK_INT_STABLE, SD_WAIT_US)) {
        return false;
    }
    mmio_write16(base + SDHCI_CLOCK_CONTROL, clk | SDHCI_CLOCK_CARD_EN);
    return true;
}

/*
 * Send command @index with transfer mode @mode and wait for its response,
 * the first word of which goes to @resp; false on an error or timeout,
 * with INT_STATUS in sd_status
 */
static bool sd_command(uintptr_t base, unsigned index, uint32_t arg,
                       uint16_t flags, uint16_t mode, uint32_t *resp)
{
    uint32_t inhibit = SDHCI_CMD_INHIBIT;

    if ((flags & SDHCI_CMD_DATA) ||
        (flags & SDHCI_CMD_RESP_SHORT_BUSY) == SDHCI_CMD_RESP_SHORT_BUSY) {
        inhibit |= SDHCI_DATA_INHIBIT;
    }
    if (!wait_until(!(mmio_read32(base + SDHCI_PRESENT_STATE) & inhibit),
                    SD_WAIT_US)) {
        sd_status = 0;
        return false;
    }
    mmio_write32(base + SDHCI_INT_STATUS, 0xffffffff);
    mmio_write32(base + SDHCI_ARGUMENT, arg);
    mmio_write32(base + SDHCI_TRANSFER_MODE,
                 (uint32_t)(index << 8 | flags) << 16 | mode);
    if (!wait_until((sd_status = mmio_read32(base + SDHCI_INT_STATUS)) &
                    (SDHCI_INT_RESPONSE | SDHCI_INT_ERROR), SD_WAIT_US) ||
        (sd_status & SDHCI_INT_ERROR)) {
        return false;
    }
    mmio_write32(base + SDHCI_INT_STATUS, SDHCI_INT_RESPONSE);
    if (resp) {
        *resp = mmio_read32(base + SDHCI_RESPONSE);
    }
    return true;
}

/* Wait for any of @bits in INT_STATUS, and acknowledge them */
static bool sd_wait(uintptr_t base, uint32_t bits)
{
    if (!wait_until((sd_status = mmio_read32(base + SDHCI_INT_STATUS)) &
                    (bits | SDHCI_INT_ERROR), SD_WAIT_US) ||
        (sd_status & SDHCI_INT_ERROR)) {
        return false;
    }
    mmio_write32(base + SDHCI_INT_STATUS, bits);
    return true;
}

static uint32_t le32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/*
 * sd/mbr: bring the card from reset to the transfer state at 400 kHz,
 * with the host's interrupts polled, never signalled; read its first
 * block at 25 MHz, a word at a time through the buffer; and find a master
 * boot record there, whose partitions go to the transcript.
 */
TEST(sd_mbr, "sd/mbr")
{
    static uint32_t block[SD_BLOCK_SIZE / 4];
    const uint8_t *mbr = (const uint8_t *)block;
    const char *source;
    uintptr_t base;
    uint32_t ocr, rca, resp;
    uint64_t deadline;

    if (!find_sdio1(&base, &source)) {
        SKIP("no enabled brcm,bcm2712-sdhci node for mmc0");
    }
    mmio_write8(base + SDHCI_SOFTWARE_RESET, SDHCI_RESET_ALL);
    ASSERT(wait_until(!(mmio_read8(base + SDHCI_SOFTWARE_RESET) &
                        SDHCI_RESET_ALL), SD_WAIT_US));
    if (!wait_until(mmio_read32(base + SDHCI_PRESENT_STATE) &
                    SDHCI_CARD_PRESENT, SD_WAIT_US)) {
        SKIP("no card in the slot");
    }
    mmio_write8(base + SDHCI_POWER_CONTROL, SDHCI_POWER_330 | SDHCI_POWER_ON);
    ASSERT(sd_set_clock(base, SD_ID_CLOCK_HZ));
    mmio_write32(base + SDHCI_INT_ENABLE, SDHCI_INT_ERROR_MASK |
                 SDHCI_INT_DATA_AVAIL | SDHCI_INT_DATA_END |
                 SDHCI_INT_RESPONSE);
    mmio_write32(base + SDHCI_SIGNAL_ENABLE, 0);

    ASSERT_MSG(sd_command(base, SD_GO_IDLE_STATE, 0, 0, 0, NULL),
               "CMD0: status 0x%x", sd_status);
    ASSERT_MSG(sd_command(base, SD_SEND_IF_COND, SD_IF_COND_CHECK,
                          SDHCI_CMD_R1, 0, &resp),
               "CMD8: status 0x%x", sd_status);
    ASSERT_EQ(resp & 0xfff, SD_IF_COND_CHECK);
    /* ACMD41 until the card has powered up, for as long as it may take */
    deadline = timeout_us(SD_POWER_UP_US);
    do {
        ASSERT_MSG(sd_command(base, SD_APP_CMD, 0, SDHCI_CMD_R1, 0, NULL),
                   "CMD55: status 0x%x", sd_status);
        ASSERT_MSG(sd_command(base, SD_APP_OP_COND,
                              SD_OCR_CCS | SD_OCR_VDD_32_34, SDHCI_CMD_R3,
                              0, &ocr),
                   "ACMD41: status 0x%x", sd_status);
    } while (!(ocr & SD_OCR_BUSY) && !timeout_expired(deadline));
    ASSERT_MSG(ocr & SD_OCR_BUSY, "powering up: OCR 0x%x", ocr);
    ASSERT_MSG(sd_command(base, SD_ALL_SEND_CID, 0, SDHCI_CMD_R2, 0, NULL),
               "CMD2: status 0x%x", sd_status);
    ASSERT_MSG(sd_command(base, SD_SEND_RELATIVE_ADDR, 0, SDHCI_CMD_R1, 0,
                          &rca),
               "CMD3: status 0x%x", sd_status);
    rca >>= 16;
    ASSERT_MSG(sd_command(base, SD_SELECT_CARD, rca << 16, SDHCI_CMD_R1B, 0,
                          NULL),
               "CMD7: status 0x%x", sd_status);
    if (!(ocr & SD_OCR_CCS)) {
        ASSERT_MSG(sd_command(base, SD_SET_BLOCKLEN, SD_BLOCK_SIZE,
                              SDHCI_CMD_R1, 0, NULL),
                   "CMD16: status 0x%x", sd_status);
    }
    ASSERT(sd_set_clock(base, SD_CLOCK_HZ));

    /* Block 0, whether the card takes block or byte addresses */
    mmio_write32(base + SDHCI_BLOCK_SIZE, 1 << 16 | SD_BLOCK_SIZE);
    ASSERT_MSG(sd_command(base, SD_READ_SINGLE_BLOCK, 0,
                          SDHCI_CMD_R1 | SDHCI_CMD_DATA, SDHCI_TRNS_READ,
                          NULL),
               "CMD17: status 0x%x", sd_status);
    ASSERT_MSG(sd_wait(base, SDHCI_INT_DATA_AVAIL),
               "waiting for data: status 0x%x", sd_status);
    for (unsigned i = 0; i < ARRAY_SIZE(block); i++) {
        block[i] = mmio_read32(base + SDHCI_BUFFER);
    }
    ASSERT_MSG(sd_wait(base, SDHCI_INT_DATA_END),
               "waiting for the end: status 0x%x", sd_status);

    bm_test_note("sd/mbr: host 0x%lx (%s), %s card, RCA 0x%x",
                 (unsigned long)base, source,
                 ocr & SD_OCR_CCS ? "SDHC/SDXC" : "SDSC", rca);
    ASSERT_MSG(mbr[MBR_SIGNATURE] == 0x55 && mbr[MBR_SIGNATURE + 1] == 0xaa,
               "no MBR signature: 0x%02x 0x%02x", mbr[MBR_SIGNATURE],
               mbr[MBR_SIGNATURE + 1]);
    for (unsigned i = 0; i < 4; i++) {
        const uint8_t *entry = mbr + MBR_PARTITIONS + i * MBR_ENTRY_SIZE;

        if (entry[MBR_ENTRY_TYPE]) {
            bm_test_note("sd/mbr: partition %u: type 0x%02x, %u sectors "
                         "from %u", i + 1, entry[MBR_ENTRY_TYPE],
                         le32(entry + MBR_ENTRY_SECTORS),
                         le32(entry + MBR_ENTRY_START));
        }
    }
}
