#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# P5.1 chaos campaign (runs on the laptop): for each corruption mode, run
# shm_chaos against /dev/shm/mlink-shm for DUR s while TCP + ping run, then
# stop and check the driver (a) did not crash, (b) counted the damage,
# (c) re-established the link by itself, (d) passes the dmesg gate.
#
# Usage: bash ~/D/mlink/scripts/host/chaos.sh [DUR] [RATE] [SEED]
set -uo pipefail
mkdir -p "$HOME/D/mlink-work/tmp"	# scratch files stay inside the project (hard rule 2)

DUR=${1:-10}
RATE=${2:-20}
SEED=${3:-12345}
CHAOS=~/D/mlink-work/bin/shm_chaos
G() {
	local port=2221
	[ "$1" = modem ] && port=2222
	ssh -o BatchMode=yes -o ConnectTimeout=5 -o StrictHostKeyChecking=no \
	    -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -p $port root@127.0.0.1 "${@:2}"
}
both_up() {
	for _ in $(seq 1 150); do
		if G ap "grep -q 'state: *UP' /sys/kernel/debug/mlink/link" &&
		   G modem "grep -q 'state: *UP' /sys/kernel/debug/mlink/link"; then
			return 0
		fi
		sleep 0.2
	done
	return 1
}
counters() {
	G "$1" "ethtool -S mlink0 | grep -E 'rx_len_errors|rx_proto_errors|ring_corruptions|link_resets|rx_stalls|hdr_repairs|hb_saved' | tr -s ' ' | tr '\n' ' '"
}

MARK="chaos-$(date +%s)"
for vm in ap modem; do
	G $vm "echo $MARK > /dev/kmsg; bash /mnt/share/mlink-down.sh >/dev/null; bash /mnt/share/mlink-up.sh $vm >/dev/null"
done
both_up || { echo "link not up at start"; exit 1; }
G modem "pkill iperf3; iperf3 -s -D"
sleep 1

echo "=== chaos campaign $(date -Is) dur=${DUR}s rate=${RATE}/s seed=$SEED"
for mode in ${MODES:-flip len idx flags magic hb mix}; do
	echo "--- mode $mode"
	G ap "iperf3 -c 10.77.0.2 -t $((DUR + 5)) -f m" > $HOME/D/mlink-work/tmp/chaos-iperf.$$ 2>&1 &
	IP=$!
	sleep 2
	$CHAOS -m $mode -r "$RATE" -d "$DUR" -s "$SEED" > $HOME/D/mlink-work/tmp/chaos-ops.$$ 2>&1
	t1=$(date +%s.%N)
	echo "chaos ops: $(grep -c -vE '^shm_chaos' $HOME/D/mlink-work/tmp/chaos-ops.$$) ($(head -1 $HOME/D/mlink-work/tmp/chaos-ops.$$ | cut -c1-60))"
	if both_up; then
		echo "recovered: both sides UP $(echo "$(date +%s.%N) - $t1" | bc | cut -c1-5) s after chaos stopped (incl. ssh polling)"
	else
		echo "recovered: NO (link not UP 30 s after chaos)"
	fi
	wait $IP
	if [ -n "${TRACE_STATE:-}" ]; then
		for i in 1 2 3 4 5 6 7 8; do
			echo "  t+${i}s ap: $(G ap "grep -E 'state|link_resets' /sys/kernel/debug/mlink/link | tr -s ' ' | paste -sd' '") modem: $(G modem "grep -E 'state' /sys/kernel/debug/mlink/link | tr -s ' '") ping: $(G ap "ping -c 1 -W 1 10.77.0.2 >/dev/null && echo ok || echo FAIL")"
			sleep 1
		done
	fi
	echo "ping after: $(G ap "ping -c 20 -i 0.05 -W 1 -q 10.77.0.2 | grep -o '[0-9.]*% packet loss'")"
	echo "ap:    $(counters ap)"
	echo "modem: $(counters modem)"
	SEED=$((SEED + 1))
done
rm -f $HOME/D/mlink-work/tmp/chaos-iperf.$$ $HOME/D/mlink-work/tmp/chaos-ops.$$
echo "--- dmesg gate"
for vm in ap modem; do G $vm "bash /mnt/share/dmesg-gate.sh $MARK"; done
echo "=== end $(date -Is) temp=$(( $(cat /sys/class/thermal/thermal_zone5/temp) / 1000 ))C"
