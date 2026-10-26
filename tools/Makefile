# SPDX-License-Identifier: GPL-2.0-only
# Userspace tools. shm_chaos runs on the HOST (needs /dev/shm/mlink-shm);
# udp_pingpong runs in the guests.
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=gnu11

TOOLS := shm_chaos udp_pingpong

all: $(TOOLS)

shm_chaos: shm_chaos.c ../driver/mlink_shm.h
	$(CC) $(CFLAGS) -o $@ shm_chaos.c

udp_pingpong: udp_pingpong.c
	$(CC) $(CFLAGS) -static -o $@ udp_pingpong.c	# runs in the guests: no glibc version coupling

clean:
	rm -f $(TOOLS)

.PHONY: all clean
