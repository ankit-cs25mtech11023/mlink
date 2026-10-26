# mlink — Linux Network Driver for a Shared-Memory Modem Link

An out-of-tree Linux PCI network driver that rebuilds, in miniature, the data path between a
phone's **application processor (AP)** and its **modem**: two KVM guests share memory through
QEMU's `ivshmem-doorbell` device, and each runs `mlink.ko`, which exposes a **raw-IP
point-to-point `net_device` (`mlink0`)** with **MSI-X doorbells, NAPI, lock-free SPSC rings in
shared memory**, and **subsystem-restart (SSR) recovery** when the other side dies.

```
 VM "ap"                                              VM "modem"
  TCP/IP stack                                         TCP/IP stack
  mlink0 (raw IP, NAPI) ─────── 4 MiB shared memory ─── mlink0
  mlink.ko role=ap      ◄── doorbell → MSI-X ──►        mlink.ko role=modem
        └──────── QEMU ivshmem-doorbell + ivshmem-server (KVM host) ────────┘
```

| Snapdragon concept | mlink equivalent |
|---|---|
| SMEM (shared memory) | ivshmem BAR2, ABI in `driver/mlink_shm.h` |
| IPCC / mailbox interrupts | ivshmem doorbell → peer MSI-X vector |
| MHI / BAM-DMUX channels, `mhi_net`, `rmnet` | descriptor rings + raw-IP netdev with NAPI |
| Subsystem Restart (SSR) | heartbeat/ring-progress watchdog + generation handshake |

## Status

Phases 0–5 implemented and tested on a **debug kernel** (KASAN, lockdep, kmemleak).
Phase 6 (perf-kernel measurements) in progress. See [`checkbox.md`](checkbox.md).

## What's in it

- **PCI/MSI-X driver** (`mlink_main.c`): managed resources, 4 vectors (DATA, SPACE, STATE,
  TEST), BAR2 mapped write-back with `virt_*` barriers, careful teardown ordering.
- **SPSC rings** (`mlink_ring.c`): free-running indices, release/acquire publication, single-
  fetch validation of every peer-written field, **kick suppression** (store-buffering pattern
  with full barriers) — verified with **LKMM litmus tests** (`litmus/`).
- **Raw-IP netdev** (`mlink_net.c`): `ARPHRD_RAWIP` like `mhi_net`, NAPI + GRO, queue
  stop/wake with re-check, `xmit_more` batching, `u64_stats`, ethtool `-S`, tracepoints.
- **Link state machine & SSR** (`mlink_link.c`): two-way generation handshake, heartbeat or
  ring-progress liveness, reset that quiesces the data path without locks in it, STATE-vector
  fast path for graceful restarts.
- **Hardening**: host-side **shared-memory corruption injector** (`tools/shm_chaos.c`),
  driver fault knobs, lost-kick watchdogs on local facts only, header self-repair;
  `checkpatch --strict`, sparse and `W=1` clean.

## Results so far (debug kernel — not headline numbers)

| What | Result | Source |
|---|---|---|
| Doorbell round trip (IRQ→IRQ) | p50 8–12 µs, 0 lost / 15 000 | `bench/results/p1-doorbell-rtt-debug.txt` |
| Ring selftest, 10 M msgs each way | 0 lost/dup/corrupt; kick suppression 2.9× msgs/s with ~3000× fewer interrupts | `bench/results/p2-selftest-debug.txt` |
| LKMM litmus | publish / slot-reuse / kick: *Never*; each barrier removed: *Sometimes* | `litmus/herd7-results.txt` |
| 10-min soak (bidir TCP + ping flood + 105 link flaps) | clean dmesg gate, 0 stalls/corruptions | `bench/results/p3-soak-debug.txt` |
| SSR: modem / AP / both driver restarts under TCP | TCP survives, 1.3–2.1 s outage; 100/100 restarts | `bench/results/p4-ssr-debug.txt` |
| SSR: modem VM `kill -9` | detected at 520 ms heartbeat age, re-handshake with new peer ID | `bench/results/p4-ssr-debug.txt` |
| shm_chaos, 7 modes, 1201 corruptions under TCP | no crash, KASAN clean, all recover ≤ 3.2 s | `bench/results/p5-chaos-debug.txt` |

Debug-kernel TCP throughput (~47 Mbit/s) is bound by the KASAN-instrumented sender, not the
link; perf-kernel numbers will replace it.

## How to run

Full rebuild-from-zero instructions: [`docs/environment.md`](docs/environment.md). In short:
```bash
bash scripts/win/rbuild.sh        # sync to the KVM host, build mlink.ko, stage to the guests
bash scripts/win/test.sh          # smoke test: load, link up, ping, iperf3, unload, dmesg gate
ssh mlink-ap 'bash /mnt/share/mlink-up.sh ap'; ssh mlink-modem 'bash /mnt/share/mlink-up.sh modem'
ssh mlink-ap 'ping 10.77.0.2'
```

## Design & study material
- [`docs/design.md`](docs/design.md) — layout, vectors, ring protocol, state machine.
- [`docs/walkthrough/`](docs/walkthrough) — per-phase line-by-line explanations.
- [`docs/debugging-log.md`](docs/debugging-log.md) — the bugs found and how.

## Prior art (and what's different)
- Siemens Jailhouse **`ivshmem-net`** (out-of-tree Ethernet over ivshmem) — read as prior art.
- Linux **`mhi_net`**, **`rmnet`** (modem raw-IP netdevs), **`virtio_net`** (rings, kick
  suppression). mlink's focus: modem-style raw-IP semantics, SSR, fault injection, memory-model
  verification and measured numbers — not novelty of the transport.

## Limitations
Not a production driver: fixed layout, copy-based (two copies per packet), x86-only memory
ordering tested at runtime (LKMM covers the model), single queue, both peers are Linux VMs on
one host. Numbers are VM-to-VM on a 2017 laptop CPU.

## License
GPL-2.0-only. See [LICENSE](LICENSE).
