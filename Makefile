# SPDX-License-Identifier: GPL-2.0-or-later
#
# Top-level developer entry points. QEMU itself is built with its own
# configure/meson/ninja machinery in $(BUILD_DIR); this file only wires the
# overlay, the build and the tests together. Run 'make help' for a summary.
# This repository is an out-of-tree fork: the patches and overlay are applied
# to a pinned QEMU and built here; they are not prepared for upstream.

BUILD_DIR   ?= build
QEMU_SRC    := $(CURDIR)/qemu
QEMU_BIN    := $(BUILD_DIR)/qemu-system-aarch64
QTEST_BIN   := $(BUILD_DIR)/tests/qtest/raspi5b-test
DTMERGE     := $(BUILD_DIR)/dtmerge
GUEST_DIR   := tests/guest
GUEST_BUILD := $(CURDIR)/$(GUEST_DIR)/build
NINJA       ?= ninja
PYTHON      ?= python3

CONFIGURE_FLAGS ?= --target-list=aarch64-softmmu --disable-docs --disable-user
EXTRA_CONFIGURE_FLAGS ?=

.DEFAULT_GOAL := build
.PHONY: FORCE help setup apply unapply status configure build guest check \
        check-qtest check-smoke check-minimal check-dt firmware check-firmware \
        lint run-hello \
        clean distclean

help:
	@echo 'setup        initialise the submodules and apply the overlay'
	@echo 'apply        apply patches/ and link overlay/ into qemu/'
	@echo 'unapply      restore qemu/ to the pristine pinned commit'
	@echo 'status       show overlay/patch state and unmanaged qemu/ changes'
	@echo 'configure    configure QEMU in $$(BUILD_DIR) (default: build/)'
	@echo 'build        build qemu-system-aarch64 and dtmerge (default target)'
	@echo 'guest        build the bare-metal test guests (clang + lld)'
	@echo 'check        run the raspi5b qtest and the smoke tests'
	@echo 'check-minimal configure QEMU with raspi5b as its only board'
	@echo 'check-dt     validate the built-in device tree (needs dtschema, network)'
	@echo 'firmware     build the pinned firmware check-firmware boots (network)'
	@echo 'check-firmware boot real firmware with -bios (needs aarch64-linux-gnu-gcc)'
	@echo 'lint         shellcheck the shell scripts, ruff the Python'
	@echo 'run-hello    boot the hello guest interactively'
	@echo 'clean        remove guest builds; distclean also removes $$(BUILD_DIR)'

setup:
	git submodule update --init --depth 1 qemu rpi-utils
	scripts/qemu-tree apply

apply unapply status:
	scripts/qemu-tree $@

CONFIGURE_ARGS  := $(strip $(CONFIGURE_FLAGS) $(EXTRA_CONFIGURE_FLAGS))
CONFIGURE_STAMP := $(BUILD_DIR)/.configure-args

# Holds the arguments of the last configure and is rewritten only when they
# change, which is what makes build.ninja out of date in that case
$(CONFIGURE_STAMP): FORCE
	@mkdir -p $(@D)
	@printf '%s\n' '$(CONFIGURE_ARGS)' | cmp -s - $@ || \
		printf '%s\n' '$(CONFIGURE_ARGS)' >$@

# 'apply' is idempotent and runs before every build (order-only, so it never
# forces a reconfigure): this keeps qemu/ in sync after 'make unapply', a
# checkout, or new overlay files. Ninja itself re-runs meson when the glue
# changes.
$(BUILD_DIR)/build.ninja: $(CONFIGURE_STAMP) | apply
	cd $(BUILD_DIR) && $(QEMU_SRC)/configure $(CONFIGURE_ARGS)

configure: $(BUILD_DIR)/build.ninja

# Ninja tracks every dependency, so these always defer to it
build: configure $(DTMERGE)
	$(NINJA) -C $(BUILD_DIR) qemu-system-aarch64

# The firmware's dtoverlay code as raspberrypi/utils builds it into
# dtmerge, which scripts/rpi5-boot applies config.txt's overlays with. It
# reads the overlays of the cards rpi5-boot boots, so it is built with
# patches/rpi-utils/, applied to a copy: Deferred (PLAN.md P25).
RPI_UTILS_SRCS := rpi-utils/dtmerge/dtmerge.c rpi-utils/dtmerge/dtoverlay.c
DTMERGE_PATCH  := patches/rpi-utils/0001-dtoverlay-Refuse-an-override-s-offset-outside-a-property.patch
DTOVERLAY_SRC  := $(BUILD_DIR)/dtmerge-src/dtoverlay.c

RPI_UTILS_STAMP := $(BUILD_DIR)/.rpi-utils-commit

# Holds the rpi-utils commit this checkout pins (its gitlink in the index,
# which 'git submodule update' checks out) and is rewritten only when that
# changes. Every make checks the submodule against it: one that is not
# checked out, or at another commit (after switching to a revision that
# pins another), is updated first, so dtmerge is never built from a stale
# checkout. One target runs git, so -j runs it once. Outside a git
# checkout the sources are taken as they are.
$(RPI_UTILS_STAMP): FORCE
	@mkdir -p $(@D)
	@want=$$(git rev-parse -q --verify :rpi-utils 2>/dev/null); \
	if [ -n "$$want" ]; then \
		have=; \
		if [ -e rpi-utils/.git ]; then \
			have=$$(git -C rpi-utils rev-parse -q --verify HEAD); \
		fi; \
		if [ "$$have" != "$$want" ]; then \
			echo "git submodule update --init --depth 1 rpi-utils"; \
			git submodule update --init --depth 1 rpi-utils || exit 1; \
		fi; \
	else \
		want=untracked; \
	fi; \
	printf '%s\n' "$$want" | cmp -s - $@ || printf '%s\n' "$$want" >$@

# The sources are as new as the stamp says: made by its recipe
$(RPI_UTILS_SRCS): $(RPI_UTILS_STAMP) ;

$(DTOVERLAY_SRC): rpi-utils/dtmerge/dtoverlay.c $(DTMERGE_PATCH) \
		$(RPI_UTILS_STAMP)
	@mkdir -p $(@D)
	patch -s -o $@ rpi-utils/dtmerge/dtoverlay.c $(DTMERGE_PATCH)

$(DTMERGE): rpi-utils/dtmerge/dtmerge.c $(DTOVERLAY_SRC) $(RPI_UTILS_STAMP)
	@mkdir -p $(@D)
	$(CC) -O2 -Irpi-utils/dtmerge -o $@ $(filter %.c,$^) -lfdt

# After 'build', never beside it: two ninja processes must not share a
# build directory (make -j would otherwise run both at once)
$(QTEST_BIN): build
	$(NINJA) -C $(BUILD_DIR) tests/qtest/raspi5b-test

guest:
	$(MAKE) -C $(GUEST_DIR) OUT=$(GUEST_BUILD)

check: check-qtest check-smoke

# QEMU configured with the raspi5b machine and nothing else: Kconfig
# resolves what the machine selects without the other Raspberry Pi boards
# (CONFIG_RASPI) and without contradictions. Configure only: the build and
# the tests of the full configuration cover the code itself, and a device
# the machine fails to select would show only at run time (docs/PLAN.md,
# P24). Ninja's build.ninja target reruns meson when its inputs, the
# Kconfig files among them, have changed since. configure
# looks the device file up relative to qemu/configs/devices/aarch64-softmmu/.
MINIMAL_BUILD_DIR ?= build-minimal
MINIMAL_DEVICES   := tests/configs/raspi5b-only
MINIMAL_CONFIGURE_FLAGS := --without-default-devices \
	--with-devices-aarch64=../../../../$(MINIMAL_DEVICES)

check-minimal:
	$(MAKE) BUILD_DIR=$(MINIMAL_BUILD_DIR) \
		EXTRA_CONFIGURE_FLAGS='$(MINIMAL_CONFIGURE_FLAGS)' configure
	$(NINJA) -C $(MINIMAL_BUILD_DIR) build.ninja
	@grep -qx 'CONFIG_RASPI5=y' \
		$(MINIMAL_BUILD_DIR)/aarch64-softmmu-config-devices.mak || \
		{ echo 'check-minimal: CONFIG_RASPI5 is not enabled' >&2; exit 1; }
	@! grep -qx 'CONFIG_RASPI=y' \
		$(MINIMAL_BUILD_DIR)/aarch64-softmmu-config-devices.mak || \
		{ echo 'check-minimal: CONFIG_RASPI is enabled' >&2; exit 1; }

check-qtest: build $(QTEST_BIN)
	QTEST_QEMU_BINARY=$(abspath $(QEMU_BIN)) $(QTEST_BIN) --tap

check-smoke: build guest
	QEMU=$(abspath $(QEMU_BIN)) GUEST=$(GUEST_BUILD)/hello.elf \
		SUITE=$(GUEST_BUILD)/suite.elf DTMERGE=$(abspath $(DTMERGE)) \
		$(PYTHON) -m unittest discover -s tests/smoke -v

# The built-in device tree is validated against the kernel's bindings at a
# pinned tag, fetched once (bindings only) and processed by dt-schema's
# dt-mk-schema (pip install dtschema). Making the schema clears and clones
# linux/ in $(DT_SCHEMA_DIR), and distclean removes the directory, so it
# must be new, empty, or marked as made here by an earlier run.
LINUX_DT_TAG  ?= v6.18
DT_SCHEMA_DIR ?= build-dt-schema
DT_SCHEMA     := $(DT_SCHEMA_DIR)/linux-$(LINUX_DT_TAG).json
DT_SCHEMA_MARKER := $(DT_SCHEMA_DIR)/.check-dt

$(DT_SCHEMA):
	@if [ -e '$(DT_SCHEMA_DIR)' ] && [ ! -f '$(DT_SCHEMA_MARKER)' ] && \
	   [ -n "$$(find -H '$(DT_SCHEMA_DIR)' -mindepth 1 -maxdepth 1 -print -quit 2>&1)" ]; then \
		echo "check-dt: $(DT_SCHEMA_DIR) exists and was not made here;" \
		     "set DT_SCHEMA_DIR to a new or empty directory" >&2; exit 1; \
	fi
	mkdir -p $(DT_SCHEMA_DIR) && touch $(DT_SCHEMA_MARKER)
	rm -rf $(DT_SCHEMA_DIR)/linux
	git clone --quiet --depth 1 --filter=blob:none --sparse \
		--branch $(LINUX_DT_TAG) https://github.com/torvalds/linux.git \
		$(DT_SCHEMA_DIR)/linux
	git -C $(DT_SCHEMA_DIR)/linux sparse-checkout set \
		Documentation/devicetree/bindings
	dt-mk-schema -j $(DT_SCHEMA_DIR)/linux/Documentation/devicetree/bindings \
		> $@.tmp
	mv $@.tmp $@
	rm -rf $(DT_SCHEMA_DIR)/linux

check-dt: build guest $(DT_SCHEMA)
	QEMU=$(abspath $(QEMU_BIN)) GUEST=$(GUEST_BUILD)/hello.elf \
		DT_SCHEMA=$(abspath $(DT_SCHEMA)) \
		$(PYTHON) -m unittest discover -s tests/smoke -p test_dt_schema.py -v

# Real firmware booted with -bios: scripts/firmware builds it at pinned
# versions in $(FIRMWARE_DIR), which must be new, empty, or marked as made
# there by an earlier run, as for check-dt. It skips what is up to date.
# The rpi5-boot tests use its overlays and kernel.
FIRMWARE_DIR ?= build-firmware

firmware:
	scripts/firmware $(FIRMWARE_DIR)

check-firmware: build guest firmware
	QEMU=$(abspath $(QEMU_BIN)) GUEST=$(GUEST_BUILD)/hello.elf \
		SUITE=$(GUEST_BUILD)/suite.elf FIRMWARE=$(abspath $(FIRMWARE_DIR)) \
		DTMERGE=$(abspath $(DTMERGE)) \
		PYTHONPATH=$(CURDIR)/tests/smoke$${PYTHONPATH:+:$$PYTHONPATH} \
		$(PYTHON) -m unittest -v test_firmware test_rpi5_boot

# The repository's own scripts: the shell ones with shellcheck, the Python
# ones (tests/smoke and scripts/rpi5-boot) with ruff, configured in
# ruff.toml
SHELL_SCRIPTS  := scripts/qemu-tree scripts/firmware
PYTHON_SCRIPTS := scripts/rpi5-boot tests/smoke

lint:
	shellcheck $(SHELL_SCRIPTS)
	ruff check $(PYTHON_SCRIPTS)

run-hello: build guest
	$(QEMU_BIN) -M raspi5b -nographic -kernel $(GUEST_BUILD)/hello.elf

clean:
	$(MAKE) -C $(GUEST_DIR) OUT=$(GUEST_BUILD) clean

# distclean removes a directory only when this repository made it, since
# each of these variables can point anywhere: a build directory carries
# configure's stamp, the schema directory the marker check-dt writes, and the firmware directory
# the marker scripts/firmware writes. A directory that holds a
# repository is never a build directory, whatever it carries:
# not this one or a parent of it ('make configure BUILD_DIR=.' leaves the
# stamp in the checkout), and not another checkout (qemu/ included).
ROOT := $(realpath $(CURDIR))

define rm_made
@if [ -d '$(1)' ]; then \
	d=$$(cd '$(1)' && pwd -P) && case '$(ROOT)/' in "$${d%/}/"*) \
		echo "distclean: $(1) holds this repository, leaving it" >&2; exit 1;; \
	esac; \
	if [ -e '$(1)/.git' ]; then \
		echo "distclean: $(1) is a checkout, leaving it" >&2; exit 1; \
	fi; \
fi; \
if [ -e '$(1)' ] && ! { [ -d '$(1)' ] && $(2); }; then \
	echo "distclean: $(1) was not made here, leaving it" >&2; exit 1; \
fi; rm -rf '$(1)'
endef

distclean: clean
	$(call rm_made,$(BUILD_DIR),[ -f '$(BUILD_DIR)/$(notdir $(CONFIGURE_STAMP))' ])
	$(call rm_made,$(MINIMAL_BUILD_DIR),[ -f '$(MINIMAL_BUILD_DIR)/$(notdir $(CONFIGURE_STAMP))' ])
	$(call rm_made,$(DT_SCHEMA_DIR),[ -f '$(DT_SCHEMA_MARKER)' ])
	$(call rm_made,$(FIRMWARE_DIR),[ -f '$(FIRMWARE_DIR)/.check-firmware' ])
