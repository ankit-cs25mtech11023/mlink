#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# P10 (D22): reproduce and test the QEMU contrib/ivshmem-client stdin-EOF busy
# loop. Builds and runs only the GPG-verified QEMU 10.2.2 tree (P0.5); its
# contrib/ivshmem-client/main.c is byte-identical to QEMU master's, so the
# result carries over. The upstream patch is made in src/qemu-master with git.
#   build   rebuild the client in the existing 10.2.2 build dir (P0.6)
#   repro   run it with stdin at EOF for 3 s: bytes printed, exit status, CPU use
#   patch   apply src/qemu-master's top commit to the 10.2.2 tree (main.c.orig kept)
#   unpatch restore the original main.c
set -euo pipefail
W="$HOME/D/mlink-work"
SRC="$W/src/qemu-10.2.2"
B="$SRC/build"
CLIENT="$B/contrib/ivshmem-client/ivshmem-client"
F="$SRC/contrib/ivshmem-client/main.c"

case "${1:-}" in
build)
	ninja -C "$B" contrib/ivshmem-client/ivshmem-client | tail -1
	;;
repro)
	if [ -f "$F.orig" ]; then echo "client: 10.2.2 + patch"; else echo "client: 10.2.2 unmodified"; fi
	set +e
	TIMEFORMAT="elapsed=%Rs user=%Us sys=%Ss"
	time (timeout 3 "$CLIENT" -S "$W/run/ivshmem.sock" </dev/null \
		>"$W/logs/p10-client.out" 2>"$W/logs/p10-client.err"; echo "exit=$? (124 = still running when timeout killed it)")
	set -e
	echo "stdout bytes=$(wc -c <"$W/logs/p10-client.out") prompts=$(grep -o 'cmd> ' "$W/logs/p10-client.out" | wc -l)"
	echo "stderr: $(tail -1 "$W/logs/p10-client.err")"
	;;
functest)
	# 1. piped commands still run; 2. a client whose stdin hit EOF still
	#    receives notifications (sent by a second client).
	S="$W/run/ivshmem.sock" A="$W/logs/p10-func-a.out"
	# stdbuf: stdout is a file, and timeout's SIGTERM would discard a full buffer
	timeout 8 stdbuf -oL "$CLIENT" -S "$S" </dev/null >"$A" 2>&1 &
	sleep 1
	# B1: piped "dump" (after the server's peer list has arrived). A is the
	# newest peer other than B1 itself.
	(sleep 1; echo dump; sleep 1) | timeout 3 stdbuf -oL "$CLIENT" -S "$S" \
		>"$W/logs/p10-func-b.out" 2>&1 || true
	own=$(sed -n 's/.*our_id = \([0-9]*\).*/\1/p' "$W/logs/p10-func-b.out" | head -1)
	id=$(sed -n 's/.*peer_id = \([0-9]*\).*/\1/p' "$W/logs/p10-func-b.out" | grep -vx "$own" | sort -n | tail -1)
	echo "client B1 (piped dump): our_id $own, peers listed: $(grep -c 'peer_id =' "$W/logs/p10-func-b.out") -> client A (stdin at EOF) is peer $id"
	# B2: ring A on vectors 0 and 3
	(sleep 1; echo "int $id 0"; echo "int $id 3"; sleep 1) | timeout 3 stdbuf -oL "$CLIENT" -S "$S" \
		>>"$W/logs/p10-func-b.out" 2>&1 || true
	wait || true
	echo "client A notifications:"; grep 'receive notification' "$A" || echo "  NONE"
	;;
patch)
	[ -f "$F.orig" ] || cp "$F" "$F.orig"
	cp "$F.orig" "$F"
	git -C "$W/src/qemu-master" format-patch -1 --stdout | patch -p1 -d "$SRC"
	;;
unpatch)
	[ -f "$F.orig" ] && mv "$F.orig" "$F"
	;;
*)
	echo "usage: $0 build|repro|patch|unpatch" >&2
	exit 2
	;;
esac
