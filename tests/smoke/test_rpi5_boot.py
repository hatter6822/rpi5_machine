# SPDX-License-Identifier: GPL-2.0-or-later
#
# scripts/rpi5-boot, which boots an SD card image on raspi5b as a
# Raspberry Pi 5's firmware boots the card: config.txt and the files it
# names, the device tree with its overlays and parameters, the command
# line, the card's partitions, and QEMU run once per boot. Reading and
# writing images needs mtools, device trees are compiled with dtc, and
# overlays are applied with dtmerge (make build, or DTMERGE); the boots
# need QEMU and the guests (make build guest). Under 'make
# check-firmware' (FIRMWARE set), Raspberry Pi OS's kernel boots from a
# card laid out as Raspberry Pi OS's, with the pinned release's overlays,
# through a first boot that rewrites the card and reboots.

import importlib.machinery
import importlib.util
import json
import os
import shlex
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest import mock

import fdt
from test_dtb import FIRMWARE_ARGS
from test_hello import GUEST, QEMU, ROOT
from test_suite import DTS

SCRIPT = ROOT / "scripts" / "rpi5-boot"
FIRSTBOOT = GUEST.with_name("firstboot.elf")
REBOOT_GUEST = GUEST.with_name("reboot.elf")

FIRMWARE = Path(os.environ.get("FIRMWARE", "/nonexistent"))
# The firmware's dtoverlay code, which rpi5-boot applies overlays with
DTMERGE = Path(os.environ.get("DTMERGE") or ROOT / "build" / "dtmerge")
OVERLAYS = FIRMWARE / "overlays"
KERNEL = FIRMWARE / "kernel_2712.img"
FIRMWARE_DTB = FIRMWARE / "bcm2712-rpi-5-b.dtb"

HAVE_MTOOLS = all(shutil.which(tool) for tool in ("mformat", "mcopy",
                                                  "mmd", "mdir"))
HAVE_DTC = all(shutil.which(tool) for tool in ("dtc", "fdtget", "fdtput"))
HAVE_DTMERGE = HAVE_DTC and os.access(DTMERGE, os.X_OK)

# How long rpi5-boot may take to run a bare-metal guest, and Linux twice
RUN_TIMEOUT = 60
LINUX_TIMEOUT = 300


def load_script():
    """scripts/rpi5-boot as a module, for its parts, leaving no bytecode
    beside it"""
    loader = importlib.machinery.SourceFileLoader("rpi5_boot", str(SCRIPT))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    dont_write = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    try:
        loader.exec_module(module)
    finally:
        sys.dont_write_bytecode = dont_write
    return module


rb = load_script()


def dtc(source):
    """@source, a device tree or an overlay, compiled with its labels as
    the Raspberry Pi's trees are (dtc -@)"""
    return subprocess.run(["dtc", "-q", "-@", "-I", "dts", "-O", "dtb",
                           "-o", "-", "-"], input=source.encode(),
                          capture_output=True, check=True).stdout


def children(pid):
    """The processes whose parent is process @pid"""
    found = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            stat = (entry / "stat").read_text()
        except OSError:
            continue
        # The command name, in parentheses, may hold anything
        if int(stat.rsplit(")", 1)[1].split()[1]) == pid:
            found.append(int(entry.name))
    return found


def running(pid):
    """Whether process @pid runs: it exists, and is no zombie"""
    try:
        stat = Path(f"/proc/{pid}/stat").read_text()
    except OSError:
        return False
    return stat.rsplit(")", 1)[1].split()[0] != "Z"


def kill_group(pgid):
    """SIGKILL whatever is left of process group @pgid"""
    try:
        os.killpg(pgid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def cells(*values):
    return b"".join(struct.pack(">I", v) for v in values)


def string(text):
    return text.encode() + b"\0"


class Logs:
    """What rpi5-boot logs while this is in effect, line by line"""

    def __init__(self):
        self.lines = []
        self.patch = mock.patch.object(rb, "log", self.lines.append)

    def __enter__(self):
        self.patch.start()
        return self.lines

    def __exit__(self, *exc):
        self.patch.stop()


class FakeFs:
    """A boot partition in memory, looked up as rpi5-boot's BootFs looks
    names up: without regard to case"""

    def __init__(self, files):
        self.data = {}
        for name, data in files.items():
            if isinstance(data, str):
                data = data.encode()
            self.data[rb.BootFs.normalize(name).lower()] = data

    def exists(self, name):
        return rb.BootFs.normalize(name).lower() in self.data

    def read(self, name):
        try:
            return self.data[rb.BootFs.normalize(name).lower()]
        except KeyError:
            raise rb.BootError(f"{name} is not in the boot partition") \
                from None

    def copy(self, name, dest):
        Path(dest).write_bytes(self.read(name))


# ---------------------------------------------------------------------------
# Cards

SECTOR = 512


def mtools(*args):
    subprocess.run(args, check=True, capture_output=True,
                   env=dict(os.environ, MTOOLS_SKIP_CHECK="1"))


def fat_files(spec, files, tmp):
    """Copy @files ({name: bytes, str or Path}) into the FAT file system
    mtools names @spec"""
    made = set()
    for name, data in files.items():
        parts = name.split("/")[:-1]
        for i in range(1, len(parts) + 1):
            folder = "/".join(parts[:i])
            if folder not in made:
                mtools("mmd", "-i", spec, f"::/{folder}")
                made.add(folder)
        if isinstance(data, Path):
            src = data
        else:
            src = Path(tmp) / "file"
            src.write_bytes(data.encode() if isinstance(data, str) else data)
        # mcopy matches the target's directory as a pattern, and takes
        # the new file's name as it is
        folder = "".join(part.replace("[", "[[]") + "/" for part in parts)
        leaf = name.split("/")[-1]
        mtools("mcopy", "-i", spec, str(src), f"::/{folder}{leaf}")


def format_fat(image, start, sectors, files, fat32=False):
    """A FAT file system of @sectors at sector @start of @image, holding
    @files"""
    spec = f"{image}@@{start * SECTOR}"
    mtools("mformat", "-i", spec, *(["-F"] if fat32 else []),
           "-T", str(sectors), "-H", str(start), "::")
    with tempfile.TemporaryDirectory() as tmp:
        fat_files(spec, files, tmp)


def mbr(entries, disk_id=0x12345678):
    """A master boot record: @entries are (type, first sector, sectors)"""
    sector = bytearray(SECTOR)
    struct.pack_into("<I", sector, 440, disk_id)
    for i, (kind, first, count) in enumerate(entries):
        struct.pack_into("<B3xB3xII", sector, 446 + 16 * i, 0, kind, first,
                         count)
    sector[510:512] = b"\x55\xaa"
    return bytes(sector)


def blank_image(path, size):
    with open(path, "wb") as f:
        f.truncate(size)


def write_at(path, offset, data):
    with open(path, "r+b") as f:
        f.seek(offset)
        f.write(data)


def make_card(path, files, size=32 << 20, disk_id=0x12345678,
              boot_start=2048, boot_sectors=16384, fat32=False):
    """A card laid out as Raspberry Pi OS's: the boot partition, FAT,
    holding @files, then a Linux partition to the end"""
    blank_image(path, size)
    root_start = boot_start + boot_sectors
    write_at(path, 0, mbr([(0x0c, boot_start, boot_sectors),
                           (0x83, root_start, size // SECTOR - root_start)],
                          disk_id))
    format_fat(path, boot_start, boot_sectors, files, fat32)
    return path


def read_card_file(path, name, boot_start=2048):
    """@name, read from the boot partition of the card at @path"""
    return subprocess.run(["mcopy", "-n", "-i",
                           f"{path}@@{boot_start * SECTOR}", f"::/{name}",
                           "-"], capture_output=True, check=True,
                          env=dict(os.environ,
                                   MTOOLS_SKIP_CHECK="1")).stdout


def newc(files):
    """A cpio archive in the newc format, as an initramfs is: @files maps
    a name to None for a directory, else to (mode, bytes)"""
    out = bytearray()
    entries = list(files.items()) + [("TRAILER!!!", (0, b""))]
    for ino, (name, entry) in enumerate(entries, 1):
        mode, data = (0o40755, b"") if entry is None else entry
        if name == "TRAILER!!!":
            ino = 0
        header = "070701" + "".join(f"{v:08x}" for v in (
            ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0,
            len(name) + 1, 0))
        out += header.encode() + name.encode() + b"\0"
        out += bytes(-len(out) % 4)
        out += data
        out += bytes(-len(out) % 4)
    return bytes(out)


def run_boot(*args, timeout=RUN_TIMEOUT, env=None):
    """Run rpi5-boot to its end: (exit status, stdout, stderr)"""
    proc = subprocess.run([sys.executable, str(SCRIPT), *map(str, args)],
                          stdin=subprocess.DEVNULL, capture_output=True,
                          timeout=timeout, env=env)
    return (proc.returncode, proc.stdout.decode(errors="replace"),
            proc.stderr.decode(errors="replace"))


# ---------------------------------------------------------------------------
# Device trees, overlays and parameters

# A Raspberry Pi 5's tree, cut down: its serial and I2C aliases, and
# parameters of each kind the firmware's trees have
BASE_DTS = """
/dts-v1/;

/memreserve/ 0x0 0x1000;

/ {
    compatible = "raspberrypi,5-model-b", "brcm,bcm2712";
    #address-cells = <2>;
    #size-cells = <1>;

    aliases {
        serial0 = &uart0;
        serial1 = &uart1;
        serial10 = &uart10;
        console = &uart10;
        bluetooth = &bt;
        i2c0 = &i2c0;
        i2c1 = &i2c1;
    };

    chosen {
        bootargs = "reboot=w coherent_pool=1M";
    };

    leds {
        led_act: led-act {
            label = "ACT";
            linux,default-trigger = "mmc0";
        };
    };

    soc: soc@107c000000 {
        compatible = "simple-bus";
        #address-cells = <1>;
        #size-cells = <1>;
        ranges = <0x7c000000 0x10 0x7c000000 0x4000000>;

        uart10: serial@7d001000 {
            compatible = "arm,pl011", "arm,primecell";
            reg = <0x7d001000 0x200>;
        };

        uart0: serial@7d002000 {
            compatible = "arm,pl011-axi";
            reg = <0x7d002000 0x200>;
            status = "disabled";
        };

        uart1: serial@7d50c000 {
            compatible = "brcm,bcm7271-uart";
            reg = <0x7d50c000 0x20>;

            bt: bluetooth {
                compatible = "brcm,bcm43438-bt";
            };
        };

        i2c0: i2c@7d005000 {
            compatible = "brcm,brcmstb-i2c";
            reg = <0x7d005000 0x58>;
            clock-frequency = <100000>;
            status = "disabled";
        };

        i2c1: i2c@7d005600 {
            compatible = "brcm,brcmstb-i2c";
            reg = <0x7d005600 0x58>;
            clock-frequency = <100000>;
            status = "disabled";
        };
    };

    __overrides__ {
        uart0 = <&uart0>, "status";
        i2c0 = <&i2c0>, "status";
        i2c1 = <&i2c1>, "status";
        i2c0_baudrate = <&i2c0>, "clock-frequency:0";
        act_led_trigger = <&led_act>, "linux,default-trigger";
        krnbt = <&bt>, "status";
        neg = <&uart0>, "clock-frequency:-4";
    };
};
"""

# An overlay with a fragment for a node of the base without a phandle, one
# that adds a node referring to the base and to itself, a dormant one, one
# for the command line, and a parameter of each kind
OVERLAY_DTS = """
/dts-v1/;
/plugin/;

/ {
    compatible = "brcm,bcm2712";

    fragment@0 {
        target = <&uart10>;
        frag0: __overlay__ {
            current-speed = <115200>;
            status = "okay";
        };
    };

    fragment@1 {
        target-path = "/";
        __overlay__ {
            rtc: rtc@68 {
                compatible = "test,rtc";
                reg = <0x68>;
                uart = <&uart0>;
                self = <&rtc>;
                speed-mode = "normal";
                label = "rtc";
            };
        };
    };

    fragment@2 {
        target = <&i2c0>;
        __dormant__ {
            status = "okay";
        };
    };

    fragment@3 {
        target-path = "/chosen";
        __overlay__ {
            bootargs = "overlay=1";
        };
    };

    __overrides__ {
        speed = <&frag0>, "current-speed:0";
        flow = <&frag0>, "uart-has-rtscts?";
        noecho = <&frag0>, "echo!";
        label = <&rtc>, "label";
        mac = <&rtc>, "local-mac-address[";
        i2c = <0>, "=2";
        mode = <&rtc>, "speed-mode{fast=high,slow=low}";
        irq = <&rtc>, "interrupts:0{a=5,b=6}";
        fixed = <&rtc>, "label=fixed";
        rtc = <&rtc>, "status";
        addr = <&rtc>, "reg:0";
    };
};
"""

# The overlay map of the firmware, cut down
OVERLAY_MAP_DTS = """
/dts-v1/;

/ {
    test-old {
        renamed = "test";
    };
    test-gone {
        deprecated = "use test instead";
    };
    test-pi4 {
        bcm2711;
    };
    test-mapped {
        bcm2711 = "test-pi4";
        bcm2712 = "test,speed=1200";
    };
    test {
        bcm2712;
    };
};
"""


def compose(config, files=None, verbose=False):
    """The tree config.txt makes of BASE_DTS, with the overlay above as
    "test" and its map: (the tree as fdt.parse gives it, what was
    logged)"""
    card = {"config.txt": config, "kernel_2712.img": b"kernel",
            "bcm2712-rpi-5-b.dtb": dtc(BASE_DTS),
            "overlays/test.dtbo": dtc(OVERLAY_DTS),
            "overlays/overlay_map.dtb": dtc(OVERLAY_MAP_DTS)}
    card.update(files or {})
    fs = FakeFs(card)
    with Logs() as logs:
        config = rb.Config(fs, "config.txt", rb.BootVars(1), verbose)
        boot = rb.Boot(fs, config)
        rb.choose_files(boot, verbose)
        rb.compose_tree(boot, verbose, str(DTMERGE))
    return fdt.parse(boot.tree), logs


@unittest.skipUnless(HAVE_DTMERGE, "needs dtc, fdtget, fdtput and dtmerge "
                     "(make build)")
class OverlayTest(unittest.TestCase):
    """config.txt's dtoverlay and dtparam lines, applied as the firmware
    applies them"""

    UART10 = "/soc@107c000000/serial@7d001000"
    UART0 = "/soc@107c000000/serial@7d002000"
    I2C0 = "/soc@107c000000/i2c@7d005000"
    I2C1 = "/soc@107c000000/i2c@7d005600"

    def test_merge(self):
        # dtc gives every node with a label a phandle; the first fragment's
        # target is left without one
        with tempfile.TemporaryDirectory() as tmp:
            dtb = Path(tmp, "base.dtb")
            dtb.write_bytes(dtc(BASE_DTS))
            subprocess.run(["fdtput", "-d", dtb, self.UART10, "phandle"],
                           check=True)
            base = dtb.read_bytes()
        base_max = max(struct.unpack(">I", node["phandle"])[0]
                       for node in fdt.parse(base).values()
                       if "phandle" in node)
        tree, logs = compose("dtoverlay=test\n",
                             files={"bcm2712-rpi-5-b.dtb": base})
        self.assertEqual(logs, [])
        uart10 = tree[self.UART10]
        self.assertEqual(uart10["status"], string("okay"))
        self.assertEqual(uart10["current-speed"], cells(115200))
        # which gets the next phandle
        self.assertEqual(uart10["phandle"], cells(base_max + 1))
        rtc = tree["/rtc@68"]
        self.assertEqual(rtc["uart"], tree[self.UART0]["phandle"])
        self.assertEqual(rtc["self"], rtc["phandle"])
        self.assertGreater(struct.unpack(">I", rtc["phandle"])[0],
                           base_max + 1)
        # A dormant fragment stays out; the command line is appended to
        self.assertEqual(tree[self.I2C0]["status"], string("disabled"))
        self.assertEqual(tree["/chosen"]["bootargs"],
                         string("reboot=w coherent_pool=1M overlay=1"))
        # The overlay's labels are its own: it exports none
        self.assertNotIn("rtc", tree["/__symbols__"])
        self.assertEqual(tree["/chosen"]["os_prefix"], string(""))
        self.assertEqual(tree["/chosen"]["overlay_prefix"],
                         string("overlays/"))

    def test_parameters(self):
        tree, logs = compose(
            "dtoverlay=test,speed=9600,flow,noecho=off,label=clock\n"
            "dtparam=mac=01:02:03,i2c=on,mode=fast,irq=b,rtc=on\n"
            "dtparam=addr=0x51\n")
        self.assertEqual(logs, [])
        uart10 = tree[self.UART10]
        self.assertEqual(uart10["current-speed"], cells(9600))
        self.assertEqual(uart10["uart-has-rtscts"], b"")
        self.assertEqual(uart10["echo"], b"")
        self.assertEqual(tree[self.I2C0]["status"], string("okay"))
        self.assertNotIn("/rtc@68", tree)
        rtc = tree["/rtc@51"]
        self.assertEqual(rtc["reg"], cells(0x51))
        self.assertEqual(rtc["label"], string("clock"))
        self.assertEqual(rtc["local-mac-address"], b"\x01\x02\x03")
        self.assertEqual(rtc["speed-mode"], string("high"))
        self.assertEqual(rtc["interrupts"], cells(6))
        self.assertEqual(rtc["status"], string("okay"))

        tree, _ = compose("dtoverlay=test,fixed,flow=off,i2c=off,rtc=off\n")
        self.assertEqual(tree["/rtc@68"]["label"], string("fixed"))
        self.assertNotIn("uart-has-rtscts", tree[self.UART10])
        self.assertEqual(tree[self.I2C0]["status"], string("disabled"))
        self.assertEqual(tree["/rtc@68"]["status"], string("disabled"))

    def test_base_parameters(self):
        """A parameter the open overlay lacks is the base tree's, as is
        every one once "dtoverlay=" closes it; I2C's synonyms are made
        when the tree has no "i2c" alias"""
        tree, logs = compose(
            "dtparam=uart0=on,act_led_trigger=heartbeat\n"
            "dtoverlay=test\n"
            "dtparam=speed=4800,i2c_arm=on\n"
            "dtoverlay=\n"
            "dtparam=i2c_vc,i2c_arm_baudrate=400000\n"
            "dtparam=speed=1\n", verbose=True)
        self.assertIn("unknown parameter 'speed'", logs)
        self.assertEqual(tree[self.UART0]["status"], string("okay"))
        self.assertEqual(tree["/leds/led-act"]["linux,default-trigger"],
                         string("heartbeat"))
        self.assertEqual(tree[self.UART10]["current-speed"], cells(4800))
        self.assertEqual(tree[self.I2C0]["status"], string("okay"))
        self.assertEqual(tree[self.I2C0]["clock-frequency"], cells(400000))
        self.assertEqual(tree[self.I2C1]["status"], string("okay"))

        with_alias = BASE_DTS.replace("i2c0 = &i2c0;", "i2c = &i2c0;")
        tree, logs = compose("dtparam=i2c_arm=on\n", verbose=True,
                             files={"bcm2712-rpi-5-b.dtb": dtc(with_alias)})
        self.assertEqual(logs[-1], "unknown parameter 'i2c_arm'")
        self.assertEqual(tree[self.I2C0]["status"], string("disabled"))

    def test_failures(self):
        """What cannot be applied is skipped, as the firmware skips it"""
        tree, logs = compose("dtoverlay=nosuch\n"
                             "dtoverlay=test,speed=fast\n"
                             "dtparam=nosuch=1\n"
                             "dtparam=neg=0x55\n"
                             "dtoverlay=broken\n",
                             files={"overlays/broken.dtbo": b"junk"})
        self.assertEqual(logs[:2], [
            "failed to load overlay 'nosuch': failed to open "
            "'overlays/nosuch.dtbo'",
            "failed to set speed=fast: invalid override value 'fast' - "
            "ignored"])
        # dtmerge writes outside the property for a negative offset, and
        # may crash for it; whatever it says, the parameter is skipped
        self.assertTrue(logs[2].startswith("failed to set neg=0x55: "),
                        logs[2])
        self.assertTrue(logs[-1].startswith(
            "failed to load overlay 'broken': "), logs[-1])
        self.assertEqual(tree[self.UART10]["current-speed"], cells(115200))
        # A byte that is not UTF-8 is written as it is, as dtmerge does
        tree, logs = compose(b"dtoverlay=test,label=a\xffb\n"
                             b"os_prefix=\xe9/\n",
                             files={"\udce9/kernel_2712.img": b"",
                                    "\udce9/bcm2712-rpi-5-b.dtb":
                                    dtc(BASE_DTS)})
        self.assertEqual(logs, [])
        self.assertEqual(tree["/rtc@68"]["label"], b"a\xffb\0")
        self.assertEqual(tree["/chosen"]["os_prefix"], b"\xe9/\0")
        # An unknown parameter is only reported when asked
        _, logs = compose("dtparam=nosuch=1\n", verbose=True)
        self.assertIn("unknown parameter 'nosuch'", logs)

    def test_overlay_map(self):
        """The map renames overlays, and turns away those the Pi 5 does
        not support"""
        tree, logs = compose("dtoverlay=test-old\n")
        self.assertEqual(logs, ["overlay 'test-old' has been renamed "
                                "'test'"])
        self.assertIn("/rtc@68", tree)
        tree, logs = compose("dtoverlay=test-mapped,flow\n")
        self.assertEqual(logs, [])
        self.assertEqual(tree[self.UART10]["current-speed"], cells(1200))
        self.assertEqual(tree[self.UART10]["uart-has-rtscts"], b"")
        for name, message in (
                ("test-gone", "overlay 'test-gone' is deprecated: use test "
                 "instead"),
                ("test-pi4", "overlay 'test-pi4' is not supported on the "
                 "'bcm2712' platform")):
            with self.subTest(overlay=name):
                tree, logs = compose(f"dtoverlay={name}\n")
                self.assertEqual(logs, [message])
                self.assertNotIn("/rtc@68", tree)


# ---------------------------------------------------------------------------
# config.txt and the files it names

def config(text, files=None, partition=1, verbose=False, requested=0,
           reset=None):
    """config.txt evaluated for a Pi 5 booting from @partition, after
    @reset (a power-on's by default) and asked for @requested: (the
    Config, what was logged)"""
    card = {"config.txt": text}
    card.update(files or {})
    with Logs() as logs:
        result = rb.Config(FakeFs(card), "config.txt",
                           rb.BootVars(partition, requested, reset), verbose)
    return result, logs


def boot_files(text, files, verbose=False):
    """The files a boot loads, as config.txt @text chooses them from
    @files: (the Boot, what was logged)"""
    card = {"config.txt": text}
    card.update(files)
    fs = FakeFs(card)
    with Logs() as logs:
        boot = rb.Boot(fs, rb.Config(fs, "config.txt", rb.BootVars(1),
                                     verbose))
        rb.choose_files(boot, verbose)
    return boot, logs


class ConfigTest(unittest.TestCase):

    def test_format(self):
        """The format the firmware reads: the last setting of a name wins,
        names are not case-sensitive, a "#" after a space or at the start
        of a line starts a comment, lines are 98 characters at most"""
        long_value = "x" * 100
        conf, _ = config("# comment\n"
                         "kernel=first.img\r\n"
                         "KERNEL = second.img   # comment\n"
                         "cmdline cmd#line.txt\n"
                         f"os_prefix={long_value}\n"
                         "  arm_boost=1  \n"
                         "flag\n"
                         "empty=\n")
        self.assertEqual(conf.get("kernel"), "second.img")
        self.assertEqual(conf.get("cmdline"), "cmd#line.txt")
        self.assertEqual(conf.get("os_prefix"), "x" * (98 - 10))
        self.assertEqual(conf.get("arm_boost"), "1")
        self.assertEqual(conf.get("flag"), "")
        self.assertEqual(conf.get("empty"), "")
        self.assertIsNone(conf.get("comment"))
        self.assertEqual(conf.number("arm_boost", 0), 1)
        self.assertEqual(conf.number("missing", 7), 7)
        self.assertEqual(conf.number("kernel", 7), 0)

    def test_filters(self):
        """A filter replaces the one of its kind before it; filters of
        different kinds must all match; [all] clears them"""
        conf, _ = config(
            "[pi4]\n"
            "a=pi4\n"
            "[pi5]\n"
            "a=pi5\n"
            "[cm5]\n"
            "b=cm5\n"
            "[PI5]\n"
            "[tryboot]\n"
            "c=tryboot\n"
            "[all]\n"
            "[board-type=0x17]\n"
            "d=pi5\n"
            "[board-type=0x18]\n"
            "e=cm5\n"
            "[all]\n"
            "[0x89abcdef]\n"
            "f=serial\n"
            "[0x12345678]\n"
            "g=other-serial\n"
            "[all]\n"
            "[boot_partition=1]\n"
            "h=partition-1\n"
            "[boot_partition>1]\n"
            "i=partition-2\n"
            "[partition=0]\n"
            "j=no-request\n"
            "[boot_count&1=1]\n"
            "k=count\n"
            "[boot_arg1&1]\n"
            "l=arg\n"
            "[all]\n"
            "[gpio4=1]\n"
            "m=gpio\n"
            "[edid=DEL-DELL_U2422H]\n"
            "n=edid\n"
            "[all]\n"
            "[unknown]\n"
            "o=unknown\n"
            "[none]\n"
            "p=none\n"
            "[all]\n"
            "q=all\n")
        self.assertEqual(
            conf.values,
            {"a": "pi5", "d": "pi5", "f": "serial", "h": "partition-1",
             "j": "no-request", "k": "count", "q": "all"})

    def test_boot_filters(self):
        """The reset that starts the boot sets what [tryboot], boot_count
        and partition test: the tryboot flag, the boot count (8 bits)
        and the partition the OS asked for"""
        text = ("[tryboot]\na=tryboot\n[all]\n"
                "[boot_count=5]\nb=five\n[all]\n"
                "[partition=3]\nc=asked\n[all]\n"
                "[boot_count=0]\nd=wrapped\n")
        conf, _ = config(text, requested=3,
                         reset=rb.Reset(0x1000, rb.TRYBOOT_FLAG, 4))
        self.assertEqual(conf.values,
                         {"a": "tryboot", "b": "five", "c": "asked"})
        conf, _ = config(text, reset=rb.Reset(0x1000, 0, 255))
        self.assertEqual(conf.values, {"d": "wrapped"})

    def test_autoboot_filters(self):
        """autoboot.txt has [tryboot] beside [all] and [none], and no
        other filter"""
        fs = FakeFs({"autoboot.txt": "[pi5]\na=1\n[all]\n[tryboot]\nb=1\n"
                                     "[none]\nc=1\n[all]\nd=1\n"})
        for flags, values in ((0, {"d": "1"}),
                              (rb.TRYBOOT_FLAG, {"b": "1", "d": "1"})):
            conf = rb.Config(fs, "autoboot.txt", rb.AutobootVars(
                1, reset=rb.Reset(0x1000, flags, 0)))
            self.assertEqual(conf.values, values)

    def test_reset(self):
        """What a reset leaves, as the machine's properties read it"""
        power_on = rb.Reset()
        self.assertEqual((power_on.partition, power_on.tryboot,
                          power_on.boot_count, power_on.machine_options()),
                         (0, False, 1, []))
        # The watchdog's reset to partition 4 (bits 0, 2, .. 10), and a
        # tryboot flag; the next boot is the fourth
        watchdog = rb.Reset(0x30, 0x1, 3)
        self.assertEqual((watchdog.partition, watchdog.tryboot,
                          watchdog.boot_count), (4, True, 4))
        self.assertEqual(watchdog.machine_options(),
                         ["reset-status=0x30", "reboot-flags=0x1",
                          "boot-count=3"])
        self.assertEqual(rb.Reset(0x555).partition, 63)
        self.assertEqual(rb.Reset(0x1000, 0, 255).boot_count, 0)

    def test_include(self):
        """An included file is read in place, filters and all"""
        conf, logs = config(
            "kernel=a.img\n"
            "[pi4]\n"
            "include pi4.txt\n"
            "[pi5]\n"
            "include pi5.txt\n"
            "include missing.txt\n"
            "[all]\n"
            "include=loop.txt\n",
            files={"pi4.txt": "kernel=pi4.img\n",
                   "pi5.txt": "kernel=pi5.img\n[all]\ninitramfs=x\n",
                   "loop.txt": "include loop.txt\ncmdline=loop.txt\n"},
            verbose=True)
        self.assertEqual(conf.get("kernel"), "pi5.img")
        self.assertEqual(conf.get("initramfs"), "x")
        self.assertEqual(conf.get("cmdline"), "loop.txt")
        self.assertIn("config.txt: no missing.txt to include", logs)
        self.assertIn("loop.txt: includes nest too deeply", logs)

    def test_device_tree_lines(self):
        """dtoverlay and dtparam lines are kept in order, their old names
        too, and ramfsfile is initramfs's"""
        conf, _ = config("dtparam=audio=on\n"
                         "device_tree_overlay=a,x=1\n"
                         "dtoverlay=b\n"
                         "device_tree_param=y\n"
                         "[pi4]\n"
                         "dtoverlay=pi4-only\n"
                         "[all]\n"
                         "dtoverlay=\n"
                         "ramfsfile=old.cpio\n")
        self.assertEqual(conf.dt_lines, [
            ("dtparam", "audio=on"), ("dtoverlay", "a,x=1"),
            ("dtoverlay", "b"), ("dtparam", "y"), ("dtoverlay", "")])
        self.assertEqual(conf.get("initramfs"), "old.cpio")


class BootFilesTest(unittest.TestCase):
    """The kernel, device tree, initramfs and command line config.txt
    chooses"""

    FILES = {"kernel8.img": b"k8", "kernel_2712.img": b"k2712",
             "bcm2712-rpi-5-b.dtb": b"dtb", "cmdline.txt": "root=x\n"}

    def test_defaults(self):
        boot, logs = boot_files("", self.FILES)
        self.assertEqual((boot.kernel, boot.dtb, boot.initramfs,
                          boot.armstub, boot.os_prefix, boot.overlay_dir),
                         ("kernel_2712.img", "bcm2712-rpi-5-b.dtb", [], None,
                          "", "overlays/"))
        self.assertEqual(logs, [])
        files = dict(self.FILES)
        del files["kernel_2712.img"]
        boot, _ = boot_files("", files)
        self.assertEqual(boot.kernel, "kernel8.img")
        boot, _ = boot_files("kernel=Image\ndevice_tree=my.dtb\n",
                             dict(files, **{"Image": b"", "MY.DTB": b""}))
        self.assertEqual((boot.kernel, boot.dtb), ("Image", "my.dtb"))

    def test_missing(self):
        for text, files, message in (
                ("kernel=Image\n", self.FILES,
                 "no kernel: the boot partition has none of Image"),
                ("", {"bcm2712-rpi-5-b.dtb": b""},
                 "no kernel: the boot partition has none of "
                 "kernel_2712.img, kernel8.img"),
                ("", {"kernel8.img": b""},
                 "no device tree: the boot partition has no "
                 "bcm2712-rpi-5-b.dtb (os_check=0 boots without one)"),
                ("armstub=bl31.bin\n", self.FILES,
                 "armstub bl31.bin not found")):
            with self.subTest(message=message):
                with self.assertRaises(rb.BootError) as caught:
                    boot_files(text, files)
                self.assertEqual(str(caught.exception), message)
        boot, _ = boot_files("os_check=0\n", {"kernel8.img": b""})
        self.assertIsNone(boot.dtb)
        boot, _ = boot_files("device_tree=\n", {"kernel8.img": b""})
        self.assertIsNone(boot.dtb)

    def test_os_prefix(self):
        """os_prefix applies when its directory has a kernel and a device
        tree; the overlays are taken from under it when it has a README
        for them"""
        files = dict(self.FILES, **{"next/kernel_2712.img": b"",
                                    "next/bcm2712-rpi-5-b.dtb": b"",
                                    "next/overlays/README": b"",
                                    "half/kernel_2712.img": b""})
        boot, _ = boot_files("os_prefix=next/\n", files)
        self.assertEqual((boot.kernel, boot.dtb, boot.os_prefix,
                          boot.overlay_dir),
                         ("next/kernel_2712.img", "next/bcm2712-rpi-5-b.dtb",
                          "next/", "next/overlays/"))
        del files["next/overlays/README"]
        boot, _ = boot_files("os_prefix=next/\noverlay_prefix=o/\n", files)
        self.assertEqual(boot.overlay_dir, "o/")
        boot, logs = boot_files("os_prefix=half/\n", files, verbose=True)
        self.assertEqual((boot.kernel, boot.os_prefix),
                         ("kernel_2712.img", ""))
        self.assertEqual(logs, ["os_prefix half/ ignored: no kernel and "
                                "device tree there"])
        # A name starting with "/" is the boot partition's file, whatever
        # the prefix, and keeps the prefix viable for the rest
        files["next/overlays/README"] = b""
        files.update({"a.cpio": b"", "next/b.cpio": b""})
        boot, _ = boot_files("os_prefix=next/\nkernel=/kernel8.img\n"
                             "initramfs /a.cpio,b.cpio\n", files)
        self.assertEqual((boot.kernel, boot.dtb, boot.os_prefix,
                          boot.overlay_dir, boot.initramfs),
                         ("/kernel8.img", "next/bcm2712-rpi-5-b.dtb",
                          "next/", "next/overlays/",
                          ["/a.cpio", "next/b.cpio"]))
        boot, _ = boot_files("os_prefix=next/\ndevice_tree=/own.dtb\n",
                             dict(files, **{"own.dtb": b""}))
        self.assertEqual((boot.kernel, boot.dtb),
                         ("next/kernel_2712.img", "/own.dtb"))
        with self.assertRaises(rb.BootError) as caught:
            boot_files("os_prefix=next/\nkernel=/none.img\nos_prefix=\n",
                       files)
        self.assertEqual(str(caught.exception),
                         "no kernel: the boot partition has none of /none.img")

    def test_initramfs(self):
        """initramfs names files to load one after the other, under
        os_prefix; auto_initramfs follows the kernel's name"""
        files = dict(self.FILES, **{"a.cpio": b"a", "b.cpio": b"b",
                                    "initramfs_2712": b"i2712",
                                    "initramfs8": b"i8"})
        boot, logs = boot_files("initramfs a.cpio,missing,b.cpio "
                                "followkernel\n", files)
        self.assertEqual(boot.initramfs, ["a.cpio", "b.cpio"])
        self.assertEqual(logs, ["initramfs missing not found"])
        boot, _ = boot_files("auto_initramfs=1\n", files)
        self.assertEqual(boot.initramfs, ["initramfs_2712"])
        boot, _ = boot_files("auto_initramfs=1\nkernel=kernel8.img\n",
                             files)
        self.assertEqual(boot.initramfs, ["initramfs8"])
        boot, _ = boot_files("auto_initramfs=1\ninitramfs=a.cpio\n", files)
        self.assertEqual(boot.initramfs, ["a.cpio"])
        del files["initramfs_2712"]
        boot, _ = boot_files("auto_initramfs=1\n", files)
        self.assertEqual(boot.initramfs, [])

    def test_armstub(self):
        boot, _ = boot_files("armstub=bl31.bin\n",
                             dict(self.FILES, **{"bl31.bin": b""}))
        self.assertEqual(boot.armstub, "bl31.bin")


@unittest.skipUnless(HAVE_DTC, "needs dtc (device-tree-compiler)")
class CommandLineTest(unittest.TestCase):
    """cmdline.txt as the kernel gets it"""

    def cmdline(self, text, conf="", root=None, append="", tree=True):
        files = {"kernel8.img": b"", "cmdline.txt": text,
                 "bcm2712-rpi-5-b.dtb": dtc(BASE_DTS)}
        if not tree:
            conf += "device_tree=\n"
        boot, logs = boot_files(conf, files)
        with Logs() as more:
            if boot.dtb:
                rb.compose_tree(boot, False, str(DTMERGE))
            rb.kernel_cmdline(boot, root, append)
        return boot.cmdline, logs + more

    def test_serial(self):
        """serial0 is the console UART, serial1 the Bluetooth one, when
        the tree has aliases for them"""
        text = ("console=serial0,115200 console=tty1 kgdboc=serial1 "
                "root=PARTUUID=12345678-02 rootwait\nignored\n")
        self.assertEqual(self.cmdline(text)[0],
                         "console=ttyAMA10,115200 console=tty1 "
                         "kgdboc=ttyS1 root=PARTUUID=12345678-02 rootwait")
        self.assertEqual(self.cmdline(text, tree=False)[0],
                         "console=serial0,115200 console=tty1 "
                         "kgdboc=serial1 root=PARTUUID=12345678-02 rootwait")
        self.assertEqual(self.cmdline(text, "enable_uart=0\n")[0],
                         "console=tty1 kgdboc=ttyS1 "
                         "root=PARTUUID=12345678-02 rootwait")

    def test_root_and_append(self):
        self.assertEqual(self.cmdline("console=tty1 root=/dev/sda2 x",
                                      root="/dev/mmcblk0p2",
                                      append="init=/bin/sh")[0],
                         "console=tty1 root=/dev/mmcblk0p2 x init=/bin/sh")
        self.assertEqual(self.cmdline("quiet", root="/dev/mmcblk0p2")[0],
                         "quiet root=/dev/mmcblk0p2")

    def test_file(self):
        """cmdline names the file, under os_prefix"""
        self.assertEqual(self.cmdline("a", "cmdline=missing.txt\n"),
                         ("", ["no missing.txt: the kernel's command line "
                               "is empty"]))


# ---------------------------------------------------------------------------
# Cards

# What a boot partition needs for rpi5-boot to boot from it
BOOT_FILES = {"config.txt": "", "kernel8.img": b"kernel",
              "bcm2712-rpi-5-b.dtb": b"dtb"}


@unittest.skipUnless(HAVE_MTOOLS, "needs mtools")
class CardTest(unittest.TestCase):
    """The partition the bootloader boots, and its files"""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        self.image = self.tmp / "card.img"

    def boot_partition(self, requested=None, reset=None, explicit=True):
        """The partition the bootloader boots after @reset (a power-on's
        by default), asked for @requested by the user (@explicit) or by
        the reboot: (its number, its file system, what was logged, and
        whether autoboot.txt makes a tryboot's switch at the partition)"""
        with Logs() as logs:
            part, fs, a_b = rb.boot_partition(
                str(self.image), requested, reset or rb.Reset(), False,
                explicit)
        return part.number, fs, logs, a_b

    def test_mbr(self):
        make_card(self.image, dict(BOOT_FILES, **{
            "Overlays/Test.dtbo": b"overlay", "CmdLine.txt": "x"}))
        self.assertEqual(
            [(p.number, p.start, p.size) for p in rb.partitions(self.image)],
            [(1, 2048 * SECTOR, 16384 * SECTOR),
             (2, 18432 * SECTOR, (65536 - 18432) * SECTOR)])
        number, fs, logs, _ = self.boot_partition()
        self.assertEqual((number, logs), (1, []))
        # Names are found whatever their case, as the firmware finds them
        self.assertTrue(fs.exists("overlays/test.dtbo"))
        self.assertTrue(fs.exists("./OVERLAYS//TEST.DTBO"))
        self.assertFalse(fs.exists("overlays"))
        self.assertEqual(fs.read("cmdline.txt"), b"x")
        dest = self.tmp / "copy"
        fs.copy("KERNEL8.IMG", dest)
        self.assertEqual(dest.read_bytes(), b"kernel")
        with self.assertRaises(rb.BootError):
            fs.read("nosuch.txt")

    def test_whole_card(self):
        """A card that is one FAT file system is partition 0"""
        blank_image(self.image, 16 << 20)
        format_fat(self.image, 0, 32768, BOOT_FILES)
        self.assertEqual([p.number for p in rb.partitions(self.image)], [0])
        self.assertEqual(self.boot_partition()[0], 0)

    def test_logical(self):
        """Logical partitions are numbered from 5, as the bootloader and
        Linux number them; the boot partition is the first FAT one with
        a config.txt"""
        blank_image(self.image, 32 << 20)
        extended = 4096
        write_at(self.image, 0, mbr([(0x83, 2048, 2048),
                                     (0x0f, extended, 65536 - extended)]))
        # Each extended boot record: a logical partition, then the next
        write_at(self.image, extended * SECTOR,
                 mbr([(0x0c, 2048, 8192), (0x05, 12288, 12288)], 0))
        write_at(self.image, (extended + 12288) * SECTOR,
                 mbr([(0x0c, 2048, 8192)], 0))
        format_fat(self.image, extended + 2048, 8192, {"cmdline.txt": ""})
        format_fat(self.image, extended + 12288 + 2048, 8192, BOOT_FILES)
        self.assertEqual(
            [(p.number, p.start // SECTOR) for p in
             rb.partitions(self.image)],
            [(1, 2048), (5, extended + 2048), (6, extended + 14336)])
        self.assertEqual(self.boot_partition()[0], 6)

    def test_gpt(self):
        blank_image(self.image, 32 << 20)
        write_at(self.image, 0, mbr([(0xee, 1, 65535)], 0))
        header = bytearray(92)
        header[:8] = b"EFI PART"
        struct.pack_into("<QII", header, 72, 2, 128, 128)
        write_at(self.image, SECTOR, header)
        entries = bytearray(128 * 128)
        for i, (first, last) in enumerate(((2048, 10239), (10240, 18431))):
            entries[i * 128:i * 128 + 16] = bytes(range(1, 17))
            struct.pack_into("<QQ", entries, i * 128 + 32, first, last)
        write_at(self.image, 2 * SECTOR, entries)
        format_fat(self.image, 10240, 8192, BOOT_FILES)
        self.assertEqual(
            [(p.number, p.start // SECTOR, p.size // SECTOR)
             for p in rb.partitions(self.image)],
            [(1, 2048, 8192), (2, 10240, 8192)])
        self.assertEqual(self.boot_partition()[0], 2)

    def make_fat_card(self, *partitions):
        """A card of FAT partitions, holding @partitions' files in turn"""
        blank_image(self.image, 64 << 20)
        entries = [(0x0c, 2048 + 8192 * i, 8192)
                   for i in range(len(partitions))]
        write_at(self.image, 0, mbr(entries))
        for (_, start, sectors), files in zip(entries, partitions):
            format_fat(self.image, start, sectors, files)

    def make_ab_card(self, autoboot):
        """Partition 1 FAT with autoboot.txt, 2 and 3 FAT boot partitions"""
        self.make_fat_card({"autoboot.txt": autoboot}, BOOT_FILES,
                           BOOT_FILES)

    def test_autoboot(self):
        """autoboot.txt in the first partition names the one to boot, as
        its filters select"""
        self.make_ab_card("[all]\ntryboot_a_b=1\nboot_partition=3\n"
                          "[tryboot]\nboot_partition=2\n")
        number, _, logs, _ = self.boot_partition()
        self.assertEqual((number, logs), (3, []))
        self.assertEqual(self.boot_partition(2)[0], 2)
        for requested, message in (
                (1, "partition 1 has no config.txt, so a Pi 5 will not "
                    "boot from it"),
                (4, f"{self.image} has no FAT partition 4")):
            with self.subTest(partition=requested):
                with self.assertRaises(rb.BootError) as caught:
                    self.boot_partition(requested)
                self.assertEqual(str(caught.exception), message)

    def test_autoboot_fallback(self):
        """A partition autoboot.txt names that cannot boot is passed
        over"""
        self.make_ab_card("boot_partition=1\n")
        number, _, logs, _ = self.boot_partition()
        self.assertEqual(number, 2)
        self.assertEqual(logs,
                         ["autoboot.txt: partition 1 is not bootable"])

    def test_tryboot(self):
        """A tryboot boots the partition autoboot.txt's [tryboot] section
        names; with tryboot_a_b, that partition's config.txt is read, not
        its tryboot.txt"""
        for a_b in (0, 1):
            with self.subTest(tryboot_a_b=a_b):
                self.make_ab_card(f"[all]\ntryboot_a_b={a_b}\n"
                                  "boot_partition=2\n"
                                  "[tryboot]\nboot_partition=3\n")
                self.assertEqual(self.boot_partition()[::3], (2, bool(a_b)))
                self.assertEqual(
                    self.boot_partition(reset=rb.Reset(
                        0x1000, rb.TRYBOOT_FLAG, 1))[::3], (3, bool(a_b)))

    def test_reboot_partition(self):
        """The partition the OS asks for at its reboot is booted, whatever
        autoboot.txt names, and a tryboot's too"""
        self.make_ab_card("[all]\ntryboot_a_b=1\nboot_partition=2\n"
                          "[tryboot]\nboot_partition=3\n")
        self.assertEqual(self.boot_partition(3, explicit=False)[0], 3)
        self.assertEqual(self.boot_partition(
            2, rb.Reset(0x1004, rb.TRYBOOT_FLAG, 1), False)[::3], (2, True))

    def test_partition_walk(self):
        """A partition the OS asks for that cannot boot is passed over for
        the next that can, up to partition 8, and then the first, as the
        bootloader's PARTITION_WALK passes over it; as does one
        autoboot.txt names. One --partition names is an error."""
        self.make_fat_card({"autoboot.txt": "boot_partition=3\n"},
                           BOOT_FILES, {"cmdline.txt": ""}, BOOT_FILES)
        number, _, logs, _ = self.boot_partition()
        self.assertEqual((number, logs),
                         (4, ["autoboot.txt: partition 3 is not bootable"]))
        for requested, number in ((3, 4), (4, 4), (5, 2), (1, 2)):
            with self.subTest(requested=requested):
                self.assertEqual(
                    self.boot_partition(requested, explicit=False)[:3:2],
                    (number, [] if requested == number else [
                        f"partition {requested}, which the reboot asks "
                        "for, is not bootable"]))
        with self.assertRaisesRegex(rb.BootError, "partition 3 has no "
                                    "config.txt"):
            self.boot_partition(3)

    def test_no_boot_partition(self):
        make_card(self.image, {"cmdline.txt": ""})
        with self.assertRaisesRegex(rb.BootError, "no FAT partition of .* "
                                    "has a config.txt"):
            self.boot_partition()
        blank_image(self.image, 1 << 20)
        with self.assertRaisesRegex(rb.BootError, "has no FAT partition to "
                                    "boot from"):
            self.boot_partition()

    def test_pattern_names(self):
        """A file whose name mcopy would read as a pattern is read as the
        file it is"""
        make_card(self.image, dict(BOOT_FILES, **{
            "a[b].txt": "BRACKET", "ab.txt": "PLAIN", "dir[1]/f.txt": "IN"}))
        _, fs, _, _ = self.boot_partition()
        self.assertEqual(fs.read("a[b].txt"), b"BRACKET")
        self.assertEqual(fs.read("ab.txt"), b"PLAIN")
        self.assertEqual(fs.read("dir[1]/f.txt"), b"IN")
        fs.copy("A[B].TXT", self.tmp / "out")
        self.assertEqual((self.tmp / "out").read_bytes(), b"BRACKET")

    def test_image_checks(self):
        """Images QEMU's SD card cannot take are turned away, with what to
        do about them"""
        for size, fix in ((3 << 20, "4M"), ((2 << 30) + 4096, "4G")):
            with self.subTest(size=size):
                blank_image(self.image, size)
                with self.assertRaises(rb.BootError) as caught:
                    rb.check_image(str(self.image))
                self.assertIn(f"truncate -s {fix} {self.image}",
                              str(caught.exception))
        for size in (1 << 20, 3 << 30):
            blank_image(self.image, size)
            rb.check_image(str(self.image))
        for magic, what in ((b"\xfd7zXZ\0", "xz"), (b"\x1f\x8b", "gzip"),
                            (b"PK\x03\x04", "zip"), (b"QFI\xfb", "qcow2")):
            with self.subTest(what=what):
                self.image.write_bytes(magic + bytes(1024 - len(magic)))
                with self.assertRaisesRegex(rb.BootError,
                                            f"is {what}-compressed or not "
                                            "raw"):
                    rb.check_image(str(self.image))
        self.image.write_bytes(b"")
        with self.assertRaisesRegex(rb.BootError, "is empty"):
            rb.check_image(str(self.image))
        # mtools reads what follows "@@" in a name as an offset
        with self.assertRaisesRegex(rb.BootError, "cannot contain @@"):
            rb.check_image(str(self.tmp / "card@@1.img"))


@unittest.skipUnless(HAVE_MTOOLS and HAVE_DTC, "needs mtools and dtc")
class PrepareTest(unittest.TestCase):
    """The boot a reset starts, read from the card: a tryboot reads
    tryboot.txt, and config.txt's filters see the boot count and the
    partition asked for"""

    CONFIG = ("device_tree=\n"
              "[boot_count=3]\ncmdline=third.txt\n[all]\n"
              "[partition=1]\ncmdline=asked.txt\n")
    FILES = {"kernel8.img": b"", "cmdline.txt": "first",
             "third.txt": "third", "asked.txt": "asked", "try.txt": "try"}

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        self.image = self.tmp / "card.img"

    def prepare(self, reset, *options):
        args = rb.parse_args([*options, str(self.image)])
        with Logs() as logs:
            boot = rb.prepare(str(self.image), args, reset)
        return boot.config.name, boot.cmdline, logs

    def test_config_files(self):
        make_card(self.image, dict(self.FILES, **{
            "config.txt": self.CONFIG,
            "tryboot.txt": "device_tree=\ncmdline=try.txt\n"}))
        for reset, options, expected in (
                (rb.Reset(), (), ("config.txt", "first")),
                # A tryboot, and the boot after it
                (rb.Reset(0x1000, rb.TRYBOOT_FLAG, 1), (),
                 ("tryboot.txt", "try")),
                (rb.Reset(0x1000, 0, 2), (), ("config.txt", "third")),
                # A reboot to partition 1, and --partition 1
                (rb.Reset(0x1001, 0, 0), (), ("config.txt", "asked")),
                (rb.Reset(), ("--partition", "1"), ("config.txt", "asked"))):
            with self.subTest(reset=vars(reset), options=options):
                self.assertEqual(self.prepare(reset, *options),
                                 (*expected, []))

    def test_tryboot_a_b(self):
        """With tryboot_a_b, a tryboot reads config.txt: autoboot.txt
        switches partitions for it"""
        make_card(self.image, dict(self.FILES, **{
            "config.txt": self.CONFIG, "tryboot.txt": "cmdline=try.txt\n",
            "autoboot.txt": "tryboot_a_b=1\n"}))
        self.assertEqual(self.prepare(rb.Reset(0x1000, rb.TRYBOOT_FLAG, 0)),
                         ("config.txt", "first", []))

    def test_no_tryboot_txt(self):
        """A tryboot without tryboot.txt boots with the firmware's
        defaults"""
        make_card(self.image, dict(self.FILES, **{
            "config.txt": self.CONFIG,
            "bcm2712-rpi-5-b.dtb": dtc(BASE_DTS)}))
        self.assertEqual(
            self.prepare(rb.Reset(0x1000, rb.TRYBOOT_FLAG, 0)),
            (None, "first", ["partition 1 has no tryboot.txt for the "
                             "tryboot: the firmware's defaults apply"]))


@unittest.skipUnless(HAVE_MTOOLS and HAVE_DTC, "needs mtools and dtc")
class PrintTest(unittest.TestCase):
    """--print: one boot's files written, and QEMU's command line"""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        self.image = self.tmp / "card, with a comma.img"
        make_card(self.image, {
            "config.txt": "dtoverlay=test,speed=9600\n"
                          "initramfs a.cpio,b.cpio\n"
                          "armstub=bl31.bin\n"
                          "device_tree_address=0x1f0000\n",
            "cmdline.txt": "console=serial0,115200 root=/dev/sda2\n",
            "kernel_2712.img": b"kernel", "a.cpio": b"a", "b.cpio": b"b",
            "bl31.bin": b"bl31", "bcm2712-rpi-5-b.dtb": dtc(BASE_DTS),
            "overlays/test.dtbo": dtc(OVERLAY_DTS)})

    def test_print(self):
        out = self.tmp / "files"
        status, stdout, stderr = run_boot(
            "--print", "-o", out, "--qemu", "/opt/qemu", "--root",
            "/dev/mmcblk0p2", "--append", "quiet", self.image, "--",
            "-m", "8G")
        self.assertEqual((status, stderr), (0, ""))
        image = str(self.image).replace(",", ",,")
        cmdline = ("console=ttyAMA10,115200 root=/dev/mmcblk0p2 quiet")
        self.assertEqual(shlex.split(stdout), [
            "/opt/qemu", "-M",
            "raspi5b,secure=on,dtb-address=0x1f0000,boot-partition=1",
            "-nographic", "-drive", f"if=sd,format=raw,file={image}",
            "-bios", f"{out}/armstub.bin", "-kernel", f"{out}/kernel.img",
            "-dtb", f"{out}/device-tree.dtb", "-initrd", f"{out}/initramfs",
            "-append", cmdline, "-action", "reboot=shutdown", "-m", "8G"])
        self.assertEqual(sorted(p.name for p in out.iterdir()), [
            ".rpi5-boot", "armstub.bin", "cmdline.txt", "device-tree.dtb",
            "initramfs", "kernel.img"])
        self.assertEqual((out / "kernel.img").read_bytes(), b"kernel")
        self.assertEqual((out / "armstub.bin").read_bytes(), b"bl31")
        self.assertEqual((out / "initramfs").read_bytes(), b"ab")
        self.assertEqual((out / "cmdline.txt").read_text(), cmdline + "\n")
        tree = fdt.load(out / "device-tree.dtb")
        self.assertEqual(tree["/soc@107c000000/serial@7d001000"]
                         ["current-speed"], cells(9600))

        # The next boot's files replace these; what it lacks goes
        conf = self.tmp / "config.txt"
        conf.write_text("device_tree=\n")
        mtools("mcopy", "-o", "-i", f"{self.image}@@{2048 * SECTOR}",
               str(conf), "::/config.txt")
        status, stdout, stderr = run_boot("--print", "-o", out,
                                          "--graphics", self.image)
        self.assertEqual((status, stderr), (0, ""))
        self.assertEqual(shlex.split(stdout)[1:3],
                         ["-M", "raspi5b,builtin-dtb=off,boot-partition=1"])
        self.assertNotIn("-nographic", stdout)
        self.assertEqual(sorted(p.name for p in out.iterdir()),
                         [".rpi5-boot", "cmdline.txt", "kernel.img"])
        self.assertEqual((out / "cmdline.txt").read_text(),
                         "console=serial0,115200 root=/dev/sda2\n")

    def test_pcie2_preinit(self):
        """PCIe2 starts as the firmware leaves it with pciex4_reset=0, the
        default with enable_rp1_uart=1"""
        for n, (config, preinit) in enumerate((
                ("", False), ("pciex4_reset=0\n", True),
                ("enable_rp1_uart=1\n", True),
                ("enable_rp1_uart=1\npciex4_reset=1\n", False))):
            with self.subTest(config=config):
                image = self.tmp / f"pcie{n}.img"
                make_card(image, {
                    "config.txt": config, "cmdline.txt": "quiet\n",
                    "kernel_2712.img": b"kernel",
                    "bcm2712-rpi-5-b.dtb": dtc(BASE_DTS)})
                status, stdout, stderr = run_boot(
                    "--print", "-o", self.tmp / "out", image)
                self.assertEqual((status, stderr), (0, ""))
                self.assertEqual(
                    "pcie2-preinit=on" in shlex.split(stdout)[2].split(","),
                    preinit)

    def test_output_directory(self):
        """Files go to a new or empty directory, or one rpi5-boot made:
        ./rpi5-boot-files by default with --print"""
        proc = subprocess.run([sys.executable, str(SCRIPT), "--print",
                               str(self.image)], cwd=self.tmp,
                              capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertTrue((self.tmp / "rpi5-boot-files/.rpi5-boot").exists())
        other = self.tmp / "other"
        other.mkdir()
        (other / "precious").write_text("")
        status, stdout, stderr = run_boot("--print", "-o", other,
                                          self.image)
        self.assertEqual((status, stdout), (1, ""))
        self.assertEqual(stderr, f"rpi5-boot: {other} exists and was not "
                         "made by rpi5-boot; use a new or empty directory\n")
        self.assertEqual([p.name for p in other.iterdir()], ["precious"])


# ---------------------------------------------------------------------------
# Boots

def aarch64(*instructions):
    return b"".join(struct.pack("<I", i) for i in instructions)


# Kernels loaded as raw images: one that asks QEMU's PSCI for a system
# reset at once (SYSTEM_RESET, 0x84000009), one that runs until stopped
RESET_KERNEL = aarch64(0xd2800120,              # mov x0, #0x9
                       0xf2b08000,              # movk x0, #0x8400, lsl #16
                       0xd4000003,              # smc #0
                       0x14000000)              # b .
SPIN_KERNEL = aarch64(0x14000000)               # b .


@unittest.skipUnless(HAVE_MTOOLS and HAVE_DTC, "needs mtools and dtc")
@unittest.skipUnless(QEMU.exists() and GUEST.exists(),
                     "build QEMU and the guests first (make build guest)")
class RunTest(unittest.TestCase):
    """QEMU run for a boot, which ends when the guest powers off, reboots
    or is stopped"""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        self.image = self.tmp / "card.img"
        # The temporary directory each run makes, and removes
        self.tmpdir = self.tmp / "tmp"
        self.tmpdir.mkdir()
        self.env = dict(os.environ, QEMU=str(QEMU), TMPDIR=str(self.tmpdir))

    def make_card(self, kernel):
        make_card(self.image, {"config.txt": "", "kernel8.img": kernel,
                               "bcm2712-rpi-5-b.dtb": dtc(DTS.read_text()),
                               "cmdline.txt": "console=serial0,115200\n"})

    def test_power_off(self):
        """The hello guest runs, as the kernel config.txt names, and
        powers off"""
        self.make_card(GUEST)
        status, out, err = run_boot(self.image, env=self.env)
        self.assertEqual(status, 0, out + err)
        self.assertIn("raspi5b: core 0 up at EL2", out)
        self.assertIn("raspi5b: PSCI SYSTEM_OFF", out)
        self.assertNotIn("rpi5-boot:", err)
        self.assertEqual(list(self.tmpdir.iterdir()), [])

    def test_no_reboot(self):
        self.make_card(RESET_KERNEL)
        status, out, err = run_boot("--no-reboot", self.image,
                                    env=self.env)
        self.assertEqual(status, 0, out + err)
        self.assertIn("rpi5-boot: the guest rebooted: stopping, as "
                      "--no-reboot asks\n", err)
        self.assertEqual(list(self.tmpdir.iterdir()), [])

    def start(self):
        """Run the guest that never ends: the rpi5-boot process, once it
        has started QEMU"""
        self.make_card(SPIN_KERNEL)
        proc = subprocess.Popen([sys.executable, str(SCRIPT), "-v",
                                 str(self.image)], stdin=subprocess.DEVNULL,
                                stdout=subprocess.DEVNULL,
                                stderr=subprocess.PIPE, env=self.env,
                                start_new_session=True)
        self.addCleanup(proc.stderr.close)
        self.addCleanup(proc.wait)
        self.addCleanup(proc.kill)
        # -v logs QEMU's command before running it; a boot that never
        # gets there is killed rather than waited for
        timer = threading.Timer(RUN_TIMEOUT, proc.kill)
        timer.start()
        try:
            for line in proc.stderr:
                if b"-action reboot=shutdown" in line:
                    break
            else:
                self.fail("rpi5-boot ended before running QEMU")
        finally:
            timer.cancel()
        time.sleep(2)
        return proc

    def stop(self, send):
        """Run the guest that never ends, and stop it with @send(the
        rpi5-boot process) once QEMU runs: rpi5-boot's exit status"""
        proc = self.start()
        send(proc)
        proc.wait(timeout=RUN_TIMEOUT)
        self.assertEqual(list(self.tmpdir.iterdir()), [])
        return proc.returncode

    def test_stop(self):
        """SIGTERM stops QEMU, and then rpi5-boot; so does a Ctrl-C, the
        SIGINT the whole process group gets, which rpi5-boot leaves to
        QEMU and reports as the interrupted command's status 130"""
        status = self.stop(lambda proc: proc.terminate())
        self.assertIn(status, (0, 128 + signal.SIGTERM))
        status = self.stop(lambda proc: os.killpg(proc.pid, signal.SIGINT))
        self.assertEqual(status, 128 + signal.SIGINT)

    @unittest.skipUnless(Path("/proc/self/stat").exists(), "needs /proc")
    def test_killed(self):
        """QEMU ends with rpi5-boot, even when a SIGKILL leaves rpi5-boot
        no time to stop it, so that no QEMU goes on writing to the card"""
        proc = self.start()
        self.addCleanup(kill_group, proc.pid)
        qemu = children(proc.pid)
        self.assertEqual(len(qemu), 1)
        proc.kill()
        proc.wait(timeout=RUN_TIMEOUT)
        deadline = time.monotonic() + RUN_TIMEOUT
        while running(qemu[0]) and time.monotonic() < deadline:
            time.sleep(0.1)
        self.assertFalse(running(qemu[0]), "QEMU outlived rpi5-boot")


@unittest.skipUnless(HAVE_MTOOLS and HAVE_DTC, "needs mtools and dtc")
@unittest.skipUnless(QEMU.exists() and REBOOT_GUEST.exists(),
                     "build QEMU and the guests first (make build guest)")
class ResetStateTest(unittest.TestCase):
    """What a reboot leaves reaches the boot it starts, as on a Pi 5: the
    tryboot flag, the partition the OS asked for and the boot count pick
    the partition and the configuration file, and the machine reports
    them in /chosen/bootloader. The guest reports each boot, and ends it
    as the "bootN=" word of its command line says."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        self.image = self.tmp / "card.img"
        self.env = dict(os.environ, QEMU=str(QEMU))
        self.files = {"kernel8.img": REBOOT_GUEST,
                      "bcm2712-rpi-5-b.dtb": dtc(DTS.read_text())}

    def boots(self):
        """Run the card to its end: what the guest said of each boot, and
        what rpi5-boot logged"""
        status, out, err = run_boot(self.image, env=self.env)
        self.assertEqual(status, 0, out + err)
        return [line[len("reboot: "):] for line in out.splitlines()
                if line.startswith("reboot: ")], err

    def test_tryboot_txt(self):
        """A tryboot reads tryboot.txt, the boot after it config.txt, in
        which boot_count selects the third boot's command line"""
        make_card(self.image, dict(self.files, **{
            "config.txt": "[boot_count=3]\ncmdline=third.txt\n",
            "tryboot.txt": "cmdline=try.txt\n",
            "cmdline.txt": "tag=config boot1=tryboot\n",
            "try.txt": "tag=tryboot boot2=reboot\n",
            "third.txt": "tag=third\n"}))
        boots, err = self.boots()
        self.assertEqual(boots, [
            "boot 1 from partition 1, reset status 0x1000, tryboot 0, "
            "tag config: tryboot",
            "boot 2 from partition 1, reset status 0x1000, tryboot 1, "
            "tag tryboot: reboot",
            "boot 3 from partition 1, reset status 0x1000, tryboot 0, "
            "tag third: off"])
        self.assertEqual(err, "rpi5-boot: the guest rebooted: reading the "
                         "card again\n" * 2)

    def test_a_b(self):
        """autoboot.txt's A/B boot: a tryboot boots B, the boot after it A
        again; a reboot to partition 4, which the watchdog's reset status
        carries, boots it whatever autoboot.txt says; partition 63 halts"""
        autoboot = ("[all]\ntryboot_a_b=1\nboot_partition=2\n"
                    "[tryboot]\nboot_partition=3\n")
        partitions = [{"autoboot.txt": autoboot}] + [
            dict(self.files, **{"config.txt": "", "cmdline.txt": cmdline})
            for cmdline in ("tag=A boot1=tryboot boot3=partition4\n",
                            "tag=B boot2=reboot\n", "tag=C boot4=halt\n")]
        blank_image(self.image, 64 << 20)
        entries = [(0x0c, 2048 + 16384 * i, 16384) for i in range(4)]
        write_at(self.image, 0, mbr(entries))
        for (_, start, sectors), files in zip(entries, partitions):
            format_fat(self.image, start, sectors, files)
        boots, err = self.boots()
        self.assertEqual(boots, [
            "boot 1 from partition 2, reset status 0x1000, tryboot 0, "
            "tag A: tryboot",
            "boot 2 from partition 3, reset status 0x1000, tryboot 1, "
            "tag B: reboot",
            "boot 3 from partition 2, reset status 0x1000, tryboot 0, "
            "tag A: partition4",
            # The watchdog's reset: HADWRF, and partition 4 in bit 4
            "boot 4 from partition 4, reset status 0x30, tryboot 0, "
            "tag C: halt"])
        self.assertEqual(err, "rpi5-boot: the guest rebooted: reading the "
                         "card again\n" * 3)


# Stands in for QEMU: speaks QMP as QEMU does, and plays a boot. A command
# line with "firstboot" is Raspberry Pi OS's first boot: it takes
# "firstboot" out of cmdline.txt on the card and reboots, and then waits
# for the machine's state to be read (a plain reset's: this boot's
# count), unless FAKE_QEMU_GONE has it exit as QEMU does without
# shutdown=pause. Otherwise the guest powers off, or QEMU fails with the
# status FAKE_QEMU_STATUS gives.
FAKE_QEMU = """\
import json, os, re, socket, subprocess, sys

args = sys.argv[1:]
with open(os.environ["FAKE_QEMU_LOG"], "a") as log:
    log.write(json.dumps(args) + "\\n")
if os.environ.get("FAKE_QEMU_STATUS"):
    sys.exit(int(os.environ["FAKE_QEMU_STATUS"]))
fd = int(re.search(r"fd=(\\d+)", args[args.index("-chardev") + 1])[1])
qmp = socket.socket(fileno=fd)
lines = qmp.makefile("rb")


def send(message):
    qmp.sendall(json.dumps(message).encode() + b"\\n")


def expect(command):
    message = json.loads(lines.readline())
    if message["execute"] != command:
        sys.exit(2)
    return message


send({"QMP": {"version": {}, "capabilities": []}})
for command in ("qmp_capabilities", "cont"):
    send({"return": {}, "id": expect(command)["id"]})
cmdline = args[args.index("-append") + 1]
if "firstboot" in cmdline:
    new = os.path.join(os.path.dirname(os.environ["FAKE_QEMU_LOG"]),
                       "cmdline.txt")
    with open(new, "w") as f:
        f.write(cmdline.replace(" firstboot", "") + "\\n")
    subprocess.run(["mcopy", "-o", "-i", os.environ["FAKE_QEMU_CARD"], new,
                    "::/cmdline.txt"], check=True)
    reason = "guest-reset"
else:
    reason = "guest-shutdown"
send({"event": "SHUTDOWN", "data": {"guest": True, "reason": reason}})
if os.environ.get("FAKE_QEMU_GONE"):
    sys.exit(0)
if reason == "guest-reset":
    count = re.search(r"boot-count=(\\d+)", args[args.index("-M") + 1])
    state = {"reset-status": 0x1000, "reboot-flags": 0,
             "boot-count": int(count[1]) + 1 if count else 1}
    for _ in state:
        message = expect("qom-get")
        send({"return": state[message["arguments"]["property"]],
              "id": message["id"]})
send({"return": {}, "id": expect("quit")["id"]})
"""


@unittest.skipUnless(HAVE_MTOOLS, "needs mtools")
class RebootTest(unittest.TestCase):
    """A reboot is a new boot: rpi5-boot reads the card again, as the
    bootloader does, and runs QEMU again"""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        self.image = self.tmp / "card.img"
        make_card(self.image, {"config.txt": "device_tree=\n",
                               "kernel8.img": b"",
                               "cmdline.txt": "root=/dev/mmcblk0p2 "
                                              "firstboot\n"})
        self.qemu = self.tmp / "qemu"
        self.qemu.write_text(f"#!{sys.executable}\n{FAKE_QEMU}")
        self.qemu.chmod(0o755)
        self.log = self.tmp / "boots"
        self.env = dict(os.environ, FAKE_QEMU_LOG=str(self.log),
                        FAKE_QEMU_CARD=f"{self.image}@@{2048 * SECTOR}",
                        MTOOLS_SKIP_CHECK="1")

    def boots(self):
        return [json.loads(line) for line in
                self.log.read_text().splitlines()]

    def test_reboot(self):
        """The next boot is the reset's: the machine starts with what the
        reset left, as QEMU read it"""
        status, _, err = run_boot("--qemu", self.qemu, self.image,
                                  env=self.env)
        self.assertEqual((status, err), (
            0, "rpi5-boot: the guest rebooted: reading the card again\n"))
        boots = self.boots()
        self.assertEqual(len(boots), 2)
        for args, cmdline, machine in zip(
                boots, ("root=/dev/mmcblk0p2 firstboot",
                        "root=/dev/mmcblk0p2"),
                ("raspi5b,builtin-dtb=off,boot-partition=1",
                 "raspi5b,builtin-dtb=off,boot-partition=1,"
                 "reset-status=0x1000,reboot-flags=0x0,boot-count=1")):
            self.assertEqual(args[args.index("-append") + 1], cmdline)
            self.assertEqual(args[args.index("-M") + 1], machine)
            self.assertIn("-S", args)
            # QEMU stops the guest at the reboot, for its state to be
            # read, and at a power-off
            self.assertEqual([args[i + 1] for i, arg in enumerate(args)
                              if arg == "-action"],
                             ["reboot=shutdown", "shutdown=pause"])
            # and it ends with rpi5-boot
            self.assertEqual(args[args.index("-run-with") + 1],
                             "exit-with-parent=on")

    def test_state_lost(self):
        """A QEMU that ends at the reboot, before it is asked what the
        reset left, leaves the next boot a power-on's"""
        status, _, err = run_boot("--qemu", self.qemu, self.image,
                                  env=dict(self.env, FAKE_QEMU_GONE="1"))
        self.assertEqual((status, err), (
            0, "rpi5-boot: QEMU did not say what the reset left: the next "
               "boot is a power-on's\n"
               "rpi5-boot: the guest rebooted: reading the card again\n"))
        self.assertEqual([args[args.index("-M") + 1]
                          for args in self.boots()],
                         ["raspi5b,builtin-dtb=off,boot-partition=1"] * 2)

    def test_qemu_fails(self):
        status, _, err = run_boot("--qemu", self.qemu, self.image,
                                  env=dict(self.env, FAKE_QEMU_STATUS="3"))
        self.assertEqual((status, err), (3, ""))
        self.assertEqual(len(self.boots()), 1)
        status, _, err = run_boot("--qemu", self.tmp / "nosuch",
                                  self.image, env=self.env)
        self.assertEqual((status, err), (
            1, f"rpi5-boot: cannot run {self.tmp / 'nosuch'}: No such file "
               "or directory\n"))


# ---------------------------------------------------------------------------
# With the pinned firmware (make check-firmware)

@unittest.skipUnless(os.environ.get("FIRMWARE"),
                     "run through 'make check-firmware'")
@unittest.skipUnless(QEMU.exists() and FIRSTBOOT.exists(),
                     "build QEMU and the guests first (make build guest)")
@unittest.skipUnless(HAVE_MTOOLS, "needs mtools")
class FirstBootTest(unittest.TestCase):
    """Raspberry Pi OS's kernel, booted from a card laid out as the OS's:
    its config.txt and cmdline.txt, the release's device tree and
    overlays, and an initramfs whose /init plays the OS's first boot. That
    boot gives the card a new disk identifier, rewrites cmdline.txt to
    match and reboots; rpi5-boot reads the card again for the next boot,
    which powers off."""

    # config.txt and cmdline.txt as pi-gen, which builds Raspberry Pi OS,
    # writes them (stage1/00-boot-files, at commit 6a0419c199db): the
    # image's PARTUUID in place of ROOTDEV, and the init= of the first boot
    CONFIG = """\
# For more options and information see
# http://rptl.io/configtxt
# Some settings may impact device functionality. See link above for details

# Uncomment some or all of these to enable the optional hardware interfaces
#dtparam=i2c_arm=on
#dtparam=i2s=on
#dtparam=spi=on

# Enable audio (loads snd_bcm2835)
dtparam=audio=on

# Additional overlays and parameters are documented
# /boot/firmware/overlays/README

# Automatically load overlays for detected cameras
camera_auto_detect=1

# Automatically load overlays for detected DSI displays
display_auto_detect=1

# Automatically load initramfs files, if found
auto_initramfs=1

# Enable DRM VC4 V3D driver
dtoverlay=vc4-kms-v3d
max_framebuffers=2

# Don't have the firmware create an initial video= setting in cmdline.txt.
# Use the kernel's default instead.
disable_fw_kms_setup=1

# Disable compensation for displays with overscan
disable_overscan=1

# Run as fast as firmware / board allows
arm_boost=1

[cm4]
# Enable host mode on the 2711 built-in XHCI USB controller.
# This line should be removed if the legacy DWC2 controller is required
# (e.g. for USB device mode) or if USB support is not required.
otg_mode=1

[cm5]
dtoverlay=dwc2,dr_mode=host

[pi5]
dtoverlay=nospi10

[all]
"""
    CMDLINE = ("console=serial0,115200 console=tty1 root=ROOTDEV "
               "rootfstype=ext4 fsck.repair=yes rootwait resize")
    FIRST_INIT = " init=/usr/lib/raspberrypi-sys-mods/firstboot"
    DISK_ID, NEW_DISK_ID = "12345678", "5eed2712"

    def test_first_boot(self):
        tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, tmp)
        image = tmp / "raspios.img"
        cmdline = self.CMDLINE.replace(
            "ROOTDEV", f"PARTUUID={self.DISK_ID}-02") + self.FIRST_INIT
        initramfs = newc({"dev": None, "proc": None, "boot": None,
                          "init": (0o100755, FIRSTBOOT.read_bytes())})
        files = {"config.txt": self.CONFIG, "cmdline.txt": cmdline + "\n",
                 "kernel_2712.img": KERNEL,
                 "bcm2712-rpi-5-b.dtb": FIRMWARE_DTB,
                 "initramfs_2712": initramfs}
        for overlay in sorted(OVERLAYS.iterdir()):
            files[f"overlays/{overlay.name}"] = overlay
        # Raspberry Pi OS's layout: a FAT32 boot partition from 4 MiB
        make_card(image, files, size=256 << 20,
                  disk_id=int(self.DISK_ID, 16), boot_start=8192,
                  boot_sectors=128 << 11, fat32=True)

        out = tmp / "files"
        status, stdout, stderr = run_boot(
            "-v", "-o", out, image, timeout=LINUX_TIMEOUT,
            env=dict(os.environ, QEMU=str(QEMU)))
        self.assertEqual(status, 0, stdout + stderr)
        stdout = stdout.replace("\r\n", "\n")
        self.assertNotIn("FAILED", stdout)
        # The firmware's overlays for the Pi 5, the base tree's parameters
        # (the Pi 5's has no audio), and the auto_initramfs file
        for line in ("loaded overlay 'vc4-kms-v3d-pi5'",
                     "loaded overlay 'nospi10'",
                     "unknown parameter 'audio'",
                     "partition 1: kernel kernel_2712.img, device tree "
                     "bcm2712-rpi-5-b.dtb, initramfs initramfs_2712"):
            self.assertEqual(stderr.count(f"rpi5-boot: {line}\n"), 2,
                             stderr)
        self.assertEqual(stderr.count("rpi5-boot: the guest rebooted: "
                                      "reading the card again\n"), 1)

        # The kernel's command line as the firmware makes it: the tree's
        # bootargs, its own arguments, then cmdline.txt's
        self.assertEqual(stdout.count("reboot: Restarting system"), 1)
        first, second = stdout.split("reboot: Restarting system")
        bootargs = fdt.load(out / "device-tree.dtb")["/chosen"]["bootargs"]
        bootargs = bootargs.rstrip(b"\0").decode()
        prefix = f"Kernel command line: {bootargs}  {FIRMWARE_ARGS}  "
        final = cmdline.replace("console=serial0", "console=ttyAMA10")
        self.assertIn(prefix + final + "\n", first)
        self.assertIn(f"firstboot: disk identifier {self.DISK_ID}\n", first)
        self.assertIn(f"firstboot: new disk identifier {self.NEW_DISK_ID}\n",
                      first)
        final = final.replace(self.DISK_ID, self.NEW_DISK_ID)
        final = final.replace(self.FIRST_INIT, "")
        self.assertIn(prefix + final + "\n", second)
        self.assertIn(f"firstboot: disk identifier {self.NEW_DISK_ID}\n",
                      second)
        self.assertIn("firstboot: powering off\n", second)
        self.assertIn("reboot: Power down", second)
        self.assertNotIn(self.FIRST_INIT, second)

        # The card as the first boot left it
        written = cmdline.replace(self.DISK_ID, self.NEW_DISK_ID)
        written = written.replace(self.FIRST_INIT, "")
        self.assertEqual(read_card_file(image, "cmdline.txt", 8192),
                         written.encode() + b"\n")
        with open(image, "rb") as card:
            card.seek(440)
            self.assertEqual(card.read(4), struct.pack(
                "<I", int(self.NEW_DISK_ID, 16)))
        self.assertEqual((out / "cmdline.txt").read_text(), final + "\n")


if __name__ == "__main__":
    unittest.main()
