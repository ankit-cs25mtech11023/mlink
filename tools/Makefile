# SPDX-License-Identifier: GPL-2.0-only
# Userspace tools. shm_chaos runs on the HOST (needs /dev/shm/mlink-shm);
# udp_pingpong runs in the guests.
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=gnu11

TOOLS := shm_chaos

all: $(TOOLS)

shm_chaos: shm_chaos.c ../driver/mlink_shm.h
	$(CC) $(CFLAGS) -o $@ shm_chaos.c

clean:
	rm -f $(TOOLS)

.PHONY: all clean
