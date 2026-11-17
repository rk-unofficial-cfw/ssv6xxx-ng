# SPDX-License-Identifier: GPL-2.0
#
# ssv6xxx-ng - out-of-tree build for the SSV6051 WLAN driver
#
# Usage:
#   make                          # build against the running kernel
#   make KDIR=/path/to/kernel     # build against a specific kernel tree
#   make install                  # install the module
#   make clean
#
KERNELRELEASE ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KERNELRELEASE)/build

obj-m += ssv6051.o
ssv6051-objs := main.o sdio.o fw.o efuse.o init.o hci.o key.o tx.o rx.o debugfs.o

ccflags-y += -I$(src)
# Compile dev_dbg() in: the ssv6051 debug= module parameter still gates
# what is actually printed (see ssv6051.h DBG_* flags).
ccflags-y += -DDEBUG

.PHONY: all modules install clean check

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

install: modules
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

check: modules
	@echo "Build OK (kernel $(KERNELRELEASE))"
