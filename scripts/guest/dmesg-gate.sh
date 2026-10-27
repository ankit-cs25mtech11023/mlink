#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Fail if the kernel log contains a bug report. With a marker argument, only
# lines after the last occurrence of the marker are checked (test.sh writes
# one to /dev/kmsg at the start of each run).
#
# Usage (in a guest): bash /mnt/share/dmesg-gate.sh [marker]
#   KCSAN_FILTER=1: on the KCSAN kernel, keep only data-race reports whose
#   racing functions (the "data-race in A / B" header) include an mlink one.
#   Strict KCSAN also reports races inside core code (qdisc, timekeeping,
#   nohz), some reached through mlink calls; those are counted, not gated.
set -uo pipefail

MARK=${1:-}
PAT='BUG:|WARNING:|KASAN|possible circular|inconsistent lock|INFO: task .* blocked|scheduling while atomic|Oops|UBSAN|kmemleak: [0-9]+ new suspected|NETDEV WATCHDOG|general protection|Call Trace'

if [ -n "$MARK" ]; then
	log=$(dmesg | awk -v m="$MARK" 'index($0, m) { buf = ""; next } { buf = buf $0 "\n" } END { printf "%s", buf }')
else
	log=$(dmesg)
fi

if [ "${KCSAN_FILTER:-0}" = 1 ]; then
	# A report runs from "BUG: KCSAN" to the "=====" line.
	log=$(printf '%s\n' "$log" | awk '
		/BUG: KCSAN/ { inb = 1; blk = ""; total++; own = ($0 ~ /\[mlink\]/); via = 0 }
		inb { blk = blk $0 "\n"; if ($0 ~ /\[mlink\]/) via = 1
		      if ($0 ~ /=====/) { if (own) { printf "%s", blk; kept++ } else if (via) viac++
		                          inb = 0 }
		      next }
		{ print }
		END { printf "KCSAN-FILTER: %d report(s): %d in mlink code, %d in core code reached via mlink, %d other\n",
		      total, kept, viac, total - kept - viac > "/dev/stderr" }')
fi

hits=$(printf '%s' "$log" | grep -E "$PAT")
if [ -n "$hits" ]; then
	echo "DMESG GATE: FAIL ($(hostname))"
	printf '%s\n' "$hits" | head -40
	exit 1
fi
echo "DMESG GATE: PASS ($(hostname))"
