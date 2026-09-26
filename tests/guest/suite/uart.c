/*
 * PL011 (UART10) receive-path tests.
 *
 * Copyright (c) 2026 A7om
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <bm/console.h>
#include <bm/exception.h>
#include <bm/gic.h>
#include <bm/runtime.h>
#include <bm/test.h>
#include <bm/timer.h>

/* PrimeCell UART (PL011) TRM, chapter 3 */
#define UART_DR                 0x00
#define UART_FR                 0x18
#define UART_CR                 0x30
#define UART_IMSC               0x38
#define UART_MIS                0x40
#define UART_ICR                0x44

#define UART_DR_ERRORS          (0xfu << 8)     /* OE, BE, PE, FE */
#define UART_FR_BUSY            BIT(3)
#define UART_FR_RXFE            BIT(4)
#define UART_FR_TXFE            BIT(7)
#define UART_CR_LBE             BIT(7)
#define UART_INT_RX             BIT(4)
#define UART_INT_RT             BIT(6)

static uint32_t uart_read(uint32_t reg)
{
    return mmio_read32(bm_plat.uart + reg);
}

static void uart_write(uint32_t reg, uint32_t val)
{
    mmio_write32(bm_plat.uart + reg, val);
}

static bool uart_rx_ready(void)
{
    return !(uart_read(UART_FR) & UART_FR_RXFE);
}

/* Until every character written so far has left the transmitter */
static void uart_drain_tx(void)
{
    while ((uart_read(UART_FR) & (UART_FR_TXFE | UART_FR_BUSY)) !=
           UART_FR_TXFE) {
        cpu_relax();
    }
}

/*
 * uart/echo: the peer on the other end of UART10 (QEMU's -serial backend,
 * a terminal on hardware) answers the prompt with a line, which must
 * arrive intact. With no peer, or no line within the wait, the test is
 * skipped. Input sent before the prompt may be lost: every reset, and
 * enabling the FIFO, empties the receiver.
 */
#define ECHO_PROMPT             "uart/echo: send a line"
#define ECHO_WAIT_US            500000
#define ECHO_MAX                64

TEST(uart_echo, "uart/echo")
{
    char line[ECHO_MAX + 1];
    unsigned n = 0;
    uint64_t deadline;
    bool eol = false;

    while (uart_rx_ready()) {
        uart_read(UART_DR);             /* stale input */
    }
    bm_test_note(ECHO_PROMPT);
    deadline = timeout_us(ECHO_WAIT_US);

    while (n < ECHO_MAX && !timeout_expired(deadline)) {
        uint32_t dr;
        char c;

        if (!uart_rx_ready()) {
            cpu_relax();
            continue;
        }
        dr = uart_read(UART_DR);
        ASSERT_MSG(!(dr & UART_DR_ERRORS), "receive error, DR 0x%x", dr);
        c = dr & 0xff;
        if (c == '\r' || c == '\n') {
            eol = true;
            break;
        }
        ASSERT_MSG(c >= 0x20 && c < 0x7f, "unprintable character 0x%x",
                   (unsigned)c);
        line[n++] = c;
        deadline = timeout_us(ECHO_WAIT_US);
    }
    line[n] = '\0';
    if (n == 0 && !eol) {
        SKIP("no input");
    }
    bm_test_note("uart/echo: received \"%s\"", line);
    ASSERT_MSG(eol, "no end of line after %u characters", n);
}

/*
 * uart/loopback: with CR.LBE the transmitter feeds the receiver. The
 * first half of the message is received by polling, the rest through the
 * RX and receive-timeout interrupts (QEMU raises RX for every character;
 * hardware raises RT once the line goes idle below the FIFO level).
 *
 * QEMU still sends looped-back characters to the -serial backend, so the
 * message is itself a transcript comment line.
 */
static const char loop_msg[] = "# uart/loopback: this line looped back\n";
#define LOOP_POLLED             (sizeof(loop_msg) / 2)

static volatile char loop_rx[sizeof(loop_msg)];
static volatile unsigned loop_count, loop_irqs, loop_errors;

static void uart_handler(unsigned intid, void *arg)
{
    (void)intid;
    (void)arg;
    loop_irqs++;
    while (uart_rx_ready()) {
        uint32_t dr = uart_read(UART_DR);

        if ((dr & UART_DR_ERRORS) || loop_count >= sizeof(loop_rx)) {
            loop_errors++;
        } else {
            loop_rx[loop_count++] = dr & 0xff;
        }
    }
    uart_write(UART_ICR, UART_INT_RX | UART_INT_RT);
}

TEST(uart_loopback, "uart/loopback")
{
    const unsigned len = sizeof(loop_msg) - 1;
    unsigned intid = bm_plat.uart_intid;
    uint32_t cr = uart_read(UART_CR), imsc = uart_read(UART_IMSC);
    bool ok = true;

    uart_drain_tx();
    while (uart_rx_ready()) {
        uart_read(UART_DR);             /* stale input */
    }
    loop_count = loop_irqs = loop_errors = 0;
    uart_write(UART_CR, cr | UART_CR_LBE);

    for (unsigned i = 0; i < LOOP_POLLED && ok; i++) {
        uart_write(UART_DR, (uint8_t)loop_msg[i]);
        ok = wait_until(uart_rx_ready(), 10000);
        if (ok) {
            loop_rx[loop_count++] = uart_read(UART_DR) & 0xff;
        }
    }

    irq_register(intid, uart_handler, NULL);
    gic_enable(intid);
    uart_write(UART_ICR, UART_INT_RX | UART_INT_RT);
    uart_write(UART_IMSC, UART_INT_RX | UART_INT_RT);
    irq_unmask();
    for (unsigned i = LOOP_POLLED; i < len && ok; i++) {
        uart_write(UART_DR, (uint8_t)loop_msg[i]);
        ok = wait_until(loop_count == i + 1, 10000);
    }
    irq_mask();
    uart_write(UART_IMSC, imsc);
    gic_disable(intid);
    irq_unregister(intid);
    uart_drain_tx();
    uart_write(UART_CR, cr);

    ASSERT_MSG(ok, "character %u did not come back", loop_count);
    ASSERT_EQ(loop_errors, 0);
    ASSERT_EQ(loop_count, len);
    for (unsigned i = 0; i < len; i++) {
        ASSERT_MSG(loop_rx[i] == loop_msg[i], "character %u: 0x%x, not 0x%x",
                   i, (unsigned)loop_rx[i], (unsigned)loop_msg[i]);
    }
    ASSERT_GE(loop_irqs, 1);
    ASSERT_EQ(uart_read(UART_MIS), 0);
}
