# SPDX-License-Identifier: GPL-2.0-or-later
#
# Device-tree fix-ups applied by the raspi5b machine to a -dtb blob,
# checked on a minimal tree through -machine dumpdtb.

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_hello import GUEST, QEMU, TIMEOUT

# Modelled devices and only one of the compatibles raspi5b disables,
# which also checks that absent ones are skipped rather than treated as
# errors.
MINIMAL_DTS = """
/dts-v1/;
/ {
    #address-cells = <2>;
    #size-cells = <2>;
    compatible = "raspberrypi,5-model-b", "brcm,bcm2712";

    cpus {
        #address-cells = <1>;
        #size-cells = <0>;
        cpu@0 { device_type = "cpu"; reg = <0x000>; };
        cpu@1 { device_type = "cpu"; reg = <0x100>; };
        cpu@2 { device_type = "cpu"; reg = <0x200>; };
        cpu@3 { device_type = "cpu"; reg = <0x300>; };
    };

    timer@107c003000 {
        compatible = "brcm,bcm2835-system-timer";
        reg = <0x10 0x7c003000 0x0 0x1000>;
    };

    mailbox@107c013880 {
        compatible = "brcm,bcm2835-mbox";
        reg = <0x10 0x7c013880 0x0 0x40>;
    };

    watchdog@107d200000 {
        compatible = "brcm,bcm2712-pm";
        reg = <0x10 0x7d200000 0x0 0x308>;
    };

    rng@107d208000 {
        compatible = "brcm,bcm2711-rng200";
        reg = <0x10 0x7d208000 0x0 0x28>;
    };

    v3d@1002000000 {
        compatible = "brcm,2712-v3d";
        reg = <0x10 0x02000000 0x0 0x4000>;
    };
};
"""


def fdtget(dtb, node, prop):
    """Return a string property, or None when it is absent."""
    result = subprocess.run(["fdtget", "-t", "s", str(dtb), node, prop],
                            capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else None


@unittest.skipUnless(QEMU.exists() and GUEST.exists(),
                     "build QEMU and the guest first (make build guest)")
@unittest.skipUnless(shutil.which("dtc") and shutil.which("fdtget"),
                     "needs dtc and fdtget (device-tree-compiler)")
class DtbFixupTest(unittest.TestCase):

    def fixed_up(self, *machine_args):
        """Run QEMU's fix-ups on MINIMAL_DTS and return the resulting dtb."""
        tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, tmp)
        subprocess.run(["dtc", "-q", "-I", "dts", "-O", "dtb",
                        "-o", tmp / "in.dtb"],
                       input=MINIMAL_DTS, text=True, check=True)
        out = tmp / "out.dtb"
        result = subprocess.run(
            [str(QEMU), "-M", f"raspi5b,dumpdtb={out}", "-display", "none",
             "-dtb", str(tmp / "in.dtb"), "-kernel", str(GUEST),
             *machine_args],
            stdin=subprocess.DEVNULL, capture_output=True, text=True,
            timeout=TIMEOUT)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(os.path.getsize(out) > 0)
        return out

    def test_unmodelled_devices_disabled(self):
        dtb = self.fixed_up()
        self.assertEqual(fdtget(dtb, "/v3d@1002000000", "status"),
                         "disabled")

    def test_modelled_devices_untouched(self):
        dtb = self.fixed_up()
        self.assertIsNone(fdtget(dtb, "/timer@107c003000", "status"))
        self.assertIsNone(fdtget(dtb, "/watchdog@107d200000", "status"))
        self.assertIsNone(fdtget(dtb, "/mailbox@107c013880", "status"))
        self.assertIsNone(fdtget(dtb, "/rng@107d208000", "status"))

    def test_memory_leaves_out_videocore(self):
        """The top 4 MiB of the first GiB belong to the VideoCore."""
        for ram, reg in (("1G", "0 0 0 3fc00000"),
                         ("4G", "0 0 0 3fc00000 0 40000000 0 c0000000")):
            with self.subTest(ram=ram):
                dtb = self.fixed_up("-m", ram)
                result = subprocess.run(["fdtget", "-t", "x", str(dtb),
                                         "/memory", "reg"],
                                        capture_output=True, text=True,
                                        check=True)
                self.assertEqual(result.stdout.strip(), reg)

    def test_board_revision(self):
        dtb = self.fixed_up("-m", "4G")
        result = subprocess.run(["fdtget", "-t", "x", str(dtb), "/system",
                                 "linux,revision"],
                                capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout.strip(), "c04170")

    def test_all_cpus_present(self):
        dtb = self.fixed_up()
        for core in range(4):
            self.assertIsNone(fdtget(dtb, f"/cpus/cpu@{core}", "status"))

    def test_absent_cpus_failed(self):
        dtb = self.fixed_up("-smp", "2")
        self.assertIsNone(fdtget(dtb, "/cpus/cpu@0", "status"))
        self.assertIsNone(fdtget(dtb, "/cpus/cpu@1", "status"))
        self.assertEqual(fdtget(dtb, "/cpus/cpu@2", "status"), "fail")
        self.assertEqual(fdtget(dtb, "/cpus/cpu@3", "status"), "fail")


if __name__ == "__main__":
    unittest.main()
