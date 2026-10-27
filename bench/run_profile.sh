#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# E7 (P6.4): where does the time go? Sample kernel instruction pointers on both
# guests with bpftrace (499 Hz per CPU) during a TCP AP->modem run and print
# the top kernel functions with their share of all samples (idle included).
# Runs on the laptop (ON_HOST style). Output: stdout (tee it to a results file).
#
# Usage: bash ~/D/mlink/bench/run_profile.sh [SECS]
set -uo pipefail

SECS=${1:-8}
G() {
	local port=2221
	[ "$1" = modem ] && port=2222
	ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
	    -o LogLevel=ERROR -p $port root@127.0.0.1 "${@:2}"
}
PROG="profile:hz:499 { @[kstack(1)] = count(); } interval:s:$SECS { exit(); }"

echo "# E7 kernel profile, $(G ap uname -r), $(date -Iseconds), TCP AP->modem MTU 1500, ${SECS}s @ 499 Hz/CPU"
G modem "pgrep -x iperf3 >/dev/null || iperf3 -s -D"
G ap "iperf3 -c 10.77.0.2 -t $((SECS + 4)) -f m" > /tmp/prof-iperf.$$ 2>&1 &
sleep 2
for vm in ap modem; do
	G $vm "bpftrace -q -e '$PROG' 2>/dev/null" > /tmp/prof-$vm.$$ &
done
wait
echo "# iperf3: $(grep receiver /tmp/prof-iperf.$$ | awk '{print $7, $8}')"
for vm in ap modem; do
	echo "## $vm (top 20 kernel functions, % of samples)"
	# bpftrace prints "@[\n    func+off\n]: N" blocks; reduce to "func N".
	awk '/^@\[/ {getline f; gsub(/^[ \t]+|\+.*$/, "", f); next} /^\]: / {n[f] += $2; t += $2}
	     END {for (k in n) printf "%6.2f%%  %s\n", 100 * n[k] / t, k}' /tmp/prof-$vm.$$ |
		sort -rn | head -20
done
rm -f /tmp/prof-*.$$
