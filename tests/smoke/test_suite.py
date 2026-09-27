# SPDX-License-Identifier: GPL-2.0-or-later
#
# Run the bare-metal test suite (tests/guest/suite) on raspi5b across core
# counts, with the machine's built-in device tree, a -dtb one and none, at
# EL2 and at EL3, and check its transcript (format: tests/guest/README.md).

import collections
import os
import re
import shutil
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path

from test_hello import QEMU, ROOT, TIMEOUT

SUITE = Path(os.environ.get("SUITE", ROOT / "tests/guest/build/suite.elf"))
DTS = Path(__file__).with_name("bcm2712-min.dts")

RESULT = re.compile(r"^(PASS|FAIL|SKIP): ([^:]+)(?:: (.*))?$")

# Every test the suite's sources declare, so that one missing from the
# build or the transcript fails the run rather than going unnoticed
TEST_DECL = re.compile(r'^TEST\(\w+,\s*"([^"]+)"\)', re.MULTILINE)
EXPECTED = {name for src in (ROOT / "tests/guest/suite").glob("*.c")
            for name in TEST_DECL.findall(src.read_text())}

# uart/echo asks for a line with this note and skips without an answer
ECHO_PROMPT = "# uart/echo: send a line"

# Skipped wherever the suite runs: uart/echo, unless answered
ALWAYS_SKIPPED = {"uart/echo"}


# Device trees the suite runs with: the machine's own, a -dtb blob, none
DT_MODES = ("builtin", "file", "none")

# One boot of the suite: the results {name: (outcome, detail)} parsed from
# the transcript, the transcript itself, QEMU's exit status, and whether
# QEMU had to be killed at TIMEOUT
Run = collections.namedtuple("Run", "results out status timed_out")


def run_suite(*machine_args, dtb=None, secure=False, bios=None, answer=None):
    """Boot the suite and return its Run.

    @dtb is a -dtb blob, None for the built-in tree, or "none" for no tree.
    @secure gives the suite EL3; @bios is firmware to own EL3 instead,
    which starts the suite as the firmware would a kernel.
    @answer is the line to send when uart/echo asks for one.
    """
    machine = "raspi5b"
    if secure or bios:
        machine += ",secure=on"
    if dtb == "none":
        machine += ",builtin-dtb=off"
    cmd = [str(QEMU), "-M", machine, "-display", "none", "-monitor", "none",
           "-serial", "stdio", "-kernel", str(SUITE), *machine_args]
    if dtb not in (None, "none"):
        cmd += ["-dtb", str(dtb)]
    if bios:
        cmd += ["-bios", str(bios)]
    elif secure:
        cmd += ["-semihosting-config", "enable=on,target=native"]
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True)
    timed_out = threading.Event()

    def kill():
        timed_out.set()
        proc.kill()

    timer = threading.Timer(TIMEOUT, kill)
    timer.start()
    lines = []
    try:
        for line in proc.stdout:
            lines.append(line)
            if answer is not None and line.startswith(ECHO_PROMPT):
                proc.stdin.write(answer + "\n")
                proc.stdin.flush()
        status = proc.wait()
    finally:
        timer.cancel()
        proc.stdin.close()
        proc.stdout.close()
    out = "".join(lines).replace("\r\n", "\n")
    results = {}
    for line in out.splitlines():
        m = RESULT.match(line)
        if m:
            results[m.group(2)] = (m.group(1), m.group(3))
    return Run(results, out, status, timed_out.is_set())


class SuiteChecks:
    """Checks of a suite Run, for the test cases that boot the suite."""

    def assertExited(self, run):
        """QEMU ran to the guest's exit and returned 0: a transcript alone
        would also pass with the guest hung in SYSTEM_OFF, or QEMU crashing
        after the summary."""
        self.assertFalse(run.timed_out,
                         f"QEMU was killed after {TIMEOUT} s\n{run.out}")
        self.assertEqual(run.status, 0,
                         f"QEMU exited with {run.status}\n{run.out}")

    def check(self, run, skipped):
        """QEMU exited cleanly; every declared test ran, and passed except
        @skipped."""
        self.assertExited(run)
        out = run.out
        skipped = set(skipped) | ALWAYS_SKIPPED
        self.assertIn("END: PASS", out, out)
        self.assertNotIn("PANIC", out, out)
        self.assertIn(f"# {len(EXPECTED)} tests\n", out, out)
        self.assertEqual(set(run.results), EXPECTED, out)
        self.assertLessEqual(skipped, EXPECTED)
        for name, (outcome, detail) in run.results.items():
            expected = "SKIP" if name in skipped else "PASS"
            self.assertEqual(outcome, expected, f"{name}: {detail}\n{out}")


@unittest.skipUnless(QEMU.exists() and SUITE.exists(),
                     "build QEMU and the guests first (make build guest)")
@unittest.skipUnless(shutil.which("dtc"),
                     "needs dtc (device-tree-compiler)")
class SuiteTest(SuiteChecks, unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp())
        cls.dtb = cls.tmp / "bcm2712-min.dtb"
        subprocess.run(["dtc", "-q", "-I", "dts", "-O", "dtb",
                        "-o", cls.dtb, DTS], check=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp)

    def suite_dtb(self, mode):
        return {"builtin": None, "file": self.dtb, "none": "none"}[mode]

    def test_el2(self):
        for smp in (1, 2, 4):
            for mode in DT_MODES:
                with self.subTest(smp=smp, dt=mode):
                    run = run_suite("-smp", str(smp), dtb=self.suite_dtb(mode))
                    skipped = {"timer/secure-physical",
                               "gic/security-groups"}
                    if smp == 1:
                        skipped.add("psci/cpu-on-off")
                    if mode == "none":
                        skipped.add("platform/device-tree")
                    self.check(run, skipped)
                    self.assertIn(f"# EL2, {smp} cores", run.out)
                    self.assertIn(f"# smp/sgi: {smp} cores", run.out)

    def test_el3(self):
        for mode in DT_MODES:
            with self.subTest(dt=mode):
                run = run_suite(dtb=self.suite_dtb(mode), secure=True)
                skipped = {"psci/version", "psci/cpu-on-off", "smp/cpu-on",
                           "timer/el2-physical"}
                if mode == "none":
                    skipped.add("platform/device-tree")
                self.check(run, skipped)
                self.assertIn("# EL3, 4 cores", run.out)
                self.assertIn("# smp/sgi: 4 cores", run.out)

    def test_dt_addresses_used(self):
        """Every device the runtime uses is found in both trees."""
        for mode in ("builtin", "file"):
            with self.subTest(dt=mode):
                run = run_suite(dtb=self.suite_dtb(mode))
                self.assertExited(run)
                self.assertIn("uart 0x107d001000 (dt), gic 0x107fff9000/"
                              "0x107fffa000 (dt), systimer 0x107c003000 "
                              "intid 96-99 (dt)", run.out)
                self.assertIn("# pm 0x107d200000 (dt)", run.out)
                self.assertIn("mbox 0x107c013880 (dt)", run.out)
                self.assertIn("rng 0x107d208000 (dt)", run.out)

    def test_no_dt_uses_defaults(self):
        run = run_suite(dtb="none")
        self.assertExited(run)
        self.assertIn("no device tree", run.out)
        self.assertIn("uart 0x107d001000 (default)", run.out)
        self.assertIn("# pm 0x107d200000 (default)", run.out)

    def test_uart_echo(self):
        """The suite receives the line it asks for over UART10."""
        line = "raspi5b uart/echo 0123456789"
        run = run_suite(answer=line)
        self.assertExited(run)
        self.assertEqual(run.results.get("uart/echo"), ("PASS", None), run.out)
        self.assertIn(f'# uart/echo: received "{line}"', run.out)

    def test_probe(self):
        """The identification registers are dumped, sorted by name."""
        run = run_suite()
        self.assertExited(run)
        names = [line.split("=")[0] for line in run.out.splitlines()
                 if line.startswith("# probe: ")]
        self.assertGreater(len(names), 10, run.out)
        self.assertEqual(names, sorted(names))
        self.assertIn("# probe: midr_el1=0x414fd0b1", run.out)

    def test_resets(self):
        """The resetting tests really reset the machine, and only once each
        time they ask: once for pm/watchdog-reset, four times (three PSCI
        SYSTEM_RESETs, then the watchdog) for reset/system-reset."""
        for secure in (False, True):
            with self.subTest(secure=secure):
                run = run_suite(secure=secure)
                self.assertExited(run)
                out = run.out
                self.assertIn("# pm 0x107d200000 (dt), boot 1, reset "
                              "status 0x1000", out)
                self.assertIn("# boot 2: pm/watchdog-reset reset", out)
                for boot in range(3, 7):
                    self.assertIn(f"# boot {boot}: reset/system-reset "
                                  "reset", out)
                self.assertNotIn("boot 7", out)
                for name in ("pm/watchdog-reset", "reset/system-reset"):
                    self.assertEqual(out.count(f"PASS: {name}\n"), 1, out)

if __name__ == "__main__":
    unittest.main()
