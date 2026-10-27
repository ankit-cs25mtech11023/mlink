# Phase 8 walkthrough — a Zephyr "modem" on the same shared-memory ABI

> Goal: replace the Linux modem VM with a tiny RTOS image and show that the AP driver does not
> notice the difference. This is what a real modem is: different OS, same wire contract.
> Evidence: `bench/results/p8-zephyr-doorbell.txt`, `bench/results/p8-zephyr-modem.txt`.
> Decision record: plan.md D19.

## 1. The risk check (P8.1)

Before writing anything, answer "can Zephyr even be a peer?". Zephyr ships an ivshmem driver
(`drivers/virtualization/virt_ivshmem.c`) with doorbell + MSI-X support and a sample for
`qemu_x86_64`. Two things had to change for our server:

| Stock sample | Our server | Override (`zephyr/p81-doorbell.conf`) |
|---|---|---|
| 2 MSI-X vectors | 5 (`ivshmem-server -n 5`, ABI v3) | `CONFIG_IVSHMEM_MSI_X_VECTORS=5` |
| 8 KiB malloc arena | sample copies the whole 4 MiB region | 4.1 MiB arena |

The first run failed with "Could not allocate message buffer", so the problem was the heap,
not the doorbell. After the override, `ivshmem-client` → `int 5 0` printed
`received IRQ and full message: MLNK^C`, i.e. Zephyr was reading **our** header (magic `MLNK`,
abi_version 3) from the same RAM the Linux AP uses.

## 2. One ring implementation, two kernels

`zephyr/mlink-modem/CMakeLists.txt` compiles `driver/mlink_ring.c` itself, unchanged. It
needs only what the kernel's headers would give it, so `zephyr/mlink-modem/compat/` supplies:

| Kernel name | Zephyr build | Why it is correct |
|---|---|---|
| `READ_ONCE` / `WRITE_ONCE` | volatile access | one untorn access the compiler cannot merge or repeat |
| `virt_load_acquire` / `virt_store_release` | `__atomic_load_n(ACQUIRE)` / `__atomic_store_n(RELEASE)` | same release/acquire publication as Linux |
| `virt_mb()` | `__atomic_thread_fence(SEQ_CST)` | the SB kick pattern needs store→load ordering (x86: `mfence`/locked op) |
| `virt_wmb` / `virt_rmb` | release / acquire fence | on x86 TSO a compiler barrier; correct by construction on weaker CPUs |

**Key line** (`compat/asm/barrier.h`): Zephyr runs on one vCPU here, so a Zephyr developer
might reach for a compiler-only barrier. The *other side of the ring is a different VM on
another physical CPU*, so these must be real CPU barriers. That is exactly why Linux has
`virt_*` separate from `smp_*` (which become compiler barriers on `!CONFIG_SMP`).

## 3. The modem side in one thread (`src/main.c`)

```
for (;;) {
    k_poll(5 doorbell signals, timeout = until next 50 ms tick)   // a kick wakes us at once
    clear signaled events
    if tick due: link_tick()          // heartbeat, handshake, reset decisions
    if link UP:  data_poll()          // drain ring A, reflect into ring B
}
```

- `link_tick()` is `mlink_link_work()` for the modem role, line for line: read the AP block
  once (magic with acquire, `virt_rmb()` between ack and gen), ack the AP's gen, go UP when the
  AP has acked ours, reset on peer DOWN / gen change / heartbeat stale / ring error.
- `link_new_gen()` reads the old gen **from shared memory** and adds 1. Zephyr has no state
  across a reboot, but the shm does, so the AP always sees a new gen. This is what makes the
  fast restart path work (§5).
- It also resets the *control* rings it owns although it never serves them: a stale producer
  index left by an earlier Linux modem would look like corruption to the AP.
- `data_poll()` is a NAPI poll without NAPI: disarm `want_kick`, consume up to 64, release
  `cons` once, SPACE-kick if the AP asked, DATA-kick if the AP asked, then arm + re-check
  before sleeping (the SB pattern from phase 2). When ring B is full it leaves the packet in
  ring A and arms `want_space`, i.e. back-pressure travels end to end.

## 4. The loopback (`reflect()`)

Modems have a "loopback test mode" that bounces IP traffic back. Rules:

1. **Copy first, parse the copy** (`memcpy(pkt, d.data, d.len)`): never parse shared memory in
   place, since the peer could change it between check and use (phase 2's double-fetch rule).
2. **Swapping two 16-bit-aligned fields does not change a one's complement sum.** Swapping
   src/dst IP leaves the IP header checksum valid; swapping UDP ports leaves the UDP checksum
   valid; and a CHECKSUM_PARTIAL seed (pseudo-header sum, D17) is still right, so the desc
   `flags/csum_start/csum_offset` are reflected unchanged and the AP finishes the job.
3. ICMP echo request → reply changes one 16-bit word (`type/code` 0x0800 → 0x0000), so the
   checksum gets an **RFC 1624** incremental update: `HC' = ~(~HC + ~m + m')`
   (`csum_replace16()`, the same idea as the kernel's `csum_replace2()`).
4. **Fragments:** a non-first fragment has no L4 header, so only the addresses are swapped. The
   first fragment's L4 checksum covers the whole datagram, so the incremental update is still
   right. Found by testing: `ping -s 3000` at MTU 1500 failed 5/5 until this was handled.

## 5. Results and what they mean

| Test | Result |
|---|---|
| ping 20× / 3000 B fragmented / 3900 B at MTU 4000 | all replies |
| UDP 64 B / 3000 B, 2000 / 500 round trips | 0 lost (p50 ~0.29 / 0.26 ms; TCG modem) |
| flood, 10 000 echoes, 400 in flight | AP IP layer: 10 000 out, 10 000 replies in |
| modem killed + restarted, 5 rounds | detected via **gen change**, recovery 44–53 ms, ping gap 319–343 ms |
| modem killed, not restarted | **heartbeat timeout**, detected 521 ms after last heartbeat |
| AP driver reloaded | Zephyr: "peer went down" → re-handshake, ping OK |

- Two detection paths showed up naturally. Zephyr boots in ~0.3 s, faster than the 500 ms
  heartbeat timeout, so the AP learns of the crash from the **new generation number** before
  the heartbeat could expire. A modem that stays dead is caught by the heartbeat (521 ms, the
  same as the Linux modem's 520 ms in E6, as expected: the AP code is the same).
- `ping -f` reported ~18 % "loss" while the kernel counted every reply. The modem under TCG is
  slower than the AP, so replies come back in large bursts and `ping`'s socket misses some.
  Lesson: **measure at the layer you are making a claim about** (nstat), not from the tool's
  summary line.
- Timings with the modem under TCG are functional, not performance numbers.

## 6. Questions to be ready for

1. Why must `virt_mb()` be a real fence on a uniprocessor Zephyr build?
2. Why does reflecting a CHECKSUM_PARTIAL UDP packet need no checksum work at all?
3. Write the RFC 1624 update for changing ICMP type 8 → 0. Why not just recompute?
4. Why does Zephyr read its old `gen` from shared memory instead of starting at 1?
5. The AP detected the restart in ~1 ms of heartbeat age. Which check fired, and why first?
6. Spot the bug: an earlier `reflect()` required `tot >= ihl + 8` for every packet. What broke?
7. What does Zephyr do when ring B is full, and how does the AP learn it may send again?
