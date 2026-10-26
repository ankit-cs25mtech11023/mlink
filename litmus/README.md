# LKMM litmus tests for the mlink ring

Each test models one ordering rule of `driver/mlink_ring.c` in the Linux-kernel memory model
(LKMM) and is checked exhaustively by herd7. "Never" = the bad outcome in `exists (...)` is
impossible under LKMM; "Sometimes" = some allowed execution reaches it.

`virt_store_release` / `virt_load_acquire` / `virt_mb` are modelled with their `smp_*`
counterparts: in LKMM they have the same semantics; the `virt_` variants only differ in that
they stay real barriers on `CONFIG_SMP=n` builds (the peer is another VM, not another CPU of
this kernel).

| Test | Pattern | Models | herd7 |
|---|---|---|---|
| `mlink-publish` | MP | publish: payload, then store-release `prod`; consume: load-acquire `prod`, then payload | **Never** |
| `mlink-publish-noacquire` | MP (broken) | consumer reads `prod` without acquire | Sometimes |
| `mlink-slot-reuse` | LB | consumer reads slot, store-release `cons`; producer load-acquire `cons`, overwrite slot | **Never** |
| `mlink-kick` | SB | kick suppression with `virt_mb()` on both sides | **Never** |
| `mlink-kick-no-producer-mb` | SB (broken) | producer relies on the release of `prod` alone | Sometimes |
| `mlink-kick-no-consumer-mb` | SB (broken) | consumer arms `want_kick` without a barrier | Sometimes |

The SPACE direction (`want_space` / `cons`) is the same SB shape with the roles swapped.

**Key insight (`mlink-kick-no-producer-mb`):** a store-release orders *earlier* accesses
before the store. It does not order the store before *later loads*. Only a full barrier
(`smp_mb`/`virt_mb`) prevents store→load reordering, which x86 TSO does allow through the
store buffer.

## How to run

```bash
# on the laptop (herdtools7 7.58 from the Kali archive)
cd ~/D/mlink-work/src/linux/tools/memory-model
for f in ~/D/mlink/litmus/*.litmus; do herd7 -conf linux-kernel.cfg "$f"; done
```

Full output: [`herd7-results.txt`](herd7-results.txt) (kernel v6.18.59 `tools/memory-model`).

## Relation to the runtime experiment (P2.4)

`unsafe_no_mb=1` removes exactly the barriers that the two broken SB tests drop. In 40 M
selftest messages (debug kernel) no lost wakeup was observed — see
`bench/results/p2-lost-wakeup-debug.txt` and `docs/debugging-log.md`. That is consistent with
the race window being tiny in this workload; it is not evidence that the barriers are
unnecessary. The litmus results are the proof.
