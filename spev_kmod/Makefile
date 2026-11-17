obj-m += spev_drv.o
spev_drv-y := spev_diag_main.o

ccflags-y += -I$(src)

KDIR ?= /lib/modules/$(shell uname -r)/build
BUILD_DIR ?= $(CURDIR)/build
KBUILD_HAS_MO := $(shell test -r "$(KDIR)/Makefile" && \
	grep -q 'KBUILD_EXTMOD_OUTPUT' "$(KDIR)/Makefile" && echo 1)
MODULE_INPUTS := Makefile spev_diag_main.c spev_diag_uapi.h
SOURCE_INPUT_PATHS := $(foreach file,$(MODULE_INPUTS),"$(CURDIR)/$(file)")
STAGED_INPUT_PATHS := $(foreach file,$(MODULE_INPUTS),"$(BUILD_DIR)/$(file)")

ifeq ($(KBUILD_HAS_MO),1)
KBUILD_MODULE_DIR := $(CURDIR)
KBUILD_OUTPUT_ARG := MO="$(BUILD_DIR)"
else
# Linux 6.12 and older do not support MO=. Build from a staged source copy so
# Kbuild still keeps generated objects out of the production source directory.
KBUILD_MODULE_DIR := $(BUILD_DIR)
KBUILD_OUTPUT_ARG :=
endif

.PHONY: all clean prepare

all: prepare
	$(MAKE) -C "$(KDIR)" M="$(KBUILD_MODULE_DIR)" $(KBUILD_OUTPUT_ARG) modules

prepare:
	mkdir -p "$(BUILD_DIR)"
ifeq ($(KBUILD_HAS_MO),)
	cp -f $(SOURCE_INPUT_PATHS) "$(BUILD_DIR)/"
endif

clean:
ifeq ($(KBUILD_HAS_MO),1)
	$(MAKE) -C "$(KDIR)" M="$(KBUILD_MODULE_DIR)" $(KBUILD_OUTPUT_ARG) clean
else
	if test -f "$(BUILD_DIR)/Makefile"; then \
		$(MAKE) -C "$(KDIR)" M="$(KBUILD_MODULE_DIR)" clean; \
	fi
	rm -f $(STAGED_INPUT_PATHS)
endif
