# mlink — Linux Network Driver for a Shared-Memory Modem Link

> Blueprint for the whole project. If a decision changes anything here, update this file
> FIRST, then `checkbox.md`, then implement (see `CLAUDE.md` → Protocols).
> Task IDs (P3.4 etc.) are shared with `checkbox.md`.

---

## 1. Overview

### What we are building
A small version of the **Qualcomm modem ↔ application-processor data path**, built entirely in
software on two QEMU/KVM virtual machines that share memory:

```
 Windows PC (Claude Code + source of truth: D:\Projects\Embedded\mlink, git → GitHub)
     │  ssh (Tailscale 100.83.145.82)
     ▼
 Kali laptop "kali" (i5-8250U 4C/8T, 7.6 GB RAM, KVM)                     host = NEVER loads mlink.ko
 ┌──────────────────────────────────────────────────────────────────────────────────────────┐
 │  VM "ap" (Linux, role=ap)                         VM "modem" (Linux, role=modem)          │
 │   iperf3 / ping / udp_pingpong                     iperf3 / ping / udp_pingpong           │
 │   TCP/IP stack                                     TCP/IP stack                           │
 │   mlink0 (raw-IP p2p net_device, NAPI) ◄────────►  mlink0                                 │
 │   [P7: /dev/mlink_ctrl control plane]              [P7: mlinkd control daemon]            │
 │   mlink.ko ── PCI 1af4:1110 ivshmem-doorbell ──┐ ┌── mlink.ko                             │
 │                                                ▼ ▼                                        │
 │                 ivshmem-server: POSIX shm (BAR2) + eventfd doorbells → MSI-X              │
 └──────────────────────────────────────────────────────────────────────────────────────────┘
```

| Real Snapdragon concept | mlink equivalent |
|---|---|
| SMEM (AP↔subsystem shared memory) | ivshmem BAR2, laid out by `driver/mlink_shm.h` |
| IPCC / APCS mailbox interrupts | ivshmem Doorbell register → peer MSI-X vector |
| MHI / BAM-DMUX data channels, `mhi_net`, `rmnet` | TX/RX descriptor rings + raw-IP `net_device` with NAPI |
| QMI control channel | P7 control ring + `/dev/mlink_ctrl` (stretch) |
| SSR (Subsystem Restart) | Heartbeat watchdog + generation-number re-handshake |

### Goals (in priority order)
1. **MVP (resume-bullet quality):** working raw-IP net driver with NAPI, `ping`/`iperf3` across
   VMs, modem-restart recovery (SSR), fault-injection survival, and *measured* numbers.
2. **Correctness evidence:** KASAN/lockdep clean, checkpatch/sparse clean, LKMM litmus tests for
   the ring protocol, untrusted-shared-memory validation (double-fetch safe).
3. **Stretch:** QMI-like control plane, Zephyr "modem", aarch64 guest, upstream patch, real HW.
4. **Interview readiness:** the user can defend every line (study gates after each phase).

### Non-goals
- Not a production driver; not upstreamable as-is (an `ivshmem-net` exists out-of-tree in the
  Siemens Jailhouse project — we read it as prior art and say so; our differentiators are raw-IP
  modem semantics, SSR, fault injection, verification and measurements).
- No real radio / 3GPP stack.

### Resume bullet targets (fill ONLY with measured numbers from `docs/results.md`)
- After P3 (minimum): *"Wrote a Linux raw-IP network driver (PCI, MSI-X, NAPI) over a
  shared-memory link between two KVM guests; X Gbit/s TCP, Y µs p99 RTT, KASAN/lockdep clean."*
- After P6 (target): *"Built a modem-style AP↔modem data path as a Linux PCI network driver
  (MSI-X, lock-free shared-memory rings, NAPI, kick suppression) across two KVM guests: X Gbit/s
  TCP, Y µs p99 RTT; subsystem-restart recovery in Z ms with live TCP flows surviving; survived N
  shared-memory corruption injections KASAN-clean; ring ordering verified with LKMM litmus tests."*
- CV title: **Linux Network Device Driver for a Shared-Memory Modem Link** (PCI, MSI-X, NAPI,
  TCP/IP, QEMU/KVM).

---

## 2. Environment facts (verified 2026-10-26)

### Windows PC (where Claude Code runs)
- i5-13450HX, 16 GB, Windows 11, Git Bash available, `ssh/scp/tar/python` present, **no WSL**,
  **no `gh` CLI**. SSH key: `~/.ssh/id_ed25519` (key auth to laptop already works).
- git identity: `ankit-cs25mtech11023` / `cs25mtech11023@iith.ac.in`.
- Project root (source of truth): `D:\Projects\Embedded\mlink` (= `/d/Projects/Embedded/mlink`
  in Git Bash).

### Kali laptop (`ankit-kali@100.83.145.82`, via Tailscale)
- Kali Rolling 2025.4, host kernel 6.16.8, i5-8250U (4C/8T, VT-x), 7.6 GB RAM + 7.9 GB swap.
- `/dev/kvm` OK, user in `kvm` group → QEMU runs **without sudo**.
- Disk: `/` 54 GB free; `~/D` (sda1) **190 GB free** → all work goes under `~/D`.
- Present: gcc 15.2, make, git, QEMU 10.2.2 (x86_64 only), tmux, ccache, ethtool, pahole, ninja,
  pkg-config, qemu-img, python3.
- Missing (installed in P0.4): flex, bison, bc, libelf-dev, libssl-dev, libglib2.0-dev,
  python3-venv, mmdebstrap/debootstrap, debian-archive-keyring, sparse, iperf3, trace-cmd,
  bpftrace, cpupower, **ivshmem-server** (not packaged by Debian/Kali — built in P0.6).
- **sudo requires a password.** The user runs the one-time root script (P0.4) in their own
  terminal. Afterwards (D13) the user stores the password in `~/D/mlink-work/.secrets/sudo.pass`
  (0600, never in git, never read by Claude) and Claude runs bounded root steps through
  `scripts/host/msudo.sh` (`sudo -A` + askpass, every call logged). `mlink-bench-prep` keeps its
  NOPASSWD rule.
- Remote-safety already OK: sleep/suspend masked, lid switch ignored, `ssh` + `tailscaled`
  enabled at boot, no disk encryption, on AC.
- Background load seen: Firefox (+extension at ~87% CPU), `uvicorn` (~1.8 GB RSS), Docker
  containers `dev-api-1`, `dev-db-1`. **User allowed stopping them** for builds/benchmarks;
  restart Docker containers afterwards (`docker start dev-api-1 dev-db-1`), and tell the user.

### Directory layout
```
Windows  D:\Projects\Embedded\mlink\          ← git repo, edited by Claude, pushed to GitHub
Laptop   ~/D/mlink/                            ← DISPOSABLE mirror, wiped + re-extracted by sync
         ~/D/mlink-work/                       ← persistent, never in git
             src/linux/            kernel source (stable LTS, shallow clone)
             src/qemu-10.2.2/      QEMU source (only to build ivshmem-server)
             build/debug/          kernel build: KASAN + lockdep + BTF
             build/perf/           kernel build: no debug (benchmarks only)
             build/kcsan/          (optional) KCSAN build
             images/base.img       Debian trixie rootfs (raw ext4)
             images/ap.qcow2       overlay (backing = base.img)
             images/modem.qcow2    overlay
             share/                9p-shared into both VMs at /mnt/share (mlink.ko, tools)
             logs/                 serial logs ap-serial.log, modem-serial.log, build logs
             run/                  pidfiles, ivshmem socket
             results/              raw benchmark output (pulled back to Windows)
             bin/ivshmem-server
```

---

## 3. Tech stack
- **Kernel driver:** C (GNU11, kernel style), out-of-tree module, Kbuild. License GPL-2.0-only.
- **Guest kernel:** latest **LTS** from kernel.org at P0.5 time (expected 6.18.y — record the
  exact tag in Key Decisions). Two builds: `debug`, `perf`.
- **Hypervisor:** QEMU 10.2.2 + KVM, device `ivshmem-doorbell` (+ `ivshmem-server`).
- **Guest userspace:** Debian 13 "trixie" minbase via `mmdebstrap`, systemd, openssh, iproute2,
  iputils, iperf3, ethtool, tcpdump, trace-cmd, bpftrace, strace, kmod, python3.
- **Tools (userspace, C):** `udp_pingpong` (latency histogram), `shm_chaos` (corruption injector,
  runs on the laptop host against `/dev/shm/mlink-shm`), P7 `mlinkctl`/`mlinkd`.
- **Verification:** KASAN, lockdep (PROVE_LOCKING), DEBUG_ATOMIC_SLEEP, kmemleak, KCSAN (opt),
  checkpatch.pl --strict, sparse (`C=1`), `W=1`, herd7 + LKMM litmus tests.
- **Measurement:** iperf3, udp_pingpong, bpftrace/trace-cmd, custom `TRACE_EVENT`s, debugfs
  counters, `/proc/interrupts`, `mpstat`-style CPU accounting, python/matplotlib plots.
- **Scripts:** bash (Git Bash on Windows side, bash on laptop/guests). Always invoke as
  `bash script.sh` (tar from Windows does not reliably preserve +x).

---

## 4. Ways of working (summary — full protocol in CLAUDE.md)
- **Claude writes the code; the user studies it.** Every phase ends with a
  `docs/walkthrough/phaseN.md` explainer and a **study gate** (quiz + explain-back + spot-the-bug).
  A phase is complete only when its study gate is passed.
- **Edit on Windows → `scripts/win/sync.sh` → build/run on laptop → test in VMs.**
- **The driver is never loaded on the laptop host kernel.** Only inside VMs. A host panic while
  the user is off-campus = lockout until they return.
- **The user's files are off-limits.** On the laptop Claude only creates/modifies/deletes inside
  `~/D/mlink/`, `~/D/mlink-work/` and `/dev/shm/mlink-shm`. Everything else in `~/D` (codeforces,
  DL, Documents, IPACO, Leetcode, Misc, NS, decks…) and the rest of the system is never deleted,
  moved or overwritten. `sync.sh` wipes `~/D/mlink` only if it contains the marker file
  `.mlink-mirror` and the path resolves exactly to `$HOME/D/mlink` (refuses otherwise).
- **Never ask for or accept the sudo password in chat.** Root steps go through `msudo.sh`
  (askpass from the user-written password file, logged, bounded by CLAUDE.md hard rule 3).
- Every non-trivial bug that takes > 30 min goes into `docs/debugging-log.md`
  (symptom → hypothesis → evidence → root cause → fix). These become interview stories.
- **Timebox rule:** stuck > 2 h on one problem → apply the documented fallback for that step,
  log it, move on, revisit later.
- **No fabricated numbers.** Every number in README/results/resume comes from a committed raw
  result file + the exact command that produced it.

---

## 5. Repository layout (target)
```
mlink/
├── CLAUDE.md  plan.md  checkbox.md  README.md  LICENSE (GPL-2.0)  .gitattributes  .gitignore
├── driver/
│   ├── Kbuild  Makefile
│   ├── mlink.h            private structs (struct mlink_dev, ring handles, stats)
│   ├── mlink_shm.h        SHARED-MEMORY ABI: header, state blocks, ring ctrl, descriptors
│   │                      (+ static_assert on every offset/size)
│   ├── mlink_main.c       module init/exit, pci_driver probe/remove, BARs, MSI-X, params
│   ├── mlink_ring.c/.h    SPSC ring ops on shared memory (publish/consume, kick logic)
│   ├── mlink_net.c        net_device, ndo_*, TX path, NAPI RX path, ethtool_ops
│   ├── mlink_link.c       link state machine, heartbeat, watchdog, SSR re-handshake
│   ├── mlink_debugfs.c    stats, ring dumps, doorbell ping-pong, fault-injection knobs
│   ├── mlink_trace.h      TRACE_EVENT definitions
│   └── mlink_ctrl.c       (P7) misc char device control plane
├── tools/                 udp_pingpong.c  shm_chaos.c  (P7) mlinkctl.c mlinkd.c  Makefile
├── scripts/
│   ├── win/               sync.sh  rbuild.sh  pull-results.sh  test.sh  (run from Git Bash)
│   ├── host/              setup-root.sh (USER runs with sudo)  fetch-sources.sh
│   │                      build-ivshmem-server.sh  build-kernel.sh  mkrootfs.sh
│   │                      ivshmem.sh  vm.sh  bench-prep.sh  bench-restore.sh  ssr.sh
│   └── guest/             mlink-up.sh  mlink-down.sh  dmesg-gate.sh
├── configs/               kernel fragments: mlink-base.config debug.config perf.config kcsan.config
├── bench/                 run_*.sh  plot.py  results/ (curated CSV committed; raw gitignored)
├── litmus/                *.litmus (LKMM) + README with herd7 outputs
└── docs/
    ├── design.md          shm layout diagram, vectors, ring protocol, state machine, rationale
    ├── walkthrough/       phase0.md … phase6.md (study material, line-by-line rationale)
    ├── interview-qa.md    growing Q&A bank (answers in the user's words after study gates)
    ├── debugging-log.md   war stories
    ├── results.md         final numbers + plots + methodology
    └── environment.md     how to rebuild the whole environment from zero
```

---

## 6. Core design (v1 — refined in P1.1, recorded in `docs/design.md`)

### 6.1 ivshmem-doorbell device facts (verify against `docs/specs/ivshmem-spec.rst` in QEMU src)
- PCI `1af4:1110`. **BAR0** = 256-byte MMIO registers: `0x00 IntrMask`, `0x04 IntrStatus`
  (INTx only), `0x08 IVPosition` (RO: *this* VM's peer ID), `0x0C Doorbell` (WO:
  `(peer_id << 16) | vector`). **BAR1** = MSI-X table/PBA. **BAR2** = shared memory.
- Peer IDs are assigned by `ivshmem-server` and **change when a VM reconnects** → the driver must
  learn the peer's ID from shared memory (each side publishes its own `IVPosition` in its state
  block). The device itself never tells the guest that a peer died → heartbeat is required.
- Doorbells to a peer that is not connected are silently dropped.
- QEMU cmdline piece: `-chardev socket,path=$RUN/ivshmem.sock,id=ivs
  -device ivshmem-doorbell,chardev=ivs,vectors=4`.
- Server: `ivshmem-server -F -v -S $RUN/ivshmem.sock -M mlink-shm -l 4M -n 4`
  (**verify flags with `ivshmem-server -h`**; shm object appears as `/dev/shm/mlink-shm`).

### 6.2 Roles
- Same `mlink.ko` in both VMs; module param `role=ap|modem` (required, no default).
- AP is the **layout master**: on first start, if magic is absent, it writes the header (magic,
  ABI version, ring sizes, slot size). Modem waits for valid magic, validates, then proceeds.

### 6.3 Shared-memory layout (4 MiB, fixed constants for MVP; configurable is stretch)
```
0x000000  Global header (4 KiB): magic "MLNK", abi_version, layout params, layout_crc
0x001000  State block AP    (64 B-aligned fields, ONLY AP writes)
0x001040  State block MODEM (ONLY modem writes)
            { peer_id(IVPosition), state, gen, peer_gen_seen, heartbeat, mtu, features }
0x002000  Ring AP→MODEM  ctrl: prod line (AP writes) | cons line (MODEM writes) |
                         prod_flags line | cons_flags line  — each on its own 64 B cache line
          desc[256] (16 B: u32 len, u16 proto, u16 flags, u32 seq, u32 rsvd)
          slots[256] × 4 KiB  (slot i belongs to desc i)        ≈ 1 MiB
0x10xxxx  Ring MODEM→AP  same structure                         ≈ 1 MiB
          rest: reserved (P7 control ring pair goes here)
```
- **Single-writer rule:** every field has exactly one writer side. Document the writer next to
  each field in `mlink_shm.h`.
- Indices are free-running `u32`; slot = `idx & (N-1)`; N power of two; `prod - cons ≤ N`.

### 6.4 Interrupt vectors
| vector | meaning (sender → receiver) |
|---|---|
| 0 | DATA: "new descriptors in the ring you consume" |
| 1 | SPACE: "I consumed descriptors, you may resume TX" |
| 2 | STATE: "my state block changed" (graceful down/up, fast SSR) |
| 3 | TEST: P1 doorbell ping-pong / diagnostics |

### 6.5 Ring protocol & memory ordering
- BAR2 mapped with `devm_memremap(..., MEMREMAP_WB)` → normal cacheable memory; use
  **`virt_*` barriers** (`virt_store_release`, `virt_load_acquire`, `virt_mb`) because the other
  side is a different VM, and `READ_ONCE/WRITE_ONCE` for every shared field.
- **Publish (producer):** copy payload → write desc → `virt_store_release(&prod, prod+1)`.
- **Consume (consumer):** `p = virt_load_acquire(&prod)`; for each desc: **read `len` exactly
  once into a local (READ_ONCE), validate (`len ≤ slot_size`, `proto` known)**, then copy out;
  `virt_store_release(&cons, cons+1)`.
- **Shared memory is untrusted input** (the peer can be buggy or malicious): validate indices
  (`prod - cons ≤ N`), never double-fetch a length, never use a peer-provided offset as a pointer.
  Corruption → count error + force link reset, never crash.
- **Kick suppression (the classic lost-wakeup / store-buffering race):**
  consumer sets `cons_flags.want_kick=1` before sleeping, then `virt_mb()`, then re-checks
  `prod`; producer publishes `prod`, then `virt_mb()`, then reads `want_kick` → kick if set.
  Both full barriers are required (SB pattern) — proven with a litmus test in P2.5.
- Same pattern for TX space: producer sets `prod_flags.want_space=1`, `virt_mb()`, re-checks
  free space before `netif_stop_queue` is final.

### 6.6 Network device
- Raw-IP point-to-point (modem-style, like `rmnet`/`mhi_net`; **verify the `type` mhi_net uses
  in the chosen kernel**): `dev->type = ARPHRD_RAWIP` (fallback `ARPHRD_NONE`),
  `IFF_POINTOPOINT | IFF_NOARP`, `hard_header_len = 0`, `addr_len = 0`, MTU 1500,
  `max_mtu = slot_size - 0`. `skb->protocol` set on RX from the IP version nibble.
- Addresses: `ip addr add 10.77.0.1 peer 10.77.0.2 dev mlink0` (ap), mirrored on modem.
  Management NIC (virtio-net, user-mode, `10.0.2.x`) is separate — **always confirm benchmark
  traffic uses mlink0** (`ip route get 10.77.0.2`, mlink0 counters move).
- TX: `ndo_start_xmit` (serialized by the core's TX lock → single producer), copy skb into slot
  (`skb_copy_bits`), publish, kick-if-wanted, `dev_consume_skb_any`. Stop the queue when free
  slots < 1 (with the re-check race above). Return `NETDEV_TX_OK` (never BUSY in steady state).
- RX: MSI-X vector 0 hard-IRQ → disable further kicks (`want_kick=0`) → `napi_schedule_irqoff`.
  NAPI poll (softirq context, cannot sleep): consume ≤ budget, `napi_alloc_skb`,
  `skb_put_data`, `napi_gro_receive`; publish `cons`; send SPACE kick if peer wants it;
  if `done < budget`: `napi_complete_done`, set `want_kick=1`, `virt_mb()`, re-check ring →
  `napi_schedule` again if non-empty.
- Stats: `ndo_get_stats64` with `u64_stats_sync`; driver counters via `ethtool -S`.

### 6.7 Link state machine & SSR
States: `DOWN → WAIT_PEER → UP → PEER_LOST → RESETTING → WAIT_PEER …`
- On probe: bump own `gen`, publish own `peer_id`, `state=WAIT_PEER`, start heartbeat
  `delayed_work` (100 ms; param `hb_ms`).
- Link UP when: peer magic/ABI valid, peer `state ≥ WAIT_PEER`, `peer.peer_gen_seen == my.gen`
  and `my.peer_gen_seen == peer.gen` (two-way acknowledged generations). Then
  `netif_carrier_on`, `napi_enable`, wake queue.
- Peer lost: peer heartbeat unchanged for `hb_timeout_ms` (default 500) **or** peer `gen`
  changed **or** STATE kick with `state=DOWN`. → `netif_carrier_off`, `netif_stop_queue`,
  `napi_disable` (waits for in-flight poll), drop un-consumed TX, reset the indices *this side
  owns* (own TX `prod`, own RX `cons`, own flags), `state=WAIT_PEER`, re-handshake.
- Graceful `rmmod`: set `state=DOWN`, STATE kick, then teardown (fast path for SSR timing).
- Measured: detection latency, recovery latency, packets lost, whether TCP flows survive.

### 6.8 Kernel configs (fragments in `configs/`)
- **base:** `x86_64_defconfig` + `kvm_guest.config` + PCI_MSI, MODULES, MODULE_UNLOAD,
  NET, INET, IPV6, VIRTIO_NET, VIRTIO_BLK, NET_9P, NET_9P_VIRTIO, 9P_FS, 9P_FS_POSIX_ACL,
  DEBUG_FS, FTRACE, FUNCTION_TRACER, DYNAMIC_FTRACE, KPROBES, UPROBES, BPF_SYSCALL, BPF_JIT,
  BPF_EVENTS, DEBUG_INFO_DWARF5, DEBUG_INFO_BTF, IKCONFIG_PROC, UNWINDER_ORC (x86 default;
  FRAME_POINTER only exists with the frame-pointer unwinder — dropped 2026-10-27), GDB_SCRIPTS,
  DYNAMIC_DEBUG, MAGIC_SYSRQ. Disable to save build time on the old laptop: DRM, SOUND,
  WLAN, WIRELESS, USB_SUPPORT, MEDIA_SUPPORT, INPUT_JOYSTICK/TABLET/TOUCHSCREEN, HID (keep
  minimal), NFS client/server, most filesystems except ext4/9p/tmpfs.
- **debug:** + KASAN (generic, inline), PROVE_LOCKING, DEBUG_ATOMIC_SLEEP, DEBUG_KMEMLEAK,
  DEBUG_OBJECTS, DEBUG_LIST, SLUB_DEBUG, LOCKUP_DETECTOR, DETECT_HUNG_TASK, PANIC_ON_OOPS=n.
- **perf:** base only (no KASAN/lockdep — debug options distort numbers by 2–10×).
- **kcsan (optional):** base + KCSAN (incompatible with KASAN → separate build). Note: KCSAN
  only sees races *inside one kernel*, not across VMs — cross-VM ordering is covered by LKMM.

### 6.9 VM launch (scripts/host/vm.sh)
```
qemu-system-x86_64 -enable-kvm -cpu host -smp 2 -m 1024 -name $VM \
  -kernel $BUILD/arch/x86/boot/bzImage \
  -append "root=/dev/vda rw console=ttyS0 nokaslr panic=1 systemd.hostname=mlink-$VM" \
  -drive file=$IMG/$VM.qcow2,if=virtio,format=qcow2 \
  -nic user,model=virtio-net-pci,hostfwd=tcp:127.0.0.1:$SSHPORT-:22 \
  -virtfs local,path=$WORK/share,mount_tag=share,security_model=none,id=share \
  -chardev socket,path=$RUN/ivshmem.sock,id=ivs -device ivshmem-doorbell,chardev=ivs,vectors=4 \
  -chardev file,id=ser0,path=$LOGS/$VM-serial.log -serial chardev:ser0 \
  -display none -daemonize -pidfile $RUN/$VM.pid -no-reboot \
  -gdb tcp:127.0.0.1:$GDBPORT
```
- Ports: ap → ssh 2221, gdb 1234; modem → ssh 2222, gdb 1235.
- Serial log to file means **a guest panic is always captured** even when its ssh is dead.
- `-no-reboot` + `panic=1` → a panicking guest exits instead of hanging.
- Start order: `ivshmem.sh start` → `vm.sh start ap` → `vm.sh start modem`. Restarting
  `ivshmem-server` requires restarting both VMs.
- Windows `~/.ssh/config` aliases (P0.3): `kali`; `mlink-ap` / `mlink-modem`
  (`HostName 127.0.0.1`, `Port 2221/2222`, `User root`, `ProxyJump kali`,
  `StrictHostKeyChecking no`, `UserKnownHostsFile /dev/null` because images get rebuilt).

---

## 7. Project phases

Effort estimates assume ~5–7 focused hours/day and the old laptop. "Gate" = exit criteria.
Every phase ends with: walkthrough doc → study gate → commit (+ push once remote confirmed).

---

### Phase 0 — Environment & bootstrap (est. 1.5–2 days, much of it waiting on builds)

- **P0.1 Repo skeleton on Windows.** In `D:\Projects\Embedded\mlink`: `git init -b main`,
  `git config core.autocrlf false`, `.gitattributes` (`* text=auto eol=lf`, `*.png binary`),
  `.gitignore` (`*.o *.ko *.mod* *.cmd .tmp_versions Module.symvers modules.order
  bench/results/raw/ __pycache__/`), GPL-2.0 `LICENSE`, stub `README.md`, the directory tree
  from §5 with `.gitkeep` where empty, `docs/design.md` stub. First commit.
- **P0.2 GitHub remote.** `gh` is not installed. Either the user creates an empty **public**
  repo `mlink` on github.com (no README/license), or installs `gh` (`winget install
  GitHub.cli`, then user runs `gh auth login` themselves). Check `ssh -T git@github.com`
  works with the Windows key. `git remote add origin git@github.com:<user>/mlink.git`.
  **First push only after the user confirms.**
- **P0.3 SSH aliases (Windows).** Show the user the proposed `~/.ssh/config` block (§6.9) and
  add it after they approve (it's their personal file). Verify `ssh kali hostname`.
- **P0.4 One-time root setup — USER RUNS IT.** Claude writes `scripts/host/setup-root.sh`;
  Claude syncs it; the user runs in **their own terminal**:
  `ssh -t ankit-kali@100.83.145.82 'sudo bash ~/D/mlink/scripts/host/setup-root.sh'`.
  It: `apt-get install -y` flex bison bc libelf-dev libssl-dev libglib2.0-dev python3-venv
  mmdebstrap debootstrap debian-archive-keyring e2fsprogs sparse iperf3 trace-cmd bpftrace
  linux-cpupower dwarves cpio rsync; installs `/usr/local/sbin/mlink-bench-prep`
  (root-owned: set governor performance/restore, disable turbo toggle, drop caches) and
  `/etc/sudoers.d/mlink` with **NOPASSWD only for that one script** (validate with
  `visudo -cf`); creates `~/D/mlink-work/{src,build,images,share,logs,run,results,bin}` owned
  by the user. Script must be idempotent and print a summary.
  - Stuck: apt lock held → wait/`ps aux | grep apt`; Kali repo errors → `apt-get update` first;
    a package missing in Kali → skip it, note it, use fallback (e.g. bpftrace → trace-cmd).
- **P0.5 Fetch sources** (`scripts/host/fetch-sources.sh`, no sudo): kernel LTS shallow clone
  `git clone --depth 1 --branch v6.18.<latest> https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git`
  (pick latest LTS tag at that time; record it in Key Decisions); QEMU
  `https://download.qemu.org/qemu-10.2.2.tar.xz` (match host QEMU version) + verify signature
  or at least sha256 from the site.
  - Stuck: git.kernel.org slow → use the GitHub mirror `gregkh/linux` or a tarball from
    cdn.kernel.org.
- **P0.6 Build `ivshmem-server`** (`build-ivshmem-server.sh`): in qemu src
  `./configure --target-list=x86_64-softmmu --disable-docs --disable-werror` then
  `ninja -C build contrib/ivshmem-server/ivshmem-server`, copy to `~/D/mlink-work/bin/`.
  Test: start it, confirm `/dev/shm/mlink-shm` appears and the socket exists.
  - Stuck A: configure missing deps → read the error, it names the package (usually glib/venv).
  - Stuck B: target not found → read `contrib/ivshmem-server/meson.build` for the gating
    condition (tools/ivshmem enabled); try `--enable-tools`.
  - **Fallback (2 h timebox):** write `tools/ivshmem-lite-server.c` (~200 lines) from
    `docs/specs/ivshmem-spec.rst`: unix socket; per client send int64 protocol version (0),
    int64 client ID, then `-1` + shm fd (SCM_RIGHTS), then for every existing peer and itself:
    peer ID + one eventfd per vector; on disconnect broadcast the bare peer ID. (Good
    interview material on fd passing/eventfd.)
- **P0.7 Kernel configs + debug build.** Write `configs/*.config`; `build-kernel.sh debug`:
  `make O=$BUILD/debug x86_64_defconfig kvm_guest.config`, merge fragments with
  `scripts/kconfig/merge_config.sh`, `make olddefconfig`, then
  `nice -n 10 make -j6 CC="ccache gcc" bzImage modules` in a **tmux session** with log to
  `logs/build-debug.log`. Expect 30–60 min first build (KASAN + BTF). Stop Firefox/uvicorn/Docker
  first; restart Docker after. Verify `.config` actually contains the requested options
  (`grep` each — merge_config silently drops options whose dependencies are missing).
  - Stuck: BTF/pahole errors → check pahole version vs kernel requirement; worst case disable
    DEBUG_INFO_BTF in debug build (bpftrace then needs kernel headers; use trace-cmd instead).
  - Stuck: out of memory during link → `-j4`, ensure swap, stop background apps.
  - Stuck: gcc 15 warnings-as-errors in kernel → build with `WERROR=0`/`CONFIG_WERROR=n`.
- **P0.8 Rootfs** (`mkrootfs.sh`, needs the user's sudo run OR mmdebstrap unshare mode —
  try unshare first, no sudo): `mmdebstrap --mode=unshare --variant=minbase
  --include=systemd-sysv,udev,dbus,openssh-server,iproute2,iputils-ping,iperf3,ethtool,
  tcpdump,trace-cmd,bpftrace,strace,kmod,procps,python3,less,ca-certificates,netcat-openbsd
  trixie base.tar http://deb.debian.org/debian`; customize via `--customize-hook`:
  root ssh `authorized_keys` (Windows key + laptop key), `PermitRootLogin prohibit-password`,
  fstab `/dev/vda / ext4` + `share /mnt/share 9p trans=virtio,version=9p2000.L,nofail 0 0`,
  systemd-networkd DHCP for the virtio NIC only (`[Match] Driver=virtio_net`),
  empty root password disabled, `/etc/modules-load.d` nothing. Then
  `mkfs.ext4 -d base.tar base.img 3G` (e2fsprogs ≥ 1.47.1 accepts a tarball; otherwise do the
  loop-mount in a sudo script the user runs). Overlays:
  `qemu-img create -f qcow2 -b base.img -F raw ap.qcow2` (same for modem).
  - Stuck: unshare mode fails (user namespaces disabled) → user runs a sudo variant once.
  - Stuck: guest ssh refuses → check serial log, `sshd -t`, host keys generated (customize
    hook must run `ssh-keygen -A`).
- **P0.9 Boot both VMs.** `ivshmem.sh start`, `vm.sh start ap`, `vm.sh start modem`; from
  Windows: `ssh mlink-ap uname -a`, `ssh mlink-modem uname -a`; `lspci -nn | grep 1af4:1110`
  shows the ivshmem device in both; `/mnt/share` mounted.
  - Stuck: VM won't boot → read `logs/<vm>-serial.log`; "VFS: unable to mount root" →
    virtio-blk not built-in (must be `=y`, not `=m`, since no initramfs).
  - Stuck: no ivshmem device → server not running before VM start / socket path mismatch.
  - Stuck: `/dev/kvm` permission → user in `kvm` group (already true); fallback TCG
    (`-accel tcg`) only for functional work — numbers invalid.
- **P0.10 Hello-module loop.** Minimal `driver/` with module init/exit printing the ivshmem
  BAR sizes (no binding yet). `scripts/win/rbuild.sh`: sync → `make -C $BUILD/debug
  M=~/D/mlink/driver` → copy `.ko` to `share/`. In guest: `insmod /mnt/share/mlink.ko`,
  `dmesg | tail`, `rmmod`. Also `scripts/guest/dmesg-gate.sh` (fails if dmesg contains
  `BUG:|WARNING:|KASAN|possible circular|inconsistent lock|INFO: task .* blocked|
  scheduling while atomic|Oops|UBSAN`). And `scripts/win/test.sh` skeleton.
  - Stuck: "invalid module format" → module built against a different tree than the booted
    kernel; vermagic mismatch → rebuild against `$BUILD/debug`.
- **P0.11 Docs:** `docs/environment.md` (rebuild-from-zero steps, real commands used),
  `docs/walkthrough/phase0.md` (QEMU flags, KVM vs TCG, 9p, why out-of-tree modules, Kbuild,
  vermagic, why serial-to-file).
- **P0.12 Study gate 0** (see §8).
- **Gate P0:** both VMs boot to ssh in < 60 s, ivshmem visible in both, hello module loads/
  unloads cleanly, dmesg gate passes, environment.md lets a stranger rebuild the setup.

---

### Phase 1 — PCI driver & doorbell interrupts (est. 2–3 days)

- **P1.1 Design doc v1** (`docs/design.md`): everything in §6 as diagrams + rationale + the
  single-writer table. User reviews before code.
- **P1.2 `mlink_shm.h` v1:** header + state blocks only, `__packed`-free natural alignment,
  explicit `____cacheline_aligned` padding, `static_assert(offsetof(...))` for every field,
  `__le32`-style comment noting both sides are x86 little-endian (endianness note for aarch64
  stretch).
- **P1.3 PCI probe/remove** (`mlink_main.c`): `pci_device_id` table `{PCI_DEVICE(0x1af4,
  0x1110)}`, `pcim_enable_device`, map BAR0 with the **current** managed API of the chosen kernel
  (`pcim_iomap_region` — older `pcim_iomap_regions` is deprecated; check
  `include/linux/pci.h`), BAR2 via `devm_memremap(WB)`, read `IVPosition` (`ioread32`),
  validate BAR2 size ≥ layout, module param `role` validated in `mlink_init`.
  - **Important:** the host kernel's own ivshmem? None is bound (no in-tree ivshmem driver),
    but check `lspci -k` in guest shows `mlink` as driver in use.
- **P1.4 MSI-X:** `pci_alloc_irq_vectors(pdev, 4, 4, PCI_IRQ_MSIX)`,
  `devm_request_irq` per vector with per-vector handlers + names (`mlink-data`, `mlink-space`,
  `mlink-state`, `mlink-test`), visible in `/proc/interrupts`.
  - Stuck: alloc fails → QEMU `vectors=4` missing or `msi=off`; check `lspci -vv` capabilities.
  - Fallback (timebox 2 h): INTx mode using IntrMask/IntrStatus with a single shared handler
    that demuxes (ivshmem supports INTx without MSI-X). Document the limitation.
- **P1.5 Peer discovery:** write own `peer_id` + `magic` into own state block; poll the peer
  block (delayed_work every 100 ms) until valid; log "peer found: id=N".
- **P1.6 debugfs** (`/sys/kernel/debug/mlink/`): `info` (role, own id, peer id, BAR sizes),
  `irq_counts`, `ring_doorbell` (write `vector` → doorbell to peer), `pingpong` (write N →
  AP sends N TEST kicks one at a time, modem's TEST handler kicks back immediately, AP measures
  each RTT with `ktime_get_ns()`; read → count, min, p50, p90, p99, max).
- **P1.7 First number:** doorbell RTT histogram (debug kernel; label as debug — redo on perf
  kernel in P6). Save to `bench/results/`.
- **P1.8 Robustness:** `insmod/rmmod` 100× loop on each side while the other side keeps
  ringing; KASAN/lockdep clean; no IRQ after free (handlers check a `dying` flag; teardown
  order: stop work → mask/free IRQs → unmap).
- **P1.9 Walkthrough `phase1.md`** + **P1.10 Study gate 1.**
- **Gate P1:** both sides find each other; pingpong works and gives a histogram; 100× reload
  loop clean; `lspci -k` shows driver bound; dmesg gate passes.
- Stuck guide P1:
  - IRQ never arrives → wrong peer ID (re-read peer block), doorbell value packing
    `(peer<<16)|vec`, server started after VM (restart VMs), peer not connected.
  - IVPosition reads 0 / garbage → device is `ivshmem-plain` not `-doorbell`, or chardev missing.
  - Hang on rmmod → `cancel_delayed_work_sync` missing or IRQ handler re-arming work.

---

### Phase 2 — Shared-memory SPSC rings (est. 2–3 days)

- **P2.1 Ring ABI** in `mlink_shm.h`: ctrl block with prod / cons / prod_flags / cons_flags on
  separate 64 B lines, desc array, slot area; static_asserts; layout written by AP (§6.2).
- **P2.2 Ring ops** (`mlink_ring.c`): `ring_free()`, `ring_reserve()`, `ring_publish()`,
  `ring_peek()`, `ring_release()`; `virt_store_release`/`virt_load_acquire`; consumer copies
  `len` once (READ_ONCE) and validates; `prod - cons > N` → corruption → error counter + reset
  request. Kick decision helpers implementing §6.5 (with `virt_mb()`).
- **P2.3 In-kernel ring selftest** (debugfs `selftest`): AP pushes N messages with
  `seq`+payload pattern (e.g. xorshift seeded by seq) as fast as possible, modem verifies order,
  content and count, reports `lost/dup/corrupt/ooo` = 0 and msgs/s. Run with kick suppression
  on/off and record kicks per message.
- **P2.4 Lost-wakeup demonstration (great interview story):** temporarily remove one `virt_mb()`
  behind a debug param `unsafe_no_mb=1`, run selftest for long durations and show stalls (may
  not reproduce on x86 TSO for the release/acquire part — the SB race *can* reproduce on x86
  because store→load reordering is allowed). Record findings honestly either way.
- **P2.5 LKMM litmus tests** (`litmus/`): install herdtools7 (`opam install herdtools7` on the
  laptop, in tmux — slow; fallback: herd web UI at diy.inria.fr/www). Tests:
  `mlink-publish.litmus` (MP: release/acquire publish ⇒ consumer sees payload) and
  `mlink-kick.litmus` (SB: both `smp_mb()` ⇒ "no lost wakeup" state forbidden; without one
  barrier ⇒ allowed). Run with `herd7 -conf linux-kernel.cfg` from the kernel's
  `tools/memory-model/`. Commit outputs.
- **P2.6 Walkthrough `phase2.md`** + **P2.7 Study gate 2.**
- **Gate P2:** 10 M-message selftest with 0 lost/dup/corrupt both directions; litmus results
  committed and explained; KASAN clean.
- Stuck guide P2:
  - Consumer sees stale data → missing release/acquire pairing; check READ_ONCE on prod.
  - Throughput low / huge kick count → want_kick never cleared while NAPI running.
  - Random corruption → slot reused before consumer released (free computation off by one;
    use `prod - cons` with unsigned wrap, test with N=4 to hit wraparound fast).

---

### Phase 3 — Network device data path (est. 3–4 days) ★ core of the project

- **P3.1 net_device setup** (`mlink_net.c`): `alloc_netdev(sizeof(priv), "mlink%d",
  NET_NAME_ENUM, mlink_setup)`; raw-IP p2p per §6.6; `SET_NETDEV_DEV(ndev, &pdev->dev)`;
  `register_netdev` in probe *after* rings ready; carrier off until link UP (P4 makes it
  dynamic; in P3 carrier on when peer found).
- **P3.2 TX path:** `ndo_start_xmit` per §6.6; drop + count oversize; stop/wake with re-check;
  BQL (`netdev_tx_sent_queue`/`completed_queue`) as stretch.
- **P3.3 RX path / NAPI:** `netif_napi_add` (weight 64; param via `netif_napi_add_weight`),
  vector-0 handler, poll per §6.6, `skb->protocol` from version nibble, drop+count unknown,
  `napi_gro_receive`.
- **P3.4 SPACE path:** vector-1 handler → `netif_wake_queue` if enough space.
- **P3.5 Stats:** `ndo_get_stats64` (rx/tx packets/bytes/dropped/errors) with
  `u64_stats_sync`; `ethtool_ops`: `get_drvinfo`, `get_link`, `get_ringparam` (read-only),
  `get_sset_count`/`get_strings`/`get_ethtool_stats` (tx_kicks, tx_kick_suppressed,
  tx_queue_stops, tx_queue_wakes, rx_irqs, napi_polls, napi_budget_exhausted, rx_len_errors,
  rx_proto_errors, ring_corruptions).
- **P3.6 Tracepoints** (`mlink_trace.h`): `mlink_xmit`, `mlink_kick`, `mlink_irq`,
  `mlink_napi_poll` (budget, done), `mlink_rx`.
- **P3.7 Bring-up** (`scripts/guest/mlink-up.sh <ap|modem>`): insmod with role, addr/peer,
  `ip link set mlink0 up`. Tests: `ping -c 100` both ways, `tcpdump -ni mlink0`,
  `iperf3 -s` on modem / `iperf3 -c 10.77.0.2 -t 30` (TCP), `-u -b 0 -l 1400` (UDP), `-R`
  (reverse), `-P 4`. Counters on both sides must agree (tx_packets(ap) == rx_packets(modem)
  + drops). Confirm route uses mlink0.
- **P3.8 Soak** (debug kernel): bidirectional `iperf3 -P 8` + `ping -f` for 10 min, plus link
  `ip link set down/up` every 5 s on one side; dmesg gate passes; no NETDEV WATCHDOG timeouts.
- **P3.9 `scripts/win/test.sh` full smoke:** sync → build → stage → (re)load both sides →
  wait link → ping 100 → iperf3 5 s → unload → dmesg gate → PASS/FAIL summary. Run after every
  driver change from now on.
- **P3.10 First real numbers** (debug kernel, clearly labeled): TCP Gbit/s, ping RTT.
- **P3.11 Walkthrough `phase3.md`** + **P3.12 Study gate 3.**
- **Gate P3:** ping + TCP/UDP iperf3 both directions, 10-min soak clean, counters consistent.
  **→ Minimum resume bullet is now legitimate (update README with honest status).**
- Stuck guide P3:
  - ping: "Destination Host Unreachable" → addresses/peer config or route via management NIC.
  - Packets leave AP (tx counter ↑) but modem rx stays 0 → modem NAPI never scheduled (kick
    suppressed forever → want_kick logic), check `/proc/interrupts` + debugfs ring dump.
  - Modem rx ↑ but ping fails → `skb->protocol` wrong / network header not reset / dev not up.
  - `NETDEV WATCHDOG: mlink0: transmit queue 0 timed out` → lost SPACE wake (re-check race).
  - `BUG: scheduling while atomic` → sleeping alloc (`GFP_KERNEL`) in xmit/NAPI → use atomic /
    napi_alloc_skb.
  - KASAN slab-out-of-bounds in copy → unvalidated len or wrong slot index math.
  - Throughput tiny → kicking every packet + interrupts every packet; check suppression and GRO.

---

### Phase 4 — Link state machine & Subsystem Restart (est. 2–3 days)

- **P4.1 State machine spec** in `docs/design.md` (diagram, transitions, who writes what,
  generation handshake from §6.7).
- **P4.2 Heartbeat + watchdog** (`mlink_link.c`): `delayed_work` on `system_wq`; params
  `hb_ms` (100), `hb_timeout_ms` (500).
- **P4.3 Teardown/re-handshake:** carrier off → stop queue → `napi_disable` → drop in-flight →
  reset own indices/flags → `state=WAIT_PEER` → on handshake UP: `napi_enable`, carrier on,
  wake. Locking: link state changes serialized by a mutex in process context (work item);
  data path never takes it (document why).
- **P4.4 STATE vector:** graceful down sends STATE kick → near-instant detection.
- **P4.5 SSR scenario script** (`scripts/host/ssr.sh` + guest helpers), each with a live
  `iperf3 -t 120` TCP flow and `ping -i 0.01` running:
  (a) modem graceful driver restart (`rmmod; sleep 1; insmod`),
  (b) modem VM `kill -9` + restart (gets a **new peer ID**),
  (c) AP driver restart, (d) both restart, (e) 100 consecutive (a) cycles.
  Record: detection time, recovery time (carrier on), ping gap, TCP survived? (`ss -ti`
  retrans), packets lost. Use `ip monitor link` timestamps + driver tracepoints.
- **P4.6 Walkthrough `phase4.md`** + **P4.7 Study gate 4.**
- **Gate P4:** all scenarios recover automatically; 100× restart loop clean; numbers recorded.
- Stuck guide P4:
  - Deadlock on teardown → `napi_disable` called from within NAPI/softirq or holding a lock the
    poll needs; teardown only from the work item.
  - Link never comes back after VM kill → still kicking old peer ID; peer ID must be re-read
    from the peer state block on every handshake.
  - Flapping → timeout too tight on a loaded old laptop (VM vCPU descheduled) — raise
    `hb_timeout_ms`, record why (good discussion point: false positives vs detection latency).

---

### Phase 5 — Hardening & correctness (est. 2 days)

- **P5.1 `tools/shm_chaos.c`** (runs on the **laptop host**, `mmap`s `/dev/shm/mlink-shm`):
  modes: random byte flips in desc area, bogus `len`, index jumps (`prod += 1000`), flags
  storms, header magic corruption, "freeze heartbeat". Rate-limited, seedable, logs actions.
  Driver must: not crash, KASAN clean, count errors, reset link, recover when chaos stops.
- **P5.2 debugfs fault knobs:** `fault_drop_kick` (1/N), `fault_delay_kick_us`,
  `fault_corrupt_tx_len` (1/N). Show TCP still works (retransmits) and the link self-heals.
- **P5.3 Static checks:** `checkpatch.pl --strict --no-tree -f driver/*.[ch]` clean;
  `make C=1` (sparse) clean; `W=1` clean.
- **P5.4 (optional) KCSAN kernel build + soak.** Skip if time is short.
- **P5.5 Long soak:** 1 h bidirectional traffic + periodic chaos + periodic SSR; dmesg gate.
- **P5.6 Walkthrough `phase5.md`** + **P5.7 Study gate 5.**
- **Gate P5:** N chaos runs with 0 crashes/KASAN reports, static checks clean, soak clean.

---

### Phase 6 — Measurements, results & write-up (est. 2–3 days) → **MVP CUT LINE**

- **P6.1 Perf kernel build** (`build-kernel.sh perf`) and boot VMs with it. Module rebuilt
  against `build/perf`.
- **P6.2 Benchmark hygiene** (`bench-prep.sh` / `bench-restore.sh`): stop Firefox (ask/notify),
  uvicorn, Docker containers; `sudo -n /usr/local/sbin/mlink-bench-prep on` (governor
  performance); pin each QEMU process to disjoint physical cores (check `lscpu -e` for
  sibling layout; e.g. ap → CPUs {0,4}, modem → {1,5}); warm-up run discarded; ≥ 5 repetitions;
  report median + p99 + min/max; record laptop temperature before/after (throttling note).
  Restore everything afterwards.
- **P6.3 `tools/udp_pingpong.c`:** UDP request/response over mlink0, `clock_gettime(MONOTONIC)`,
  HDR-style log-bucketed histogram, prints p50/p90/p99/p99.9/max, CSV output; optional
  `SO_BUSY_POLL` mode.
- **P6.4 Experiment matrix** (`bench/run_*.sh` → CSV in `bench/results/`):
  1. Doorbell RTT (P1 pingpong) on perf kernel.
  2. TCP/UDP throughput vs MTU (576, 1500, 4000) and direction.
  3. Throughput vs ring size (64/256/1024) and NAPI weight (8/64/256).
  4. Latency: interrupt mode vs kick-suppression vs busy-poll (`net.core.busy_poll` /
     `SO_BUSY_POLL`).
  5. Interrupts per packet & CPU% vs offered load (show mitigation working).
  6. SSR: detection & recovery times (from P4 runs, rerun on perf kernel).
  7. Copy cost: perf/bpftrace profile of TX/RX to show where time goes (latency breakdown).
- **P6.5 `bench/plot.py`** → PNGs in `docs/img/` (matplotlib; run on Windows after
  `scripts/win/pull-results.sh`). Create the venv only now: `python -m venv .venv` in the repo
  root (Windows Python 3.13), `bench/requirements.txt` (matplotlib, pandas) pinned, `.venv/`
  added to `.gitignore`. No other Python env is needed anywhere in the project.
- **P6.6 `docs/results.md`** (methodology, hardware, exact commands, caveats: KVM guests on a
  bare-metal 2017-era laptop CPU, noisy, VM-to-VM shared memory rather than a real NIC/modem
  link) + **README** (what/why/diagram/how to run/results/prior art/limitations).
- **P6.7 Resume bullet** filled from results.md; user approves wording.
- **P6.8 Walkthrough `phase6.md`** + **P6.9 Study gate 6** (results defense: "why is X
  slower than Y", "what dominates latency", "what would change on real hardware").
- **P6.10 Interview mock grill:** Claude runs a 30-question mixed grill over all phases;
  weak answers go into `docs/interview-qa.md` for revision.
- **Gate P6 = MVP DONE:** public repo with README + results + plots; resume bullet frozen.

========================== MVP CUT LINE (resume freeze) ==========================

### Phase 7 — (stretch) QMI-like control plane (est. 3 days)
- P7.1 Control ring pair in reserved shm area (reuse ring code, message-oriented).
- P7.2 TLV message format: `{msg_id, txn_id, len, tlvs[]}`; request/response/indication.
- P7.3 `/dev/mlink_ctrl` misc device: `write()` request, `read()` response/indication (blocking
  + `poll`/`O_NONBLOCK`), per-open-file txn tracking, `ioctl(MLINK_IOC_GET_INFO)`.
- P7.4 `tools/mlinkd` on modem (answers GET_LINK_INFO, GET_STATS, SET_DATA_CALL up/down →
  toggles modem-side carrier, sends indications); `tools/mlinkctl` CLI on AP.
- P7.5 Behaviour on SSR: pending requests fail with `-ENOTCONN`, readers get `POLLHUP`.
- P7.6 Walkthrough + study gate.

### Phase 8 — (stretch) Zephyr "modem"
- P8.1 Day-1 risk check: which QEMU boards Zephyr's ivshmem doorbell sample supports
  (`samples/drivers/virtualization/ivshmem/`), Zephyr SDK size vs laptop disk/RAM.
- P8.2 Zephyr app: same shm ABI (shared header copied), **loopback modem** (reflects IP packets
  with swapped src/dst + fixed-up checksums) — standard modem test mode.
- P8.3 Then optionally real Zephyr IP stack via a Zephyr net driver on the ring.

### Phase 9 — (stretch) aarch64 guest
- P9.1 `qemu-system-aarch64` (needs package install by user), TCG (slow — functional only).
- P9.2 Build same driver for arm64; run selftest + ping; discuss weak memory (x86 host TCG
  will not expose all reorderings — the litmus tests are the real evidence).

### Phase 10 — (stretch) Upstream contribution
- P10.1 Pick a real small issue found during this work (doc fix in ivshmem spec, a bug, a
  checkpatch-clean cleanup in a subsystem you touched); follow
  `Documentation/process/submitting-patches.rst`, `get_maintainer.pl`, `git send-email`
  (needs SMTP setup by the user). Start early — review takes weeks.

### Phase 11 — (stretch) Real hardware
- P11.1 ₹500 Raspberry Pi Pico 2 as a USB "modem" (TinyUSB vendor class), Linux USB driver
  (`usb_driver`, URBs, bulk endpoints) exposing the same `mlink0` semantics.

---

## 8. Study-gate protocol ("Claude writes, user studies")
For each phase N:
1. Claude writes `docs/walkthrough/phaseN.md`: purpose of each file/function, line-level "why"
   for every non-obvious line (barriers, contexts, ordering, error paths), what breaks if it's
   removed, links to kernel docs/source used.
2. User reads it with the code open; asks questions freely.
3. **Quiz (8–12 questions)** asked one batch at a time in chat: concept (e.g. "what context does
   NAPI poll run in, can it sleep?"), what-if ("peer dies between desc write and prod
   publish?"), design ("why copy instead of zero-copy here?"), and **1 spot-the-bug** (Claude
   shows a modified snippet with a planted bug).
4. **Explain-back:** user explains the main path of the phase in their own words (e.g. TX path
   from `send()` to peer NAPI).
5. Claude grades honestly. Pass = ≥ 80% + correct spot-the-bug + acceptable explain-back.
   Fail → Claude re-teaches the weak points, re-quizzes those.
6. Good answers (in the user's words) are appended to `docs/interview-qa.md`.
7. Only then tick "Study gate N passed" in checkbox.md.

---

## 9. Debugging toolkit (use in this order)
1. Reproduce with the smallest test (`test.sh`, selftest, single ping).
2. `logs/<vm>-serial.log` (survives guest panic) and `dmesg` / dmesg-gate.
3. debugfs: `info`, ring dump (prod/cons/flags/first descs), counters, `ethtool -S mlink0`.
4. `/proc/interrupts` deltas, `ip -s link show mlink0`.
5. `dyndbg` (`echo 'module mlink +p' > /sys/kernel/debug/dynamic_debug/control`),
   `trace-cmd record -e mlink:*`, bpftrace one-liners on tracepoints/kprobes.
6. Kernel gdb: QEMU gdbstub (`-gdb tcp:...`, `nokaslr`), on laptop
   `gdb $BUILD/debug/vmlinux` → `target remote :1234`, `lx-symbols`, `lx-dmesg`
   (needs `CONFIG_GDB_SCRIPTS` + `add-auto-load-safe-path`).
7. `git bisect` between last-known-good commit and now (commit small and often).
8. Timebox 2 h → fallback/simplify → log in `docs/debugging-log.md`.

## 10. Risk register
| # | Risk | Likelihood | Mitigation / fallback |
|---|---|---|---|
| R1 | ivshmem-server won't build | M | own `ivshmem-lite-server.c` from the spec (P0.6) |
| R2 | Kernel builds too slow on i5-8250U | H | trim config, ccache, builds in tmux overnight, only rebuild kernel when config changes (module builds take seconds); last resort: install WSL2 on the Windows PC (user approval, reboot) and build bzImage there, scp it over |
| R3 | RAM pressure (7.6 GB) | M | VMs 1 GB → 768 MB, stop Docker/uvicorn/Firefox during work |
| R4 | Laptop unreachable while user is away | L–M | source of truth is on Windows + GitHub, nothing lost; continue design/docs/litmus/tools that don't need the laptop; never load mlink.ko on host |
| R5 | Guest panic loops | M | `-no-reboot panic=1`, serial log to file, `vm.sh kill` |
| R6 | bpftrace/BTF issues | M | trace-cmd + tracepoints + debugfs counters instead |
| R7 | One-way latency across VMs unmeasurable (clocks) | H | report RTT only; per-side component times from tracepoints |
| R8 | MSI-X trouble | L | INTx fallback (P1.4) |
| R9 | Deadline arrives early | ? | minimum bullet after P3; each phase leaves the repo in a presentable state |
| R10 | herd7 install heavy | M | herd web UI; or install via opam on laptop overnight |
| R11 | Kernel API churn (pcim_*, netif_napi_add*, etc.) | M | always check the chosen tree's headers; compile warnings are errors in our Makefile (`ccflags-y += -Werror`) |
| R12 | Benchmarks noisy (thermal throttling, shared laptop) | H | hygiene script, pinning, repetitions, medians, report variance & caveats honestly |
| R13 | Interview: "it's just emulation" | M | answer: real PCI/IRQ/netdev APIs, only the device is virtual; P11 real HW stretch |
| R14 | CRLF line endings break scripts/Kbuild | M | `.gitattributes eol=lf`, `core.autocrlf false`, sync script sanity check (`file` on scripts) |

## 11. Key decisions (log — newest at bottom)
- **D1 (2026-10-26)** Project = raw-IP shared-memory "modem link" net driver (covers OS +
  networking + architecture gaps) instead of DSP-offload RPC; control plane moved to stretch.
- **D2** Run builds/VMs on the Kali laptop over SSH (KVM works, sleep disabled, Tailscale);
  Claude Code stays on the Windows PC. No WSL for now (fallback only).
- **D3** Windows folder is the git source of truth; laptop gets a disposable mirror via
  `scripts/win/sync.sh` (tar over ssh). Heavy artifacts live only in `~/D/mlink-work`.
- **D4** Two Linux guests running the same driver with `role=` param; Zephyr modem is stretch.
- **D5** Raw-IP point-to-point netdev (modem-authentic, like rmnet/mhi_net), copy-based fixed
  slots (simple, safe with untrusted shm), BAR2 mapped WB + `virt_*` barriers.
- **D6** Heartbeat + generation handshake for SSR because ivshmem gives no peer-death signal
  to the guest.
- **D7** Separate debug and perf kernels; numbers only from perf kernel.
- **D8** Claude writes the code; user studies with gated quizzes before each phase is closed.
- **D9** Public GitHub repo, GPL-2.0 (required for kernel module code).
- **D10** Deadline unknown → MVP cut after P6, presentable minimum after P3.
- **D11** Claude may stop Firefox/uvicorn/Docker on the laptop for builds and benchmarks
  (restart Docker containers afterwards).
- **D12** Hard file-safety boundary on the laptop (only `~/D/mlink`, `~/D/mlink-work`,
  `/dev/shm/mlink-shm`); sudo password never goes through chat — user runs root steps.
- **D13 (2026-10-27)** User chose to let Claude use sudo so work is not blocked while they are
  away. Mitigations: password file written by the user on the laptop only
  (`~/D/mlink-work/.secrets/sudo.pass`, 0600), used solely via `sudo -A` askpass in
  `scripts/host/msudo.sh` so the password never enters Claude's context; every root command
  logged to `logs/sudo.log` and reported to the user; allow/deny list in CLAUDE.md hard rule 3.
  Supersedes the "no sudo from Claude" part of D12.
- **D14 (2026-10-27)** Guest kernel = **v6.18.59** (latest 6.18 longterm per kernel.org
  releases.json on 2026-10-27; 7.2.13 stable / 7.3 mainline not chosen — LTS for stable APIs).
  QEMU source 10.2.2, GPG signature verified (key CEACC9E15534EBABB82D3FA03353C9CEF108B584),
  sha256 784b296ff29c1417aa72323abcb2d2ea9ab9771724f577dcd785c3b04f21e176.

## 12. Notes & references
- QEMU: `docs/specs/ivshmem-spec.rst`, `docs/system/devices/ivshmem.rst`,
  `contrib/ivshmem-server/`.
- Linux: `drivers/net/mhi_net.c`, `drivers/net/ethernet/qualcomm/rmnet/`,
  `drivers/net/virtio_net.c`, `Documentation/networking/napi.rst`,
  `Documentation/networking/netdevices.rst`, `Documentation/PCI/pci.rst`,
  `Documentation/PCI/msi-howto.rst`, `Documentation/memory-barriers.txt`,
  `tools/memory-model/` (LKMM, herd7), `Documentation/dev-tools/kasan.rst`,
  `Documentation/process/submitting-patches.rst`.
- Prior art: Siemens Jailhouse `ivshmem-net` (out-of-tree) — read, cite, don't copy.
- Qualcomm concepts to be able to talk about: SMEM, IPCC, GLINK, QRTR, QMI, MHI, rmnet,
  remoteproc/PIL, SSR, FastRPC.
