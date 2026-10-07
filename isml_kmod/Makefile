obj-m += isml_diag.o
isml_diag-y := isml_diag_main.o

ccflags-y += -I$(src)

KDIR ?= /lib/modules/$(shell uname -r)/build
BUILD_DIR ?= $(CURDIR)/build

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) MO=$(BUILD_DIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) MO=$(BUILD_DIR) clean
