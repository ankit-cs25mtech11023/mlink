# Phase 9 walkthrough — the same driver on arm64, talking to x86

> Goal: build `mlink.ko` for a weakly ordered CPU and run it as the modem against the x86 AP
> on the **same shared memory**. Evidence: `bench/results/p9-arm64.txt`,
> `bench/results/p9-arm64-barriers.txt`. Decision record: plan.md D21.

## 1. Getting arm64 tools without touching the system (D21)

`apt-get -s install qemu-system-arm gcc-aarch64-linux-gnu` showed that apt would **upgrade**
the user's `qemu-system-x86` (10.2.2 → 11.1.0) and system `binutils`/`linux-libc-dev`. Upgrades
are off limits (and swapping the QEMU behind running VMs is a bad idea anyway), so:

| Need | Source | Trust |
|---|---|---|
| `qemu-system-aarch64` | QEMU 10.2.2 source from P0.5, `--target-list=aarch64-softmmu` | GPG-verified tarball |
| cross GCC | kernel.org crosstool 15.2.0 nolibc | sha256 + signature by the kernel.org autosigner (key via kernel.org WKD) |
| userland | `busybox-static:arm64` via non-root `apt-get download` with a private state dir | Kali archive signatures |

All of it lives in `~/D/mlink-work/opt`. The QEMU build has no SLIRP (no `libslirp-dev`), which
later decided the control channel (§3).

## 2. A minimal kernel

`make ARCH=arm64 allnoconfig` + `configs/arm64-min.config` (PL011 console, initramfs, PCIe
generic ECAM host, **GICv3 ITS** for MSI-X, modules, inet). Builds in 7 min instead of an
hour for `defconfig`. Two lessons:

- `merge_config.sh` silently drops options with unmet dependencies, so the build script checks
  every requested option landed (as for x86).
- Build `Image modules`, not `Image modules_prepare`: only `modules` runs modpost on vmlinux
  and writes `Module.symvers`. Without it every export used by `mlink.ko` was "undefined".

`mlink.ko` is built from an untouched copy of `driver/` (`build/mlink-arm64`) so x86 and arm64
objects never mix. Same source, `-Werror`, zero warnings.

## 3. Driving a guest with no network

The guest is an initramfs: busybox, `mlink.ko`, `/init` (`scripts/guest/arm64-init`). Things
that bit, in order (worth knowing for any embedded bring-up):

1. `DEVTMPFS_MOUNT` does not apply to an initramfs: mount devtmpfs yourself or `/dev` is empty.
2. PID 1 must never exit (kernel panic), so `/init` ends in a sleep loop, not a shell.
3. busybox `ash` on a tty is interactive and echoes input through its line editor whatever
   `stty -echo` says. Feeding it through a pipe (`cat tty | sh`) makes it non-interactive.
4. `stty` settings vanish when the tty is fully closed; apply them on the fd that stays open.
5. A virtio console hangs up in the guest when the host side disconnects, so per-command
   connections lost input. Fix: one persistent connection held by `scripts/host/arm64-bridge.py`,
   which serves requests on its own socket (`vm-arm64.sh sh '<cmds>'`).
6. On the PL011 under TCG the last line moved only when more input arrived (one-step lag).
   The bridge sends an empty line (a no-op for sh) whenever it has waited 1 s for output.

## 4. Results (x86 KVM AP ↔ arm64 TCG modem, one ring in shared memory)

| Test | Result |
|---|---|
| link bring-up after boot | UP 1.75 s after power-on, same handshake as x86↔x86 |
| ring selftest x86 → arm64, 1 M msgs | 0 lost / dup / corrupt (112 k msg/s, arm64 consumer is the bottleneck: 15 614 space waits) |
| ring selftest arm64 → x86, 1 M msgs | 0 lost / dup / corrupt (167 k msg/s; 27 % of publishes kicked, since the fast x86 consumer keeps going idle) |
| ping both ways, 3000 B fragments | all replies |
| flood 5 025 echoes | 5 025 out, 5 025 in at the AP IP layer |
| arm64 rmmod/insmod × 20 | AP re-handshook 20 times (graceful STATE path, `peer_down` +20) |
| arm64 VM killed | AP: heartbeat timeout, 520 ms after the last heartbeat; restart → UP |
| arm64 dmesg | 0 warnings / oopses |

## 5. Weak memory: what this run proves and what it does not

What the arm64 build proves is **that the right instructions are there**
(`bench/results/p9-arm64-barriers.txt`, from `objdump` of `mlink_ring.o`):

| Source (`mlink_ring.c`) | arm64 | Role |
|---|---|---|
| `virt_store_release(&prod)` in publish, `(&cons)` in release | `stlr` ×2 | payload/descriptor visible before the index |
| `virt_load_acquire(&cons)` in free (+ inlined in arm_space), `(&prod)` in peek | `ldar` ×3 | index read before the slot is touched |
| `virt_mb()` in the four kick decisions | `dmb ish` ×4 | store→load order for the SB pattern |

On x86 the same source compiles to plain `mov`s for release/acquire (TSO already gives that
order) and a locked instruction/`mfence` only for `virt_mb()`. On arm64 every one of them
matters: drop the `stlr` and a consumer can see the new `prod` before the payload; drop a
`dmb` and both sides can miss each other's flag and index (the lost wake-up from phase 2).

What it does **not** prove: the arm64 guest runs under TCG on an x86 host. QEMU executes guest
loads/stores as host loads/stores, so the hardware underneath is still TSO; ARM's extra
reorderings (store→store, load→load) never actually happen. Correctness on real weak hardware
rests on the **LKMM litmus tests** (phase 2: MP with release/acquire, SB with full barriers),
whose model is weaker than ARMv8, plus the instruction mapping above. A real arm64 board or
KVM on an arm64 host would be the next step.

Endianness: both sides are little-endian, so the plain `u32` fields in `mlink_shm.h` work. A
big-endian peer would need `__le32` + `le32_to_cpu()` (noted in the header since P1).

## 6. Questions to be ready for

1. Why couldn't the arm64 tools come from apt here, and what replaced each one?
2. Which arm64 instruction does `smp_store_release`/`virt_store_release` become, and what
   ordering does it give that a plain `str` does not?
3. Why are there four `dmb ish` and not two? Name the four functions and the race each prevents.
4. This run passed 1 M messages each way with 0 errors. Why is that still not evidence of
   correctness on a real ARM CPU? What is?
5. Why did x86 → arm64 run at 112 k msg/s with 15 k space waits, and arm64 → x86 with 27 % kicks?
6. Why does `Module.symvers` matter for an out-of-tree module?
