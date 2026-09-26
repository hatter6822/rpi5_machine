# SPDX-License-Identifier: GPL-2.0-or-later
#
# The -bios options raspi5b refuses, each with the message it gives.
# Booting real firmware with -bios is 'make check-firmware'
# (tests/smoke/test_firmware.py); the armstub handoff itself is checked at
# register level by the qtest.

import subprocess
import tempfile
import unittest
from pathlib import Path

from test_hello import GUEST, QEMU, TIMEOUT

# Where the VideoCore's memory starts, below which everything is loaded
VC_RAM_BASE = 0x3fc00000


@unittest.skipUnless(QEMU.exists() and GUEST.exists(),
                     "build QEMU and the guest first (make build guest)")
class BiosOptionsTest(unittest.TestCase):

    def assertRefused(self, message, *args):
        result = subprocess.run([str(QEMU), "-display", "none",
                                 "-monitor", "none", *args],
                                stdin=subprocess.DEVNULL, capture_output=True,
                                text=True, timeout=TIMEOUT)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(message, result.stderr)

    def test_needs_secure(self):
        self.assertRefused("-bios loads firmware that runs at EL3; "
                           "use -M raspi5b,secure=on",
                           "-M", "raspi5b", "-bios", str(GUEST))

    def test_missing_file(self):
        self.assertRefused("could not load the armstub 'no-such-armstub'",
                           "-M", "raspi5b,secure=on",
                           "-bios", "no-such-armstub")

    def test_dtb_address_needs_bios(self):
        self.assertRefused("dtb-address places the device tree for -bios "
                           "only",
                           "-M", "raspi5b,secure=on,dtb-address=0x1f0000",
                           "-kernel", str(GUEST))

    def test_dtb_address_range(self):
        for addr in (0x1f0004, VC_RAM_BASE):
            with self.subTest(addr=hex(addr)):
                self.assertRefused("dtb-address must be a multiple of 8 "
                                   f"below {VC_RAM_BASE:#x}",
                                   "-M", f"raspi5b,secure=on,"
                                   f"dtb-address={addr:#x}",
                                   "-bios", str(GUEST))

    def test_kernel_too_large(self):
        """An Image whose declared size, BSS included, reaches the
        VideoCore's memory"""
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp) / "Image"
            header = bytearray(64)
            header[16:24] = (1 << 30).to_bytes(8, "little")    # image_size
            header[56:60] = b"ARM\x64"
            image.write_bytes(header)
            self.assertRefused("the kernel, initrd and device tree must fit "
                               f"below {VC_RAM_BASE:#x}",
                               "-M", "raspi5b,secure=on",
                               "-bios", str(GUEST), "-kernel", str(image))


if __name__ == "__main__":
    unittest.main()
