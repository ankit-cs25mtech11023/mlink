# Rebuilding the mlink environment from zero

Everything below was actually run on 2026-10-27; versions are the ones used. Two machines:
a **Windows PC** (editing, git, Claude Code) and a **Linux laptop with KVM** (builds, VMs).
Any x86-64 Linux host with KVM works; Kali Rolling 2025.4 (host kernel 6.16) was used.

## 0. Requirements
- Host: x86-64 with VT-x/AMD-V, `/dev/kvm` accessible (user in group `kvm`), ~15 GB free
  disk, ≥ 6 GB RAM, QEMU ≥ 10 (`qemu-system-x86_64`), git, gcc, make, tmux, ccache.
- Windows side: Git Bash (ssh, tar, base64), Python 3 (later: matplotlib for plots).
- **Thermals:** on a thin laptop disable turbo for long jobs (see §4) — the i5-8250U reaches
  its 98 °C critical trip with turbo on.

## 1. Repository and mirror
```bash
git clone git@github.com:ankit-cs25mtech11023/mlink.git && cd mlink
bash scripts/win/sync.sh          # tar over ssh -> ~/D/mlink on the host (host alias "kali")
```
`sync.sh` only ever wipes `~/D/mlink`, and only if it contains `.mlink-mirror`.
SSH aliases used (`~/.ssh/config` on Windows): `kali` (the host), `mlink-ap` / `mlink-modem`
(`127.0.0.1:2221/2222`, `User root`, `ProxyJump kali`).

## 2. One-time root setup (run by a human)
```bash
ssh -t kali 'sudo bash ~/D/mlink/scripts/host/setup-root.sh'
```
Installs build deps (flex bison bc libelf-dev libssl-dev dwarves libglib2.0-dev
libpixman-1-dev zlib1g-dev python3-venv), rootfs tools (mmdebstrap uidmap e2fsprogs),
analysis tools (sparse iperf3 trace-cmd bpftrace linux-cpupower), adds a sub-UID range for
the user, installs `/usr/local/sbin/mlink-bench-prep` with a NOPASSWD sudoers rule for exactly
`on|off|status|drop-caches`, and creates `~/D/mlink-work/{src,build,images,share,logs,run,results,bin}`.
Later added: `herdtools7` (Kali package 7.58) for the litmus tests.

## 3. Sources
```bash
ssh kali 'bash ~/D/mlink/scripts/host/fetch-sources.sh'
```
Shallow clone of `linux-stable` tag **v6.18.59**; QEMU **10.2.2** tarball, GPG-verified
(key `CEACC9E15534EBABB82D3FA03353C9CEF108B584`, private keyring in `mlink-work/gnupg`).

## 4. ivshmem-server
```bash
ssh kali 'bash ~/D/mlink/scripts/host/build-ivshmem-server.sh'   # ~5 min
ssh kali 'bash ~/D/mlink/scripts/host/ivshmem.sh start'           # /dev/shm/mlink-shm + run/ivshmem.sock
```

## 5. Guest kernel (debug flavour)
```bash
ssh kali 'sudo -n /usr/local/sbin/mlink-bench-prep on'   # turbo off: thermals
ssh kali "tmux new -d -s kbuild 'cd ~/D/mlink-work && bash ~/D/mlink/scripts/host/thermal-run.sh \
    bash ~/D/mlink/scripts/host/build-kernel.sh debug > logs/build-debug.log 2>&1'"
ssh kali 'sudo -n /usr/local/sbin/mlink-bench-prep off'  # afterwards
```
`build-kernel.sh` = `x86_64_defconfig` + `kvm_guest.config` + `configs/mlink-base.config` +
`configs/debug.config`, then verifies all 81 requested options landed. First build took
~25 min of compile time at `-j6` with turbo off (~70 °C). `perf` flavour: same with `perf`.

## 6. Guest root filesystem (no root needed)
```bash
# public keys allowed to log in as root in the guests:
cat ~/.ssh/id_ed25519.pub | ssh kali 'cat >> ~/D/mlink-work/images/authorized_keys'
ssh kali 'cat ~/.ssh/id_ed25519.pub >> ~/D/mlink-work/images/authorized_keys'
ssh kali 'bash ~/D/mlink/scripts/host/mkrootfs.sh'       # ~2 min
```
Debian trixie minbase via `mmdebstrap --mode=unshare`, `mkfs.ext4 -d` (no loop mount),
qcow2 overlays `ap.qcow2`, `modem.qcow2` on top of `base.img`.

## 7. Boot the guests
```bash
ssh kali 'S=~/D/mlink/scripts/host; bash $S/ivshmem.sh start; bash $S/vm.sh start ap; bash $S/vm.sh start modem'
ssh mlink-ap uname -r      # 6.18.59-mlink-debug, ssh up in ~16-22 s
```
Serial consoles: `~/D/mlink-work/logs/{ap,modem}-serial.log` (read these first after a hang).

## 8. Build, load, test
```bash
bash scripts/win/rbuild.sh       # sync + build mlink.ko against build/debug + stage to /mnt/share
bash scripts/win/test.sh         # full smoke test -> TEST: PASS
ssh mlink-ap 'bash /mnt/share/mlink-up.sh ap'; ssh mlink-modem 'bash /mnt/share/mlink-up.sh modem'
ssh mlink-ap 'ping -c 3 10.77.0.2'
```

## 9. Teardown
```bash
ssh kali 'S=~/D/mlink/scripts/host; bash $S/vm.sh stop modem; bash $S/vm.sh stop ap; bash $S/ivshmem.sh stop'
```
