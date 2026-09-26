# SPDX-License-Identifier: GPL-2.0-or-later
#
# End-to-end smoke tests: boot the bare-metal "hello" guest on raspi5b and
# check what it reports over UART10.
#
#   QEMU=build/qemu-system-aarch64 GUEST=tests/guest/build/hello.elf \
#       python3 -m unittest discover -s tests/smoke -v

import os
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
QEMU = Path(os.environ.get("QEMU", ROOT / "build" / "qemu-system-aarch64"))
GUEST = Path(os.environ.get("GUEST", ROOT / "tests/guest/build/hello.elf"))
TIMEOUT = 60


def boot(*machine_args):
    """Run the guest to completion and return (exit status, UART output)."""
    cmd = [
        str(QEMU), "-display", "none", "-monitor", "none",
        "-serial", "stdio", "-kernel", str(GUEST), *machine_args,
    ]
    result = subprocess.run(cmd, stdin=subprocess.DEVNULL,
                            capture_output=True, text=True, timeout=TIMEOUT)
    return result.returncode, result.stdout.replace("\r\n", "\n")


@unittest.skipUnless(QEMU.exists() and GUEST.exists(),
                     "build QEMU and the guest first (make build guest)")
class HelloTest(unittest.TestCase):

    def assertBooted(self, output, el):
        self.assertIn(f"raspi5b: core 0 up at EL{el}, "
                      "MPIDR 0x0000000080000000, CNTFRQ 54000000 Hz", output)

    def test_default_el2_psci(self):
        status, out = boot("-M", "raspi5b")
        self.assertEqual(status, 0, out)
        self.assertBooted(out, 2)
        self.assertIn("raspi5b: PSCI 1.1", out)
        for core in (1, 2, 3):
            self.assertIn(f"raspi5b: core {core} online, "
                          f"MPIDR 0x00000000{0x80000000 | core << 8:08x}", out)
        self.assertIn("raspi5b: PSCI SYSTEM_OFF", out)
        self.assertNotIn("SYSTEM_OFF returned", out)

    def test_smp2_rejects_missing_cores(self):
        # No device tree, so the guest tries every core
        status, out = boot("-M", "raspi5b,builtin-dtb=off", "-smp", "2",
                           "-m", "8G")
        self.assertEqual(status, 0, out)
        self.assertIn("raspi5b: core 1 online", out)
        # PSCI_RET_INVALID_PARAMS (-2) for cores that do not exist
        self.assertIn("raspi5b: core 2 CPU_ON failed: 2", out)
        self.assertIn("raspi5b: core 3 CPU_ON failed: 2", out)

    def test_secure_el3(self):
        status, out = boot("-M", "raspi5b,secure=on",
                           "-semihosting-config", "enable=on,target=native")
        self.assertEqual(status, 0, out)
        self.assertBooted(out, 3)
        self.assertIn("raspi5b: EL3 owned by guest", out)


if __name__ == "__main__":
    unittest.main()
