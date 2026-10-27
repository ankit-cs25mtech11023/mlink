# SPDX-License-Identifier: GPL-2.0-only
# Userspace tools. shm_chaos runs on the HOST (needs /dev/shm/mlink-shm);
# udp_pingpong runs in the guests.
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=gnu11

TOOLS := shm_chaos udp_pingpong mlinkd mlinkctl

all: $(TOOLS)

shm_chaos: shm_chaos.c ../driver/mlink_shm.h
	$(CC) $(CFLAGS) -o $@ shm_chaos.c

udp_pingpong: udp_pingpong.c
	$(CC) $(CFLAGS) -static -o $@ udp_pingpong.c	# runs in the guests: no glibc version coupling

mlinkd: mlinkd.c mlink_ctrl_lib.h ../driver/mlink_ctrl.h
	$(CC) $(CFLAGS) -static -o $@ mlinkd.c

mlinkctl: mlinkctl.c mlink_ctrl_lib.h ../driver/mlink_ctrl.h
	$(CC) $(CFLAGS) -static -o $@ mlinkctl.c

clean:
	rm -f $(TOOLS)

.PHONY: all clean
