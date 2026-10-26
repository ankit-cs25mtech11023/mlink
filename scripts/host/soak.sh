#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# P3.8 soak (runs on the laptop, in tmux): bidirectional iperf3 -P 8 + ping
# flood AP->modem + mlink0 down/up flaps on the modem every 5 s, for DUR s.
# A temperature watchdog aborts the traffic above ABORT_C. Then: dmesg gate
# on both guests and a counter summary.
#
# Usage: bash ~/D/mlink/scripts/host/soak.sh [DUR] > ~/D/mlink-work/logs/soak.log
set -uo pipefail

DUR=${1:-600}
ABORT_C=${ABORT_C:-92}
G() {	# G ap|modem cmd...
	local port=2221
	[ "$1" = modem ] && port=2222
	ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
	    -o LogLevel=ERROR -p $port root@127.0.0.1 "${@:2}"
}
temp() { echo $(( $(cat /sys/class/thermal/thermal_zone5/temp) / 1000 )); }

MARK="soak-$(date +%s)"
echo "=== soak start $(date -Is) dur=${DUR}s temp=$(temp)C"
for vm in ap modem; do
	G $vm "echo $MARK > /dev/kmsg; bash /mnt/share/mlink-down.sh >/dev/null; bash /mnt/share/mlink-up.sh $vm"
done
sleep 2
G modem "pkill iperf3; iperf3 -s -D"
sleep 1

G ap "iperf3 -c 10.77.0.2 --bidir -P 8 -t $DUR -f m" > /tmp/mlink-soak-iperf.$$ 2>&1 &
IPERF=$!
G ap "ping -f -w $DUR -q 10.77.0.2" > /tmp/mlink-soak-ping.$$ 2>&1 &
PING=$!
G modem "end=\$((SECONDS + $DUR)); n=0; while [ \$SECONDS -lt \$end ]; do sleep 5; ip link set mlink0 down; sleep 0.5; ip link set mlink0 up; n=\$((n+1)); done; echo flaps=\$n" > /tmp/mlink-soak-flap.$$ 2>&1 &
FLAP=$!

aborted=0
maxt=0
while kill -0 $IPERF 2>/dev/null; do
	t=$(temp)
	[ "$t" -gt "$maxt" ] && maxt=$t
	if [ "$t" -ge "$ABORT_C" ]; then
		echo "!!! temp ${t}C >= ${ABORT_C}C: aborting traffic"
		G ap "pkill iperf3; pkill ping"
		aborted=1
		break
	fi
	sleep 2
done
wait $IPERF $PING $FLAP 2>/dev/null

echo "=== iperf3 (last lines)"; grep -E "SUM.*(sender|receiver)" /tmp/mlink-soak-iperf.$$ | tail -4
echo "=== ping flood"; tail -3 /tmp/mlink-soak-ping.$$
echo "=== flaps"; cat /tmp/mlink-soak-flap.$$
rm -f /tmp/mlink-soak-*.$$
for vm in ap modem; do
	echo "=== $vm counters"
	G $vm "ip -s link show mlink0 | sed -n '3,6p'; ethtool -S mlink0 | tail -n +2 | tr -s ' ' | tr '\n' ' '; echo"
	G $vm "bash /mnt/share/dmesg-gate.sh $MARK"
done
echo "=== soak end $(date -Is) max_temp=${maxt}C aborted=$aborted"
