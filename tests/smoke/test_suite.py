# SPDX-License-Identifier: GPL-2.0-or-later
#
# Run the bare-metal test suite (tests/guest/suite) on raspi5b across core
# counts, with and without a device tree, at EL2 and at EL3, and check its
# transcript (format: tests/guest/README.md).

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_hello import QEMU, ROOT, TIMEOUT

SUITE = Path(os.environ.get("SUITE", ROOT / "tests/guest/build/suite.elf"))
DTS = Path(__file__).with_name("bcm2712-min.dts")

RESULT = re.compile(r"^(PASS|FAIL|SKIP): ([^:]+)(?:: (.*))?$")


def run_suite(*machine_args, dtb=None, secure=False):
    """Boot the suite; return (results {name: (outcome, detail)}, output)."""
    machine = "raspi5b,secure=on" if secure else "raspi5b"
    cmd = [str(QEMU), "-M", machine, "-display", "none", "-monitor", "none",
           "-serial", "stdio", "-kernel", str(SUITE), *machine_args]
    if dtb:
        cmd += ["-dtb", str(dtb)]
    if secure:
        cmd += ["-semihosting-config", "enable=on,target=native"]
    result = subprocess.run(cmd, stdin=subprocess.DEVNULL,
                            capture_output=True, text=True, timeout=TIMEOUT)
    out = result.stdout.replace("\r\n", "\n")
    results = {}
    for line in out.splitlines():
        m = RESULT.match(line)
        if m:
            results[m.group(2)] = (m.group(1), m.group(3))
    return results, out


@unittest.skipUnless(QEMU.exists() and SUITE.exists(),
                     "build QEMU and the guests first (make build guest)")
@unittest.skipUnless(shutil.which("dtc"),
                     "needs dtc (device-tree-compiler)")
class SuiteTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp())
        cls.dtb = cls.tmp / "bcm2712-min.dtb"
        subprocess.run(["dtc", "-q", "-I", "dts", "-O", "dtb",
                        "-o", cls.dtb, DTS], check=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp)

    def check(self, results, out, skipped):
        """Every test passed except @skipped, which were skipped."""
        self.assertIn("END: PASS", out, out)
        self.assertNotIn("PANIC", out, out)
        self.assertTrue(results, out)
        for name, (outcome, detail) in results.items():
            expected = "SKIP" if name in skipped else "PASS"
            self.assertEqual(outcome, expected, f"{name}: {detail}\n{out}")
        self.assertLessEqual(set(skipped), set(results), out)

    def test_el2(self):
        for smp in (1, 2, 4):
            for dtb in (None, self.dtb):
                with self.subTest(smp=smp, dtb=bool(dtb)):
                    results, out = run_suite("-smp", str(smp), dtb=dtb)
                    skipped = {"timer/secure-physical"}
                    if not dtb:
                        skipped.add("platform/device-tree")
                    self.check(results, out, skipped)
                    self.assertIn(f"# EL2, {smp if dtb else 4} cores", out)

    def test_el3(self):
        for dtb in (None, self.dtb):
            with self.subTest(dtb=bool(dtb)):
                results, out = run_suite(dtb=dtb, secure=True)
                skipped = {"psci/version", "smp/cpu-on",
                           "timer/el2-physical"}
                if not dtb:
                    skipped.add("platform/device-tree")
                self.check(results, out, skipped)
                self.assertIn("# EL3, 4 cores", out)

    def test_dt_addresses_used(self):
        _, out = run_suite(dtb=self.dtb)
        self.assertIn("uart 0x107d001000 (dt), gic 0x107fff9000/0x107fffa000"
                      " (dt), systimer 0x107c003000 intid 96-99 (dt)", out)
        self.assertIn("# pm 0x107d200000 (dt)", out)
        self.assertIn("mbox 0x107c013880 (dt)", out)
        self.assertIn("rng 0x107d208000 (dt)", out)

    def test_resets(self):
        """The resetting tests really reset the machine, and only once each
        time they ask: once for pm/watchdog-reset, four times (three PSCI
        SYSTEM_RESETs, then the watchdog) for reset/system-reset."""
        for secure in (False, True):
            with self.subTest(secure=secure):
                _, out = run_suite(secure=secure)
                self.assertIn("# pm 0x107d200000 (default), boot 1, reset "
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
