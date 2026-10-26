# Project Progress — mlink

> Mirrors `plan.md` task IDs. Tick `[x]` only when the task is fully implemented AND verified.
> A phase is closed only after its study gate is passed.

## Phase 0: Environment & bootstrap
- [x] P0.1 Create repo skeleton on Windows (git init, LF .gitattributes, .gitignore, GPL-2.0 LICENSE, directory tree, first commit)
- [ ] P0.2 Create public GitHub repo `mlink`, add SSH remote, verify `ssh -T git@github.com`, first push after user confirms
- [x] P0.3 Add user-approved `~/.ssh/config` aliases on Windows (kali, mlink-ap, mlink-modem) and verify `ssh kali hostname`
- [x] P0.4 Write `setup-root.sh`; user runs it with sudo in their own terminal (packages, work dirs, narrow NOPASSWD rule for mlink-bench-prep only)
- [x] P0.4b User writes `~/D/mlink-work/.secrets/sudo.pass` (0600); `msudo.sh true` works and logs to `logs/sudo.log` without exposing the password
- [x] P0.5 Fetch kernel LTS source (shallow) and QEMU 10.2.2 tarball; record exact kernel tag in plan.md Key Decisions
- [x] P0.6 Build `ivshmem-server` from QEMU source (or fallback: write `ivshmem-lite-server.c`) and verify socket + `/dev/shm/mlink-shm`
- [ ] P0.7 Write kernel config fragments and build the debug kernel (KASAN, lockdep, BTF) in tmux; verify options landed in `.config`
- [x] P0.8 Build Debian trixie rootfs with mmdebstrap (ssh keys, 9p fstab, networkd for virtio NIC) and create ap/modem qcow2 overlays
- [x] P0.9 Boot both VMs via `ivshmem.sh` + `vm.sh`; ssh from Windows via aliases; ivshmem 1af4:1110 visible in both; /mnt/share mounted
- [x] P0.10 Hello-module loop working (rbuild.sh sync→build→stage, insmod/rmmod in guest), plus dmesg-gate.sh and test.sh skeleton
- [ ] P0.11 Write `docs/environment.md` (rebuild from zero) and `docs/walkthrough/phase0.md`
- [ ] P0.12 Study gate 0 passed (QEMU/KVM/9p/Kbuild/vermagic quiz)

## Phase 1: PCI driver & doorbell interrupts
- [ ] P1.1 Write `docs/design.md` v1 (shm layout, roles, vectors, single-writer table) and get user review
- [x] P1.2 Write `mlink_shm.h` v1 (header + state blocks, cacheline padding, static_asserts on offsets)
- [x] P1.3 Implement PCI probe/remove: enable device, map BAR0 (current pcim API) and BAR2 (memremap WB), read IVPosition, validate `role` param
- [x] P1.4 Allocate 4 MSI-X vectors and register per-vector IRQ handlers visible in /proc/interrupts (INTx fallback only if blocked)
- [x] P1.5 Implement peer discovery through state blocks (publish own peer_id + magic, poll for peer)
- [x] P1.6 Add debugfs: info, irq_counts, ring_doorbell, and in-kernel doorbell ping-pong RTT histogram
- [x] P1.7 Record first doorbell RTT histogram (debug kernel, labeled) in bench/results
- [ ] P1.8 Pass 100× insmod/rmmod loop on each side under traffic with KASAN/lockdep clean and correct teardown order
- [ ] P1.9 Write `docs/walkthrough/phase1.md`
- [ ] P1.10 Study gate 1 passed (PCI, BARs, MSI-X, IRQ context, devm, teardown ordering)

## Phase 2: Shared-memory SPSC rings
- [ ] P2.1 Define ring ABI (ctrl lines for prod/cons/flags on separate cache lines, desc array, slots) with static_asserts
- [ ] P2.2 Implement ring ops with virt_* release/acquire, single-fetch length validation, corruption detection, kick-decision helpers
- [ ] P2.3 Implement debugfs ring selftest (seq + payload pattern) reporting lost/dup/corrupt/ooo and msgs/s, with and without kick suppression
- [ ] P2.4 Run the lost-wakeup experiment with `unsafe_no_mb=1` and record honest findings in debugging-log.md
- [ ] P2.5 Write and run LKMM litmus tests (publish MP + kick SB) with herd7 (or web UI) and commit outputs
- [ ] P2.6 Write `docs/walkthrough/phase2.md`
- [ ] P2.7 Study gate 2 passed (memory ordering, release/acquire, SB race, untrusted shared memory, double fetch)

## Phase 3: Network device data path ★
- [ ] P3.1 Create raw-IP point-to-point net_device `mlink%d` (ARPHRD_RAWIP, IFF_POINTOPOINT|IFF_NOARP) registered after rings are ready
- [ ] P3.2 Implement TX path (`ndo_start_xmit` copy into slot, publish, suppressed kick, stop/wake queue with re-check)
- [ ] P3.3 Implement RX path with NAPI (vector-0 IRQ → napi_schedule, budgeted poll, protocol from IP nibble, GRO, complete+re-arm+re-check)
- [ ] P3.4 Implement SPACE vector handling to wake the TX queue
- [ ] P3.5 Add stats64 with u64_stats_sync and ethtool ops (drvinfo, link, ringparam, `ethtool -S` driver counters)
- [ ] P3.6 Add TRACE_EVENT tracepoints (xmit, kick, irq, napi_poll, rx)
- [ ] P3.7 Bring-up script + verify ping, tcpdump, iperf3 TCP/UDP/reverse/parallel both ways, counters consistent, route via mlink0
- [ ] P3.8 Pass 10-minute bidirectional soak with ping flood and link flaps (no watchdog timeouts, dmesg gate clean)
- [ ] P3.9 Complete `scripts/win/test.sh` end-to-end smoke test (sync→build→load→ping→iperf3→unload→dmesg gate)
- [ ] P3.10 Record first throughput and RTT numbers (debug kernel, labeled)
- [ ] P3.11 Write `docs/walkthrough/phase3.md`
- [ ] P3.12 Study gate 3 passed (net_device, sk_buff, NAPI, softirq context, queue stop/wake, GRO) — minimum resume bullet unlocked

## Phase 4: Link state machine & Subsystem Restart
- [ ] P4.1 Specify link state machine + generation handshake in `docs/design.md` (diagram, single-writer fields)
- [ ] P4.2 Implement heartbeat + watchdog delayed_work with `hb_ms` / `hb_timeout_ms` params
- [ ] P4.3 Implement teardown and re-handshake (carrier off, stop queue, napi_disable, drop in-flight, reset owned indices, re-UP)
- [ ] P4.4 Implement STATE vector for fast graceful-down detection
- [ ] P4.5 Run SSR scenarios a–e with live TCP + ping and record detection/recovery times, ping gap, TCP survival, packets lost
- [ ] P4.6 Write `docs/walkthrough/phase4.md`
- [ ] P4.7 Study gate 4 passed (state machines, workqueues vs softirq, napi_disable semantics, failure detection trade-offs)

## Phase 5: Hardening & correctness
- [ ] P5.1 Write host-side `shm_chaos` corruption injector and show driver survives (no crash, KASAN clean, counts errors, self-heals)
- [ ] P5.2 Add debugfs fault knobs (drop kick, delay kick, corrupt tx len) and verify TCP + link self-heal
- [ ] P5.3 Make driver checkpatch --strict, sparse (C=1) and W=1 clean
- [ ] P5.4 (optional) Build KCSAN kernel and run soak
- [ ] P5.5 Pass 1-hour soak with traffic + chaos + periodic SSR (dmesg gate clean)
- [ ] P5.6 Write `docs/walkthrough/phase5.md`
- [ ] P5.7 Study gate 5 passed (untrusted input, fault injection, static analysis, sanitizers)

## Phase 6: Measurements, results & write-up → MVP
- [ ] P6.1 Build perf kernel (no debug options), boot VMs on it, rebuild module against it
- [ ] P6.2 Write bench-prep/restore (stop background apps, governor via NOPASSWD script, CPU pinning, repetitions) and restore afterwards
- [ ] P6.3 Write `tools/udp_pingpong.c` latency tool (histogram percentiles, CSV, optional SO_BUSY_POLL)
- [ ] P6.4 Run experiment matrix (doorbell RTT, throughput vs MTU/ring/NAPI weight, latency modes, IRQs/packet vs load, SSR times, profile breakdown)
- [ ] P6.5 Create Windows `.venv` + pinned `bench/requirements.txt`, write `bench/plot.py`, and generate plots into docs/img
- [ ] P6.6 Write `docs/results.md` (methodology + caveats) and full README (diagram, how to run, results, prior art, limitations)
- [ ] P6.7 Fill the resume bullet from measured numbers and get user approval
- [ ] P6.8 Write `docs/walkthrough/phase6.md`
- [ ] P6.9 Study gate 6 passed (results defense)
- [ ] P6.10 Run full 30-question interview mock grill and update `docs/interview-qa.md`

======== MVP CUT LINE (resume freeze) ========

## Phase 7: (stretch) QMI-like control plane
- [ ] P7.1 Add control ring pair in reserved shm area (message-oriented, reusing ring code)
- [ ] P7.2 Define TLV message format (msg_id, txn_id, request/response/indication)
- [ ] P7.3 Implement `/dev/mlink_ctrl` misc device (write/read/poll/O_NONBLOCK, per-file txn tracking, GET_INFO ioctl)
- [ ] P7.4 Write `mlinkd` (modem daemon: link info, stats, data-call up/down + indications) and `mlinkctl` CLI
- [ ] P7.5 Handle SSR in control plane (pending requests fail -ENOTCONN, readers get POLLHUP)
- [ ] P7.6 Write walkthrough and pass study gate 7

## Phase 8: (stretch) Zephyr "modem"
- [ ] P8.1 Risk check: Zephyr ivshmem doorbell board support and SDK footprint on laptop
- [ ] P8.2 Zephyr loopback-modem app speaking the same shm ABI
- [ ] P8.3 (optional) Zephyr net driver on the ring with real Zephyr IP stack

## Phase 9: (stretch) aarch64 guest
- [ ] P9.1 Install/run qemu-system-aarch64 (TCG) guest with arm64 kernel
- [ ] P9.2 Build and run driver on arm64 (selftest + ping) and write weak-memory discussion

## Phase 10: (stretch) Upstream contribution
- [ ] P10.1 Prepare and send one small upstream patch (submitting-patches flow, get_maintainer, send-email)

## Phase 11: (stretch) Real hardware
- [ ] P11.1 Pi Pico 2 USB "modem" + Linux USB driver exposing mlink0 semantics
