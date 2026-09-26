# SPDX-License-Identifier: GPL-2.0-or-later
#
# Boot real firmware with -bios, as the Raspberry Pi firmware would: TF-A's
# rpi5 BL31 starting the bare-metal guests at EL2 and serving their PSCI
# calls. Runs from 'make check-firmware', which builds the firmware at
# pinned versions (scripts/firmware) and points FIRMWARE at it; skipped
# otherwise.

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_hello import GUEST, QEMU, boot
from test_suite import DT_MODES, DTS, SUITE, SuiteChecks, run_suite

FIRMWARE = Path(os.environ.get("FIRMWARE", "/nonexistent"))
BL31 = FIRMWARE / "bl31.bin"

# What TF-A prints each time it starts
TFA_BANNER = "NOTICE:  BL31: v2.15.0(release)"

# The suite's tests for EL3, which runs TF-A here
EL3_ONLY = {"timer/secure-physical", "gic/security-groups"}


@unittest.skipUnless(os.environ.get("FIRMWARE"),
                     "run through 'make check-firmware'")
@unittest.skipUnless(QEMU.exists() and GUEST.exists() and SUITE.exists(),
                     "build QEMU and the guests first (make build guest)")
@unittest.skipUnless(shutil.which("dtc"),
                     "needs dtc (device-tree-compiler)")
class TfaTest(SuiteChecks, unittest.TestCase):
    """TF-A's PSCI in place of QEMU's. Its rpi5 port counts on all four
    cores, so these tests run with the default -smp 4."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp())
        cls.dtb = cls.tmp / "bcm2712-min.dtb"
        subprocess.run(["dtc", "-q", "-I", "dts", "-O", "dtb",
                        "-o", cls.dtb, DTS], check=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp)

    def test_hello(self):
        status, out = boot("-M", "raspi5b,secure=on", "-bios", str(BL31))
        self.assertEqual(status, 0, out)
        self.assertIn(TFA_BANNER, out)
        self.assertIn("raspi5b: core 0 up at EL2, MPIDR 0x0000000081000000, "
                      "CNTFRQ 54000000 Hz", out)
        self.assertIn("raspi5b: PSCI 1.1", out)
        for core in (1, 2, 3):
            self.assertIn(f"raspi5b: core {core} online, "
                          f"MPIDR 0x00000000{0x81000000 | core << 8:08x}", out)
        self.assertIn("raspi5b: PSCI SYSTEM_OFF", out)
        self.assertNotIn("SYSTEM_OFF returned", out)

    def test_suite(self):
        dtbs = {"builtin": None, "file": self.dtb, "none": "none"}
        for mode in DT_MODES:
            with self.subTest(dt=mode):
                run = run_suite(dtb=dtbs[mode], bios=BL31)
                skipped = set(EL3_ONLY)
                if mode == "none":
                    skipped.add("platform/device-tree")
                self.check(run, skipped)
                self.assertIn("# EL2, 4 cores", run.out)

    def test_resets(self):
        """Each reset the suite asks for starts TF-A again, which starts
        the suite again, as on hardware."""
        run = run_suite(bios=BL31)
        self.assertExited(run)
        out = run.out
        self.assertIn("# pm 0x107d200000 (dt), boot 1, reset status 0x1000",
                      out)
        self.assertIn("# boot 2: pm/watchdog-reset reset", out)
        for boot_nr in range(3, 7):
            self.assertIn(f"# boot {boot_nr}: reset/system-reset reset", out)
        self.assertNotIn("boot 7", out)
        self.assertEqual(out.count(TFA_BANNER), 6, out)


if __name__ == "__main__":
    unittest.main()
