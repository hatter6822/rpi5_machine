# Bare-metal test guests

Freestanding AArch64 programs that run on `raspi5b` and, unchanged, on a
real Raspberry Pi 5. They are built with clang and lld (`make guest` from
the repository root) and booted by the smoke tests in `tests/smoke/`.

| Directory | Contents |
| --- | --- |
| `lib/` | the runtime every guest links: entry, exception vectors, console, device-tree walker, GIC-400 driver, PSCI, watchdog and reset status (PM), firmware mailbox, RNG200, generic timer helpers, a switch to Non-secure EL2 for a guest that owns EL3, test runner |
| `hello/` | the original smoke guest: boot EL, MPIDR, CNTFRQ, PSCI `CPU_ON` of every core, `SYSTEM_OFF` |
| `suite/` | the bare-metal test suite (WS9.2), one file per area |

## Runtime

`lib/start.S` runs at the exception level the machine enters at (EL2 by
default, EL3 with `secure=on`, EL2 again when TF-A loaded with `-bios`
starts it) and stays there, with the MMU and caches off. Consequences for
test code:

* every data access is to Device memory: naturally aligned accesses only
  (the guests are built with `-mstrict-align`), no exclusives, so cores
  share state through single-writer variables;
* only core 0 prints; secondaries report through memory;
* physical interrupts are routed to the running exception level
  (`HCR_EL2.{IMO,FMO,AMO}` or `SCR_EL3.{IRQ,FIQ,EA}`), and the GIC delivers
  every interrupt as IRQ: in Group 0, or under TF-A in the Non-secure
  Group 1 it assigns;
* at EL3, `bm_run_nonsecure_el2()` runs a function at Non-secure EL2 and
  returns when it does (through an SMC), with the IRQ, FIQ and SError
  routing the caller chooses; `gic/security-groups` uses it.

Before `bm_main()` runs on core 0, the runtime looks for a device tree in
`x0` (the Linux boot protocol, used by firmware and by QEMU for an `Image`)
and then at physical address 0 (where QEMU places the device tree for an
ELF image). With a tree, the console (`/chosen/stdout-path`), the GIC
(`arm,gic-400`), the system timer (`brcm,bcm2835-system-timer`), the
power management block (`brcm,bcm2712-pm`), the mailbox
(`brcm,bcm2835-mbox`), the RNG (`brcm,bcm2711-rng200`) and the number of
usable cores come from it, with `reg` translated through every parent's
`ranges`; without one, built-in raspi5b addresses are used and PSCI
`AFFINITY_INFO` tells which cores exist.
Secondary cores are started with `bm_start_core()` and are off again when
their function returns. Below EL3 that is PSCI `CPU_ON` and `CPU_OFF`;
TF-A's Raspberry Pi port reports a core off before the core is back in
its holding pen and ready for another `CPU_ON`, so a core that has run
gets 10 ms before it is started again. A
guest that owns EL3 has no PSCI firmware: every core enters the image at
reset, and the secondaries wait in a spin table until `bm_start_core()`
releases them, which returns the PSCI statuses (`ALREADY_ON`,
`INVALID_PARAMS`) for the same cases; `bm_core_is_off()` answers for both.

`bm_main()`'s return value is the exit code: below EL3 the guest ends with
PSCI `SYSTEM_OFF`, at EL3 with semihosting `SYS_EXIT` (QEMU needs
`-semihosting-config enable=on,target=native`; on hardware the core parks).
Unexpected exceptions and interrupts print a `PANIC:` line and end the
program.

A system reset (the watchdog, PSCI `SYSTEM_RESET`) keeps RAM and starts
the program again from its entry point. `bm_boot_count()` numbers the
boots since power-on, and `bm_persistent()` gives the program memory that
is zero at power-on and kept by later boots; both live beyond the stacks,
outside every loadable segment, so reloading the image leaves them alone.
A program may define `bm_early()`, which runs before the runtime touches
any device, to look at the state a reset left behind.

## Writing a test

```c
#include <bm/test.h>

TEST(gic_geometry, "gic/geometry")
{
    ASSERT_EQ(gic_num_irqs(), 320);
}
```

Tests run on core 0 one after another, sorted by name. `ASSERT()`,
`ASSERT_EQ()`/`_NE`/`_LE`/`_GE` and `ASSERT_MSG()` end the test at the
first failure; `SKIP()` ends it as skipped. Interrupts are masked and the
synchronous exception hook is cleared after every test; anything else a
test changes (enabled interrupts, handlers, device state) it restores
itself.

A test may reset the machine. The runner keeps its progress in
`bm_persistent()`, so after the reset it runs the same test again, with
`bm_test_resets()` counting the resets it has caused and
`bm_test_scratch()` holding what it saved before, then carries on with the
next test:

```c
TEST(pm_watchdog_reset, "pm/watchdog-reset")
{
    if (bm_test_resets() == 0) {
        bm_test_scratch()[0] = bm_boot_count();
        pm_watchdog_start(10);
        wait_until(false, 100000);
        ASSERT_MSG(false, "no reset");
    }
    ASSERT_EQ(bm_boot_count(), bm_test_scratch()[0] + 1);
}
```

A test that resets more than eight times fails.

## Transcript

The suite reports over the console, one line per event, so that a UART
capture from hardware can be compared with QEMU's:

```
# raspi5b bare-metal tests
# EL2, 4 cores, device tree at 0x0 (177480 bytes)
# pm 0x107d200000 (dt), boot 1, reset status 0x1000, mbox 0x107c013880 (dt), rng 0x107d208000 (dt)
# uart 0x107d001000 (dt), gic 0x107fff9000/0x107fffa000 (dt), systimer 0x107c003000 intid 96-99 (dt)
# 18 tests
PASS: gic/geometry
# raspi5b bare-metal tests
# EL2, 4 cores, device tree at 0x0 (177480 bytes)
# pm 0x107d200000 (dt), boot 2, reset status 0x464, mbox 0x107c013880 (dt), rng 0x107d208000 (dt)
# uart 0x107d001000 (dt), gic 0x107fff9000/0x107fffa000 (dt), systimer 0x107c003000 intid 96-99 (dt)
# boot 2: pm/watchdog-reset reset the machine, running it again
PASS: pm/watchdog-reset
FAIL: systimer/compare: systimer.c:98: st_cs == BIT(n) (0x0 vs 0x1)
SKIP: timer/secure-physical: runs at EL3 only
# passed 10, failed 1, skipped 1
END: FAIL
```

| Line | Meaning |
| --- | --- |
| `# ...` | information: platform, measurements, a reset a test caused (the header is printed again after it); not parsed |
| `PASS: <name>` | the test passed |
| `FAIL: <name>: <file>:<line>: <detail>` | the first failed assertion; `FAIL: <name>: panic` after a `PANIC:` line |
| `SKIP: <name>: <reason>` | not applicable in this configuration |
| `PANIC: <message>` | unexpected exception or interrupt; the run ends |
| `END: PASS` / `END: FAIL` | always the last line of a completed run |

`tests/smoke/test_suite.py` runs the suite on 1, 2 and 4 cores, with the
machine's built-in device tree, with `tests/smoke/bcm2712-min.dts` (which
mirrors the structure of the firmware's `bcm2712-rpi-5-b.dtb`) and with
none (`builtin-dtb=off`), and at EL3, and requires every test to pass or
to be skipped for a stated reason. `uart/echo` prints
`# uart/echo: send a line` and waits half a second for the peer to answer
with one; the smoke test answers in one run and lets it skip in the
others.

## The suite

| Area | Tests |
| --- | --- |
| CPU | the Cortex-A76 IMPLEMENTATION DEFINED registers firmware writes, read and written back at EL2 and EL3 |
| GIC | geometry, an SGI to self, Group 0 and 1 across the Secure and Non-secure worlds (EL3) |
| Generic timers | frequency; the EL1, EL2 and Secure physical timers; the EL1 physical and virtual timers on every core, with a virtual offset above the count |
| SMP | each core's `MPIDR_EL1`, PSCI `CPU_ON` of every core, SGIs between every pair of cores and to all others, an SPI routed to each core in turn; `CPU_ON`/`CPU_OFF`/`AFFINITY_INFO` statuses |
| System timer | rate against the generic counter, every comparator's interrupt |
| Firmware | the mailbox's board revision, the identity tags |
| PM, reset | watchdog countdown and reset, three PSCI `SYSTEM_RESET`s and a watchdog reset that must restore the boot state |
| RNG | a 1 KiB draw, Linux's recovery sequence |
| UART | a line from the peer, internal loopback polled and by interrupt |
| Platform | device-tree discovery, PSCI version, an identification-register dump (`# probe: name=value`, sorted) |
