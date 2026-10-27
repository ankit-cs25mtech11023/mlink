#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Take mlink0 down and unload the driver. Usage: bash /mnt/share/mlink-down.sh
ip link set mlink0 down 2>/dev/null || true
# an open /dev/mlink_ctrl pins the module; mlinkd (P7) may hold one
if pkill -x mlinkd; then
	for _ in $(seq 1 20); do pgrep -x mlinkd >/dev/null || break; sleep 0.1; done
fi
lsmod | grep -q '^mlink ' && rmmod mlink
echo "mlink unloaded"
