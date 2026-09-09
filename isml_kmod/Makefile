obj-m += isml_diag.o
isml_diag-y := isml_diag_main.o

ccflags-y += -I$(src)/include/uapi

KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
