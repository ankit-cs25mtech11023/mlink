# Debugging log

Format: symptom → hypotheses → evidence → root cause → fix → lesson. Newest at the bottom.

---

## 2026-10-27 — Laptop approaching thermal shutdown during the kernel build

- **Symptom:** debug kernel build at `-j6`: CPU package at 96 °C. Dropping to `-j4` changed
  nothing (still 95–96 °C).
- **Evidence:** `/sys/class/thermal/thermal_zone*/trip_point_*`: zone `B0D4` (processor) has
  a **critical trip at 98.05 °C** → the kernel would do an orderly power-off. The user is often
  off-campus, so that means a lockout. Pausing the build dropped it to 61 °C within seconds.
- **First fix attempt:** a guard that freezes the job (SIGSTOP to its process group) at
  ≥ 88 °C and resumes (SIGCONT) at ≤ 78 °C, sampling every 3 s. Result: the log showed
  `FREEZE at 96C / resume at 50C` every 3 s — the temperature jumped from ~50 °C to 96–97 °C
  **within one sample**, even at `-j3`.
- **Root cause:** Turbo Boost. The i5-8250U boosts from 1.6 GHz to 3.4 GHz; power scales
  roughly with f·V², so the 15 W chip briefly draws far more than the thin chassis can remove.
  The heat is in the power spikes, not in slowly accumulated heat (hence fast cool-down).
- **Fix:** turbo off (`mlink-bench-prep on` writes `intel_pstate/no_turbo=1`) + the guard
  sampling every 1 s. Turbo off at `-j3` and at `-j6` both hold **~70 °C** (the chip is then
  power-limited, so more jobs just use the cores better).
- **Lesson:** measure before tuning (`-j` was the wrong knob); know the platform's trip points;
  a remote machine needs a safety interlock, not just a careful operator.

---

## 2026-10-27 — `fixdep: error opening file: net/netfilter/.nfnetlink.o.d`

- **Symptom:** the restarted `-j6` build failed with `fixdep: error opening file ... .o.d:
  No such file or directory` in two different files.
- **Hypotheses:** (1) disk/ccache corruption; (2) a partially written object from an earlier
  killed build; (3) two `make` processes writing the same tree.
- **Evidence:** the errors were "missing dependency file right after compiling", in different
  files each time — the signature of another process deleting/rewriting the same outputs.
  `thermal-run.sh` starts the job with `setsid` (own session, so the guard can signal its whole
  process group). Killing the tmux session sends SIGHUP to the pane's foreground process group
  — which no longer contains the job.
- **Root cause:** each "restart" killed only the wrapper; the previous `make -j3` kept running
  in its own session and raced the new `make -j6` on the same `build/debug` tree.
- **Fix:** `thermal-run.sh` traps HUP/INT/TERM and forwards SIGTERM (+SIGCONT, in case the job
  is frozen) to the job's process group. Verified: HUP to the wrapper → job gone.
- **Lesson:** `setsid` detaches a process from terminal job control — whoever does that owns
  its lifetime. Same idea as a driver that schedules work: whoever schedules it must cancel it
  (`cancel_delayed_work_sync` in `remove()`).

---

## 2026-10-27 — P2.4: trying to provoke a lost wakeup with `unsafe_no_mb=1`

- **Goal:** show the store-buffering race behind kick suppression at runtime.
- **Setup:** `unsafe_no_mb=1` on both guests drops `virt_mb()` from all four kick-path
  helpers. Detectors: producer `lost_space_wakeups` (space appeared but no SPACE irq arrived
  while armed); consumer `rx_stalls` (link work sees `want_kick` armed + data pending on two
  consecutive 100 ms polls, then recovers).
- **Result:** 2 × 10 M messages with barriers removed: **0 lost wakeups, 0 stalls**, same
  throughput (~268k msg/s) as with barriers. File: `bench/results/p2-lost-wakeup-debug.txt`.
- **Why it doesn't reproduce here (analysis, not proof):**
  1. Streaming keeps the ring mostly full (~120k space-waits per 10 M): the consumer rarely
     goes idle exactly while the producer publishes, so the race window is rarely entered.
  2. Every kick is an MMIO write → VM exit, which is serializing on x86 and drains the
     store buffer on that path.
  3. `wait_event`/`queue_work` take spinlocks (LOCK-prefixed atomics = full barriers on x86)
     close to the arm/check points, which often provide the missing ordering by accident.
- **Conclusion:** the barriers are proven necessary by LKMM (`litmus/`, two "Sometimes"
  results); the runtime test shows only that this workload rarely hits the window. Next idea:
  a *paced* selftest (one message, short idle, repeat) to maximise arm/publish collisions.
- **Lesson:** absence of a failure in testing ≠ correctness; memory-ordering bugs need a model
  checker or a targeted stress that lives inside the race window.

---

## 2026-10-27 — P4.5: endless "ring error" reset loop after a restart under TCP

- **Symptom:** SSR scenario (a) (modem driver restart with live iperf3): the link recovered
  and TCP survived, but the AP logged ~180 resets in 30 s, all `ring error`, each ~110 ms
  (one link-work period) after `link UP`. Scenario (e) — 100 restarts *without* traffic — was
  clean.
- **Hypothesis 1:** a SPACE irq wakes the TX queue between `netif_tx_disable()` and the
  index reset, so `xmit`'s `idx++` races `reset_producer()`'s `idx = 0`. Real race (the reset
  cleared `link_up` *after* disabling TX, and the SPACE handler only checks `link_up`). Fixed:
  `link_up = false` first → `synchronize_irq(SPACE)` → `netif_tx_disable` → `napi_disable`;
  `xmit` also refuses to run unless `link_up`. **Loop persisted** → not the (only) cause.
- **Evidence (instrumentation instead of guessing):** the ring now remembers the peer index
  that made a check fail, and the reset prints it:
  `ring error: tx_broken=1 idx=0 peer_cons=0 | rx_broken=0 ...` — the ring was perfectly
  consistent; the **flag** was stale.
- **Root cause:** `mlink_link_new_gen()` cleared `rx_broken` but not `tx_broken` (added later in
  the same patch). The first legitimate restart latched `tx_broken`; every subsequent UP was
  reset on the next tick. No traffic → nothing ever set `tx_broken` → (e) was clean.
- **Fix:** clear both latches when a new generation starts. Verified: (a)(c)(d) one reset each,
  correct reason, TCP survives.
- **Lesson:** when adding a new piece of state, grep for every place the sibling state is
  reset. And: instrument the decision point (print the values that triggered it) before
  theorising — the second hypothesis was found in one run.

---

## 2026-10-27 — P4.5: heartbeat starvation → false "peer dead" under bulk TCP

- **Symptom:** after the loop fix, (c) still showed a `heartbeat timeout` reset on the modem
  while the AP was alive, at random points during iperf3.
- **Measurement:** added `max_hb_gap_ms` (longest peer-heartbeat interval while UP). With
  `hb_timeout_ms=5000` and 70 s of TCP: **3.6 s and 5.2 s** gaps — each guest's link work
  (which writes the heartbeat) sometimes didn't run for seconds, even after moving it to
  `system_highpri_wq` (nice −20 workers).
- **Suspected cause (not yet proven):** debug kernel (KASAN) + `PREEMPT_VOLUNTARY`: long
  non-preemptible kernel stretches in the TCP send path and softirq processing on 2 vCPUs;
  the heartbeat timer and work can't run. To test later: boot with `preempt=full`, perf kernel.
- **Fix (design):** liveness = heartbeat **or ring progress** by the peer (its `prod` on the
  ring it sends on, its `cons` on the ring it consumes). A dead peer makes neither; a busy peer
  whose housekeeping is starved still moves its rings. Counter `hb_saved_by_ring` shows when
  this rescued the link. Result: (a)(c)(d) with no false timeouts.
- **Lesson / interview point:** failure detectors trade detection latency against false
  positives; a heartbeat produced by a *different* execution context than the one doing the
  work can starve independently — use the work itself as evidence of life where possible.

---

## 2026-10-27 — P5.1: shm_chaos "flags" → silent black hole (link UP, 100 % loss)

- **Symptom:** after a storm of random `want_kick`/`want_space` writes, both sides stayed UP
  with no reset, but ping failed indefinitely. Other chaos modes self-healed.
- **First fix (needed, not sufficient):** a netdev TX watchdog — queue stopped while the ring
  has room for two polls → wake it (`tx_stalls`).
- **Evidence:** ring dump on the modem: RX ring full (`prod - cons = 256`) with
  `want_kick = 0`, NAPI idle. `want_kick = 0` means "I'm polling, don't kick", so the producer
  stopped kicking; the consumer was actually asleep.
- **Root cause:** the RX lost-kick watchdog only fired when `want_kick == 1` — it trusted a
  flag that lives in shared memory, i.e. exactly the untrusted input the design says never to
  trust.
- **Fix:** decide on local facts only: data pending and our consumer index unchanged across
  two polls → schedule NAPI (which re-arms the flag correctly). Re-run: ping OK at every
  second of recovery, 0 resets; full campaign: all 7 modes recover in ≤ 3.2 s.
- **Lesson:** "untrusted shared memory" applies to *control* decisions too (watchdogs,
  liveness), not just lengths and indices. A fault injector finds what code review missed.

## 2026-10-27 — P5.5: one 22 s soft lockup on the debug-kernel modem in a 1-h soak

- **Symptom:** `watchdog: BUG: soft lockup - CPU#0 stuck for 22s! [iperf3]` on the modem, once,
  in the first 1-h soak (`p5-soak-long-debug.txt`). Trace: `<IRQ>` → `net_rx_action` →
  `mlink_napi_poll` → `mlink_rx_poll` → `mlink_net_rx` → `napi_alloc_skb` → KASAN/kmemleak
  stack saving (`unwind_next_frame`).
- **Not reproduced:** 20-min and 75-min debug soaks with IRQ/softirq sampling
  (`p5-soak-repro-debug.txt`), 1-h perf soak (`perf/p5-soak-long-perf.txt`): all clean.
- **Hypothesis 1, unbounded poll loop (our bug):** ruled out by review — every consumed entry
  (errors included) counts against the NAPI budget, budget exhaustion returns `budget`, idle
  re-arms with the SB re-check.
- **Hypothesis 2, doorbell storm:** kick suppression is cooperative (the *peer* honours
  `want_kick` in shm; `shm_chaos` scribbles on it), and since v6.4 net softirqs run again at
  every hardirq exit, so a storm + KASAN-expensive allocations could pin a CPU in irq/softirq
  context. Test `bench/run_p5_storm.sh 60 2` (`p5-storm-debug.txt`): DATA irqs 99 → 7178/s,
  NET_RX 103 → 4471/s, throughput unchanged, **no lockup** in 60 s. Not confirmed.
- **Status (timeboxed, fallback taken):** unresolved, debug-kernel only (the expensive frames
  are KASAN/kmemleak instrumentation), not reproduced in 95 min of further debug soak nor on
  the perf kernel. P5.5's criterion was met by the 75-min debug soak and the 1-h perf soak; the
  ticked box notes the one failure. Candidate hardening if it recurs: mask the DATA MSI-X vector
  locally while NAPI polls (as NICs mask in hardware) so the peer cannot drive our IRQ rate.

## 2026-10-27 — P9: control shell to the arm64 initramfs guest loses or delays commands

- **Symptom:** commands sent to the arm64 guest's shell came back late (the output of call N
  arrived with call N+1), timed out, or were lost; exit codes were attributed to the wrong call.
- **Hypotheses, in order:** (1) busybox `ash` echo → yes, its line editor echoes on a tty even
  with `stty -echo`; fixed by feeding sh through a pipe. (2) `stty` lost → yes, settings reset
  when the tty is closed; apply on the fd that stays open. (3) host reconnects → yes, a
  virtio-console disconnect hangs up hvc0 in the guest, so input around reconnects was lost;
  fixed with a persistent bridge (`scripts/host/arm64-bridge.py`). (4) still timing out →
  probe on the raw socket showed **a one-line lag**: a command ran only when the next line
  arrived.
- **Root cause (4):** not fully pinned down — the last line (or its output) on the guest's
  serial path under TCG only moved on further input. Same behaviour on virtio console and
  PL011, so likely in the tty/console path rather than one device model.
- **Fix:** the bridge sends an empty line (a no-op for sh) after every 1 s without output.
  Verified: `uname`, `false` (rc 1), multi-command batches, the full P9 run.
- **Side lessons:** `pkill -f pattern` / `pgrep -f` over ssh match the `zsh -c` command line
  that contains the pattern (killed my own ssh session once): anchor the pattern
  (`"^python3 .*arm64-bridge[.]py"`). zsh does not word-split `$VAR`, so options in a variable
  arrive as one argument: put such commands in bash scripts.
- **Time:** ~45 min, under the 2 h timebox.
