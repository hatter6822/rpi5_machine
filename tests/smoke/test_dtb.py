# SPDX-License-Identifier: GPL-2.0-or-later
#
# Device-tree fix-ups applied by the raspi5b machine: to a -dtb blob,
# checked on minimal trees through -machine dumpdtb; to the built-in tree,
# compared with a checked-in dump; and at every boot, read back from guest
# memory over QMP.

import json
import os
import queue
import shutil
import struct
import subprocess
import tempfile
import threading
import time
import unittest
from pathlib import Path

import fdt
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


# What the firmware's own tree carries for the firmware to fill in, as
# bcm2712-rpi-5-b.dts has it: bootargs, a CMA pool sized in one cell, the
# node for the bootloader configuration and the Ethernet alias
FIRMWARE_NODES = """
/ {
    aliases {
        ethernet0 = "/ethernet@100000";
        blconfig = "/reserved-memory/nvram@0";
    };

    chosen {
        bootargs = "reboot=w coherent_pool=1M";
    };

    reserved-memory {
        #address-cells = <2>;
        #size-cells = <2>;
        ranges;

        linux,cma {
            compatible = "shared-dma-pool";
            size = <0x4000000>;
            reusable;
            linux,cma-default;
        };

        nvram@0 {
            compatible = "raspberrypi,bootloader-config", "nvmem-rmem";
            #address-cells = <1>;
            #size-cells = <1>;
            reg = <0 0 0 0>;
            no-map;
            status = "disabled";
        };
    };

    ethernet@100000 {
        compatible = "cdns,macb";
    };
};
"""

# What a tree dumped from a booted Pi carries already
BOOTED_NODES = """
/ {
    chosen {
        bootloader {
            version = "0123456789abcdef0123456789abcdef01234567";
            count = <9>;
        };
        power {
            max_current = <3000>;
        };
    };
};
"""

# The prefixes a host that evaluated config.txt for the boot writes, as
# the firmware writes them, from os_prefix and overlay_prefix
PREFIX_NODES = """
/ {
    chosen {
        os_prefix = "next/";
        overlay_prefix = "ovl/";
    };
};
"""

# The board's Ethernet address: QEMU's second default one, as the first
# goes to the default NIC configuration
MAC = "52:54:00:12:34:57"

# What the firmware adds to the command line
FIRMWARE_ARGS = (f"smsc95xx.macaddr={MAC} "
                 "vc_mem.mem_base=0x3fc00000 vc_mem.mem_size=0x40000000")

# Where the bootloader configuration goes, and what it holds
BLCONFIG_ADDR = 0x3fc80000
BLCONFIG = (b"[all]\nBOOT_UART=1\nPOWER_OFF_ON_HALT=0\nBOOT_ORDER=0xf461\n"
            b"PSU_MAX_CURRENT=5000\n")

# The built-in tree as the guest gets it, printed by fdt.py
BUILTIN_TREE = Path(__file__).with_name("raspi5b-builtin-tree.txt")


def cells(*values):
    return struct.pack(f">{len(values)}I", *values)


def string(value):
    return value.encode() + b"\0"


def fdtget(dtb, node, prop):
    """Return a string property of @node, or None when the node lacks it.

    The node itself must exist: a tree the fix-ups had dropped it from
    would otherwise read as "no status property" and pass.
    """
    subprocess.run(["fdtget", "-p", str(dtb), node], capture_output=True,
                   text=True, check=True)
    result = subprocess.run(["fdtget", "-t", "s", str(dtb), node, prop],
                            capture_output=True, text=True)
    if result.returncode == 0:
        return result.stdout.strip()
    if "FDT_ERR_NOTFOUND" in result.stderr:
        return None
    raise subprocess.CalledProcessError(result.returncode, result.args,
                                        result.stdout, result.stderr)


@unittest.skipUnless(QEMU.exists() and GUEST.exists(),
                     "build QEMU and the guest first (make build guest)")
@unittest.skipUnless(shutil.which("dtc") and shutil.which("fdtget"),
                     "needs dtc and fdtget (device-tree-compiler)")
class DtbFixupTest(unittest.TestCase):

    def compile(self, dts):
        tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, tmp)
        subprocess.run(["dtc", "-q", "-I", "dts", "-O", "dtb",
                        "-o", tmp / "in.dtb"],
                       input=dts, text=True, check=True)
        return tmp / "in.dtb"

    def dumped(self, *machine_args, machine="raspi5b"):
        """Dump the tree @machine gives the guest with @machine_args."""
        tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, tmp)
        out = tmp / "out.dtb"
        result = subprocess.run(
            [str(QEMU), "-M", f"{machine},dumpdtb={out}", "-display", "none",
             "-kernel", str(GUEST), *machine_args],
            stdin=subprocess.DEVNULL, capture_output=True, text=True,
            timeout=TIMEOUT)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(os.path.getsize(out) > 0)
        return out

    def fixed_up(self, *machine_args, dts=MINIMAL_DTS):
        """Run QEMU's fix-ups on @dts and return the resulting dtb."""
        return self.dumped("-dtb", str(self.compile(dts)), *machine_args)

    def tree(self, *machine_args, dts=MINIMAL_DTS):
        """The fixed-up @dts as fdt.parse() gives it"""
        return fdt.load(self.fixed_up(*machine_args, dts=dts))

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

    def test_identity(self):
        """The model with the board revision, and the serial number"""
        for serial, text in ((None, "0123456789abcdef"),
                             ("0x1122", "0000000000001122")):
            with self.subTest(serial=serial):
                args = ("-M", f"serial={serial}") if serial else ()
                tree = self.tree(*args)
                self.assertEqual(tree["/"]["model"],
                                 string("Raspberry Pi 5 Model B Rev 1.0"))
                self.assertEqual(tree["/"]["serial-number"], string(text))
                self.assertEqual(tree["/chosen"]["rpi-serial64"],
                                 string(text))
                self.assertEqual(tree["/system"]["linux,serial"],
                                 bytes.fromhex(text))

    def test_command_line(self):
        """The tree's bootargs, then the firmware's arguments, then
        -append, which plays cmdline.txt, two spaces apart"""
        own = "reboot=w coherent_pool=1M"
        for dts, append, expected in (
                (MINIMAL_DTS, "", FIRMWARE_ARGS),
                (MINIMAL_DTS, "console=ttyAMA10,115200",
                 f"{FIRMWARE_ARGS}  console=ttyAMA10,115200"),
                (MINIMAL_DTS + FIRMWARE_NODES, "", f"{own}  {FIRMWARE_ARGS}"),
                (MINIMAL_DTS + FIRMWARE_NODES, "root=/dev/mmcblk0p2",
                 f"{own}  {FIRMWARE_ARGS}  root=/dev/mmcblk0p2")):
            with self.subTest(bootargs=dts != MINIMAL_DTS, append=append):
                tree = self.tree("-append", append, dts=dts)
                self.assertEqual(tree["/chosen"]["bootargs"],
                                 string(expected))

    def test_chosen(self):
        """What the firmware reports about the boot, the power supply, the
        memory and config.txt, and the entropy it passes on"""
        for ram, gbit in (("1G", 8), ("2G", 16), ("8G", 64)):
            with self.subTest(ram=ram):
                chosen = self.tree("-m", ram)["/chosen"]
                self.assertEqual(chosen["rpi-sdram-size-gbit"], cells(gbit))
        tree = self.tree()
        self.assertEqual(tree["/chosen"]["os_prefix"], string(""))
        self.assertEqual(tree["/chosen"]["overlay_prefix"],
                         string("overlays/"))
        self.assertEqual(len(tree["/chosen"]["kaslr-seed"]), 8)
        self.assertEqual(len(tree["/chosen"]["rng-seed"]), 32)
        self.assertEqual(tree["/chosen/bootloader"], {
            "count": cells(1),              # the first boot since power-on
            "arg1": cells(0),
            "capabilities": cells(0),
            "tryboot": cells(0),
            "rsts": cells(0x1000),          # power-on reset
            "partition": cells(0),
            "boot-mode": cells(3),          # RPIBOOT
        })
        self.assertEqual(tree["/chosen/power"], {
            "power_reset": cells(0),
            "usb_over_current_detected": cells(0),
            "usb_max_current_enable": cells(1),
            "max_current": cells(5000),
        })

    def test_booted_tree(self):
        """A tree that has been through the firmware already gets new
        values where the firmware writes them, and keeps the rest"""
        tree = self.tree(dts=MINIMAL_DTS + BOOTED_NODES)
        self.assertEqual(tree["/chosen/bootloader"]["count"], cells(1))
        self.assertEqual(tree["/chosen/bootloader"]["version"],
                         string("0123456789abcdef0123456789abcdef01234567"))
        self.assertEqual(tree["/chosen/power"]["max_current"], cells(5000))

    def test_prefixes(self):
        """A tree that names the config.txt prefixes keeps them"""
        tree = self.tree(dts=MINIMAL_DTS + PREFIX_NODES)
        self.assertEqual(tree["/chosen"]["os_prefix"], string("next/"))
        self.assertEqual(tree["/chosen"]["overlay_prefix"],
                         string("ovl/"))

    def test_cma_size(self):
        """A CMA pool sized in one cell gets the parent's two, as the
        firmware writes it (Linux warns of old firmware otherwise)"""
        tree = self.tree(dts=MINIMAL_DTS + FIRMWARE_NODES)
        self.assertEqual(tree["/reserved-memory/linux,cma"]["size"],
                         cells(0, 0x4000000))

    def test_ethernet_address(self):
        """ethernet0 gets the address the command line gives"""
        tree = self.tree(dts=MINIMAL_DTS + FIRMWARE_NODES)
        self.assertEqual(tree["/ethernet@100000"]["local-mac-address"],
                         bytes.fromhex(MAC.replace(":", "")))

    def test_bootloader_config(self):
        """The bootloader configuration's node points at a copy in the
        VideoCore's memory, which holds it once the machine is up"""
        dtb = self.compile(MINIMAL_DTS + FIRMWARE_NODES)
        tree = fdt.load(self.dumped("-dtb", str(dtb)))
        node = tree["/reserved-memory/nvram@0"]
        self.assertEqual(node["reg"], cells(0, BLCONFIG_ADDR, 0, len(BLCONFIG)))
        self.assertEqual(node["status"], string("okay"))

        qmp = Qmp(self, "-M", "raspi5b", "-S", "-kernel", str(GUEST),
                  "-dtb", str(dtb))
        self.assertEqual(qmp.memory(BLCONFIG_ADDR, len(BLCONFIG)), BLCONFIG)

    def test_armstub_reserved(self):
        """With -bios, the built-in tree reserves what the firmware's tree
        reserves for BL31, or all of a larger image in 64 KiB steps"""
        tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, tmp)
        stub = tmp / "armstub"
        for size, reserved in ((4096, 0x80000), (0x80001, 0x90000)):
            with self.subTest(size=hex(size)):
                stub.write_bytes(bytes(size))
                tree = fdt.load(self.dumped("-bios", str(stub),
                                            machine="raspi5b,secure=on"))
                node = tree["/reserved-memory/atf@0"]
                self.assertEqual(node["reg"], cells(0, 0, 0, reserved))
                self.assertIn("no-map", node)

    def test_builtin_tree(self):
        """The built-in tree, fix-ups included, as checked in"""
        tree = fdt.load(self.dumped("-append", "console=ttyAMA10,115200"))
        expected = [line for line in BUILTIN_TREE.read_text().splitlines()
                    if not line.startswith("#")]
        self.assertEqual(fdt.dump(tree), expected)


class Qmp:
    """QEMU run for a test, driven over QMP on its standard input and
    output"""

    def __init__(self, test, *args):
        self.tmp = Path(tempfile.mkdtemp())
        test.addCleanup(shutil.rmtree, self.tmp)
        self.proc = subprocess.Popen(
            [str(QEMU), "-display", "none", "-serial", "none",
             "-monitor", "none", "-qmp", "stdio", *args],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True)
        test.addCleanup(self.close)
        self.lines = queue.Queue()
        self.reader = threading.Thread(target=self.read_lines, daemon=True)
        self.reader.start()
        self.events = []
        self.receive()                  # the greeting
        self.execute("qmp_capabilities")

    def read_lines(self):
        for line in self.proc.stdout:
            self.lines.put(line)
        self.lines.put(None)

    def receive(self):
        line = self.lines.get(timeout=TIMEOUT)
        if line is None:
            raise AssertionError("QEMU exited")
        message = json.loads(line)
        if "event" in message:
            self.events.append(message["event"])
        return message

    def execute(self, command, **arguments):
        self.proc.stdin.write(json.dumps({"execute": command,
                                          "arguments": arguments}) + "\n")
        self.proc.stdin.flush()
        while True:
            message = self.receive()
            if "error" in message:
                raise AssertionError(f"{command}: {message['error']}")
            if "return" in message:
                return message["return"]

    def wait_event(self, name):
        while name not in self.events:
            self.receive()
        self.events.remove(name)

    def reset(self):
        self.execute("system_reset")
        self.wait_event("RESET")

    def migrate(self, command, uri):
        self.execute(command, uri=uri)
        deadline = time.monotonic() + TIMEOUT
        while time.monotonic() < deadline:
            status = self.execute("query-migrate").get("status")
            if status == "completed":
                return
            if status == "failed":
                raise AssertionError("migration failed")
            time.sleep(0.01)
        raise AssertionError("migration timed out")

    def memory(self, addr, size):
        path = self.tmp / "memory"
        self.execute("pmemsave", val=addr, size=size, filename=str(path))
        return path.read_bytes()

    def tree(self, addr):
        """The device tree in guest memory at @addr"""
        (size,) = struct.unpack(">I", self.memory(addr + 4, 4))
        return fdt.parse(self.memory(addr, size))

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait()
        self.reader.join(TIMEOUT)
        self.proc.stdin.close()
        self.proc.stdout.close()


@unittest.skipUnless(QEMU.exists() and GUEST.exists(),
                     "build QEMU and the guest first (make build guest)")
class BootValuesTest(unittest.TestCase):
    """What the firmware writes afresh for each boot, read from the tree
    the guest gets, at 0 for this guest, before it starts"""

    def start(self, *args, machine="raspi5b"):
        return Qmp(self, "-M", machine, "-S", "-kernel", str(GUEST), *args)

    @staticmethod
    def boot(tree):
        """What /chosen/bootloader says of the boot"""
        node = tree["/chosen/bootloader"]
        return {name: struct.unpack(">I", node[name])[0]
                for name in ("rsts", "partition", "tryboot", "count")}

    def test_each_boot(self):
        """Each reset counts a boot and brings new seeds; nothing else
        changes (a watchdog reset's status and partition are the bare-metal
        suite's pm/watchdog-reset)"""
        qmp = self.start()
        first = qmp.tree(0)
        qmp.reset()
        second = qmp.tree(0)
        self.assertEqual(first["/chosen/bootloader"]["count"], cells(1))
        self.assertEqual(second["/chosen/bootloader"]["count"], cells(2))
        for name in ("kaslr-seed", "rng-seed"):
            self.assertNotEqual(first["/chosen"][name],
                                second["/chosen"][name])
        for tree in (first, second):
            del tree["/chosen/bootloader"]["count"]
        self.assertEqual(fdt.dump(first), fdt.dump(second))

    def test_carried_reset(self):
        """A machine given what a reset left, as its properties read it
        before QEMU makes the reset, starts with the boot that reset
        would have started: its reset status and tryboot, and the count
        on, from the partition the files came from"""
        qmp = self.start(machine="raspi5b,reset-status=0x30,reboot-flags=1,"
                                 "boot-count=4,boot-partition=3")
        self.assertEqual(self.boot(qmp.tree(0)), {
            "rsts": 0x30, "partition": 3, "tryboot": 1, "count": 5})
        # The flags were for that boot only
        qmp.reset()
        self.assertEqual(self.boot(qmp.tree(0)), {
            "rsts": 0x30, "partition": 3, "tryboot": 0, "count": 6})

    def test_reset_partition(self):
        """Without boot-partition, the partition is the one the reset
        status asks for: 4, in bits 0, 2, .. 10"""
        qmp = self.start(machine="raspi5b,reset-status=0x1010")
        self.assertEqual(self.boot(qmp.tree(0)), {
            "rsts": 0x1010, "partition": 4, "tryboot": 0, "count": 1})

    def test_count_wraps(self):
        """The count is the firmware's 8-bit one"""
        qmp = self.start()
        for _ in range(255):
            qmp.reset()
        self.assertEqual(qmp.tree(0)["/chosen/bootloader"]["count"],
                         cells(0))

    def test_count_migrates(self):
        """A migrated machine counts on from its source's boots"""
        src = self.start()
        src.reset()
        src.reset()
        path = src.tmp / "migration"
        src.migrate("migrate", f"exec:cat > {path}")
        src.close()

        dst = self.start("-incoming", "defer")
        dst.migrate("migrate-incoming", f"exec:cat {path}")
        self.assertEqual(dst.tree(0)["/chosen/bootloader"]["count"],
                         cells(3))
        dst.reset()
        self.assertEqual(dst.tree(0)["/chosen/bootloader"]["count"],
                         cells(4))


if __name__ == "__main__":
    unittest.main()
