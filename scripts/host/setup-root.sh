#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# One-time root setup of the Kali laptop (P0.4). The USER runs this in their own
# terminal; Claude never runs sudo:
#
#   ssh -t ankit-kali@100.83.145.82 'sudo bash ~/D/mlink/scripts/host/setup-root.sh'
#
# Idempotent: safe to re-run. Touches only: apt packages, /etc/subuid+subgid
# (only if the user has no range), /usr/local/sbin/mlink-bench-prep,
# /etc/sudoers.d/mlink, and ~/D/mlink-work/.
set -euo pipefail

[ "$(id -u)" -eq 0 ] || { echo "run with sudo" >&2; exit 1; }
U=${SUDO_USER:-}
[ -n "$U" ] && [ "$U" != root ] || { echo "run via sudo from your user account" >&2; exit 1; }
H=$(getent passwd "$U" | cut -d: -f6)

declare -a OK=() SKIPPED=()

# --- 1. packages -----------------------------------------------------------
PKGS=(
	# kernel build
	flex bison bc libelf-dev libssl-dev dwarves cpio rsync
	# ivshmem-server (from QEMU source)
	libglib2.0-dev libpixman-1-dev zlib1g-dev python3-venv
	# rootfs (mmdebstrap unshare mode needs newuidmap)
	mmdebstrap debootstrap debian-archive-keyring e2fsprogs uidmap
	# analysis / measurement
	sparse iperf3 trace-cmd bpftrace linux-cpupower
	# misc
	curl xz-utils gnupg git
)
export DEBIAN_FRONTEND=noninteractive
APT=(apt-get -y -o DPkg::Lock::Timeout=300)
"${APT[@]}" update
# Install one at a time so a package missing from Kali is skipped, not fatal.
for p in "${PKGS[@]}"; do
	if "${APT[@]}" install --no-install-recommends "$p" >/dev/null 2>&1; then
		OK+=("$p")
	else
		SKIPPED+=("$p")
	fi
done

# --- 2. subordinate ids for mmdebstrap --mode=unshare ------------------------
for f in /etc/subuid /etc/subgid; do
	grep -q "^$U:" "$f" 2>/dev/null && continue
	if [ "$f" = /etc/subuid ]; then
		usermod --add-subuids 100000-165535 "$U"
	else
		usermod --add-subgids 100000-165535 "$U"
	fi
	echo "added $U range to $f"
done

# --- 3. mlink-bench-prep (the only thing allowed via passwordless sudo) ------
install -d -m 0755 /usr/local/sbin
cat > /usr/local/sbin/mlink-bench-prep.new <<'EOF'
#!/bin/bash
# Benchmark hygiene for mlink. Root-owned; callable via NOPASSWD sudo with
# exactly one of: on | off | status | drop-caches
set -euo pipefail
STATE=/run/mlink-bench-prep.state
NT=/sys/devices/system/cpu/intel_pstate/no_turbo

status() {
	echo "governors: $(cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor | sort | uniq -c | tr -s ' ' | tr '\n' ';')"
	[ -w "$NT" ] && echo "no_turbo: $(cat "$NT")"
	[ -f "$STATE" ] && echo "prep: ON (state saved)" || echo "prep: off"
}

case "${1:-}" in
on)
	if [ ! -f "$STATE" ]; then
		{
			for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
				echo "$g $(cat "$g")"
			done
			[ -w "$NT" ] && echo "$NT $(cat "$NT")"
		} > "$STATE"
	fi
	for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
		echo performance > "$g"
	done
	# Turbo off: lower peak, but far less run-to-run variance on a hot laptop.
	[ -w "$NT" ] && echo 1 > "$NT"
	status
	;;
off)
	[ -f "$STATE" ] || { echo "not prepped"; exit 0; }
	while read -r f v; do
		case "$f" in
		/sys/devices/system/cpu/*) echo "$v" > "$f" ;;
		esac
	done < "$STATE"
	rm -f "$STATE"
	status
	;;
status)
	status
	;;
drop-caches)
	sync
	echo 3 > /proc/sys/vm/drop_caches
	echo "caches dropped"
	;;
*)
	echo "usage: mlink-bench-prep on|off|status|drop-caches" >&2
	exit 2
	;;
esac
EOF
chown root:root /usr/local/sbin/mlink-bench-prep.new
chmod 0755 /usr/local/sbin/mlink-bench-prep.new
mv /usr/local/sbin/mlink-bench-prep.new /usr/local/sbin/mlink-bench-prep

# --- 4. sudoers: NOPASSWD for that one script and its four arguments only ----
B=/usr/local/sbin/mlink-bench-prep
TMP=$(mktemp)
cat > "$TMP" <<EOF
# mlink: passwordless benchmark prep only (installed by mlink setup-root.sh)
$U ALL=(root) NOPASSWD: $B on, $B off, $B status, $B drop-caches
EOF
# A broken sudoers file can lock sudo out entirely, so validate before installing.
if visudo -cf "$TMP" >/dev/null; then
	install -m 0440 -o root -g root "$TMP" /etc/sudoers.d/mlink
	SUDOERS=installed
else
	SUDOERS="FAILED validation (not installed)"
fi
rm -f "$TMP"

# --- 5. work directories, owned by the user ----------------------------------
# Create the parent explicitly: install -d would otherwise make it root-owned.
install -d -o "$U" -g "$U" -m 0755 "$H/D/mlink-work"
for d in src build images share logs run results bin; do
	install -d -o "$U" -g "$U" -m 0755 "$H/D/mlink-work/$d"
done

# --- summary -----------------------------------------------------------------
echo
echo "=== mlink setup-root summary ==="
echo "installed/present: ${OK[*]}"
echo "skipped (not available): ${SKIPPED[*]:-none}"
echo "subuid: $(grep "^$U:" /etc/subuid || echo none)   subgid: $(grep "^$U:" /etc/subgid || echo none)"
echo "bench-prep: $(ls -l $B | awk '{print $1, $3}')"
echo "sudoers.d/mlink: $SUDOERS"
echo "work dirs: $(ls "$H/D/mlink-work" | tr '\n' ' ')"
echo "unprivileged userns: $(sysctl -n kernel.unprivileged_userns_clone 2>/dev/null || echo n/a)"
echo "=== done ==="
