#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# P5.5 long soak (laptop, tmux): DUR seconds of bidirectional TCP (iperf3 in
# 5-minute sessions, so a TCP that dies with a restart is simply restarted)
# and a 50 ms ping, plus every 5 min a 10 s shm_chaos "mix" burst and, offset
# by 150 s, a modem driver restart. Then: dmesg gate, kmemleak scan, summary.
#
# Usage: bash ~/D/mlink/scripts/host/soak-long.sh [DUR] > ~/D/mlink-work/logs/soak-long.log
set -uo pipefail
mkdir -p "$HOME/D/mlink-work/tmp"	# scratch files stay inside the project (hard rule 2)

DUR=${1:-3600}
ABORT_C=${ABORT_C:-92}
CHAOS=~/D/mlink-work/bin/shm_chaos
G() {
	local port=2221
	[ "$1" = modem ] && port=2222
	ssh -o BatchMode=yes -o ConnectTimeout=5 -o StrictHostKeyChecking=no \
	    -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -p $port root@127.0.0.1 "${@:2}"
}
temp() { echo $(( $(cat /sys/class/thermal/thermal_zone5/temp) / 1000 )); }

MARK="soaklong-$(date +%s)"
for vm in ap modem; do
	G $vm "echo $MARK > /dev/kmsg; bash /mnt/share/mlink-down.sh >/dev/null; bash /mnt/share/mlink-up.sh $vm >/dev/null"
done
sleep 3
echo "=== long soak start $(date -Is) dur=${DUR}s temp=$(temp)C"

end=$((SECONDS + DUR))
G ap "ping -i 0.05 -w $DUR -q 10.77.0.2" > $HOME/D/mlink-work/tmp/sl-ping.$$ 2>&1 &
PING=$!
(	# TCP sessions; a session broken by a restart just ends and the next starts
	n=0
	while [ $SECONDS -lt $end ]; do
		G modem "pgrep -x iperf3 >/dev/null || iperf3 -s -D" >/dev/null 2>&1
		left=$((end - SECONDS)); [ $left -gt 300 ] && left=300
		[ $left -lt 5 ] && break
		r=$(G ap "iperf3 -c 10.77.0.2 --bidir -P 4 -t $left -f m 2>&1" | grep -E "SUM.*receiver" | tail -2 | awk '{print $7}' | paste -sd/)
		n=$((n + 1)); echo "tcp session $n: ${r:-ended early} Mbit/s (rx/tx)"
		sleep 1
	done
) > $HOME/D/mlink-work/tmp/sl-tcp.$$ 2>&1 &
TCP=$!

chaos=0; restarts=0; maxt=0; aborted=0; next_chaos=$((SECONDS + 120)); next_ssr=$((SECONDS + 270))
while [ $SECONDS -lt $end ]; do
	t=$(temp); [ "$t" -gt "$maxt" ] && maxt=$t
	if [ "$t" -ge "$ABORT_C" ]; then echo "!!! ${t}C: abort"; aborted=1; break; fi
	if [ $SECONDS -ge $next_chaos ]; then
		$CHAOS -m mix -r 20 -d 10 -s $((1000 + chaos)) > /dev/null 2>&1
		chaos=$((chaos + 1)); next_chaos=$((SECONDS + 300))
		echo "$(date +%T) chaos burst $chaos done"
	fi
	if [ $SECONDS -ge $next_ssr ]; then
		G modem "bash /mnt/share/mlink-down.sh >/dev/null; sleep 0.5; bash /mnt/share/mlink-up.sh modem >/dev/null"
		restarts=$((restarts + 1)); next_ssr=$((SECONDS + 300))
		echo "$(date +%T) modem restart $restarts done"
	fi
	sleep 5
done
[ $aborted = 1 ] && G ap "pkill iperf3; pkill ping"
wait $PING $TCP 2>/dev/null

echo "=== tcp"; cat $HOME/D/mlink-work/tmp/sl-tcp.$$
echo "=== ping"; tail -2 $HOME/D/mlink-work/tmp/sl-ping.$$
rm -f $HOME/D/mlink-work/tmp/sl-ping.$$ $HOME/D/mlink-work/tmp/sl-tcp.$$
for vm in ap modem; do
	echo "=== $vm"
	G $vm "grep -E 'state|link_ups|link_resets' /sys/kernel/debug/mlink/link | tr -s ' '"
	G $vm "ethtool -S mlink0 | grep -E 'corruptions|stalls|errors|repairs|hb_saved' | tr -s ' ' | paste -sd' '"
	G $vm "echo scan > /sys/kernel/debug/kmemleak; sleep 5; echo scan > /sys/kernel/debug/kmemleak; echo kmemleak reports: \$(grep -c unreferenced /sys/kernel/debug/kmemleak)"
	G $vm "KCSAN_FILTER=${KCSAN_FILTER:-0} bash /mnt/share/dmesg-gate.sh $MARK"
done
echo "=== long soak end $(date -Is) chaos_bursts=$chaos modem_restarts=$restarts max_temp=${maxt}C aborted=$aborted"
