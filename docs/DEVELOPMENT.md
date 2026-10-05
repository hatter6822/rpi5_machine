# Development guide

## The QEMU work tree

`qemu/` is upstream QEMU pinned to a release tag; this repository is an
out-of-tree fork of it, not a series prepared for upstream (see
[PLAN.md](PLAN.md)'s introduction). Our work reaches the tree in two ways,
both managed by `scripts/qemu-tree` (wrapped by `make`):

| Kind of change | Lives in | Reaches `qemu/` as |
| --- | --- | --- |
| New file (device model, header, test, doc page) | `overlay/<path in QEMU tree>` | a relative symlink |
| Edit to an existing QEMU file (Kconfig, meson.build, trace-events, index pages) | `patches/NNNN-*.patch` | `git apply` to the work tree |

The rule that keeps this manageable: **anything that can be a new file is a
new file.** Patches are reserved for the glue that cannot be.

Patches form a series, applied in file name order, and like the commits of
a branch they may change the same file: `hw/arm/Kconfig` is changed both
by the Kconfig split and by the BCM2712 glue. Each patch is generated against the tree with every earlier
patch applied and with the changes of later patches taken back out, so it
can still be refreshed on its own. Two consequences:

* when a patch changes (a checkout, a pull), `apply` reverts it together
  with every later patch that shares a file with it, then reapplies them
  all in order;
* an edit to a shared file shows as a local edit of every patch that
  changes the file (`make status`); refresh the one it belongs to. Edits
  to lines that a later patch changes belong to that later patch.

### Everyday loop

```
make setup            # once: fetch qemu/ and rpi-utils/, apply patches, link overlay
$EDITOR overlay/hw/arm/bcm2712.c
make build check
```

Editing `qemu/hw/arm/bcm2712.c` is equivalent: it is a symlink to the overlay.

### Adding a file

Create it under `overlay/` at its QEMU path, then `make apply` to link it.
New sources also need build glue (`meson.build`, `Kconfig`), which is a patch.

### Changing an existing QEMU file

Edit it in `qemu/`, then either

```
scripts/qemu-tree new hw-misc-add-bcm2712-foo qemu-relative/path ...   # new patch
scripts/qemu-tree refresh 0033-hw-arm-Add-the-Broadcom-BCM2712-SoC.patch
scripts/qemu-tree refresh 0033-hw-arm-Add-the-Broadcom-BCM2712-SoC.patch MAINTAINERS
```

`new` appends a patch to the series and opens git's editor for a commit
message in QEMU style (`subsystem: Imperative summary`, then the why).
`refresh` regenerates an existing patch from the files it touches, keeping
its message, author and date (parsed with `git mailinfo`), so an unchanged
tree reproduces the patch byte for byte. Files named after the patch are
added to it. When several patches change the same file, make each change
and refresh its patch in series order: a patch takes every change to its
files that no later patch holds. The script needs bash 4 and GNU coreutils (`realpath
--relative-to`); on macOS install `coreutils` from Homebrew.

`make status` lists edits in `qemu/` that belong to no patch or overlay file;
they are not tracked by this repository and will be lost by `make unapply`.

A fix already in QEMU's `master` that the pinned release lacks goes in as
a backport: the upstream commit as `git format-patch` writes it, with its
author, date and tags, and a `(cherry picked from commit ...)` line, in
the series before the patches that need it. Refresh it like any other.
It leaves the series when the pin moves to a release that has it.

### Moving to a new QEMU release

```
make unapply
git -C qemu fetch --depth 1 origin tag vX.Y.Z && git -C qemu checkout vX.Y.Z
make apply            # fix any patch that no longer applies, then refresh it
make build check lint
git add qemu patches overlay && git commit
```

Upstream API churn (header moves such as `hw/boards.h` becoming
`hw/core/boards.h` in 11.x) shows up as build failures in overlay sources.

## Coding conventions

We follow QEMU's conventions, so the code reads like QEMU's own and
rebases cleanly onto new releases:

* `docs/devel/style.rst` (QEMU's `scripts/checkpatch.pl` can check a
  file by hand; CI does not run it).
* One device per file under the matching `hw/<subsystem>/`, header in
  `include/hw/<subsystem>/`, `SPDX-License-Identifier: GPL-2.0-or-later`.
* QOM: `OBJECT_DECLARE_SIMPLE_TYPE`, `DEFINE_TYPES`, children embedded in the
  parent state and created with `object_initialize_child()`, configuration
  through `qdev` properties, never globals.
* Registers: `REG32()`/`FIELD()` from `hw/core/registerfields.h`;
  `MemoryRegionOps` with explicit `.valid` and `.impl` access sizes and
  `.endianness = DEVICE_LITTLE_ENDIAN`.
* Guest mistakes are `qemu_log_mask(LOG_GUEST_ERROR, ...)`, missing features
  `LOG_UNIMP`; never `abort()` on guest-controlled input.
* Every stateful device has a `VMStateDescription` and implements reset
  through the `Resettable` interface (or `device_class_set_legacy_reset` when
  matching an existing model).
* Observable behaviour gets `trace-events` entries (a patch to that
  directory's `trace-events` file).
* Values that come from hardware documentation or device trees cite their
  source in a comment; values that are guesses say so with
  `Unverified (PLAN.md Pnn)`, and deferred fixes with `Deferred (PLAN.md
  Pnn)`, naming a row of [PLAN.md](PLAN.md)'s section 10, which every
  marker must have.
* The scripts pass `make lint`: shellcheck for the shell ones, ruff
  (`ruff.toml`) for the Python ones.

## Tests

| Layer | Location | Run by |
| --- | --- | --- |
| Register-level device tests | `overlay/tests/qtest/*-test.c` | `make check-qtest` |
| Bare-metal guests and test suite over UART ([format](../tests/guest/README.md)) | `tests/guest/`, `tests/smoke/test_hello.py`, `test_suite.py` | `make check-smoke` |
| The `-bios` handoff: firmware started at EL3 with the kernel, initrd and device tree where the Pi's firmware puts them | `tests/smoke/test_bios.py` | `make check-smoke` |
| Kconfig resolves the machine's devices in a QEMU whose only board is `raspi5b` (configure only) | `tests/configs/raspi5b-only.mak` | `make check-minimal` |
| The device tree the guest gets: each firmware change, the built-in tree against a checked-in dump (`raspi5b-builtin-tree.txt`), and what each boot gets across resets and migration | `tests/smoke/test_dtb.py` | `make check-smoke` |
| Built-in device tree against the Linux bindings (dt-schema) | `tests/smoke/test_dt_schema.py` | `make check-dt` |
| Real firmware with `-bios`: TF-A's `rpi5` BL31 running the bare-metal guests; Linux through TF-A and through U-Boot; the EDK2 port to its shell; the changes to the firmware's own tree (`raspi5b-firmware-fixups.txt`). Pinned and built or fetched by `scripts/firmware` | `tests/smoke/test_firmware.py` | `make check-firmware` |
| `scripts/rpi5-boot`: its overlays and parameters (applied by `dtmerge`), `config.txt` code, partition tables, QEMU's runs and the reboot loop; with the firmware, a Raspberry Pi OS-style first boot | `tests/smoke/test_rpi5_boot.py` | `make check-smoke`, the firmware part by `make check-firmware` |
| Linux / firmware boots in QEMU's own test runner | `overlay/tests/functional/aarch64/` (planned, WS9.3) | QEMU functional test runner |

Each new device lands with a qtest for its registers and reset values. The
bare-metal guests are built with `clang --target=aarch64-none-elf` and `lld`,
so no cross GCC is needed; keep them freestanding and MMU-off safe (aligned
accesses only, `-mstrict-align`).

### Trying Linux

The Raspberry Pi firmware repository carries a current kernel and DTB:

```
base=https://raw.githubusercontent.com/raspberrypi/firmware/master/boot
curl -LO $base/kernel_2712.img -LO $base/bcm2712-rpi-5-b.dtb
build/qemu-system-aarch64 -M raspi5b -m 4G -nographic \
    -kernel kernel_2712.img -dtb bcm2712-rpi-5-b.dtb \
    -append "console=ttyAMA10,115200 earlycon=pl011,mmio32,0x107d001000"
```

Without storage the boot ends at the root-fs mount; `-drive
if=sd,format=raw,file=card.img` gives it a card, and `scripts/rpi5-boot
card.img` boots a card as the Pi's firmware would.

`make firmware` puts the pinned kernel and device tree in `build-firmware/`,
with TF-A's BL31, U-Boot and the EDK2 port; the machine documentation
shows how to boot each of them.

### Debugging

* `-d unimp,guest_errors` shows accesses to unmodelled blocks by name
  (`bcm2712.axi`, `bcm2712.soc`, ...).
* `-s -S` and `gdb-multiarch` for guest debugging; `-trace 'bcm2712_*'`,
  `-trace 'brcmstb_*'` and `-trace 'bcm2711_*'` follow the models
  (`-trace help` lists their trace points).
* `-machine dumpdtb=out.dtb` shows the device tree after QEMU's fix-ups;
  `python3 tests/smoke/fdt.py out.dtb` prints it, and
  `python3 tests/smoke/fdt.py in.dtb out.dtb` what they changed.
