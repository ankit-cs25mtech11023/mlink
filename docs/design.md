# mlink design (v2 — as implemented)

> Status: describes the code at the commit that last touched this file. Cross-checked against
> the implementation on 2026-10-27 (v1 was the pre-implementation draft). The user's P1.1
> review is still pending — open questions in §14. Changes that contradict `plan.md` go into
> `plan.md` first.

## 1. Picture

```
  VM "ap"                                         VM "modem"
  ┌──────────────────────────┐                    ┌──────────────────────────┐
  │ TCP/IP stack             │                    │ TCP/IP stack             │
  │ mlink0 (raw IP, NAPI)    │                    │ mlink0 (raw IP, NAPI)    │
  │ mlink.ko role=ap         │                    │ mlink.ko role=modem      │
  │  BAR0 regs  BAR2 shm ────┼──── 4 MiB POSIX ───┼──── BAR2 shm  BAR0 regs  │
  │     │ Doorbell write     │   shared memory    │     ▲ MSI-X vector       │
  └─────┼────────────────────┘                    └─────┼────────────────────┘
        └──── eventfd (ivshmem-server wiring) ──────────┘
```

A **kick** = `iowrite32((peer_id << 16) | vector, BAR0 + 0x0C)`. QEMU (VM exit) signals the
peer's eventfd for that vector; the peer's QEMU injects that MSI-X vector into the peer guest.
Shared memory carries all data and state; kicks only say "go look".

## 2. Device facts (ivshmem-doorbell, PCI 1af4:1110)

| BAR | Contents | How we use it |
|---|---|---|
| BAR0 | 256 B MMIO: `IntrMask` 0x00, `IntrStatus` 0x04 (INTx only), `IVPosition` 0x08 (RO), `Doorbell` 0x0C (WO) | `pcim_iomap_region`; `ioread32(IVPosition)` once at probe; `iowrite32(Doorbell)` per kick |
| BAR1 | MSI-X table + PBA | PCI core (`pci_alloc_irq_vectors(4, 4, PCI_IRQ_MSIX)`) |
| BAR2 | shared memory (4 MiB, ≥ `MLINK_LAYOUT_END`) | `pcim_request_region` + `devm_memremap(MEMREMAP_WB)` |

- `pci_set_master()` is required: QEMU delivers MSI-X writes through the bus-master address space.
- Peer IDs are assigned by ivshmem-server and **change when a VM reconnects** → each side
  publishes its own ID in its state block; the other side re-reads it every link poll.
- Kicks to an absent peer are silently dropped; the device never reports peer death → §9.

## 3. Roles and parameters

One `mlink.ko`; `role=ap|modem` (required, validated in `mlink_init` before the PCI driver is
registered). The **AP owns the global header**; the data path is symmetric.

| Parameter | Default | Meaning |
|---|---|---|
| `role` | — (required) | `ap` or `modem` |
| `hb_ms` | 100 | link poll / heartbeat period |
| `hb_timeout_ms` | 500 | peer declared lost after this long without heartbeat **or** ring progress |
| `napi_weight` | 64 | NAPI budget, clamped 1..64 (`NAPI_POLL_WEIGHT`) |
| `kick_suppress` | 1 | kick only when the peer asked (0 = kick every publish; experiment) |
| `unsafe_no_mb` | 0 | **experiment only**: drop the kick-path full barriers (P2.4) |

## 4. Shared-memory layout (4 MiB, fixed; `driver/mlink_shm.h` + `static_assert`s)

| Offset | Size | Region | Writer |
|---|---|---|---|
| `0x000000` | 64 B used (4 KiB page) | global header | AP |
| `0x001000` | 64 B | state block AP | AP |
| `0x001040` | 64 B | state block MODEM | modem |
| `0x002000` | 256 B (4 KiB page) | ring A (AP→MODEM) control: 4 cache lines | split, §5.1 |
| `0x003000` | 4 KiB | ring A descriptors `desc[256]` × 16 B | AP |
| `0x004000` | 1 MiB | ring A slots `slot[256]` × 4 KiB | AP |
| `0x104000` | | ring B (MODEM→AP): same structure | modem / AP |
| `0x206000` | 24 KiB | control ring C (AP→MODEM): 16 × 1 KiB slots | AP / modem (split) |
| `0x20C000` | 24 KiB | control ring D (MODEM→AP) | modem / AP |
| `0x212000` | ~1.9 MiB | reserved | — |

`N = 256` (power of two), slot = 4096 B → MTU ≤ 4096. The tool `shm_chaos` includes this same
header (userspace-safe), so layout drift between tool and driver is a compile error.

### 4.1 Global header (64 B)
`magic` "MLNK", `abi_version` (**3**: v2 added checksum offload, v3 control rings), `ring_entries`, `slot_size`, `ring_a_off`, `ring_b_off`,
`layout_crc` = `crc32_le` over `abi_version..ring_b_off`.
- **Publish (AP):** `magic = 0`, `virt_wmb()`, write fields, `virt_store_release(magic)`.
- **Validate:** `virt_load_acquire(magic)`, read every field once into a local, compare all
  fields **and** the CRC with this build's constants. Any difference = refuse (no guessing
  across version skew).
- **Repair:** the AP re-validates every link poll and rewrites a damaged header
  (`hdr_repairs`). The modem validates once before starting; afterwards it uses its compiled
  constants, so later header damage cannot change how it indexes memory.

### 4.2 State block (one 64 B cache line per side, owner writes only)
`magic`, `peer_id` (own IVPosition), `state` (DOWN 0 / WAIT_PEER 1 / UP 2), `gen`,
`peer_gen_seen`, `heartbeat`, `mtu`, `features` (unused, 0).
Published with `virt_store_release(magic)` after the other fields. `mtu` is informational:
the receiver accepts any length ≤ slot size regardless of its own MTU.

## 5. Rings

### 5.1 Control block — one writer per cache line
| Line | Field | Writer | Meaning |
|---|---|---|---|
| 0 | `prod` | producer | free-running count of published descriptors |
| 1 | `cons` | consumer | free-running count of consumed descriptors |
| 2 | `want_space` | producer | "I'm stopped on a full ring; SPACE-kick me when you free slots" |
| 3 | `want_kick` | consumer | "I'm idle; DATA-kick me when you publish" |

### 5.2 Descriptor (16 B, ABI v2) — descriptor *i* always describes slot *i*
| Field | Meaning |
|---|---|
| `u32 len` | payload bytes in the slot, 1..4096 |
| `u16 proto` | `0x0800` IPv4, `0x86dd` IPv6, `0xfffe` selftest |
| `u16 flags` | `MLINK_DESC_F_CSUM_PARTIAL` (0x1): L4 checksum not filled in |
| `u32 seq` | producer sequence (selftest, debugging) |
| `u16 csum_start`, `u16 csum_offset` | valid with `CSUM_PARTIAL` (virtio-style offload metadata) |

No peer-supplied offsets or pointers are ever used for addressing; `csum_*` are bounds-checked
offsets *within* the copied packet.

### 5.3 Indices
Free-running `u32`; slot = `idx & (N-1)`; fill = `prod - cons` (unsigned wrap is correct).
Invariant `prod - cons ≤ N`. Each side keeps a **local copy** of the index it owns and never
reads it back from shm; it reads only the other side's index.

## 6. Memory ordering (`virt_*` barriers; x86 little-endian both sides)

BAR2 is ordinary write-back memory shared with another VM, so `virt_*` barriers (they stay
real barriers even in a `CONFIG_SMP=n` guest) and `READ_ONCE/WRITE_ONCE` on every shared field.

| Operation | Sequence | Pattern / litmus |
|---|---|---|
| publish | slot payload, desc fields → `virt_store_release(prod)` | MP, `mlink-publish` |
| peek | `virt_load_acquire(prod)` → desc fields once → payload | MP |
| release | after all slot reads of a NAPI poll → `virt_store_release(cons)` (**once per poll**, batched) | LB, `mlink-slot-reuse` |
| free (producer) | `virt_load_acquire(cons)` → then overwrite slots | LB |
| kick decision (DATA) | producer: store prod → `virt_mb` → load `want_kick`; consumer: store `want_kick=1` → `virt_mb` → re-load `prod` | SB, `mlink-kick` (+2 broken variants) |
| kick decision (SPACE) | consumer: store cons → `virt_mb` → load `want_space`; producer: store `want_space=1` → `virt_mb` → re-load `cons` | SB |
| new generation | `gen++` → `virt_wmb` → zero own ring fields | §9.2 |
| ack | own reset done → `virt_wmb` → write `peer_gen_seen`; reader: load ack → `virt_rmb` → load `gen` | §9.2 |

A store-release does **not** order a store before a *later load*; that is why the kick paths
need full barriers (`mlink-kick-no-producer-mb`: Sometimes).

## 7. Shared memory is untrusted input (threat model)

The peer may be buggy, crashed mid-write, or hostile; `tools/shm_chaos` attacks from the host.
1. **Read once, validate the copy** (no double fetch): indices, `len`, `proto`, `flags`,
   `csum_*`, state-block fields, header fields.
2. **Bounds:** `prod - cons ≤ N`; `1 ≤ len ≤ 4096`; `csum_start + csum_offset + 2 ≤ len`;
   `peer_id ≤ 0xFFFF` and ≠ own; peer `state` ∈ {WAIT_PEER, UP} (anything else = down).
3. **Classify from our copy:** IP version taken from `skb->data[0]` after the copy.
4. **Escalation:** impossible index → latch `rx_broken`/`tx_broken` → link work resets.
   Peer gen changed → restart (normal); unchanged → counted as `ring_corruptions`.
5. **Control decisions use local facts only.** Watchdogs never trust shm flags (a cleared
   `want_kick` produced a silent black hole in P5.1 — debugging-log).
6. **Not defended:** a peer flooding doorbells causes an interrupt storm (no per-vector rate
   limiting; MSI-X can't be masked through ivshmem registers). Data integrity of IP payload is
   the stack's job (except with `CSUM_PARTIAL`, where shared memory is trusted not to flip
   bits — same assumption as virtio/veth).

## 8. Interrupt vectors and execution contexts

| Vector | Sender meaning | Receiver (hard-IRQ) action |
|---|---|---|
| 0 DATA | new descriptors in your RX ring | `want_kick = 0`, `napi_schedule_irqoff` |
| 1 SPACE | I freed slots in your TX ring | wake netdev queue if stopped + space; wake selftest `tx_wq` |
| 2 STATE | my state block changed | `mod_delayed_work(link_work, 0)` |
| 3 TEST | ping-pong | modem: kick back; AP: timestamp, `complete()` |
| 4 CTRL | control ring: new message and/or space freed | queue control RX work, wake blocked writers |

All handlers do nothing until `ready` (set last in probe, cleared first in remove).

| Context | Code | May sleep? | Touches |
|---|---|---|---|
| hard IRQ | 4 handlers above | no | flags, NAPI schedule, queue wake |
| softirq (NAPI) | `mlink_napi_poll` → `mlink_rx_poll` → `mlink_net_rx` | no | RX ring (consumer), skbs, rx stats |
| BH-off, TX lock | `mlink_start_xmit` | no | TX ring (producer), tx stats |
| process (WQ_HIGHPRI) | `mlink_link_work` every `hb_ms` | yes | state blocks, heartbeat, resets, watchdogs, header repair |
| process (debugfs) | pingpong, selftest TX | yes | TEST vector, TX ring (selftest owner) |

**Single producer per ring:** `tx_owner` (`atomic_cmpxchg`) admits either the netdev
(`ndo_open`) or the selftest, never both.
**Locks:** none on the data path. `st.lock` (selftest run), `pp.lock` (pingpong run). The
link work is serialized by the workqueue (one instance), and it *stops* the data path
(`netif_tx_disable`, `napi_disable`) before changing anything the data path uses.

## 9. Link state machine & Subsystem Restart

### 9.1 States (published in the state block)
```
            probe / header valid
   DOWN ─────────────────────────► WAIT_PEER ──── two-way gen ack + peer alive ────► UP
     ▲                                ▲                                              │
     │ rmmod (state=DOWN,             │      reset: link_up=0 → stop data path →     │
     │  magic=0, STATE kick)          └───── gen++ → zero own ring fields ◄──────────┘
```
UP → reset when: peer block invalid or peer `state` ∉ {WAIT_PEER, UP}; peer `gen` changed;
no heartbeat **and** no ring progress for `hb_timeout_ms`; `rx_broken`/`tx_broken` latched.

### 9.2 Why the handshake is safe
Every (re)start bumps `gen` **before** zeroing the owner's ring fields, and the ack of the
peer's gen is written **after** that (`virt_wmb`; reader `virt_rmb`). So "each side has acked
the other's *current* gen" ⇒ both finished resetting ⇒ all four indices are zero ⇒ the data
path can start. A peer that observes our zeroed index also observes our new gen, so it treats
the impossible index as a restart, not corruption.

### 9.3 Liveness
Alive = heartbeat changed **or** the peer's ring progress fingerprint (its `prod` on our RX
ring, its `cons` on our TX ring) changed within `hb_timeout_ms`. Ring progress was added after
measuring heartbeat starvation (3.6–5.2 s) under bulk TCP on the debug kernel.

### 9.4 Reset sequence (link work only)
`link_up = false` → carrier off → `synchronize_irq(SPACE)` → `netif_tx_disable` →
`napi_disable` → wake `tx_wq`, take `st.lock` (selftest stops) → `gen++`, `virt_wmb`, zero
own `prod`/`want_space`/`cons`/`want_kick`, clear both error latches → publish WAIT_PEER →
STATE kick.
**Bring-up:** publish UP → `link_up = true` → `napi_enable` → first poll (drains, arms DATA
kick) → carrier on → wake queue → STATE kick.

### 9.5 Watchdogs (UP, every link poll)
- **RX:** data pending and consumer index unchanged for 2 polls → schedule NAPI (`rx_stalls`).
- **TX:** queue stopped while the ring has space for 2 polls → wake (`tx_stalls`).

## 10. Network device

Raw-IP point-to-point like `mhi_net`: `ARPHRD_RAWIP`, `IFF_POINTOPOINT | IFF_NOARP`, no
header ops, MTU 68..4096 (default 1500), `NETIF_F_SG | NETIF_F_HW_CSUM` (D17).
- **TX:** drop unless link UP; map protocol; ring full while queue running → stop, arm
  SPACE, re-check, `NETDEV_TX_BUSY`; else `skb_copy_bits` into the slot, fill checksum
  metadata, publish; on the last free slot stop + arm + re-check; `xmit_more` → one kick per
  batch; kick only if `want_kick` (full barrier).
- **RX:** `napi_alloc_skb`, copy out, classify by IP version, optional
  `skb_partial_csum_set`, `napi_gro_receive`; one `cons` release per poll; SPACE kick if asked;
  `napi_complete_done` → arm DATA kick → re-check.
- **Stats:** `ndo_get_stats64` with `u64_stats_sync`; driver counters via `ethtool -S` and
  debugfs `stats` (one shared table).

## 11. Teardown order (`mlink_remove`)
`dying = true` → wake `tx_wq` → `debugfs_remove` (waits for running pingpong/selftest) →
`cancel_delayed_work_sync(link_work)` → `mlink_link_stop` (state DOWN, magic 0, STATE kick) →
`ready = false` → `devm_free_irq` × 4 (waits for running handlers) → `mlink_net_exit`
(`netif_tx_disable` + `napi_disable` if UP, `unregister_netdev`, `netif_napi_del`,
`free_netdev`) → devres: MSI-X vectors, BAR2 unmap/region, BAR0, disable device, `mdev`.
Rule: stop everything that can create new work before freeing what that work uses.

## 12. Observability and fault injection
- debugfs `/sys/kernel/debug/mlink/`: `info`, `link`, `ring`, `stats`, `irq_counts`,
  `ring_doorbell`, `pingpong`, `selftest`, `selftest_rx`, `fault_drop_kick`,
  `fault_delay_kick_us`, `fault_corrupt_tx_len`.
- Tracepoints `mlink:mlink_{xmit,kick,irq,napi_poll,rx,link}`.
- Host-side `tools/shm_chaos` (flip / len / idx / flags / magic / hb / mix).

## 12a. Control plane (P7, D18) — details in `docs/walkthrough/phase7.md`
`/dev/mlink_ctrl` (misc device): QMI-like header + TLVs (`driver/mlink_ctrl.h`, shared with
userspace). AP kernel assigns `txn_id` and routes RESP to the requesting file; IND goes to all
files; per-file queue bounded at 64. One mutex covers the control TX producer, the RX work, the
txn table and the file list. Control state is kref'd so open files survive an unbind (`dead`
→ `ENODEV`). On link reset: pending requests get RESP `-ENOTCONN`, rings zeroed, readers
`EPOLLHUP`. Daemon `mlinkd` (modem) and CLI `mlinkctl` (AP).

## 12b. Other peers: Zephyr and arm64 (P8, P9; D19–D21)
The ABI is OS- and ISA-neutral apart from byte order: the modem role has been taken by
- a **Zephyr** image (`zephyr/mlink-modem`) that compiles `mlink_ring.c` and `mlink_shm.h`
  unchanged through a shim mapping `READ_ONCE`/`virt_*` to volatile / C11 `__atomic`
  operations, ports the modem half of §9, serves the control rings like `mlinkd`, and either
  reflects packets (loopback test mode) or hands them to Zephyr's IP stack as a `DUMMY_L2`
  (raw-IP) interface, completing CHECKSUM_PARTIAL sums first because Zephyr verifies them;
- the same driver built for **arm64**, against the x86 AP on one region, where `virt_*` become
  `stlr` / `ldar` / `dmb ish` (`bench/results/p9-arm64-barriers.txt`).
What makes this work: every field has one writer, each side touches the other's fields only
through the ring/state accessors, and nothing in shm is a pointer. A big-endian peer would
need `__le32` fields (§6).

## 13. Known limitations
Fixed layout (no ring-size negotiation); two copies per packet; single queue per direction;
no TSO (GSO segments in software); no interrupt-rate limiting against a hostile peer (kick
suppression is cooperative: the peer can ring DATA doorbells at will; see the P5.5 entry in
the debugging log); the arm64 and Zephyr peers run under TCG on an x86 host, so weak-memory
reorderings are covered by the LKMM litmus tests, not observed at runtime; all endpoints are
VMs on one host.

## 14. Open questions for the user's review (P1.1)
1. Fixed 4 KiB slots waste memory for small packets (a 64 B ACK uses a whole slot) — accepted
   for simplicity and one-descriptor-per-slot ownership?
2. Two copies per packet (no zero-copy) — accepted for safety against an untrusted peer;
   E7 measured ~9 % of samples per side in the slot copy.
3. N = 256 fixed; a ring-size sweep needs a negotiated layout (stretch).
4. Should the modem also re-validate the header periodically, or is "validate once, then use
   compiled constants" the better choice? (Current: validate once.)
