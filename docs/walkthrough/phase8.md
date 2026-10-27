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

## 6. P8.3 — the real Zephyr IP stack on the ring (`src/net.c`, D20)

Built with `zephyr-modem.sh build net` (`net.conf` sets `CONFIG_MLINK_MODEM_NET`); the same
`main.c` link/ring core, but packets go to Zephyr's stack instead of `reflect()`.

- **Raw IP on both sides.** Linux registers `mlink0` as `ARPHRD_RAWIP` (no link header, like
  `mhi_net`); Zephyr's equivalent is a `DUMMY_L2` interface (`NET_DEVICE_INIT(... DUMMY_L2 ...)`).
  The Zephyr net core looks at the version nibble to choose IPv4/IPv6, so nothing else is
  needed: the ring payload is the IP packet.
- **Checksum completion (key lines: `csum_complete()`).** The AP sends TCP/UDP as
  CHECKSUM_PARTIAL (D17): the checksum field holds only the pseudo-header seed. Linux on the
  modem side would pass that on as CHECKSUM_PARTIAL; Zephyr's stack *verifies* L4 checksums, so
  the modem finishes the sum over `[csum_start, len)` and stores `~sum` (0 → 0xffff) like
  `skb_checksum_help()`. `csum_start/csum_offset` are peer-written, so they are bounded against
  the packet length before use. 52 928 packets were completed this way in one TCP run.
- **Who produces into ring B now? (key lines: `mlink_net_send()`).** In loopback mode only the
  poll thread produced. Now `send()` is called from Zephyr's net and socket threads, so several
  threads could produce: the ring is single-producer by design, so a `k_mutex` makes them take
  turns. The link reset zeroes the producer index under the same mutex, after clearing
  `link_up` (same order as the Linux reset: stop new work first, then reset).
- **Full ring:** arm `want_space` + re-check (SB pattern), then **drop the mutex** and sleep on
  a semaphore that the SPACE doorbell gives, with a 100 ms timeout. Holding the mutex while
  sleeping would block the link reset; the timeout doubles as a lost-kick watchdog.
- **Kconfig trap.** On QEMU boards `NETWORKING=y` defaults to SLIP over a UART pipe, which took
  over the serial console: the first boot printed nothing after SeaBIOS. `NET_SLIP_TAP=n` +
  `NET_DRIVERS=n` leaves `mlink0` as the only interface.

Results (`bench/results/p8-zephyr-net.txt`): ping, UDP echo 64/1400 B (0 lost), TCP echo 4 MiB
byte-exact, TCP discard 32 MiB at ~6 Mbit/s (the Zephyr QEMU used 128 % of a core: TCG-bound),
SSR 3 rounds (recovery 39–51 ms). A TCP connection open across a modem crash gets
**ECONNRESET ~2 s** after the restart (the new modem RSTs a connection it has no state for);
with the modem left dead the same client instead times out after 30 s. Not exercised: ring B
full (Zephyr never out-produced the AP).

## 6b. The control plane on Zephyr (`src/ctrl.c`)

`tools/mlinkd` serves P7 requests on the Linux modem through `/dev/mlink_ctrl`. Zephyr has no
such device, so `ctrl.c` speaks rings C/D directly with the kernel's framing
(`driver/mlink_ctrl.c`): one message per slot, `proto = MLINK_PROTO_CTRL`, desc `seq =
txn_id`, and **no kick suppression** on these rings (low rate, every publish or consume rings
CTRL). It reuses `tools/mlink_ctrl_lib.h` (`msg_init`, `tlv_put`, bounds-checked `tlv_get`)
and the compat shim gained the `__u*` types `mlink_ctrl.h` uses. Same rules as everywhere:
copy the slot first, then validate only the copy (`sizeof(hdr) + h->len == d.len`, type REQ).

SET_DATA_CALL down makes the modem drop data traffic (and turns the Zephyr carrier off in
IP-stack mode), then answers and sends the IND. With the unchanged `mlinkctl` on the AP
(`bench/results/p8-zephyr-ctrl.txt`): info, stats (3 pings → 3 packets / 252 bytes each way),
call down → ping 100 % loss, call up → OK, 500 GET_LINK_INFO round trips p50 438 µs (TCG),
504/504 requests answered, 0 invalid.

## 7. Questions to be ready for

1. Why must `virt_mb()` be a real fence on a uniprocessor Zephyr build?
2. Why does reflecting a CHECKSUM_PARTIAL UDP packet need no checksum work at all?
3. Write the RFC 1624 update for changing ICMP type 8 → 0. Why not just recompute?
4. Why does Zephyr read its old `gen` from shared memory instead of starting at 1?
5. The AP detected the restart in ~1 ms of heartbeat age. Which check fired, and why first?
6. Spot the bug: an earlier `reflect()` required `tot >= ihl + 8` for every packet. What broke?
7. What does Zephyr do when ring B is full, and how does the AP learn it may send again?
8. Why does the IP-stack mode need a mutex on ring B when the loopback mode does not?
9. Why must `mlink_net_send()` release the mutex before sleeping on the SPACE semaphore?
10. Why does the modem have to complete CHECKSUM_PARTIAL checksums, while a Linux modem would not?
11. A TCP transfer dies with ECONNRESET after a modem restart, but with a timeout when the modem
    stays dead. Explain both.
