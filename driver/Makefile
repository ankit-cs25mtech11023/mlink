# SPDX-License-Identifier: GPL-2.0-only
# Out-of-tree build against the guest kernel tree (never the host's).
KFLAVOUR ?= debug
KDIR ?= $(HOME)/D/mlink-work/build/$(KFLAVOUR)

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all clean
