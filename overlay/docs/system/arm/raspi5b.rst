.. SPDX-License-Identifier: GPL-2.0-or-later

Raspberry Pi 5 (``raspi5b``)
============================

The ``raspi5b`` machine models the Raspberry Pi 5 Model B, built around the
Broadcom BCM2712 SoC: four Cortex-A76 cores, a GIC-400 interrupt controller
and a 40-bit physical address map with peripherals above ``0x10_0000_0000``.
Most board I/O (Ethernet, USB, GPIO, the 40-pin header) lives on the RP1
south bridge behind PCIe, which is not modelled yet.

The machine is under active development; it currently targets bare-metal
and microkernel bring-up, with Linux support following.

Implemented devices
-------------------

* 1 to 4 Cortex-A76 CPUs (``-smp``), ``MPIDR_EL1.Aff1`` = core number,
  with ``MPIDR_EL1.MT`` set as on the silicon
* ARM generic timer at 54 MHz
* GIC-400 (GICv2 with Virtualization Extensions)
* System timer at ``0x10_7c00_3000``: 1 MHz free-running counter and four
  comparators on SPIs 64 to 67
* Power management block at ``0x10_7d20_0000``: the watchdog, which resets
  the machine (or powers it off, for Linux's partition 63 halt request)
  and follows ``-action watchdog=...``, and the reset status register,
  which survives the reset and reports a watchdog reset
* VideoCore mailbox at ``0x10_7c01_3880`` (SPI 33) with the firmware's
  property and framebuffer channels. The firmware reads requests through
  the VideoCore's view of memory: the first GiB of RAM at bus addresses
  ``0x0`` (as Linux passes them) and ``0xc000_0000`` (as code for older
  Pis does); requests elsewhere get no answer, as on hardware. The
  VideoCore keeps the top 4 MiB of that GiB, which the device tree memory
  node leaves out
* RNG200 random number generator at ``0x10_7d20_8000``, fed by QEMU's
  random source (reproducible with ``-seed``)
* UART10: the PL011 debug UART at ``0x10_7d00_1000``, connected to the
  first ``-serial`` backend
* 1, 2, 4, 8 or 16 GiB of RAM at physical address 0 (``-m``; default 2 GiB)

Every other block of the BCM2712 memory map is an ``unimplemented-device``
placeholder: reads return zero, writes are ignored and both are reported
with ``-d unimp``.

Missing devices
---------------

* Firmware property tags specific to the Pi 5 (clocks, power, RTC, GPIO
  expander); the BCM283x set is answered
* SD/eMMC controllers, PCIe root complexes and the RP1 south bridge
* GPIO, pin control, the Broadcom L2 interrupt controllers
* Power domains (only V3D's is driven by Linux on this SoC)
* Display (HVS, HDMI), V3D and ISP

Boot and exception levels
-------------------------

On real hardware the VideoCore firmware loads Trusted Firmware-A BL31 at
physical address 0; BL31 enters the operating system at EL2 and provides
PSCI through ``SMC``. By default the machine reproduces this contract with
QEMU's built-in PSCI emulation:

* EL3 is not implemented and the guest starts in EL2;
* only CPU 0 runs, the others are started with PSCI ``CPU_ON``;
* the GIC has no Security Extensions, so every interrupt is Non-secure.

With ``-machine raspi5b,secure=on`` the guest owns EL3 instead: CPUs and the
GIC implement the Security Extensions, PSCI is not emulated for guests that
start in EL3, and every CPU starts at the image entry point.

``-kernel`` accepts an AArch64 Linux ``Image`` (booted using the Linux boot
protocol, with the device tree address in ``x0``) or an ELF file (entered at
its entry point; the device tree is placed at the base of RAM if it fits
below the image).

Booting firmware with ``-bios``
-------------------------------

With ``secure=on``, ``-bios`` loads what the VideoCore firmware runs at
EL3 (the ``armstub=`` of its ``config.txt``; TF-A's ``rpi5`` BL31 on a
stock Pi 5) and hands it the rest the way the firmware does:

* the image is loaded at physical address 0, and every CPU starts there
  in EL3; PSCI is left to the image;
* ``-kernel`` is loaded at ``0x20_0000``, the firmware's
  ``kernel_address`` for 64-bit kernels (a Linux ``Image`` at that address
  plus its ``text_offset``, an ELF file at its own addresses);
* ``-initrd`` goes at 128 MiB or above the kernel, whichever is higher,
  and the device tree at the next 2 MiB boundary, or at
  ``-machine dtb-address=<addr>``, the counterpart of
  ``device_tree_address=``. None of them may overlap another, the kernel
  counting with its BSS, and all of it, the armstub included, must fit in
  the first GiB, below the VideoCore's memory;
* an image that starts with the header of TF-A's Raspberry Pi ports (the
  magic ``0x5afe570b`` at offset ``0xf0``) gets the magic cleared and the
  device tree and kernel addresses written at offsets ``0xf8`` and
  ``0xfc``; any other image is loaded as it is.

The built-in device tree then describes PSCI through ``SMC`` and reserves
the first 512 KiB of RAM for BL31 (``/reserved-memory/atf@0``), as the
firmware's tree does. A system reset loads every image again, and the
firmware starts over.

TF-A's ``rpi5`` port counts on all four cores: with fewer (``-smp``), PSCI
``CPU_ON`` of a missing core succeeds but the core never starts. Its
release builds run; debug builds also read the cluster registers of the
DynamIQ Shared Unit, which are not modelled.

What runs on TF-A is the kernel it is given: Linux itself, or U-Boot
built with ``rpi_arm64_defconfig``, as the firmware's ``kernel=``. The
community EDK2 port for the Pi 5 carries its own TF-A in its
``RPI_EFI.fd`` and expects its device tree at ``0x1f_0000``, which its
``config.txt`` asks of the firmware: load it with ``-bios RPI_EFI.fd``,
the device tree its release ships with ``-dtb``, and
``dtb-address=0x1f0000``. Without SD card, USB or network models, U-Boot
and EDK2 find nothing to boot on their own.

Firmware property interface
---------------------------

The property channel answers the BCM283x tag set. The tags that identify
the board and the firmware answer as follows:

* board revision: the new-style code of a Pi 5 with the configured RAM;
* board serial number: ``-machine raspi5b,serial=<n>`` (a 64-bit number;
  the default, ``0x0123456789abcdef``, is made up);
* firmware revision, variant and hash: a fixed revision, the standard
  ("start") firmware and an all-zero hash, since no firmware build stands
  behind the model;
* ARM and VideoCore memory: the first GiB less the top 4 MiB, and those
  4 MiB;
* command line: the ``-append`` string;
* DMA channels: 0 to 10, the channels of the ``dma32`` and ``dma40``
  device tree nodes.

Every answer stays within the value buffer its tag declares: a buffer too
small for it gets as much as fits, and the tag's response length says how
much the whole answer needs (the command line is the exception, copied
only when it fits, as the firmware does). A request that is cut short
inside a tag, or whose tag runs past the request's own length, is answered
with the interface's error code, ``0x80000001``; a request the VideoCore
cannot reach is not answered at all.

Reset and power-off
-------------------

PSCI ``SYSTEM_RESET``, the watchdog and the monitor's ``system_reset`` all
reset the machine the same way: every device returns to its reset state,
RAM is kept, the images given with ``-bios``, ``-kernel`` and ``-dtb`` are
loaded again and the boot starts over as from power-on, except that the PM
block's reset status register (``RSTS``) keeps its value and records a
watchdog reset. PSCI ``SYSTEM_OFF`` and Linux's halt request through the
watchdog (boot partition 63 in ``RSTS``) power the machine off, and QEMU
exits with status 0.

Device tree
-----------

Without ``-dtb``, the machine generates a device tree describing what it
models, derived from its memory map: the CPUs with PSCI, the generic timer,
the PMU, the GIC, the system timer, the mailbox and the firmware interface,
the PM block, the RNG, UART10 (``serial10``, the ``stdout-path``), the fixed
clocks, and a CMA pool in the first GiB, where the VideoCore can reach
Linux's buffers. Node names and properties follow Linux's ``bcm2712.dtsi``
and the firmware's tree, and the result validates against the Linux
bindings, but for what the firmware adds for the OS, which no binding
describes. ``-machine raspi5b,builtin-dtb=off`` gives the guest no device
tree at all, like an empty ``device_tree=`` line in the firmware's
``config.txt``.

Whichever tree the guest gets, generated or given with ``-dtb`` (for
example ``bcm2712-rpi-5-b.dtb``), the machine changes it as the VideoCore
firmware does before it starts the OS:

* the root's ``model`` names the board's revision ("Raspberry Pi 5 Model
  B Rev 1.0"); ``serial-number``, ``/chosen/rpi-serial64`` and
  ``/system/linux,serial`` give the serial number, and
  ``/system/linux,revision`` the board revision code;
* ``/chosen/bootargs`` is the tree's own ``bootargs``, then what the
  firmware adds for the board (``smsc95xx.macaddr=`` with the board's
  Ethernet address, ``vc_mem.mem_base=`` and ``vc_mem.mem_size=`` for the
  VideoCore's memory), then ``-append`` in the place of ``cmdline.txt``,
  two spaces apart, as the firmware joins them;
* ``/chosen`` gets ``kaslr-seed`` and ``rng-seed`` from QEMU's random
  source (reproducible with ``-seed``), ``os_prefix`` and
  ``overlay_prefix`` at their defaults, and the RAM size in
  ``rpi-sdram-size-gbit``;
* ``/chosen/bootloader`` describes the boot: ``boot-mode`` 3, RPIBOOT, in
  which the host supplies the boot files, as QEMU does; ``rsts``, the PM
  block's reset status as the boot found it; ``partition``, the
  partition asked for there (0 at power-on), which files the host
  supplies come from; ``count``, the boots since power-on, in 8 bits;
  and 0 for ``tryboot``, ``arg1`` and ``capabilities``;
* ``/chosen/power`` reports a 5 A bench supply (``max_current``), which
  turns the USB ports' high current limit on;
* the memory node leaves out the VideoCore's memory, and a
  ``/reserved-memory/linux,cma`` pool sized in one cell gets the two of
  its parent, without which Linux warns that the firmware is out of date;
* a ``raspberrypi,bootloader-config`` node (``nvram@0``, alias
  ``blconfig``, in the firmware's tree) is enabled and points at a copy of
  the bootloader configuration in the VideoCore's memory: a Pi 5's
  default, for that supply;
* the node ``ethernet0`` names gets the board's Ethernet address in
  ``local-mac-address``;
* nodes of devices that are not modelled yet get ``status = "disabled"``,
  and CPU nodes of cores ``-smp`` leaves out ``status = "fail"``.

Every reset gives the next boot its own tree, as the firmware writes one
for each boot: ``rsts``, ``partition``, ``count`` and the seeds are the
new boot's. The count moves with the machine in migration.

Compared with the tree the firmware gives a Pi 5, the machine's lacks:

* the bootloader's build (``version``, ``build-timestamp``,
  ``update-timestamp`` in ``/chosen/bootloader``), since none stands behind
  the model; ``rpi-eeprom-update`` then finds nothing to update;
* USB-PD details in ``/chosen/power`` (``usbpd_power_data_objects``,
  ``rpi_power_supply``), as for a bench supply;
* the NUMA arguments the firmware adds to the command line
  (``numa=fake=``, ``system_heap.max_order=``,
  ``iommu_dma_numa_policy=``), which follow how the SDRAM's banks are
  mapped; nor does ``console=serial0`` in ``-append`` become the UART
  that the ``serial0`` alias names;
* ``rpi-duid``, ``rpi-machine-id``, ``rpi-boardrev-ext`` and
  ``rpi-min-boot-ver``, which come from the board's QR code, its OTP and
  a hash the firmware does not publish;
* anything ``config.txt`` would change: no overlays and no ``dtparam``, as
  with an empty ``config.txt``.

``arg1`` and ``tryboot`` stay 0, as the property interface does not
model the requests that set them, and its command line tag answers with
``-append`` alone.

Examples
--------

Bare-metal ELF payload::

  $ qemu-system-aarch64 -M raspi5b -nographic -kernel payload.elf

Linux on the built-in device tree, with the console on UART10::

  $ qemu-system-aarch64 -M raspi5b -m 4G -nographic -kernel Image \
      -append "console=ttyAMA10,115200"

Linux on the firmware's device tree::

  $ qemu-system-aarch64 -M raspi5b -m 4G -nographic \
      -kernel Image -dtb bcm2712-rpi-5-b.dtb \
      -append "console=ttyAMA10,115200 earlycon=pl011,mmio32,0x107d001000"

Linux started by TF-A's BL31, as the firmware starts it::

  $ qemu-system-aarch64 -M raspi5b,secure=on -m 4G -nographic \
      -bios bl31.bin -kernel Image -dtb bcm2712-rpi-5-b.dtb \
      -append "console=ttyAMA10,115200"

U-Boot on TF-A, with a kernel for ``booti`` in memory::

  $ qemu-system-aarch64 -M raspi5b,secure=on -m 4G -nographic \
      -bios bl31.bin -kernel u-boot.bin \
      -device loader,file=Image,addr=0x10000000,force-raw=on \
      -append "console=ttyAMA10,115200"
  U-Boot> booti 0x10000000 - ${fdt_addr}

The EDK2 port, whose shell F1 starts during its boot countdown::

  $ qemu-system-aarch64 -M raspi5b,secure=on,dtb-address=0x1f0000 \
      -nographic -bios RPI_EFI.fd -dtb bcm2712-rpi-5-b.dtb
