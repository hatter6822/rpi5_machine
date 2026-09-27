# SPDX-License-Identifier: GPL-2.0-or-later
#
# Boot real firmware with -bios, as the Raspberry Pi firmware would: TF-A's
# rpi5 BL31 starting the bare-metal guests at EL2 and serving their PSCI
# calls, then Linux, U-Boot and Linux through it, and the EDK2 port to its
# UEFI shell, without storage and from an SD card. Runs from 'make
# check-firmware', which builds and fetches the firmware at pinned
# versions (scripts/firmware) and points FIRMWARE at it; skipped otherwise.

import os
import select
import shutil
import struct
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

import fdt
from test_dtb import FIRMWARE_ARGS
from test_hello import GUEST, QEMU, TIMEOUT, boot
from test_suite import DT_MODES, DTS, SUITE, SuiteChecks, run_suite

FIRMWARE = Path(os.environ.get("FIRMWARE", "/nonexistent"))
BL31 = FIRMWARE / "bl31.bin"
UBOOT = FIRMWARE / "u-boot.bin"
EDK2 = FIRMWARE / "RPI_EFI.fd"
EDK2_DTB = FIRMWARE / "edk2" / "bcm2712-rpi-5-b.dtb"
KERNEL = FIRMWARE / "kernel_2712.img"
FIRMWARE_DTB = FIRMWARE / "bcm2712-rpi-5-b.dtb"

# What TF-A prints each time it starts
TFA_BANNER = "NOTICE:  BL31: v2.15.0(release)"

# The suite's tests for EL3, which runs TF-A here
EL3_ONLY = {"timer/secure-physical", "gic/security-groups"}

# The end of a Linux boot without storage: the root file system's device
# never appears
CMDLINE = "console=ttyAMA10,115200 root=/dev/mmcblk0p2 rootwait"
ROOT_WAIT = "Waiting for root device /dev/mmcblk0p2..."

# Where U-Boot finds the kernel: a -device loader image, clear of U-Boot,
# the device tree and booti's decompression buffer (kernel_comp_addr_r)
UBOOT_KERNEL_ADDR = 0x10000000

# Linux and the UEFI shell take longer than the bare-metal guests
BOOT_TIMEOUT = 180

# F1 on a VT100 terminal, which EDK2 reads as its shell's hotkey
F1 = "\x1bOP"

# A card to boot from: 4 GiB, an SDHC card (sparse; QEMU's cards are a
# power of 2 in size), with one ext4 partition from 1 MiB, the root file
# system, whose /boot holds the kernel and the extlinux.conf U-Boot follows
SD_SIZE = 4 << 30
SD_ROOT_START = 2048                    # in 512-byte sectors
SD_ROOT_SIZE = 48 << 20
SD_CMDLINE = "console=ttyAMA10,115200 root=/dev/mmcblk0p1 rootwait"
SD_FOUND = "mmc0: new high speed SDHC card at address"
# Where these boots end: the root has no init to run
ROOT_MOUNTED = "VFS: Mounted root (ext4 filesystem) readonly on device 179:1."

# What the machine changes in the firmware's tree, checked in
FIXUPS = Path(__file__).with_name("raspi5b-firmware-fixups.txt")


def command_line(dtb):
    """The kernel's command line as the firmware builds it: the tree's
    bootargs, its own arguments, then cmdline.txt's, which -append plays,
    two spaces apart"""
    parts = [FIRMWARE_ARGS, CMDLINE]
    if dtb:
        bootargs = fdt.load(dtb)["/chosen"]["bootargs"]
        parts.insert(0, bootargs.rstrip(b"\0").decode())
    return "  ".join(parts)


def make_sd_card(path):
    """Write the card to boot from at @path."""
    root = path.parent / "root"
    (root / "boot/extlinux").mkdir(parents=True)
    shutil.copy(KERNEL, root / "boot")
    (root / "boot/extlinux/extlinux.conf").write_text(
        "default linux\n"
        "label linux\n"
        f"    kernel /boot/{KERNEL.name}\n"
        f"    append {SD_CMDLINE}\n")
    fs = path.parent / "root.img"
    with open(fs, "wb") as f:
        f.truncate(SD_ROOT_SIZE)
    subprocess.run(["mkfs.ext4", "-q", "-F", "-d", str(root), str(fs)],
                   check=True)
    mbr = bytearray(512)
    mbr[446 + 4] = 0x83                                 # Linux
    mbr[446 + 8:446 + 16] = struct.pack("<II", SD_ROOT_START,
                                        SD_ROOT_SIZE // 512)
    mbr[510:512] = b"\x55\xaa"
    with open(path, "wb") as card, open(fs, "rb") as src:
        card.truncate(SD_SIZE)
        card.write(mbr)
        card.seek(SD_ROOT_START * 512)
        shutil.copyfileobj(src, card)


def converse(args, script, timeout=BOOT_TIMEOUT):
    """Run QEMU with its console on stdio, following @script, and return
    (whether the last step's text appeared, the console output).

    @script is a list of (expect, send, every) steps: once the output since
    the previous step shows @expect, @send is typed, and typed again every
    @every seconds, if given, until the next step's @expect shows. The last
    step's @expect ends the run.
    """
    cmd = [str(QEMU), "-display", "none", "-monitor", "none",
           "-serial", "stdio", *args]
    out, mark, step, reached = b"", 0, 0, False
    repeat, sent = None, 0.0
    deadline = time.monotonic() + timeout
    with subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT) as proc:
        try:
            while not reached and time.monotonic() < deadline:
                ready, _, _ = select.select([proc.stdout], [], [], 0.1)
                if ready:
                    data = os.read(proc.stdout.fileno(), 65536)
                    if not data:
                        break
                    out += data
                expect, send, every = script[step]
                if expect.encode() in out[mark:]:
                    if step == len(script) - 1:
                        reached = True
                        break
                    mark, step = len(out), step + 1
                    repeat = (send.encode(), every) if every else None
                    proc.stdin.write(send.encode())
                    proc.stdin.flush()
                    sent = time.monotonic()
                elif repeat and time.monotonic() - sent >= repeat[1]:
                    proc.stdin.write(repeat[0])
                    proc.stdin.flush()
                    sent = time.monotonic()
        finally:
            proc.kill()
    return reached, out.decode(errors="replace")


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
        for boot_nr in (2, 3):
            self.assertIn(f"# boot {boot_nr}: mbox/tryboot reset", out)
        self.assertIn("# boot 4: pm/watchdog-reset reset", out)
        for boot_nr in range(5, 9):
            self.assertIn(f"# boot {boot_nr}: reset/system-reset reset", out)
        self.assertNotIn("boot 9", out)
        self.assertEqual(out.count(TFA_BANNER), 8, out)


@unittest.skipUnless(os.environ.get("FIRMWARE"),
                     "run through 'make check-firmware'")
@unittest.skipUnless(QEMU.exists(), "build QEMU first (make build)")
class BootTest(unittest.TestCase):
    """The Pi 5's boot chains, from TF-A (or EDK2's own) to an operating
    system's first need for storage, on the built-in device tree and on
    the one the firmware ships"""

    def assertLinuxBooted(self, out, dtb):
        self.assertIn("Machine model: Raspberry Pi 5 Model B Rev 1.0", out)
        self.assertIn("psci: PSCIv1.1 detected in firmware.", out)
        self.assertIn("smp: Brought up 1 node, 4 CPUs", out)
        self.assertIn(f"Kernel command line: {command_line(dtb)}", out)
        self.assertIn("KASLR enabled", out)
        for line in out.splitlines():
            self.assertNotRegex(line, r"WARNING|Oops|BUG:|Call trace|"
                                      r"firmware out-of-date")

    def test_device_tree(self):
        """The firmware's own tree, booted as the firmware boots it, gets
        the changes checked in"""
        tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, tmp)
        out = tmp / "fixed.dtb"
        subprocess.run([str(QEMU), "-M", f"raspi5b,secure=on,dumpdtb={out}",
                        "-display", "none", "-bios", str(BL31),
                        "-kernel", str(KERNEL), "-dtb", str(FIRMWARE_DTB),
                        "-append", CMDLINE],
                       stdin=subprocess.DEVNULL, capture_output=True,
                       check=True, timeout=TIMEOUT)
        expected = [line for line in FIXUPS.read_text().splitlines()
                    if not line.startswith("#")]
        self.assertEqual(fdt.diff(fdt.load(FIRMWARE_DTB), fdt.load(out)),
                         expected)

    def test_linux(self):
        """The firmware's own chain: TF-A enters the kernel at EL2"""
        for dtb in (None, FIRMWARE_DTB):
            with self.subTest(dtb=dtb and dtb.name):
                args = ["-M", "raspi5b,secure=on", "-bios", str(BL31),
                        "-kernel", str(KERNEL), "-append", CMDLINE]
                if dtb:
                    args += ["-dtb", str(dtb)]
                reached, out = converse(args, [(ROOT_WAIT, None, None)])
                self.assertTrue(reached, out)
                self.assertIn(TFA_BANNER, out)
                self.assertIn("CPU: All CPU(s) started at EL2", out)
                self.assertLinuxBooted(out, dtb)

    def test_u_boot_linux(self):
        """TF-A enters U-Boot, which boots the kernel with booti"""
        for dtb in (None, FIRMWARE_DTB):
            with self.subTest(dtb=dtb and dtb.name):
                args = ["-M", "raspi5b,secure=on", "-bios", str(BL31),
                        "-kernel", str(UBOOT), "-append", CMDLINE,
                        "-device", f"loader,file={KERNEL},"
                        f"addr={UBOOT_KERNEL_ADDR:#x},force-raw=on"]
                if dtb:
                    args += ["-dtb", str(dtb)]
                reached, out = converse(args, [
                    ("Hit any key to stop autoboot", "\r", None),
                    ("U-Boot> ", f"booti {UBOOT_KERNEL_ADDR:#x} - "
                                 "${fdt_addr}\r", None),
                    (ROOT_WAIT, None, None),
                ])
                self.assertTrue(reached, out)
                self.assertIn(TFA_BANNER, out)
                self.assertIn("U-Boot 2026.07", out)
                self.assertIn("RPI 5 Model B (0xb04170)", out)
                self.assertIn("Starting kernel ...", out)
                self.assertLinuxBooted(out, dtb)

    def test_edk2_shell(self):
        """The EDK2 release as its config.txt boots it: its image as the
        armstub and its device tree at 0x1f0000. F1 in the boot countdown
        starts the UEFI shell, which answers a command."""
        args = ["-M", "raspi5b,secure=on,dtb-address=0x1f0000",
                "-bios", str(EDK2), "-dtb", str(EDK2_DTB)]
        reached, out = converse(args, [
            ("F1 (shell)", F1, 0.5),
            ("Shell>", "ver\r", None),
            ("UEFI v2.70 (worproject, 0x00010000)", None, None),
        ])
        self.assertTrue(reached, out)
        self.assertIn("UEFI firmware (version v0.3", out)
        self.assertIn("UEFI Interactive Shell", out)


@unittest.skipUnless(os.environ.get("FIRMWARE"),
                     "run through 'make check-firmware'")
@unittest.skipUnless(QEMU.exists(), "build QEMU first (make build)")
@unittest.skipUnless(shutil.which("mkfs.ext4"), "needs mkfs.ext4 (e2fsprogs)")
class SdBootTest(unittest.TestCase):
    """The boot chains with a card in the SD card slot: Linux mounts its
    root file system from it, U-Boot loads Linux from it as its
    extlinux.conf says, and EDK2 finds its partition"""

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp())
        cls.card = cls.tmp / "sd.img"
        make_sd_card(cls.card)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp)

    def sd_args(self):
        # snapshot=on: no boot changes the card for the next
        return ["-drive", f"if=sd,file={self.card},format=raw,snapshot=on"]

    def assertRootMounted(self, reached, out):
        """Linux found the card and mounted the root file system from
        it, with no warning on the way there"""
        self.assertTrue(reached, out)
        out = out[:out.index(ROOT_MOUNTED)]
        self.assertIn("mmc0: SDHCI controller on 1000fff000.mmc", out)
        self.assertIn(SD_FOUND, out)
        self.assertIn(" mmcblk0: p1\n", out.replace("\r\n", "\n"))
        for line in out.splitlines():
            self.assertNotRegex(line, r"WARNING|Oops|BUG:|Call trace")

    def test_linux(self):
        """TF-A enters the kernel, which mounts its root from the card"""
        for dtb in (None, FIRMWARE_DTB):
            with self.subTest(dtb=dtb and dtb.name):
                args = ["-M", "raspi5b,secure=on", "-bios", str(BL31),
                        "-kernel", str(KERNEL), "-append", SD_CMDLINE,
                        *self.sd_args()]
                if dtb:
                    args += ["-dtb", str(dtb)]
                self.assertRootMounted(*converse(args,
                                                 [(ROOT_MOUNTED, None, None)]))

    def test_u_boot(self):
        """U-Boot boots by itself from the card: its extlinux.conf, the
        kernel it names, and the root file system beside them"""
        for dtb in (None, FIRMWARE_DTB):
            with self.subTest(dtb=dtb and dtb.name):
                args = ["-M", "raspi5b,secure=on", "-bios", str(BL31),
                        "-kernel", str(UBOOT), *self.sd_args()]
                if dtb:
                    args += ["-dtb", str(dtb)]
                reached, out = converse(args, [(ROOT_MOUNTED, None, None)])
                self.assertIn("MMC:   mmc@fff000: 0", out)
                self.assertIn("with extlinux", out)
                self.assertIn(f"Retrieving file: /boot/{KERNEL.name}", out)
                self.assertIn(f"Kernel command line: {SD_CMDLINE}", out)
                self.assertRootMounted(reached, out)

    def test_edk2(self):
        """The EDK2 release maps the card and its partition"""
        args = ["-M", "raspi5b,secure=on,dtb-address=0x1f0000",
                "-bios", str(EDK2), "-dtb", str(EDK2_DTB), *self.sd_args()]
        reached, out = converse(args, [
            ("F1 (shell)", F1, 0.5),
            ("Shell>", "map -r\r", None),
            ("Shell>", None, None),
        ])
        self.assertTrue(reached, out)
        mapped = out[out.rindex("map -r"):]
        self.assertIn("/SD(0x0)", mapped)
        self.assertIn(f"/HD(1,MBR,0x00000000,{SD_ROOT_START:#x},"
                      f"{SD_ROOT_SIZE // 512:#x})", mapped)


if __name__ == "__main__":
    unittest.main()
