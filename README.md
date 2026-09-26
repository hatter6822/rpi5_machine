# rpi5_machine

A QEMU machine model for the **Raspberry Pi 5 Model B** (Broadcom BCM2712),
developed out of tree against a pinned upstream QEMU and shaped for upstream
submission.

```
$ make setup build guest
$ build/qemu-system-aarch64 -M raspi5b -nographic -kernel tests/guest/build/hello.elf
raspi5b: core 0 up at EL2, MPIDR 0x0000000080000000, CNTFRQ 54000000 Hz
raspi5b: PSCI 1.1
raspi5b: core 1 online, MPIDR 0x0000000080000100
raspi5b: core 2 online, MPIDR 0x0000000080000200
raspi5b: core 3 online, MPIDR 0x0000000080000300
raspi5b: PSCI SYSTEM_OFF
```

## Status

The first milestone targets bare-metal and microkernel bring-up.

| Area | State |
| --- | --- |
| 4 × Cortex-A76, `MPIDR.Aff1` = core, 54 MHz generic timer | done |
| GIC-400 (GICv2 + virtualization extensions, 5 priority bits), timer/maintenance PPIs | done |
| UART10 (PL011 debug UART) | done |
| Firmware boot contract: EL2 entry, PSCI over SMC (`secure=off`); guest-owned EL3 (`secure=on`) | done |
| Complete BCM2712 memory map, unmodelled blocks logged with `-d unimp` | done |
| Linux: stock Raspberry Pi OS kernel boots to the root-fs mount with `bcm2712-rpi-5-b.dtb` | smoke-tested |
| System timer, mailbox/firmware, SD, PCIe, RP1, GPIO, … | see [docs/PLAN.md](docs/PLAN.md) |

## Repository layout

```
qemu/          upstream QEMU, pinned as a git submodule (v11.1.1)
overlay/       new files, laid out exactly as in the QEMU tree
  hw/arm/bcm2712.c              BCM2712 SoC
  hw/arm/raspi5b.c              Raspberry Pi 5 Model B board
  include/hw/arm/bcm2712.h
  tests/qtest/raspi5b-test.c
  docs/system/arm/raspi5b.rst
patches/       changes to existing QEMU files (git format-patch series)
scripts/       qemu-tree: applies the overlay and patches, creates/refreshes patches
tests/guest/   bare-metal test guests (clang + lld, no GCC cross toolchain)
tests/smoke/   end-to-end tests that boot the guests
docs/          plan, development guide, hardware reference
```

The overlay is **symlinked** into `qemu/`, so you can edit either copy; the
patches are applied to the submodule's work tree, which `.gitmodules` marks
`ignore = dirty`. See [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## Building

Requirements: the usual QEMU build dependencies (a C compiler, Python 3 with
`venv`, ninja, GLib, pixman, libfdt), plus clang and lld for the test guests
and `dtc`/`fdtget` for the device-tree tests.
On Debian/Ubuntu:

```
$ sudo apt install build-essential python3-venv ninja-build \
      libglib2.0-dev libpixman-1-dev libfdt-dev clang lld device-tree-compiler
```

| Command | Effect |
| --- | --- |
| `make setup` | fetch the pinned QEMU and apply the overlay |
| `make build` | configure (aarch64-softmmu only) and build `build/qemu-system-aarch64` |
| `make check` | run the `raspi5b` qtest and the bare-metal smoke tests |
| `make checkpatch` | run QEMU's `checkpatch.pl` over our sources and patches |
| `make status` | show overlay/patch state and any unmanaged edits in `qemu/` |
| `make unapply` | return `qemu/` to the pristine pinned commit |

## Using the machine

```
qemu-system-aarch64 -M raspi5b[,secure=on] [-smp 1-4] [-m 1G|2G|4G|8G|16G] \
    -kernel <Image|payload.elf> [-dtb bcm2712-rpi-5-b.dtb] [-append ...]
```

By default the guest enters at **EL2** and QEMU provides **PSCI over SMC**,
matching the contract of the Pi 5 firmware and its resident TF-A BL31. With
`secure=on`, EL3 and the GIC Security Extensions are exposed and every core
starts at the image entry point. Full details, including device-tree
handling, are in [the machine documentation](overlay/docs/system/arm/raspi5b.rst).

## Documentation

* [docs/PLAN.md](docs/PLAN.md): the implementation workstreams
* [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md): workflow, conventions, upstreaming
* [docs/hardware/bcm2712.md](docs/hardware/bcm2712.md): memory map, interrupts, boot flow

## License

GPL-2.0-or-later, the license of QEMU itself; see [LICENSE](LICENSE). Every
source file carries an SPDX identifier.
