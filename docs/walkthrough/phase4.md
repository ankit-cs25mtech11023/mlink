# Phase 4 walkthrough — link state machine & Subsystem Restart (SSR)

Files: `driver/mlink_link.c`, link parts of `mlink.h`/`mlink_main.c`/`mlink_net.c`,
`scripts/host/ssr.sh`, `bench/results/p4-ssr-debug.txt`, `docs/design.md` §8.1,
`docs/debugging-log.md` (three P4 stories — read them, they are interview gold).

---

## 1. The problem SSR solves

On a Snapdragon the modem DSP can crash and be restarted (PIL/remoteproc) while Linux keeps
running. The AP must: notice, stop using the shared memory safely, wait for the new modem,
re-handshake, and resume — ideally without user-visible breakage (TCP survives).
ivshmem gives **no peer-death signal** (doorbells to a dead peer are silently dropped), so we
need our own failure detector.

## 2. State in shared memory (single writer each)

`state` (DOWN/WAIT_PEER/UP), `gen` (bumped every (re)start), `peer_gen_seen` (the ack),
`heartbeat` (+1 per `hb_ms`), plus the ring fields each side owns.

## 3. Bring-up: the two-way generation handshake

WAIT_PEER → UP when: peer block valid, peer alive, peer `state ≥ WAIT_PEER`, **we acked its
current gen and it acked ours**.

Why generations and not just "peer state == UP"? Because state blocks survive in shm: after a
restart you may read a stale "UP" from the previous incarnation. A generation number makes
each incarnation distinguishable; a mutual ack proves both sides saw each other's *current*
incarnation.

**The ordering trick:** a side bumps `gen` *before* zeroing its ring fields (`virt_wmb`), and
writes its ack *after* its own reset (`virt_wmb`); the reader reads the ack, `virt_rmb`, then
the gen. Consequences:
1. A peer that sees our zeroed index (prod − cons > N) also sees the new gen → it classifies
   the error as "peer restarted", not "corruption".
2. "Both acked the current gens" ⇒ "both finished resetting" ⇒ all four indices are zero ⇒
   safe to enable the data path.

## 4. Detection (UP → reset)

| Trigger | Mechanism | Measured (debug kernel) |
|---|---|---|
| graceful rmmod of peer | peer writes `state=DOWN`, clears magic, **STATE kick** → our STATE irq → `mod_delayed_work(…, 0)` | immediate |
| peer driver reloaded | peer `gen` changed | ≤ one poll |
| peer VM killed | heartbeat (or ring progress) unchanged for `hb_timeout_ms` | heartbeat age 520 ms at detection |
| ring index impossible, gen unchanged | `rx_broken`/`tx_broken` latched by the data path | counted as `ring_corruptions` |

**Liveness = heartbeat OR ring progress.** Under bulk TCP on the debug kernel, a guest's
link work (the heartbeat writer) was measured starved for 3.6–5.2 s while its data path kept
running → false "dead peer". Ring progress by the peer is equally good proof of life and
can't be starved independently of the traffic itself.

## 5. The reset sequence (process context, link work only) — know this cold

```
link_up = false                 # data-path gates (SPACE irq, xmit, selftest) see it first
netif_carrier_off
synchronize_irq(SPACE)          # a SPACE handler that saw link_up==true has finished
netif_tx_disable                # takes the TX lock: waits for a running xmit, stops queue
napi_disable                    # waits for a running poll; no new polls
wake tx_wq; lock st.lock        # a selftest producer notices !link_up and leaves
gen++ ; wmb ; zero own ring fields ; clear error latches
publish WAIT_PEER ; STATE kick  # peer reacts immediately
```
Why the data path takes **no lock**: it runs in xmit (BH-off) and NAPI (softirq) at line rate;
a mutex is impossible there and a spinlock would serialize every packet. Instead the control
path **stops** the data path (with primitives that wait for in-flight executions) before
touching shared state. Same pattern as real drivers' `ndo_stop`/reset paths.

Bring-up is the mirror: publish UP → `napi_enable` → first poll (drain + arm DATA kick) →
carrier on → wake queue → STATE kick.

## 6. Why the link work runs on `system_highpri_wq`

It writes the heartbeat. On a normal-priority worker it competed with the iperf3 sender
(99 % sys); WQ_HIGHPRI workers run at nice −20. (Not sufficient on its own — see §4.)

## 7. Results (debug kernel, `p4-ssr-debug.txt`)

| Scenario | Outage (ping gap) | TCP | Notes |
|---|---|---|---|
| (a) modem driver restart | 2.0 s | survives | incl. deliberate 1 s sleep + debug insmod |
| (c) AP driver restart | 2.1 s | survives | |
| (d) both restart | 1.3 s | survives | |
| (e) 100 consecutive (a) | — | — | 100/100, all `peer_down` |
| (b) modem VM kill -9 | 35.5 s | dies (expected) | detection at 520 ms heartbeat age; new peer ID 2→3; UP 37.7 s after kill (VM boot) |

## 8. Bugs found by these tests (debugging-log.md)
1. Stale `tx_broken` latch → endless reset loop under TCP (found via instrumentation).
2. SPACE irq could wake the queue during reset (ordering of `link_up` vs `netif_tx_disable`).
3. Heartbeat starvation → false peer death.

## Likely quiz questions
1. Why do we need generation numbers if we already have a `state` field?
2. Explain why "both acked current gens" implies "both rings are reset". Which barriers make
   it true?
3. Why does the data path not take a lock? How is it kept away from the reset?
4. What does `synchronize_irq` guarantee? Why is it before `netif_tx_disable`?
5. What does `napi_disable` wait for? Why must it never be called from the NAPI poll itself?
6. Heartbeat timeout: what's the trade-off when you shrink it? What did we see on this laptop?
7. Why can TCP survive a modem *driver* restart but not a modem *VM* kill?
8. Spot-the-bug candidates: clearing only `rx_broken` in `new_gen` (yes, that one); writing the
   ack before zeroing the ring fields; resetting while `link_up` is still true.
