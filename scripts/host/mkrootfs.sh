#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# P0.8: build the Debian trixie guest rootfs without root (mmdebstrap unshare
# mode), turn it into a raw ext4 image, and create the per-VM qcow2 overlays.
#
# Usage: bash ~/D/mlink/scripts/host/mkrootfs.sh [--force]
#   Needs ~/D/mlink-work/images/authorized_keys (public keys allowed as root).
set -euo pipefail

W="$HOME/D/mlink-work"
IMG="$W/images"
TAR="$IMG/base.tar"
RAW="$IMG/base.img"
KEYS="$IMG/authorized_keys"
SIZE=${SIZE:-3G}

[ -s "$KEYS" ] || { echo "missing $KEYS" >&2; exit 1; }
if [ -e "$RAW" ] && [ "${1:-}" != --force ]; then
	echo "$RAW exists (use --force to rebuild; this also resets both overlays)"
	exit 0
fi

PKGS=systemd-sysv,udev,dbus,openssh-server,iproute2,iputils-ping,iperf3,ethtool
PKGS+=,tcpdump,trace-cmd,bpftrace,strace,kmod,procps,python3,less,ca-certificates
PKGS+=,netcat-openbsd,pciutils,vim-tiny,psmisc

rm -f "$TAR"
# Keep mmdebstrap's scratch dir inside the project area (file-safety rule).
export TMPDIR="$W/tmp"
mkdir -p "$TMPDIR"
# Hooks run on the host with the rootfs at "$1"; in unshare mode we are root
# inside a user namespace, so chroot and chown work without real root.
mmdebstrap --mode=unshare --variant=minbase --include="$PKGS" \
	--customize-hook='mkdir -p -m 700 "$1/root/.ssh"' \
	--customize-hook="copy-in $KEYS /root/.ssh" \
	--customize-hook='chown -R 0:0 "$1/root/.ssh"; chmod 600 "$1/root/.ssh/authorized_keys"' \
	--customize-hook='echo "PermitRootLogin prohibit-password" > "$1/etc/ssh/sshd_config.d/mlink.conf"' \
	--customize-hook='chroot "$1" ssh-keygen -A' \
	--customize-hook='chroot "$1" passwd -l root' \
	--customize-hook='mkdir -p "$1/mnt/share"' \
	--customize-hook='printf "%s\n" "/dev/vda / ext4 rw,relatime 0 1" "share /mnt/share 9p trans=virtio,version=9p2000.L,msize=262144,nofail 0 0" > "$1/etc/fstab"' \
	--customize-hook='printf "%s\n" "[Match]" "Driver=virtio_net" "" "[Network]" "DHCP=ipv4" > "$1/etc/systemd/network/10-mgmt.network"' \
	--customize-hook='chroot "$1" systemctl enable systemd-networkd' \
	--customize-hook='echo "nameserver 10.0.2.3" > "$1/etc/resolv.conf"' \
	--customize-hook='echo mlink > "$1/etc/hostname"' \
	trixie "$TAR" http://deb.debian.org/debian

# e2fsprogs >= 1.47.1 can populate an image straight from a tarball, keeping
# ownership and modes, so no loop mount (and no root) is needed.
rm -f "$RAW"
mkfs.ext4 -q -L mlink-root -d "$TAR" "$RAW" "$SIZE"
rm -f "$TAR"

for vm in ap modem; do
	rm -f "$IMG/$vm.qcow2"
	qemu-img create -q -f qcow2 -b "$RAW" -F raw "$IMG/$vm.qcow2"
done
ls -lh "$RAW" "$IMG"/*.qcow2
