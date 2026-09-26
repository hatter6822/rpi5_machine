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

* 1 to 4 Cortex-A76 CPUs (``-smp``), ``MPIDR_EL1.Aff1`` = core number
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
  the VideoCore's view of memory: the first GiB of RAM at bus address
  ``0xc000_0000`` (and at ``0x0``); requests elsewhere get no answer, as
  on hardware. The VideoCore keeps the top 4 MiB of that GiB, which the
  device tree memory node leaves out
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
* GPIO, pin control, the Broadcom L2 interrupt controllers, RNG
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
its entry point; a ``-dtb`` blob is placed at the base of RAM if it fits
below the image). ``-bios`` is not supported yet.

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

Reset and power-off
-------------------

PSCI ``SYSTEM_RESET``, the watchdog and the monitor's ``system_reset`` all
reset the machine the same way: every device returns to its reset state,
RAM is kept, the images given with ``-kernel`` and ``-dtb`` are loaded
again and the boot starts over as from power-on, except that the PM
block's reset status register (``RSTS``) keeps its value and records a
watchdog reset. PSCI ``SYSTEM_OFF`` and Linux's halt request through the
watchdog (boot partition 63 in ``RSTS``) power the machine off, and QEMU
exits with status 0.

When a device tree is supplied with ``-dtb`` (for example
``bcm2712-rpi-5-b.dtb``), QEMU sets the memory node, marks nodes of devices
that are not modelled yet as ``status = "disabled"`` and publishes the board
revision in ``/system/linux,revision``, as the VideoCore firmware does.

Examples
--------

Bare-metal ELF payload::

  $ qemu-system-aarch64 -M raspi5b -nographic -kernel payload.elf

Linux, with the console on UART10::

  $ qemu-system-aarch64 -M raspi5b -m 4G -nographic \
      -kernel Image -dtb bcm2712-rpi-5-b.dtb \
      -append "console=ttyAMA10,115200 earlycon=pl011,mmio32,0x107d001000"
