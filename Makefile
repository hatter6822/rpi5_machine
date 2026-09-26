# SPDX-License-Identifier: GPL-2.0-or-later
#
# Top-level developer entry points. QEMU itself is built with its own
# configure/meson/ninja machinery in $(BUILD_DIR); this file only wires the
# overlay, the build and the tests together. Run 'make help' for a summary.

BUILD_DIR   ?= build
QEMU_SRC    := $(CURDIR)/qemu
QEMU_BIN    := $(BUILD_DIR)/qemu-system-aarch64
QTEST_BIN   := $(BUILD_DIR)/tests/qtest/raspi5b-test
GUEST_DIR   := tests/guest
GUEST_BUILD := $(CURDIR)/$(GUEST_DIR)/build
NINJA       ?= ninja
PYTHON      ?= python3

CONFIGURE_FLAGS ?= --target-list=aarch64-softmmu --disable-docs --disable-user
EXTRA_CONFIGURE_FLAGS ?=

# Sources we own, as they appear inside the QEMU tree
OVERLAY_SRCS := $(shell cd overlay && find . -name '*.[ch]' | sed 's|^\./||')

.DEFAULT_GOAL := build
.PHONY: FORCE help setup apply unapply status configure build guest check \
        check-qtest check-smoke checkpatch run-hello clean distclean

help:
	@echo 'setup        initialise the qemu submodule and apply the overlay'
	@echo 'apply        apply patches/ and link overlay/ into qemu/'
	@echo 'unapply      restore qemu/ to the pristine pinned commit'
	@echo 'status       show overlay/patch state and unmanaged qemu/ changes'
	@echo 'configure    configure QEMU in $$(BUILD_DIR) (default: build/)'
	@echo 'build        build qemu-system-aarch64 (default target)'
	@echo 'guest        build the bare-metal test guests (clang + lld)'
	@echo 'check        run the raspi5b qtest and the smoke tests'
	@echo 'checkpatch   run QEMU checkpatch.pl over overlay sources and patches'
	@echo 'run-hello    boot the hello guest interactively'
	@echo 'clean        remove guest builds; distclean also removes $$(BUILD_DIR)'

setup:
	git submodule update --init --depth 1 qemu
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
build: configure
	$(NINJA) -C $(BUILD_DIR) qemu-system-aarch64

# After 'build', never beside it: two ninja processes must not share a
# build directory (make -j would otherwise run both at once)
$(QTEST_BIN): build
	$(NINJA) -C $(BUILD_DIR) tests/qtest/raspi5b-test

guest:
	$(MAKE) -C $(GUEST_DIR) OUT=$(GUEST_BUILD)

check: check-qtest check-smoke

check-qtest: build $(QTEST_BIN)
	QTEST_QEMU_BINARY=$(abspath $(QEMU_BIN)) $(QTEST_BIN) --tap

check-smoke: build guest
	QEMU=$(abspath $(QEMU_BIN)) GUEST=$(GUEST_BUILD)/hello.elf \
		$(PYTHON) -m unittest discover -s tests/smoke -v

checkpatch:
	@status=0; \
	for f in $(OVERLAY_SRCS); do \
		(cd overlay && $(QEMU_SRC)/scripts/checkpatch.pl --terse -f $$f) \
			|| status=1; \
	done; \
	for p in patches/*.patch; do \
		$(QEMU_SRC)/scripts/checkpatch.pl --terse --no-signoff $$p || status=1; \
	done; \
	exit $$status

run-hello: build guest
	$(QEMU_BIN) -M raspi5b -nographic -kernel $(GUEST_BUILD)/hello.elf

clean:
	$(MAKE) -C $(GUEST_DIR) OUT=$(GUEST_BUILD) clean

distclean: clean
	rm -rf $(BUILD_DIR)
