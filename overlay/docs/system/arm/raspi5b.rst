.. SPDX-License-Identifier: GPL-2.0-or-later

Raspberry Pi 5 (``raspi5b``)
============================

The ``raspi5b`` machine models the Raspberry Pi 5 Model B, built around the
Broadcom BCM2712 SoC: four Cortex-A76 cores, a GIC-400 interrupt controller
and a 40-bit physical address map with peripherals above ``0x10_0000_0000``.
Most board I/O (Ethernet, USB, GPIO, the 40-pin header) lives on the RP1
south bridge behind PCIe, which is not modelled yet.

The machine is under active development. Bare-metal code, the Pi's boot
firmware (TF-A, U-Boot, the EDK2 port) and Linux run on it, the latter
with its root file system on an SD card or an NVMe drive.

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
  node leaves out. The firmware's framebuffer, the machine's display, lies
  1 MiB into it: 640 x 480 pixels at 16 bits per pixel at boot, then the
  size and depth a guest sets, of which it keeps only the lines that fit
  in the 3 MiB left (all of 1024 x 768 at 32 bits per pixel)
* RNG200 random number generator at ``0x10_7d20_8000``, fed by QEMU's
  random source (reproducible with ``-seed``)
* The AVS monitor's temperature sensor at ``0x10_7d54_2000``, which
  Linux's ``bcm2711_thermal`` driver reads for ``thermal_zone0``. It
  reports the SoC's temperature, 25 degrees C by default, in steps of
  0.55 degrees C from -112.65 to 450 degrees C. The ``temperature``
  property of ``bcm2711-avs-monitor``, in millidegrees C, sets it:
  ``-global bcm2711-avs-monitor.temperature=65000`` at startup, or
  ``qom-set /machine/soc/avs-monitor temperature 65000`` while the
  machine runs; a value beyond the range is refused. At 110 degrees C,
  the critical trip in both device trees, Linux powers the machine off.
  The monitor's other registers read as zero and ignore writes
* The seven Broadcom level 2 interrupt controllers of the firmware's
  device tree, each in front of one SPI: the edge-latching
  ``brcm,l2-intc`` for the firmware's doorbells, ``brcm,bcm2711-l2-intc``
  (the same registers, a status that follows the inputs) for the display
  and the always-on block, and the level layout
  (``brcm,bcm7271-l2-intc``) for the other four, among them those of the
  GPIO block and the HDMI I2C controllers. The tree leaves four of them
  disabled, as the Raspberry Pi 5's does
* The two Broadcom GPIO blocks: GIO at ``0x10_7d50_8500``, 32 + 22 lines,
  interrupting through the main level 2 controller, and GIO AON at
  ``0x10_7d51_7c00``, 17 + 6 lines, whose interrupt is not connected, as
  no device tree gives it one. Every line detects edges and levels,
  outputs included
* The pin controllers beside them, at ``0x10_7d50_4100`` (12 registers)
  and ``0x10_7d51_0700`` (8 registers, always-on). They keep the function
  and pull that software selects for each pin, so that Linux reads its
  settings back, but the settings have no effect on the lines
* The DDC I2C controllers of the two HDMI ports, at ``0x10_7d50_8200``
  and ``0x10_7d50_8280``, interrupting through their level 2 controller.
  A transfer completes at once, whatever the bus speed; the combined
  formats, a write and a read in one command, are not modelled, as Linux
  does not use them. The board puts a monitor's EDID, QEMU's ``i2c-ddc``,
  at address 0x50 on HDMI0's bus, ``i2c-bus.0``. HDMI1's, ``i2c-bus.1``,
  is empty: ``-device i2c-ddc,bus=i2c-bus.1,address=0x50`` connects a
  second monitor
* On the board, the power button on GIO 20, which reads high until it is
  pressed (see below), and the green activity LED on GIO AON 9, a QEMU
  ``led`` device lit while its line is low, whose changes show as
  ``led_set_intensity`` and ``led_change_intensity`` trace events. With
  the firmware's device tree, Linux's ``gpio-leds`` waits for the power
  LED, which RP1 drives, and so leaves the activity LED alone too
* The two SD hosts: SDIO1 at ``0x10_00ff_f000`` (SPI 273), for the SD
  card slot, and SDIO2 at ``0x10_0110_0000`` (SPI 274), wired to the
  Wi-Fi radio, which is not modelled. Each is an SD Host Controller 3.00
  with SDMA and ADMA2 with 64-bit addresses, followed by Broadcom
  configuration registers that keep what software writes, to no effect.
  Cards run at 3.3 V in high-speed mode: QEMU's cards do not switch to
  1.8 V, so the UHS-I modes the controllers offer are never used. The
  command queueing engine is not modelled. The card slot is described
  under `SD card`_
* UART10: the PL011 debug UART at ``0x10_7d00_1000``, connected to the
  first ``-serial`` backend
* UARTA: the 16550 wired to the Bluetooth radio, at ``0x10_7d50_c000``,
  connected to the second ``-serial`` backend. Its 8-bit registers are 4
  bytes apart, its FIFOs hold 32 bytes, and its baud rate divides a
  96 MHz clock by 16 and by the divisor. The receive FIFO interrupts at
  the 16550A's trigger levels, 1, 4, 8 or 14 bytes, where Linux's driver
  gives the BCM7271 UART 1, 8, 16 or 30. The radio is not modelled: its
  node in the firmware's device tree is disabled, which leaves Linux's
  ``ttyS0`` a plain serial port. The Raspberry Pi OS kernel registers
  8250 ports only when its command line asks for them, which the
  firmware's tree does with ``8250.nr_uarts=1``; with the built-in tree,
  add it to ``-append``
* The three PCIe root complexes, PCIe0 to PCIe2 at ``0x10_0010_0000``,
  ``0x10_0011_0000`` and ``0x10_0012_0000``, with the reset controller
  and PHY calibration block their Linux driver uses. Each has a
  ``BCM2712 PCIe Bridge`` root port, rev 0x21 as on a C1 stepping, whose
  capabilities are those ``lspci`` shows on a Pi 5: PCIe1's link is x1
  at up to 5 GT/s with ASPM L0s and L1, the external connector;
  PCIe2's is x4 at 5 GT/s with ASPM L1 and L1 PM substates, and holds
  RP1 on a Pi 5, which is not modelled, so its bus is empty; PCIe0's is
  taken to be PCIe1's. A device goes on a root port's bus,
  ``pcie0.0``, ``pcie1.0`` or ``pcie2.0``, with ``-device ...,bus=``,
  and the link trains at once when the driver lets it; an empty port
  reports its link down, as a Pi 5 does with nothing in the connector,
  and Linux gives up on it. Each maps a 16 GiB aperture of the AXI bus
  into PCI memory and reaches all of RAM through the inbound windows
  Linux sets
* The two MIPs, at ``0x10_0013_0000`` and ``0x10_0013_1000``, which turn
  MSIs from PCIe2 and PCIe1 into SPIs: a device's MSI writes its vector
  to the MIP's first register, through the root complex's inbound window
  for the MIP's page. MIP0's 64 vectors raise SPIs 128 to 191, MIP1's
  vectors 8 to 15 SPIs 255 to 262, each an edge unless configured as a
  level. PCIe0 has no MIP: its devices' MSIs go to the root complex's
  own MSI target, as they can on the others
* 1, 2, 4, 8 or 16 GiB of RAM at physical address 0 (``-m``; default 2 GiB)

Every other block of the BCM2712 memory map is an ``unimplemented-device``
placeholder: reads return zero, writes are ignored and both are reported
with ``-d unimp``.

Missing devices
---------------

* The RP1 south bridge
* The Bluetooth radio on UARTA and the Wi-Fi radio on SDIO2
* The power LED, which RP1 drives
* Power domains (only V3D's is driven by Linux on this SoC)
* Display (HVS, HDMI), V3D and ISP
* The system DMA controller, whose channels no modelled device uses

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
start in EL3, and every CPU starts at the image entry point. The guest
starts in EL3 when it is firmware given with ``-bios`` (see below) or an
ELF file given with ``-kernel``; a Linux ``Image`` given with ``-kernel``
alone still starts in EL2 with QEMU's PSCI, as the Linux boot protocol
asks, whatever ``secure`` says.

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
  plus its ``text_offset``, taken as ``0x8_0000`` for kernels before
  Linux 3.17, whose header has no ``image_size``; an ELF file at its own
  addresses, with its entry point in one of its segments);
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
firmware's tree does, or all of a larger image, rounded up to 64 KiB. A
tree given with ``-dtb`` keeps its own reservations, as the firmware
leaves them: ``bcm2712-rpi-5-b.dtb`` reserves the same 512 KiB. A system
reset loads every image again, and the firmware starts over.

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
``dtb-address=0x1f0000``. With no USB or network models, an SD card is
the only place U-Boot and EDK2 can find something to boot on their own:
U-Boot boots, for example, by the ``extlinux.conf`` it finds on one, and
EDK2 maps the card's partitions.

PCIe devices
------------

PCIe1 is the board's external connector, and QEMU's PCI Express devices
plug into it, on bus ``pcie1.0``. Its root port links to one device, so
more than one takes a switch. Raspberry Pi OS's kernel has drivers for
NVMe drives built in and Intel's ``igb`` Ethernet as a module; it has no
virtio. For example, an NVMe drive holding the root file system and an
``igb`` network card behind a switch::

  $ qemu-system-aarch64 -M raspi5b -m 2G -kernel kernel_2712.img \
      -append "console=ttyAMA10,115200 root=/dev/nvme0n1 rootwait" \
      -drive if=none,id=nvme0,file=root.img,format=raw \
      -device x3130-upstream,id=up0,bus=pcie1.0 \
      -device xio3130-downstream,id=dn0,bus=up0,chassis=8,slot=0 \
      -device xio3130-downstream,id=dn1,bus=up0,chassis=8,slot=1 \
      -device nvme,serial=raspi5b,drive=nvme0,bus=dn0 \
      -netdev user,id=net0 -device igb,netdev=net0,bus=dn1

The root ports take chassis 0 to 2, slot 0, so a switch's downstream
ports need another chassis.
There is no PCI I/O space, as on the Pi 5, so I/O BARs stay unassigned.
MIP1 has 8 vectors for the whole connector, which the devices behind it
share: a device that cannot get the MSI-X vectors it asks for falls back
to MSI or INTx, as ``igb`` does next to an NVMe drive.

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

Where the Pi 5's firmware answers the tag set differently, or knows tags
the older Pis' firmware does not, the channel answers as the Pi 5's:

* clocks: the list holds the five clocks the Pi 5's device trees take from
  the firmware, ARM, CORE, V3D, ISP and HEVC, each on and at its most at
  boot, with ``config.txt``'s default ranges: 1.5 to 2.4 GHz for the ARM,
  500 to 960 MHz for V3D and 500 to 910 MHz for the others. A guest turns
  a clock off and on, and sets its rate, which the firmware keeps within
  the range, as Linux's ``raspberrypi-cpufreq`` sets the ARM's; the
  rates change what the firmware reports, not how fast anything runs. A
  clock that is not in the list has a rate of 0 and reports that it does
  not exist;
* power: the domains of the newer power interface, 1 to 23, of which only
  the ARM's is on at boot, and the devices of the older one, 0 to 8, all
  off at boot. Linux's ``raspberrypi-power`` switches them; no modelled
  device depends on their states. A domain that does not exist stays off,
  and a device that does not exist says so;
* temperatures: the SoC's, as the AVS monitor's sensor reads it, and the
  limit, 85 degrees C, ``config.txt``'s ``temp_limit``;
* reboot flags: a guest sets them before a reset, as Linux does for
  ``reboot "0 tryboot"``, and the next boot takes them: its device tree
  reports the tryboot, as described below. The reboot notification that
  follows has nothing to answer;
* the real-time clock, which Linux's ``rpi-rtc`` driver reads and sets,
  for ``hwclock``, as do EDK2's ``date`` and ``time``: its time starts as
  QEMU's RTC (``-rtc``), runs on through a reset, and a guest setting it
  raises the ``RTC_CHANGE`` event. Its alarm goes off when the time
  reaches it, if enabled, and stays pending until cleared; on a Pi 5 it
  powers the board back on after a halt, which QEMU leaves out, as its
  board is never off. The charger of a backup battery keeps the voltage a
  guest sets within its range, 1.3 to 4.4 V, or off; no battery is
  fitted, and it reads 0 V.

Every answer stays within the value buffer its tag declares: a buffer too
small for it gets as much as fits, and the tag's response length says how
much the whole answer needs (the command line is the exception, copied
only when it fits, as the firmware does). A request that is cut short
inside a tag, or whose tag runs past the request's own length, is answered
with the interface's error code, ``0x80000001``; a request the VideoCore
cannot reach is not answered at all. A tag the model lacks is answered
with no value and logged as unimplemented.

``vcgencmd`` sends its commands as text through ``GET_GENCMD_RESULT``,
and the firmware answers these in the Pi 5's forms:

* ``measure_temp``: the temperature the AVS monitor reads, as
  ``temp=24.9'C``;
* ``measure_clock NAME``: the rate of the clock ``arm``, ``core``,
  ``v3d``, ``isp`` or ``hevc`` as its tags set it, as
  ``frequency(0)=2400000000``, and 0 for any other clock, which the model
  does not have;
* ``get_config NAME`` and ``get_config int``: the settings the machine
  decides, each clock's range (``arm_freq``, ``arm_freq_min`` and so on),
  ``temp_limit`` and ``total_mem``; the machine does not read
  ``config.txt``, so every other setting reads 0;
* ``get_throttled``: ``throttled=0x0``, as nothing is throttled;
* ``version``: the firmware release and hash the identity tags report;
* ``commands``: the list above.

Any other command gets error ``-1`` and ``error=1 error_msg="Command not
registered"``, as the firmware answers a command it lacks, and is logged
as unimplemented. That includes ``bootloader_version``, so
``rpi-eeprom-update`` finds no bootloader to update.

Reset and power-off
-------------------

PSCI ``SYSTEM_RESET``, the watchdog and the monitor's ``system_reset`` all
reset the machine the same way: every device returns to its reset state,
RAM is kept, the images given with ``-bios``, ``-kernel`` and ``-dtb`` are
loaded again and the boot starts over as from power-on, except that the PM
block's reset status register (``RSTS``) keeps its value and records a
watchdog reset, the reboot flags a guest set in the firmware reach the
boot the reset starts, and the firmware's real-time clock runs on. PSCI
``SYSTEM_OFF`` and Linux's halt request through the watchdog (boot
partition 63 in ``RSTS``) power the machine off, and QEMU exits with
status 0.

The partition a reboot asks for travels in ``RSTS``, where Linux's
watchdog driver and bare-metal code put it. The Raspberry Pi kernel
reboots through PSCI first, though, and passes the partition of
``reboot N`` to ``SYSTEM_RESET2``, which QEMU's PSCI does not provide;
the ``SYSTEM_RESET`` it falls back to asks for no partition.

What a reset leaves for the next boot can also go from one run of QEMU to
the next, for a host that runs QEMU afresh for each boot, as the firmware
reads the SD card afresh: with ``-action reboot=shutdown,shutdown=pause``,
the guest's reset stops it before QEMU makes the reset, and the machine
can be read. The machine's properties ``reset-status`` (``RSTS``),
``reboot-flags`` and ``boot-count`` (the boots so far) read what the reset
leaves; given at startup, with ``-machine``, they make the first boot the
one that reset would have started. They can only be set then, as can
``boot-partition``, which names the partition of the card that the boot
files come from.

The monitor's ``system_powerdown`` presses the board's power button for
200 ms, as a user would; a press carries on through a reset. Linux's
``gpio-keys`` reports it as ``KEY_POWER``, which systemd-logind takes as
a request to power off.

SD card
-------

``-drive if=sd,file=<image>,format=raw`` puts a card in the SD card slot,
on SDIO1: ``mmc0`` in both device trees, whose partitions Linux names
``/dev/mmcblk0p1`` and on. Writes go to the image; ``snapshot=on`` keeps
it unchanged. Cards of up to 2 GiB are SDSC cards and take images whose
size is a power of 2; larger ones are SDHC or SDXC cards, whose images
need only be a multiple of 512 KiB. To pad an image that is neither:
``qemu-img resize -f raw <image> <size>``.

Without ``-drive if=sd``, the slot is empty. Cards go in and come out
while the machine runs: ``change sd0 <image>`` in the monitor, or
``blockdev-change-medium`` in QMP with ``"id": "/machine/sd-card"``,
inserts one, and ``eject`` takes it out. The slot's card-detect switch
drives GIO AON 5 (``SD_CDET_N``) low while a card is in, and the
controller reports the card's arrival and removal as well. Linux checks
the line every second, as neither device tree gives the always-on GPIO
block an interrupt. With ``-nodefaults``, the slot has no drive and stays
empty.

QEMU's cards move data 512 bytes at a time, at a few MB/s.

Device tree
-----------

Without ``-dtb``, the machine generates a device tree describing what it
models, derived from its memory map: the CPUs with PSCI, the generic timer,
the PMU, the GIC, the system timer, the mailbox and the firmware interface
with its clocks, reset controller, power domains and real-time clock, the
PM block, the RNG, the AVS monitor with its temperature sensor and the
thermal zone Linux reads it through, the level 2 interrupt controllers,
the GPIO blocks
and their pin controllers, the HDMI ports' DDC I2C controllers, the PCIe
root complexes under ``/axi`` with their reset controllers (PCIe1, the
external connector, enabled; PCIe0 and PCIe2 disabled, as the tree is
made before any device is plugged in) and the MIPs that take PCIe1's and
PCIe2's MSIs, the power
button with the state of its pin (GPIO, pulled up), the activity LED,
UART10 (``serial10``, the ``stdout-path``) and UARTA, the SD hosts (the
card slot on SDIO1, ``mmc0``, with its card-detect line and the
regulators GIO AON 4 and 3 switch for the card's supply and signalling;
SDIO2 disabled), the fixed clocks, and a CMA pool in the first GiB, where
the VideoCore can reach Linux's buffers. Node names and properties follow
Linux's ``bcm2712.dtsi`` and the firmware's tree, and the result validates
against the Linux bindings, but for what the firmware adds for the OS,
which no binding describes. ``-machine raspi5b,builtin-dtb=off`` gives the
guest no device tree at all, like an empty ``device_tree=`` line in the
firmware's ``config.txt``.

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
  ``overlay_prefix`` at their defaults unless the tree has them, and the
  RAM size in ``rpi-sdram-size-gbit``;
* ``/chosen/bootloader`` describes the boot: ``boot-mode`` 3, RPIBOOT, in
  which the host supplies the boot files, as QEMU does; ``rsts``, the PM
  block's reset status as the boot found it; ``partition``, the
  partition the files come from, ``boot-partition`` if it is given and
  otherwise the one asked for in ``rsts`` (0 at power-on); ``count``,
  the boots since power-on, in 8 bits; ``tryboot``, 1 when the boot
  before set the reboot flag that asks for one; and 0 for ``arg1`` and
  ``capabilities``;
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
  as does the firmware's framebuffer, whose Linux driver takes a channel
  of the DMA controller, and CPU nodes of cores ``-smp`` leaves out
  ``status = "fail"``.

Every reset gives the next boot its own tree, as the firmware writes one
for each boot: ``rsts``, ``partition``, ``count``, ``tryboot`` and the
seeds are the new boot's. The firmware counts every boot, with a tree or
without, and the count moves with the machine in migration.

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

``arg1`` stays 0, as nothing sets the reboot argument it reports
(``config.txt``'s ``set_reboot_arg1``), and the property interface's
command line tag answers with ``-append`` alone.

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

Linux with its root file system on the second partition of an SD card::

  $ qemu-system-aarch64 -M raspi5b -m 4G -nographic \
      -kernel Image -dtb bcm2712-rpi-5-b.dtb \
      -drive if=sd,file=sd.img,format=raw \
      -append "console=ttyAMA10,115200 root=/dev/mmcblk0p2 rootwait"

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
