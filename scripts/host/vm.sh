#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Start/stop the two KVM guests. Start ivshmem.sh first: QEMU connects to the
# server's socket at startup and gets its peer ID and eventfds from it.
#
# Usage: bash ~/D/mlink/scripts/host/vm.sh start|stop|kill|status ap|modem
#   KFLAVOUR=debug|perf (default debug) selects the guest kernel build.
set -euo pipefail

CMD=${1:-}
VM=${2:-}
case "$VM" in
ap)    SSHPORT=2221; GDBPORT=1234 ;;
modem) SSHPORT=2222; GDBPORT=1235 ;;
*) echo "usage: vm.sh start|stop|kill|status ap|modem" >&2; exit 2 ;;
esac

W="$HOME/D/mlink-work"
RUN="$W/run"
LOGS="$W/logs"
KFLAVOUR=${KFLAVOUR:-debug}
BZ="$W/build/$KFLAVOUR/arch/x86/boot/bzImage"
PID="$RUN/$VM.pid"
GSSH=(ssh -o BatchMode=yes -o ConnectTimeout=3 -o StrictHostKeyChecking=no
      -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -p "$SSHPORT" root@127.0.0.1)

running() { [ -f "$PID" ] && kill -0 "$(cat "$PID")" 2>/dev/null; }

case "$CMD" in
start)
	running && { echo "$VM already running (pid $(cat "$PID"))"; exit 0; }
	[ -S "$RUN/ivshmem.sock" ] || { echo "ivshmem-server not running (ivshmem.sh start)" >&2; exit 1; }
	[ -f "$BZ" ] || { echo "no kernel at $BZ" >&2; exit 1; }
	# Fresh serial log per boot: a panic trace is then easy to find.
	[ -f "$LOGS/$VM-serial.log" ] && mv "$LOGS/$VM-serial.log" "$LOGS/$VM-serial.prev.log"
	qemu-system-x86_64 -enable-kvm -cpu host -smp 2 -m 1024 -name "$VM" \
		-kernel "$BZ" \
		-append "root=/dev/vda rw console=ttyS0 nokaslr panic=1 systemd.hostname=mlink-$VM" \
		-drive "file=$W/images/$VM.qcow2,if=virtio,format=qcow2" \
		-nic "user,model=virtio-net-pci,hostfwd=tcp:127.0.0.1:$SSHPORT-:22" \
		-virtfs "local,path=$W/share,mount_tag=share,security_model=none,id=share" \
		-chardev "socket,path=$RUN/ivshmem.sock,id=ivs" \
		-device ivshmem-doorbell,chardev=ivs,vectors=4 \
		-chardev "file,id=ser0,path=$LOGS/$VM-serial.log" -serial chardev:ser0 \
		-display none -daemonize -pidfile "$PID" -no-reboot \
		-gdb "tcp:127.0.0.1:$GDBPORT"
	echo -n "$VM started (pid $(cat "$PID")), waiting for ssh"
	start=$(date +%s)
	for _ in $(seq 1 90); do
		if "${GSSH[@]}" true 2>/dev/null; then
			echo " - up in $(( $(date +%s) - start )) s"
			exit 0
		fi
		running || { echo; echo "$VM exited; see $LOGS/$VM-serial.log" >&2; exit 1; }
		echo -n .
		sleep 2
	done
	echo; echo "$VM: no ssh after 180 s; see $LOGS/$VM-serial.log" >&2
	exit 1
	;;
stop)
	running || { echo "$VM not running"; exit 0; }
	"${GSSH[@]}" poweroff 2>/dev/null || true
	for _ in $(seq 1 30); do running || break; sleep 1; done
	if running; then
		kill "$(cat "$PID")"
		echo "$VM did not power off in 30 s, killed"
	else
		echo "$VM stopped"
	fi
	rm -f "$PID"
	;;
kill)
	running && kill -9 "$(cat "$PID")" && echo "$VM killed" || echo "$VM not running"
	rm -f "$PID"
	;;
status)
	if running; then echo "$VM running (pid $(cat "$PID"))"; else echo "$VM not running"; exit 1; fi
	;;
*)
	echo "usage: vm.sh start|stop|kill|status ap|modem" >&2
	exit 2
	;;
esac
