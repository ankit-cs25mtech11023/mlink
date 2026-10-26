# Phase 2 walkthrough — shared-memory SPSC rings

Files: `driver/mlink_shm.h` (ring ABI), `driver/mlink_ring.[ch]`, `driver/mlink_selftest.c`,
`litmus/`, `bench/results/p2-*.txt`.

---

## 1. Shape of a ring

```
ctrl (4 cache lines)            desc[256] × 16 B          slots[256] × 4 KiB
 prod        (producer writes)   len proto flags seq        payload of desc i
 cons        (consumer writes)
 want_space  (producer writes)
 want_kick   (consumer writes)
```
- Two rings: A (AP→modem), B (modem→AP). Each side is producer of one, consumer of the other.
- **Descriptor i always describes slot i.** No offsets/pointers in shm → a malicious peer can't
  make us read/write outside the ring.
- Indices are **free-running u32**; slot = `idx & (N-1)`. `prod - cons` with unsigned wrap is
  the fill level even after 2³² messages. Invariant `prod - cons ≤ N`.

## 2. Local index copies (`struct mlink_ring.idx`)

Each side keeps its own index in a local variable and only *publishes* it to shm. It never
reads its own index back from shm (the peer can't change it — but corruption could; and it
saves a cache-line bounce). It only reads the *other* side's index.

## 3. Ordering, operation by operation

| Op | Code | Why |
|---|---|---|
| publish | payload, desc → `virt_store_release(prod)` | peer that sees new prod sees payload (MP) |
| peek | `virt_load_acquire(prod)` → desc → payload | pairs with the release |
| release | `virt_store_release(cons)` after reading slots | producer can't overwrite a slot we're still reading |
| free | `virt_load_acquire(cons)` → then write slot | pairs with that release (LB pattern) |
| kick decision | store own flag/index → **`virt_mb()`** → load other's | store-buffering (SB) pattern |

`virt_*` vs `smp_*`: same semantics, but `virt_` stays a real barrier in a `CONFIG_SMP=n`
guest — the other party is another VM, not another CPU of this kernel.

## 4. The store-buffering race (the core interview story)

Consumer going idle: `want_kick = 1; mb; if (prod != cons) poll again`.
Producer after publish: `prod = p+1; mb; if (want_kick) kick`.

Without the barriers, each CPU's store can sit in its **store buffer** while its following
load already executes: consumer reads old `prod` (sleeps), producer reads old `want_kick`
(no kick) → message sits in the ring forever = **lost wakeup**. x86 TSO allows exactly this
reordering (store → later load). Note: a store-*release* does not help (it orders *earlier*
accesses before the store, not the store before later loads).

Proven with herd7 (LKMM): `mlink-kick` = Never; drop either `smp_mb` = Sometimes.

## 5. Untrusted shared memory (in `mlink_ring_peek`)

1. `avail = prod - idx`; `avail > N` → `-EIO` (corruption) → counted, RX stops (`rx_broken`),
   P4 resets the link. Never index with it.
2. `len`, `proto`, `seq` read **once** with `READ_ONCE` into `struct mlink_rx_desc`; only the
   copy is validated and used. Double-fetch bug if we re-read `len` from shm for the memcpy.
3. `len == 0 || len > SLOT_SIZE` → `-EMSGSIZE`, the descriptor is skipped (consumed).

## 6. Producer back-pressure (`mlink_tx_wait_space`)

Full ring → arm `want_space` (+mb) → re-check → `wait_event_timeout(tx_wq, free || dying,
100 ms)`; the SPACE irq does `wake_up`. If a timeout ends with space available but the SPACE
irq counter didn't move → **`lost_space_wakeups`** (the runtime detector for the SB race).

## 7. Consumer watchdog (in the link work)

`want_kick` armed + data pending + **no progress** (rx idx unchanged) for two 100 ms polls →
`rx_stalls++` and a forced poll. First version lacked "no progress" and produced false
positives under TCP (the armed+pending state is normal for the µs between a publish and its
interrupt) — fixed in Phase 3.

## 8. Selftest

Payload = xorshift32 stream seeded by `seq`; length derived from `seq` (16–4096, ×4). The
consumer recomputes both → verifies order (lost / dup+ooo), length and content with no side
channel. `seq == 0` resets the verifier.

## 9. Results (debug kernel, turbo off — label as such)

| Run | msgs/s | DATA kicks per 10 M | errors |
|---|---|---|---|
| suppression on, AP→modem | 269k | 3 363 | 0 |
| suppression off, AP→modem | 94k | 10 000 000 | 0 |
| suppression on, modem→AP | 268k | 1 552 | 0 |
| unsafe_no_mb=1 (both dirs, 20 M) | ~268k | — | 0 lost wakeups |

Suppression: **2.9× throughput, ~3000× fewer interrupts**. The no-barrier run not failing is
explained in `docs/debugging-log.md` (absence of evidence ≠ evidence of absence; litmus is
the proof).

## Likely quiz questions
1. Why free-running indices instead of wrapping at N? How is "full" detected?
2. Where exactly is the release, where the acquire, and what does each pair protect?
3. Draw the SB lost-wakeup interleaving. Why does store-release not fix it?
4. Why one cache line per control word?
5. What is a double fetch? Show the line that avoids it.
6. Why might `unsafe_no_mb=1` not fail in practice? Does that mean the barrier is useless?
7. Spot-the-bug candidates: `mlink_ring_free` using `READ_ONCE(cons)` instead of acquire
   (→ slot reuse race); `peek` re-reading `sd->len` for the copy; `release` before the copy.
