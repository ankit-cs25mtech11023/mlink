# Phase 5 walkthrough — hardening & correctness

Files: `tools/shm_chaos.c`, `scripts/host/chaos.sh`, fault knobs in `mlink_net.c` /
`mlink_debugfs.c`, watchdogs + header repair in `mlink_link.c`, `bench/results/p5-*.txt`,
`docs/debugging-log.md` (the "flags black hole" story).

---

## 1. Threat model: shared memory is untrusted input

The peer may be buggy (a crashed modem scribbling memory) or hostile. Everything the peer
writes — indices, lengths, protocol, flags, state block, header — can hold any value at any
time, and can change between two reads. The driver must never crash, never read/write out of
bounds, and must recover once the garbage stops. Testing this needs an *adversary*, not just
correct traffic: that's `shm_chaos`.

## 2. `shm_chaos` (runs on the host, not in a guest)

The host can `mmap` the same POSIX shm object (`/dev/shm/mlink-shm`) the two guests see as
BAR2 — the perfect vantage point for fault injection: it writes behind both drivers' backs.
It includes the driver's own `mlink_shm.h` (made userspace-safe with `#ifdef __KERNEL__`),
so tool and driver can't disagree on offsets.

| Mode | Damage | Expected driver reaction | Seen |
|---|---|---|---|
| flip | bit flips in descriptors / control words | bad len/proto counted; impossible index → reset | ring_corruptions + resets, recover |
| len | len = 0, slot+1, 0x7fffffff, 0xffffffff | `rx_len_errors`, descriptor skipped | counted (only those still unconsumed) |
| idx | prod/cons jumps (+1000, −512) | `prod − cons > N` → `-EIO` → RING reset | resets, recover |
| flags | random want_kick/want_space | lost/extra kicks → watchdogs | **found a bug** (below) |
| magic | header / state-block magic corrupted | peer "gone" → reset; AP **rewrites the header** | 53 `hdr_repairs` |
| hb | freeze a heartbeat | ring progress keeps the peer "alive" under traffic | `hb_saved_by_ring` |
| mix | random mode per op | all of the above | recover |

Result (debug kernel, 10 s × 20 ops/s per mode, TCP running): 1201 operations, **no crash,
KASAN/lockdep clean, every mode back to UP within ≤ 3.2 s**, ping 0 % loss afterwards.

## 3. The defences, and where they live

1. **Bounds**: `prod − cons ≤ N` checked on every peek/free (`mlink_ring.c`); `len` checked
   against `SLOT_SIZE`; descriptor i ↔ slot i (no peer-supplied offsets).
2. **Single fetch**: every peer field read once into a local, then validated (no TOCTOU).
3. **Classify from our copy**: the IP version nibble is read from the skb after the copy.
4. **Escalation**: impossible index → latch (`rx_broken`/`tx_broken`) → link work resets.
   Peer gen changed → "restart" (normal); unchanged → `ring_corruptions` (attack/bug).
5. **Watchdogs on local facts only**:
   - RX: data pending and our consumer index unchanged for two link polls → schedule NAPI.
   - TX: queue stopped while the ring has space for two polls → wake the queue.
   The first RX watchdog version also required `want_kick == 1` — a flag in shm. A flags storm
   cleared it while the consumer slept: producer stopped kicking, watchdog never fired →
   link "UP" with 100 % loss. **Untrusted memory applies to control decisions too.**
6. **Owner repair**: the AP owns the header; its link work re-validates it every tick and
   rewrites it if damaged (otherwise a later modem restart would wait forever).

## 4. Driver-side fault knobs (`/sys/kernel/debug/mlink/fault_*`)

Inject faults *inside* our own TX path, deterministically and counted:
`fault_drop_kick=N` (swallow 1/N DATA kicks), `fault_delay_kick_us`, `fault_corrupt_tx_len=N`.
Results: dropped kicks absorbed (later kicks, 1 watchdog recovery); 468 injected bad lengths
= 468 `rx_len_errors` on the peer, TCP retransmitted (457), no reset.
Why both tools? Knobs test *our* handling of *our* failures precisely; chaos tests what an
arbitrary peer could do.

## 5. Static analysis

- `checkpatch.pl --strict`: 0 errors, 0 warnings; remaining CHECKs are the TRACE_EVENT macro
  layout only. Fixed: barrier comments must sit on/above the barrier, comment style, etc.
- `sparse` (`make C=2`): 0 warnings across 6 files. Sparse checks address spaces
  (`__iomem` vs normal pointers — e.g. you can't deref `mdev->regs` directly), endianness
  annotations, lock context.
- `W=1`: 0 warnings.

## 6. Sanitizers in the debug kernel (why they matter here)

KASAN (out-of-bounds / use-after-free on every access), lockdep (lock-order and IRQ-safety
proofs), `DEBUG_ATOMIC_SLEEP`, kmemleak (0 reports after 100× reloads and the soak), hung-task
and soft-lockup detectors. The dmesg gate fails a run on any report.

## Likely quiz questions
1. Name four things a malicious peer could write and how the driver contains each.
2. Why does the host make a good fault injector? What can't it simulate?
3. Explain the flags-storm black hole and why the fix is "local facts only".
4. What's the difference between `ring_corruptions` and a "peer restarted" reset?
5. What does sparse catch that gcc `-Wall` doesn't?
6. Spot-the-bug candidates: `if (len > MLINK_SLOT_SIZE)` checked on a value re-read from shm
   for the memcpy; a watchdog conditioned on a shm flag; header never re-validated.
