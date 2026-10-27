#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# P8.2 SSR: SIGKILL the Zephyr modem under a 10 ms ping from the AP, restart it
# at once, and report the AP's own detection/recovery numbers and the ping gap
# (all measured inside the AP guest, so ssh latency does not matter).
# Prereq: AP mlink0 UP with the Zephyr modem (zephyr-modem.sh run in tmux "zmodem").
# usage: bash scripts/win/zephyr-ssr.sh [rounds] [loop|net]
set -euo pipefail
ROUNDS=${1:-3}
MODE=${2:-loop}
ZM='bash ~/D/mlink/scripts/host/zephyr-modem.sh'

for r in $(seq 1 "$ROUNDS"); do
	ssh mlink-ap 'rm -f /root/ssr-ping.txt; (setsid ping -D -i 0.01 -w 30 10.77.0.2 > /root/ssr-ping.txt 2>&1 &)'
	sleep 3
	# kill + restart in one command: Zephyr is never left dead
	ssh kali "$ZM stop; tmux kill-session -t zmodem 2>/dev/null; tmux new-session -d -s zmodem '$ZM run $MODE'"
	sleep 8		# Zephyr boots in well under a second; the carrier dip is ~45 ms
	echo "== round $r"
	ssh mlink-ap 'grep -E "last_hb_age_ms|last_recover_us" /sys/kernel/debug/mlink/link
		dmesg | grep -E "link reset|link UP" | tail -2
		pkill -x ping; sleep 0.5
		awk -F"[][]" "/bytes from/ { t = \$2; if (p && t - p > g) g = t - p; p = t }
			END { printf \"max ping gap %.0f ms\n\", g * 1000 }" /root/ssr-ping.txt
		tail -2 /root/ssr-ping.txt | head -1'
done
