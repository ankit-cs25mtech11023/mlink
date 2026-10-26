#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Take mlink0 down and unload the driver. Usage: bash /mnt/share/mlink-down.sh
ip link set mlink0 down 2>/dev/null || true
lsmod | grep -q '^mlink ' && rmmod mlink
echo "mlink unloaded"
