#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Fail if the kernel log contains a bug report. With a marker argument, only
# lines after the last occurrence of the marker are checked (test.sh writes
# one to /dev/kmsg at the start of each run).
#
# Usage (in a guest): bash /mnt/share/dmesg-gate.sh [marker]
set -uo pipefail

MARK=${1:-}
PAT='BUG:|WARNING:|KASAN|possible circular|inconsistent lock|INFO: task .* blocked|scheduling while atomic|Oops|UBSAN|kmemleak|NETDEV WATCHDOG|general protection|Call Trace'

if [ -n "$MARK" ]; then
	log=$(dmesg | awk -v m="$MARK" 'index($0, m) { buf = ""; next } { buf = buf $0 "\n" } END { printf "%s", buf }')
else
	log=$(dmesg)
fi

hits=$(printf '%s' "$log" | grep -E "$PAT")
if [ -n "$hits" ]; then
	echo "DMESG GATE: FAIL ($(hostname))"
	printf '%s\n' "$hits" | head -40
	exit 1
fi
echo "DMESG GATE: PASS ($(hostname))"
