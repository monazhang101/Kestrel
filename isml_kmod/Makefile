obj-m += isml_diag.o
isml_diag-y := isml_diag_main.o

ccflags-y += -I$(src)

KDIR ?= /lib/modules/$(shell uname -r)/build
BUILD_DIR ?= $(CURDIR)/build

.PHONY: all clean selftest-tool

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) MO=$(BUILD_DIR) modules

selftest-tool:
	mkdir -p $(BUILD_DIR)
	$(CC) -Wall -Wextra -O2 -I$(CURDIR) -o $(BUILD_DIR)/irq_selftest irq_selftest.c

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) MO=$(BUILD_DIR) clean
