# rpi5_machine

A QEMU machine model for the **Raspberry Pi 5 Model B** (Broadcom BCM2712),
developed out of tree against a pinned upstream QEMU and shaped for upstream
submission.

```
$ make setup build guest
$ build/qemu-system-aarch64 -M raspi5b -nographic -kernel tests/guest/build/hello.elf
raspi5b: core 0 up at EL2, MPIDR 0x0000000081000000, CNTFRQ 54000000 Hz
raspi5b: PSCI 1.1
raspi5b: core 1 online, MPIDR 0x0000000081000100
raspi5b: core 2 online, MPIDR 0x0000000081000200
raspi5b: core 3 online, MPIDR 0x0000000081000300
raspi5b: PSCI SYSTEM_OFF
```

## Status

Milestones M1 (bare-metal and microkernel bring-up), M2 (the Pi's own
boot chain: TF-A, U-Boot and UEFI run unmodified) and M3 (Raspberry Pi
OS Lite boots from an SD card to its login prompt) are done.

| Area | State |
| --- | --- |
| 4 × Cortex-A76, `MPIDR.Aff1` = core with `MPIDR.MT` set, 54 MHz generic timer | done |
| GIC-400 (GICv2 + virtualization extensions, 5 priority bits), timer/maintenance PPIs | done |
| UART10 (PL011 debug UART) | done |
| UARTA, the Bluetooth radio's 16550 (the radio is not modelled), as the second serial port | done |
| System timer (1 MHz counter, four comparators) | done |
| Watchdog and reset status (PM block) | done |
| RNG200 random number generator | done |
| The AVS monitor's temperature sensor, which Linux's thermal driver reads, set with `-global bcm2711-avs-monitor.temperature=` or `qom-set` | done |
| Broadcom L2 interrupt controllers (7, both register layouts) | done |
| BCM2712 GPIO blocks (GIO: 32 + 22 lines, AON: 17 + 6), edge and level interrupts, and their pin controllers, which keep the functions and pulls software selects | done |
| The HDMI ports' DDC I2C controllers, with a monitor's EDID on HDMI0's bus | done |
| The board's power button, which `system_powerdown` presses, and its ACT LED, whose changes are trace events | done |
| SD hosts (SDIO1 with the SD card slot, SDIO2) with SDMA and ADMA2: `-drive if=sd` inserts a card, which the monitor can change and eject at run time, with the slot's card-detect line on GIO AON 5 | done |
| System reset (PSCI, watchdog, monitor) and power-off | done |
| Firmware boot contract: EL2 entry, PSCI over SMC (`secure=off`); guest-owned EL3 (`secure=on`) | done |
| Firmware loaded with `-bios` (`secure=on`) the way the Pi's firmware loads it: TF-A's `rpi5` BL31 runs the bare-metal suite on its own PSCI and boots Linux, directly or through U-Boot; the EDK2 port draws its boot menu on the firmware's framebuffer and reaches the UEFI shell | done |
| Complete BCM2712 memory map, unmodelled blocks logged with `-d unimp` | done |
| Built-in device tree when no `-dtb` is given, validated against the Linux bindings | done |
| The firmware's device-tree changes, made anew for each boot: model and serial number, the command line it builds, `/chosen` with the boot's reset status, partition, count and tryboot, the power supply and seeds, the CMA pool and the bootloader configuration | done |
| Bare-metal test suite (36 tests: interrupts, timers and SGIs on every core, PSCI, resets, mailbox, the firmware's clocks, real-time clock and framebuffer, a tryboot, RNG, the SoC's temperature, UART, L2 interrupt controllers, GPIO interrupts, the SD card's master boot record, Secure/Non-secure GIC groups, the A76's MPIDR and IMPDEF registers) on 1–4 cores, EL2 and EL3 | done |
| Linux: stock Raspberry Pi OS kernel mounts its root file system from an SD card, on the built-in device tree or `bcm2712-rpi-5-b.dtb`, started directly or by U-Boot from the card | smoke-tested |
| SD card images booted as a Pi 5's firmware boots them (`scripts/rpi5-boot`): `config.txt` with its filters, the kernel, device tree, overlays and parameters (the blob the firmware's `dtmerge` makes), initramfs and command line it names, the card read again at each reboot with what the reboot left (tryboot, the boot count, the partition asked for) | done: Raspberry Pi OS Lite (2026-09-15, trixie) boots to its login prompt, and `reboot` and `poweroff` work (by hand); smoke-tested in CI with Raspberry Pi OS's kernel, overlays and boot files |
| VideoCore mailbox and firmware property channel: BCM283x tag set, board and firmware identity, and the Pi 5's own answers: its clocks (cpufreq), power domains, reboot flags (tryboot), real-time clock (`hwclock`) and the SoC's temperature (`vcgencmd measure_temp`); the framebuffer, the machine's display, within the VideoCore's 4 MiB | done |
| System DMA controller | deferred: no modelled device uses it yet ([docs/PLAN.md](docs/PLAN.md), WS5.2) |
| PCIe, RP1 (with the 40-pin header's GPIO), … | see [docs/PLAN.md](docs/PLAN.md) |

## Repository layout

```
qemu/          upstream QEMU, pinned as a git submodule (v11.1.1)
overlay/       new files, laid out exactly as in the QEMU tree
  hw/arm/bcm2712.c              BCM2712 SoC
  hw/arm/raspi5b.c              Raspberry Pi 5 Model B board
  include/hw/arm/bcm2712.h
  tests/qtest/raspi5b-test.c
  docs/system/arm/raspi5b.rst
  hw/misc/bcm2711_rng200.c      RNG200 random number generator
  hw/misc/bcm2711_avs_monitor.c AVS monitor temperature sensor
  hw/intc/brcmstb_l2_intc.c     Broadcom L2 interrupt controller
  hw/gpio/brcmstb_gpio.c        Broadcom GPIO controller
  hw/gpio/brcmstb_pinctrl.c     Broadcom pin controller
  hw/i2c/brcmstb_i2c.c          Broadcom BSC I2C controller
  hw/sd/bcm2712_sdhci.c         BCM2712 SD host controller
  hw/misc/bcm2712_property.c    Raspberry Pi 5 firmware property interface
patches/       changes to existing QEMU files (git format-patch series)
series/        how patches and overlay files form the upstream series, cover letter
scripts/       qemu-tree: applies the overlay and patches, creates/refreshes
               patches, exports the upstream series; firmware: builds the
               pinned firmware the firmware tests boot; rpi5-boot: boots
               an SD card image as a Pi 5's firmware does
tests/guest/   bare-metal runtime, smoke guest and test suite, and a Linux
               /init for the rpi5-boot tests (clang + lld, no GCC)
tests/smoke/   end-to-end tests that boot the guests
tests/configs/ QEMU device configurations for test builds
docs/          plan, development guide, hardware reference
```

The overlay is **symlinked** into `qemu/`, so you can edit either copy; the
patches are applied to the submodule's work tree, which `.gitmodules` marks
`ignore = dirty`. See [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## Building

Requirements: the usual QEMU build dependencies (a C compiler, Python 3 with
`venv`, ninja, GLib, pixman, libfdt), plus clang and lld for the test guests,
`dtc`/`fdtget` for the device-tree tests and mtools, which `scripts/rpi5-boot`
reads SD card images with.
On Debian/Ubuntu:

```
$ sudo apt install build-essential python3-venv ninja-build \
      libglib2.0-dev libpixman-1-dev libfdt-dev clang lld device-tree-compiler \
      mtools
```

| Command | Effect |
| --- | --- |
| `make setup` | fetch the pinned QEMU and apply the overlay |
| `make build` | configure (aarch64-softmmu only) and build `build/qemu-system-aarch64` |
| `make check` | run the `raspi5b` qtest, the bare-metal smoke tests and the `rpi5-boot` tests |
| `make export-series` | write the upstream patch series to `build-series/` and check it (applies, checkpatch; `SERIES_FLAGS=--build` builds every commit) |
| `make check-minimal` | build a QEMU whose only board is `raspi5b` (in `build-minimal/`) and run the same tests on it |
| `make check-dt` | validate the built-in device tree against the Linux bindings, fetched into `build-dt-schema/` the first time (needs `pip install dtschema` and network access) |
| `make check-firmware` | boot real firmware with `-bios`: TF-A and U-Boot, built at pinned releases into `build-firmware/` the first time, with the EDK2 port and a Raspberry Pi OS kernel fetched there, and boot them from an SD card too; check `rpi5-boot`'s overlays against the firmware's `dtmerge`, built there too, and boot the kernel with it through a Raspberry Pi OS-style first boot (needs `gcc-aarch64-linux-gnu`, U-Boot's host-tool dependencies `bison flex libssl-dev libgnutls28-dev`, `mkfs.ext4`, and network access) |
| `make checkpatch` | run QEMU's `checkpatch.pl` over our sources and patches |
| `make status` | show overlay/patch state and any unmanaged edits in `qemu/` |
| `make unapply` | return `qemu/` to the pristine pinned commit |

## Using the machine

```
qemu-system-aarch64 -M raspi5b[,secure=on][,serial=N][,builtin-dtb=off] \
    [-smp 1-4] [-m 1G|2G|4G|8G|16G] \
    -kernel <Image|payload.elf> [-dtb bcm2712-rpi-5-b.dtb] [-append ...]
qemu-system-aarch64 -M raspi5b,secure=on[,dtb-address=ADDR] -bios bl31.bin \
    [-kernel <Image|payload.elf>] [-dtb ...] [-initrd ...] [-append ...]
```

By default the guest enters at **EL2** and QEMU provides **PSCI over SMC**,
matching the contract of the Pi 5 firmware and its resident TF-A BL31. With
`secure=on`, EL3 and the GIC Security Extensions are exposed and every core
starts at the image entry point. With `secure=on` and `-bios`, the machine
loads that firmware at address 0 as the Pi's firmware loads its armstub,
places the kernel, initrd and device tree as the firmware does and tells
the armstub where they are; TF-A then provides PSCI and enters the kernel
at EL2. Full details, including device-tree handling, are in [the machine
documentation](overlay/docs/system/arm/raspi5b.rst).

### Booting an SD card image

`scripts/rpi5-boot` boots a Raspberry Pi OS image as a Pi 5 boots its card,
without the closed firmware: it reads the card's boot partition with
mtools, evaluates `config.txt` for a Pi 5, applies the overlays and
parameters it names to the device tree as the firmware does, and runs QEMU
with the card in the SD slot and the kernel, tree, initramfs and command
line it chose. When the guest reboots, it reads the card again and starts
QEMU again, as the Pi reads its card at every boot, with what the reboot
left: the tryboot flag (`reboot "0 tryboot"`, for updates through
`tryboot.txt` or `autoboot.txt`'s A/B partitions), the boot count, and
the partition the watchdog's reset status asks for.

```
$ xz -dk raspios-lite.img.xz
$ truncate -s 8G raspios-lite.img
$ scripts/rpi5-boot raspios-lite.img -- -m 8G
```

Raspberry Pi OS Lite comes without a user. To log in on the serial
console, give the card one before its first boot, as Raspberry Pi's
headless setup does: a `userconf.txt` on the boot partition, holding the
name, a colon and the password's hash, which mtools writes at the
partition's offset (`START` below is its first sector, as `fdisk -l`
shows it). The first boot grows the root file system to the card and
trims it, which takes a few minutes.

```
$ echo "pi:$(openssl passwd -6)" > userconf.txt
$ MTOOLS_SKIP_CHECK=1 mcopy -i raspios-lite.img@@$((START * 512)) userconf.txt ::
```

The image is the card, which the guest writes to; QEMU's SD card needs a
raw image whose size is a power of two up to 2 GiB, or a multiple of
512 KiB above. `--print` writes the files for one boot and prints the QEMU
command instead of running it, `--root` and `--append` change the command
line, and anything after `--` goes to QEMU; `scripts/rpi5-boot --help` has
the rest. It runs this checkout's `build/qemu-system-aarch64`, or the QEMU
that `QEMU` or `--qemu` names.

## Documentation

* [docs/PLAN.md](docs/PLAN.md): the implementation workstreams
* [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md): workflow, conventions, upstreaming
* [docs/hardware/bcm2712.md](docs/hardware/bcm2712.md): memory map, interrupts, boot flow

## License

GPL-2.0-or-later, the license of QEMU itself; see [LICENSE](LICENSE). Every
source file carries an SPDX identifier.
