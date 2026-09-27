# SPDX-License-Identifier: GPL-2.0-or-later
#
# The -bios options raspi5b refuses, each with the message it gives.
# Booting real firmware with -bios is 'make check-firmware'
# (tests/smoke/test_firmware.py); the armstub handoff itself is checked at
# register level by the qtest.

import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_hello import GUEST, QEMU, TIMEOUT

# Where the VideoCore's memory starts, below which everything is loaded
VC_RAM_BASE = 0x3fc00000


def elf(entry, *segments):
    """A little-endian AArch64 executable with its entry point at @entry,
    loading each of @segments, (address, contents) pairs"""
    ehsize, phentsize = 64, 56
    header = struct.pack("<16sHHIQQQIHHHHHH",
                         b"\x7fELF\x02\x01\x01",    # 64-bit, LSB, version 1
                         2, 183, 1,                 # ET_EXEC, EM_AARCH64
                         entry, ehsize, 0,          # entry, phoff, shoff
                         0, ehsize, phentsize, len(segments),
                         0, 0, 0)                   # no sections
    offset = ehsize + phentsize * len(segments)
    programs, contents = b"", b""
    for addr, code in segments:
        programs += struct.pack("<IIQQQQQQ",
                                1, 5,               # PT_LOAD, R and X
                                offset + len(contents), addr, addr,
                                len(code), len(code), 0x1000)
        contents += code
    return header + programs + contents


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
        return result.stderr

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

    def test_dtb_overlap(self):
        """dtb-address on the armstub, in the BSS an Image declares, or on
        the initrd"""
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp) / "Image"
            header = bytearray(64)
            header[16:24] = (16 << 20).to_bytes(8, "little")   # image_size
            header[56:60] = b"ARM\x64"
            image.write_bytes(header)
            initrd = Path(tmp) / "initrd"
            initrd.write_bytes(bytes(1 << 20))
            for addr, what in ((0x100, "armstub at 0x0-"),
                               (0x400000, "kernel at 0x200000-0x11fffff"),
                               (0x8080000, "initrd at 0x8000000-0x80fffff")):
                with self.subTest(addr=hex(addr)):
                    err = self.assertRefused(f"the device tree at {addr:#x}-",
                                             "-M", f"raspi5b,secure=on,"
                                             f"dtb-address={addr:#x}",
                                             "-bios", str(GUEST),
                                             "-kernel", str(image),
                                             "-initrd", str(initrd))
                    self.assertIn(f" overlaps the {what}", err)

    def test_armstub_overlap(self):
        """An armstub that reaches the kernel's address, whether the kernel
        is an Image or an ELF (the guest, linked there)"""
        with tempfile.TemporaryDirectory() as tmp:
            stub = Path(tmp) / "armstub"
            stub.write_bytes(bytes(3 << 20))
            image = Path(tmp) / "Image"
            header = bytearray(64)
            header[16:24] = (1 << 20).to_bytes(8, "little")    # image_size
            header[56:60] = b"ARM\x64"
            image.write_bytes(header)
            for kernel, where in ((image, "0x200000-0x2fffff"),
                                  (GUEST, "0x200000-")):
                with self.subTest(kernel=kernel.name):
                    err = self.assertRefused(f"the kernel at {where}",
                                             "-M", "raspi5b,secure=on",
                                             "-bios", str(stub),
                                             "-kernel", str(kernel))
                    self.assertIn(" overlaps the armstub at 0x0-0x2fffff", err)

    def test_image_header(self):
        """Images whose header takes them into the VideoCore's memory: by
        the size it declares, BSS included, or by values that would wrap
        around"""
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp) / "Image"
            for text_offset, image_size in ((0, 1 << 30),
                                            ((1 << 64) - (1 << 20), 1 << 20),
                                            (0, (1 << 64) - (1 << 20))):
                header = bytearray(64)
                header[8:16] = text_offset.to_bytes(8, "little")
                header[16:24] = image_size.to_bytes(8, "little")
                header[56:60] = b"ARM\x64"
                image.write_bytes(header)
                with self.subTest(text_offset=hex(text_offset),
                                  image_size=hex(image_size)):
                    self.assertRefused(f"could not load kernel '{image}': "
                                       f"its text_offset {text_offset:#x} "
                                       f"and size {image_size:#x} take it "
                                       f"past {VC_RAM_BASE:#x}, where the "
                                       "VideoCore's memory starts",
                                       "-M", "raspi5b,secure=on",
                                       "-bios", str(GUEST),
                                       "-kernel", str(image))

    def test_elf_entry(self):
        """An ELF whose entry point, where the armstub would jump, lies
        outside every segment it loads, if only in a gap between two"""
        one = ((0x200000, bytes(16)),)
        two = one + ((0x400000, bytes(16)),)
        with tempfile.TemporaryDirectory() as tmp:
            kernel = Path(tmp) / "kernel.elf"
            for entry, segments in ((0x40000000, one), (0x1ffffc, one),
                                    (0x200010, one), (0x300000, two)):
                kernel.write_bytes(elf(entry, *segments))
                with self.subTest(entry=hex(entry)):
                    self.assertRefused(f"could not load kernel '{kernel}': "
                                       f"its entry point {entry:#x} lies "
                                       "outside every segment it loads",
                                       "-M", "raspi5b,secure=on",
                                       "-bios", str(GUEST),
                                       "-kernel", str(kernel))

            # In the second segment, it is accepted: the machine goes on to
            # dump its device tree, and exits
            kernel.write_bytes(elf(0x400008, *two))
            result = subprocess.run([str(QEMU), "-display", "none",
                                     "-M", "raspi5b,secure=on,"
                                     f"dumpdtb={Path(tmp) / 'dtb'}",
                                     "-bios", str(GUEST),
                                     "-kernel", str(kernel)],
                                    stdin=subprocess.DEVNULL,
                                    capture_output=True, text=True,
                                    timeout=TIMEOUT)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_past_videocore(self):
        """A device tree placed where it runs into the VideoCore's memory,
        which everything -bios loads must stay below, and a kernel ending
        in it, with an initrd that would have followed"""
        message = ("the armstub, kernel, initrd and device tree must fit "
                   f"below {VC_RAM_BASE:#x}")
        self.assertRefused(message, "-M", "raspi5b,secure=on,"
                           f"dtb-address={VC_RAM_BASE - 8:#x}",
                           "-bios", str(GUEST))
        with tempfile.TemporaryDirectory() as tmp:
            kernel = Path(tmp) / "kernel.elf"
            kernel.write_bytes(elf(VC_RAM_BASE - 8,
                                   (VC_RAM_BASE - 8, bytes(16))))
            self.assertRefused(message, "-M", "raspi5b,secure=on",
                               "-bios", str(GUEST), "-kernel", str(kernel),
                               "-initrd", str(kernel))


if __name__ == "__main__":
    unittest.main()
