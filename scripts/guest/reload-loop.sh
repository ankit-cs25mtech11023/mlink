#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# P1.8: load/unload mlink N times while the peer keeps kicking us.
# Usage (in a guest): bash /mnt/share/reload-loop.sh <ap|modem> <N>
set -euo pipefail

ROLE=$1
N=$2
D=/sys/kernel/debug/mlink

lsmod | grep -q '^mlink ' && rmmod mlink
for i in $(seq 1 "$N"); do
	insmod /mnt/share/mlink.ko role="$ROLE"
	# Wait (bounded) for discovery, exercise the doorbell, then unload.
	for _ in $(seq 1 20); do
		grep -q 'peer_found: 1' $D/info && break
		sleep 0.05
	done
	echo 3 > $D/ring_doorbell 2>/dev/null || true
	rmmod mlink
done
insmod /mnt/share/mlink.ko role="$ROLE"
echo "reload-loop: $N cycles done ($ROLE)"
