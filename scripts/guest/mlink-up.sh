#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Load mlink with the given role and bring up mlink0 as a point-to-point link.
#   ap:    10.77.0.1 peer 10.77.0.2      modem: 10.77.0.2 peer 10.77.0.1
# Usage (in a guest): bash /mnt/share/mlink-up.sh <ap|modem> [extra module params]
set -euo pipefail

ROLE=$1
shift
case "$ROLE" in
ap)    LOCAL=10.77.0.1; PEER=10.77.0.2 ;;
modem) LOCAL=10.77.0.2; PEER=10.77.0.1 ;;
*) echo "usage: mlink-up.sh ap|modem" >&2; exit 2 ;;
esac

lsmod | grep -q '^mlink ' || insmod /mnt/share/mlink.ko role="$ROLE" "$@"
# The netdev appears during probe; give udev a moment if needed.
for _ in $(seq 1 50); do ip link show mlink0 >/dev/null 2>&1 && break; sleep 0.1; done
ip addr flush dev mlink0
ip addr add "$LOCAL" peer "$PEER" dev mlink0
ip link set mlink0 up
echo "mlink0 up: $LOCAL -> $PEER"
