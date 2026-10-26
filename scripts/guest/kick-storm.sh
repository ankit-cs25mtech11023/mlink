#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Keep kicking the peer on every vector for <seconds> (used by P1.8).
# Errors (peer not found while it reloads) are expected and ignored.
# Usage (in a guest): bash /mnt/share/kick-storm.sh <seconds>
D=/sys/kernel/debug/mlink
end=$((SECONDS + $1))
n=0
while [ $SECONDS -lt $end ]; do
	for v in 0 1 2 3; do
		echo $v > $D/ring_doorbell 2>/dev/null && n=$((n + 1))
	done
done
echo "kick-storm: $n kicks sent"
