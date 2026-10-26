#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# P6.4 experiment matrix. Run from Git Bash after bench-prep.sh on the laptop.
# Writes CSVs to bench/results/<flavour>/ (committed) and a raw log next to them.
#
#   E1 doorbell RTT            (debugfs pingpong, interrupt to interrupt)
#   E2 throughput vs MTU       (576/1500/4000 x TCP/UDP x fwd/rev)
#   E3 throughput vs NAPI weight (8/16/32/64)
#   E4 UDP RTT: irq vs busy-poll vs no kick suppression, + loopback baseline
#   E5 interrupts per packet and guest CPU vs offered UDP load
#
# Usage: bash bench/run_matrix.sh [REPS] [SECS]
set -uo pipefail

REPS=${1:-5}
SECS=${2:-10}
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
PY="$REPO/.venv/Scripts/python"
[ -x "$PY" ] || PY=python
KREL=$(ssh mlink-ap uname -r)
FLAV=${KREL##*-}
OUT="$REPO/bench/results/$FLAV"
mkdir -p "$OUT"
RAW="$OUT/matrix-raw.log"
D=/sys/kernel/debug/mlink
g() { ssh -o BatchMode=yes "mlink-$1" "${@:2}"; }
log() { echo "$(date +%T) $*" | tee -a "$RAW"; }
up() {	# (re)load both sides with optional module params, wait for UP
	for vm in ap modem; do g $vm "bash /mnt/share/mlink-down.sh >/dev/null; bash /mnt/share/mlink-up.sh $vm $* >/dev/null"; done
	for _ in $(seq 1 100); do
		g ap "grep -q 'state: *UP' $D/link" && g modem "grep -q 'state: *UP' $D/link" && return 0
		sleep 0.2
	done
	log "link did not come UP"; return 1
}
iperf() {	# iperf <extra args>: one run AP->modem; prints "mbps,extra"
	g ap "iperf3 -c 10.77.0.2 -t $SECS -J $*" 2>/dev/null | "$PY" "$REPO/bench/iperf_json.py"
}

: > "$RAW"
log "matrix start kernel=$KREL reps=$REPS secs=$SECS"
log "host: $(ssh kali 'sudo -n /usr/local/sbin/mlink-bench-prep status' | tr '\n' ' ')"
log "temp: $(ssh kali 'echo $(( $(cat /sys/class/thermal/thermal_zone5/temp)/1000 ))C')"
up || exit 1
g modem "pkill iperf3; iperf3 -s -D; pkill udp_pingpong; nohup /mnt/share/udp_pingpong -s >/dev/null 2>&1 &"
sleep 1

# ---- warm-up (discarded) -----------------------------------------------------
iperf "" >/dev/null

# ---- E1 doorbell RTT ---------------------------------------------------------
echo "rep,count,lost,min_ns,p50_ns,p90_ns,p99_ns,max_ns" > "$OUT/e1_doorbell.csv"
for r in $(seq 1 "$REPS"); do
	l=$(g ap "echo 10000 > $D/pingpong; cat $D/pingpong")
	log "E1 rep $r: $l"
	echo "$l" | awk -v r=$r '{print r","$2","$4","$6","$8","$10","$12","$14}' >> "$OUT/e1_doorbell.csv"
done

# ---- E2 throughput vs MTU ----------------------------------------------------
echo "mtu,proto,dir,rep,mbps,extra" > "$OUT/e2_mtu.csv"
for mtu in 576 1500 4000; do
	for vm in ap modem; do g $vm "ip link set mlink0 mtu $mtu"; done
	for proto in tcp udp; do
		for dir in fwd rev; do
			a=""
			[ $proto = udp ] && a="-u -b 0 -l $((mtu - 28))"
			[ $dir = rev ] && a="$a -R"
			for r in $(seq 1 "$REPS"); do
				res=$(iperf "$a")
				log "E2 mtu=$mtu $proto $dir rep $r: $res"
				echo "$mtu,$proto,$dir,$r,$res" >> "$OUT/e2_mtu.csv"
			done
		done
	done
done
for vm in ap modem; do g $vm "ip link set mlink0 mtu 1500"; done

# ---- E3 throughput vs NAPI weight ---------------------------------------------
echo "napi_weight,rep,mbps,retrans,polls_per_s,budget_exhausted" > "$OUT/e3_napi.csv"
for w in 8 16 32 64; do
	up napi_weight=$w || continue
	g modem "pgrep -x iperf3 >/dev/null || iperf3 -s -D"
	for r in $(seq 1 "$REPS"); do
		p0=$(g modem "ethtool -S mlink0 | awk '/napi_polls:/{print \$2}'")
		b0=$(g modem "ethtool -S mlink0 | awk '/napi_budget_exhausted:/{print \$2}'")
		res=$(iperf "")
		p1=$(g modem "ethtool -S mlink0 | awk '/napi_polls:/{print \$2}'")
		b1=$(g modem "ethtool -S mlink0 | awk '/napi_budget_exhausted:/{print \$2}'")
		log "E3 weight=$w rep $r: $res polls=$((p1 - p0)) exhausted=$((b1 - b0))"
		echo "$w,$r,$res,$(( (p1 - p0) / SECS )),$((b1 - b0))" >> "$OUT/e3_napi.csv"
	done
done
up || exit 1
g modem "pgrep -x iperf3 >/dev/null || iperf3 -s -D; pgrep -x udp_pingpong >/dev/null || (nohup /mnt/share/udp_pingpong -s >/dev/null 2>&1 &)"

# ---- E4 UDP round-trip latency ------------------------------------------------
echo "mode,rep,n,lost,min_us,p50_us,p90_us,p99_us,p999_us,max_us" > "$OUT/e4_latency.csv"
pp() {	# pp <mode> <client args...>
	local m=$1; shift
	for r in $(seq 1 "$REPS"); do
		l=$(g ap "/mnt/share/udp_pingpong $* -n 20000 -w 500 -l 64")
		log "E4 $m rep $r: $l"
		echo "$l" | sed -E 's/.* n=([0-9]+) lost=([0-9]+) .*min=([0-9.]+) p50=([0-9.]+) p90=([0-9.]+) p99=([0-9.]+) p999=([0-9.]+) max=([0-9.]+).*/\1,\2,\3,\4,\5,\6,\7,\8/' |
			sed "s/^/$m,$r,/" >> "$OUT/e4_latency.csv"
	done
}
g ap "pkill udp_pingpong; nohup /mnt/share/udp_pingpong -s -p 7778 >/dev/null 2>&1 &"
pp loopback_stack_only -c 127.0.0.1 -p 7778
pp irq -c 10.77.0.2
g modem "pkill udp_pingpong; nohup /mnt/share/udp_pingpong -s -b 50 >/dev/null 2>&1 &"
pp busy_poll_50us -c 10.77.0.2 -b 50
g modem "pkill udp_pingpong; nohup /mnt/share/udp_pingpong -s >/dev/null 2>&1 &"
for vm in ap modem; do g $vm "echo 0 > /sys/module/mlink/parameters/kick_suppress"; done
pp no_kick_suppression -c 10.77.0.2
for vm in ap modem; do g $vm "echo 1 > /sys/module/mlink/parameters/kick_suppress"; done

# ---- E5 interrupts per packet vs offered load --------------------------------
echo "offered_mbps,rep,rx_mbps,lost_pct,rx_pkts,rx_irqs,irqs_per_pkt,pkts_per_poll,modem_cpu_busy_pct" > "$OUT/e5_irq_load.csv"
cpu() { g modem "awk '/^cpu /{print \$2+\$3+\$4+\$7+\$8, \$2+\$3+\$4+\$5+\$6+\$7+\$8}' /proc/stat"; }
for rate in 10 50 100 200 0; do
	for r in $(seq 1 3); do
		s0=$(g modem "ethtool -S mlink0 | awk '/rx_msgs:|rx_irqs:|napi_polls:/{printf \"%s \", \$2}'")
		c0=$(cpu)
		res=$(iperf "-u -b ${rate}M -l 1400")
		c1=$(cpu)
		s1=$(g modem "ethtool -S mlink0 | awk '/rx_msgs:|rx_irqs:|napi_polls:/{printf \"%s \", \$2}'")
		row=$(echo "$s0 $s1 $c0 $c1" | awk '{m=$4-$1; p=$5-$2; i=$6-$3; b=$9-$7; t=$10-$8;
			printf "%d,%d,%.3f,%.2f,%.1f", m, i, (m ? i/m : 0), (p ? m/p : 0), (t ? 100*b/t : 0)}')
		log "E5 offered=${rate}M rep $r: $res $row"
		echo "$rate,$r,$res,$row" >> "$OUT/e5_irq_load.csv"
	done
done

log "temp: $(ssh kali 'echo $(( $(cat /sys/class/thermal/thermal_zone5/temp)/1000 ))C')"
for vm in ap modem; do g $vm "bash /mnt/share/dmesg-gate.sh" | tee -a "$RAW"; done
log "matrix end"
