# Phase 6 walkthrough — measurements, results & defending them

Files: `bench/run_matrix.sh`, `bench/iperf_json.py`, `bench/plot.py`, `tools/udp_pingpong.c`,
`scripts/host/bench-prep.sh` / `bench-restore.sh`, `bench/results/perf/`, `docs/results.md`.

---

## 1. Why a separate perf kernel

The debug kernel instruments every memory access (KASAN), every lock (lockdep) and every
allocation (kmemleak records a stack trace). Measured effect on our TCP path: **47 Mbit/s →
7.2 Gbit/s** (~150×). Debug numbers are for correctness only; headline numbers never come
from them.

## 2. Benchmark hygiene, and why each item matters

| Item | Why |
|---|---|
| governor `performance`, turbo off | fixed 1.6 GHz: no frequency ramp-up, no thermal throttling halfway through a run (this laptop hits 96 °C with turbo) |
| VMs pinned to separate *physical* cores | CPU n and n+4 are hyperthread siblings; two vCPUs of different VMs on siblings would steal each other's execution units |
| background apps stopped | Firefox/uvicorn/Docker would add random noise |
| warm-up run discarded | first run pays page faults, cache misses, TCP slow start |
| 5 repetitions, median, min–max | one run is an anecdote; the median is robust to one outlier |
| traffic verified on mlink0 | `ip route get`, mlink0 counters move — not the management NIC |

## 3. How each experiment measures what it claims

- **E1 doorbell RTT**: both timestamps on the AP (`ktime_get_ns` before the kick and in the
  TEST hard-IRQ handler) → one clock, no cross-VM clock error. Measures: MMIO write → VM exit →
  QEMU eventfd → peer MSI-X → peer handler → same way back.
- **E2 throughput**: iperf3 `-J`, receiver-side bits/s (what actually arrived).
- **E3 NAPI weight**: polls/s and budget-exhausted counts from `ethtool -S` deltas around
  each run.
- **E4 latency**: `udp_pingpong` (one message in flight, `CLOCK_MONOTONIC` on the client,
  exact percentiles from sorted samples). Loopback inside one guest isolates the stack cost.
- **E5 interrupt mitigation**: `rx_irqs` / `rx_msgs` deltas on the modem, CPU busy from
  `/proc/stat` deltas.
- **E6 SSR**: longest gap between ping replies (10 ms ping) = user-visible outage; iperf3
  completing = TCP survived; heartbeat age at detection from the driver itself.

## 4. Results defence — questions you should be able to answer

1. *Why does throughput grow with MTU?* Per-packet costs dominate (descriptor, kick decision,
   skb alloc, two copies, protocol processing); larger packets amortise them.
2. *Why is UDP so much slower than TCP in iperf3?* One `sendto()` per datagram vs TCP's
   GSO batching on send and GRO merging on receive.
3. *Why doesn't NAPI weight change throughput here?* Nothing else competes for the softirq;
   the weight only changes how often we return to `net_rx_action` (6× more polls at 8).
4. *Why is busy-poll latency lower than loopback inside one guest?* Busy-poll removes both
   wake-ups (no interrupt, no scheduler); loopback has client and server sharing 2 vCPUs,
   with context switches.
5. *Why does kick suppression not help latency?* With one message in flight the consumer is
   always idle → every message needs a kick. It helps under load (E5, selftest 2.9×).
6. *What dominates the 44 µs interrupt-mode RTT?* Roughly: 2 × stack traversal (~27 µs for a
   round trip in loopback), doorbell IRQ path (~13 µs RTT, E1), plus wake-ups — consistent
   with the busy-poll result removing most of the difference.
7. *What would change on real hardware?* DMA instead of the second memcpy, a real interrupt
   controller instead of VM exits + eventfd (lower doorbell latency), PCIe/MHI channel
   semantics, IPA offload of the data path, power management (link states, wake latency).
8. *How do you know traffic used mlink0?* Route lookup + mlink0 counters equal to iperf3's
   byte counts (tx on one side == rx on the other).
9. *Why is the outage 90 ms when both drivers restart but 1.1 s when one restarts?* The 1.1 s
   includes a deliberate 1 s sleep in that scenario; the handshake itself is fast — the STATE
   kick makes detection immediate.
10. *Why 520 ms to detect a killed VM?* `hb_timeout_ms` = 500 plus one 100 ms poll period of
    the detector; shrinking it trades false positives (we saw heartbeat starvation under load)
    for faster detection.

## 5. Draft resume bullet (needs your approval — P6.7)

> **Linux network device driver for a shared-memory modem link** (C, Linux kernel, QEMU/KVM):
> built a raw-IP PCI `net_device` (MSI-X, NAPI, lock-free shared-memory rings with kick
> suppression) between two KVM guests — **7.2 Gbit/s TCP** (9.9 at 4 KB MTU), **15 µs p50**
> UDP round trip with busy-poll; **subsystem-restart recovery in 90 ms with live TCP flows
> surviving**; survived **1 200 shared-memory corruption injections** KASAN-clean; ring
> ordering verified with **LKMM litmus tests**.

Every number in it is in `docs/results.md` with its raw file.

## 6. Optimisation after measuring (D17): checksum offload across the link

The E7 profile showed ~6 % of all samples in software checksumming and ~2 % in software
segmentation on the sender. Over shared memory there is no wire that can flip bits, so the
L4 checksum is pure overhead — the same reasoning `veth` and `virtio_net` use.

- **Advertise** `NETIF_F_HW_CSUM | NETIF_F_SG` (`mlink_setup`). The stack then hands us skbs
  with `ip_summed == CHECKSUM_PARTIAL` (checksum *not* filled in; `csum_start`/`csum_offset`
  say where it would go) and with paged fragments.
- **TX**: copy with `skb_copy_bits` (gathers fragments) and put `CSUM_PARTIAL`,
  `csum_start`, `csum_offset` into the descriptor (ABI v2 — the old `rsvd` word).
- **RX**: the offsets come from untrusted shared memory → check
  `start + offset + 2 <= len` ourselves, then `skb_partial_csum_set()` marks the skb
  `CHECKSUM_PARTIAL`. The local stack accepts that as verified; if the packet is *forwarded*
  to a real NIC, the stack/driver there computes the checksum — correct in both cases.
  (Marking it `CHECKSUM_UNNECESSARY` instead would be wrong for forwarding: the packet would
  leave with a garbage checksum.)

Interview angle: "Why is skipping the checksum safe here, and what breaks if you just set
CHECKSUM_UNNECESSARY?"
