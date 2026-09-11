# Out-of-tree build of ov5647.ko against the L4T kernel headers.
#
# On the Jetson itself (nvidia-l4t-kernel-headers installed) just run `make`.
# For a cross build point KDIR at a configured kernel tree and set
# ARCH/CROSS_COMPILE as usual.

obj-m := ov5647.o

KVER  ?= $(shell uname -r)
KDIR  ?= /lib/modules/$(KVER)/build
# The tegracam / camera_common headers live in the sibling "nvidia" tree of
# the L4T headers package (…/linux-headers-*/nvidia/include).
NVDIR ?= $(realpath $(KDIR)/../nvidia)

ccflags-y += -I$(NVDIR)/include

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all modules clean
