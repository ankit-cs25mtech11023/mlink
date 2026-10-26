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
