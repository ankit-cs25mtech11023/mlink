#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Fail if the kernel log contains a bug report. With a marker argument, only
# lines after the last occurrence of the marker are checked (test.sh writes
# one to /dev/kmsg at the start of each run).
#
# Usage (in a guest): bash /mnt/share/dmesg-gate.sh [marker]
#   KCSAN_FILTER=1: on the KCSAN kernel, drop data-race reports that have no
#   mlink frame (strict KCSAN reports races in core kernel code at boot and
#   under load); they are counted, and any report touching mlink still fails.
set -uo pipefail

MARK=${1:-}
PAT='BUG:|WARNING:|KASAN|possible circular|inconsistent lock|INFO: task .* blocked|scheduling while atomic|Oops|UBSAN|kmemleak: [0-9]+ new suspected|NETDEV WATCHDOG|general protection|Call Trace'

if [ -n "$MARK" ]; then
	log=$(dmesg | awk -v m="$MARK" 'index($0, m) { buf = ""; next } { buf = buf $0 "\n" } END { printf "%s", buf }')
else
	log=$(dmesg)
fi

if [ "${KCSAN_FILTER:-0}" = 1 ]; then
	# A report runs from "BUG: KCSAN" to the "=====" line; keep it only if it
	# has an mlink frame (module tag or mlink_* symbol).
	log=$(printf '%s\n' "$log" | awk '
		/BUG: KCSAN/ { inb = 1; blk = ""; total++ }
		inb { blk = blk $0 "\n"; if ($0 ~ /\[mlink\]|mlink_[a-z_0-9]+\+0x/) hit = 1
		      if ($0 ~ /=====/) { if (hit) { printf "%s", blk; kept++ } inb = 0; hit = 0 }
		      next }
		{ print }
		END { printf "KCSAN-FILTER: %d report(s), %d with mlink frames\n", total, kept > "/dev/stderr" }')
fi

hits=$(printf '%s' "$log" | grep -E "$PAT")
if [ -n "$hits" ]; then
	echo "DMESG GATE: FAIL ($(hostname))"
	printf '%s\n' "$hits" | head -40
	exit 1
fi
echo "DMESG GATE: PASS ($(hostname))"
