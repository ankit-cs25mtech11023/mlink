#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# P4.5 Subsystem Restart scenarios (runs on the laptop, in tmux). Each case runs
# with a live TCP flow (iperf3) and a 10 ms ping from the AP, then reports:
#   AP link debugfs (reset reason, heartbeat age at detection, recovery time),
#   ping gap (longest interval without a reply), ping loss, TCP survived?
#
# Usage: bash ~/D/mlink/scripts/host/ssr.sh a|b|c|d|e [N for e]
#   a  modem driver graceful restart (rmmod; sleep 1; insmod + up)
#   b  modem VM kill -9 + restart (new ivshmem peer ID)
#   c  AP driver graceful restart
#   d  both drivers restart
#   e  N consecutive (a) cycles without traffic checks between them
set -uo pipefail

H=~/D/mlink/scripts/host
G() {	# G ap|modem cmd...
	local port=2221
	[ "$1" = modem ] && port=2222
	ssh -o BatchMode=yes -o ConnectTimeout=5 -o StrictHostKeyChecking=no \
	    -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -p $port root@127.0.0.1 "${@:2}"
}
up()      { G "$1" "bash /mnt/share/mlink-up.sh $1" >/dev/null; }
restart() { G "$1" "bash /mnt/share/mlink-down.sh >/dev/null; sleep ${2:-1}; bash /mnt/share/mlink-up.sh $1" >/dev/null; }
link_up() {	# wait until both sides report state UP
	for _ in $(seq 1 300); do
		if G ap "grep -q 'state: *UP' /sys/kernel/debug/mlink/link" 2>/dev/null &&
		   G modem "grep -q 'state: *UP' /sys/kernel/debug/mlink/link" 2>/dev/null; then
			return 0
		fi
		sleep 0.2
	done
	return 1
}

traffic_start() {
	G modem "pkill iperf3; iperf3 -s -D" >/dev/null 2>&1
	sleep 0.5
	G ap "ping -D -i 0.01 -w $1 10.77.0.2" > /tmp/ssr-ping.$$ 2>&1 &
	PING=$!
	G ap "iperf3 -c 10.77.0.2 -t $1 -f m" > /tmp/ssr-iperf.$$ 2>&1 &
	IPERF=$!
}

traffic_report() {
	wait $PING $IPERF 2>/dev/null
	echo "ping: $(grep -E 'packets transmitted' /tmp/ssr-ping.$$)"
	echo "ping gap (longest without reply): $(grep -o '^\[[0-9.]*\]' /tmp/ssr-ping.$$ |
		tr -d '[]' | awk 'NR>1 { g = $1 - p; if (g > m) m = g } { p = $1 } END { printf "%.0f ms", m * 1000 }')"
	# Survived = the client finished without error and the receiver got data.
	if grep -q receiver /tmp/ssr-iperf.$$ && ! grep -qi error /tmp/ssr-iperf.$$ &&
	   ! grep receiver /tmp/ssr-iperf.$$ | grep -q ' 0.00 Bytes'; then
		echo "TCP: survived — $(grep receiver /tmp/ssr-iperf.$$ | awk '{print $5, $6, $7, $8}')"
	else
		echo "TCP: did NOT survive — $(grep -iE 'error|unable' /tmp/ssr-iperf.$$ | head -1)"
	fi
	rm -f /tmp/ssr-ping.$$ /tmp/ssr-iperf.$$
}

ap_link() { G ap "grep -E 'link_resets|last_recover|last_hb_age' /sys/kernel/debug/mlink/link"; }

case "${1:-}" in
a)
	echo "=== (a) modem graceful driver restart, $(date -Is)"
	link_up || { echo "link not up"; exit 1; }
	traffic_start 30
	sleep 8
	restart modem 1
	traffic_report
	ap_link
	;;
b)
	echo "=== (b) modem VM kill -9 + restart, $(date -Is)"
	link_up || { echo "link not up"; exit 1; }
	old=$(G ap "awk '/peer_id/ {print \$2}' /sys/kernel/debug/mlink/link")
	traffic_start 60
	sleep 8
	bash $H/vm.sh kill modem
	t0=$(date +%s.%N)
	sleep 2
	bash $H/vm.sh start modem >/dev/null
	up modem
	link_up && echo "link back UP $(echo "$(date +%s.%N) - $t0" | bc | cut -c1-5) s after kill (includes VM boot)"
	traffic_report
	ap_link
	echo "peer id before: $old, after: $(G ap "awk '/peer_id/ {print \$2}' /sys/kernel/debug/mlink/link")"
	;;
c)
	echo "=== (c) AP graceful driver restart, $(date -Is)"
	link_up || { echo "link not up"; exit 1; }
	traffic_start 30
	sleep 8
	restart ap 1
	traffic_report
	G modem "grep -E 'link_resets|last_recover|last_hb_age' /sys/kernel/debug/mlink/link"
	;;
d)
	echo "=== (d) both drivers restart, $(date -Is)"
	link_up || { echo "link not up"; exit 1; }
	traffic_start 30
	sleep 8
	restart modem 0 &
	restart ap 0
	wait
	traffic_report
	ap_link
	;;
e)
	N=${2:-100}
	echo "=== (e) $N consecutive modem driver restarts, $(date -Is)"
	ok=0
	for i in $(seq 1 "$N"); do
		restart modem 0.2
		if link_up && G ap "ping -c 3 -i 0.05 -W 1 -q 10.77.0.2" >/dev/null; then
			ok=$((ok + 1))
		else
			echo "cycle $i: link/ping FAILED"
		fi
	done
	echo "cycles ok: $ok / $N"
	ap_link
	;;
*)
	echo "usage: ssr.sh a|b|c|d|e [N]" >&2
	exit 2
	;;
esac
