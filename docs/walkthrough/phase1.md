# Phase 1 walkthrough — PCI driver & doorbell interrupts

Files: `driver/mlink_shm.h`, `driver/mlink.h`, `driver/mlink_main.c`, `driver/mlink_debugfs.c`.
Read with the code open. Every section ends with **"what breaks if…"**: that's the quiz material.

---

## 1. What a PCI driver is, in one paragraph

The PCI core enumerates devices at boot (vendor:device IDs, BARs = address windows the device
exposes). A driver registers a `struct pci_driver` with an `id_table`; when a device matches,
the core calls `probe(pdev, id)`. Everything the driver allocates in probe must be undone in
`remove()` (or automatically by **devres**, the `devm_*`/`pcim_*` "managed" APIs, which free
in reverse order after `remove()` returns or when probe fails).

Our device: QEMU `ivshmem-doorbell`, `1af4:1110`.
- **BAR0**: 256 bytes of registers (MMIO). We use `IVPosition` (+0x08, our peer ID) and
  `Doorbell` (+0x0C, write-only).
- **BAR1**: MSI-X table (used by the PCI core, not by us).
- **BAR2**: the 4 MiB shared memory. Both VMs map the *same host pages*.

---

## 2. `mlink_init()` — module parameters

- `role` is `charp`; validated with `sysfs_streq()` (tolerates a trailing newline). Invalid →
  `-EINVAL` *before* `pci_register_driver()`, so a misconfigured insmod binds nothing.
- `kick_suppress`, `unsafe_no_mb` are runtime-writable (0644) under
  `/sys/module/mlink/parameters/`. *What breaks if `role` had a default:* both VMs could come
  up as AP and both would write the header.

---

## 3. `mlink_probe()` line by line (order matters)

1. `devm_kzalloc` → `mdev`. Zeroed, so every counter/flag starts at 0/false.
2. `pcim_enable_device()` → turns on memory decoding; managed (disabled again on unbind).
3. **`pci_set_master()`** → QEMU delivers MSI-X messages through the device's bus-master
   address space; without it the interrupt writes are silently dropped (no error anywhere).
   *Interview point:* an MSI is just a DMA write by the device to a magic address.
4. `pcim_iomap_region(pdev, 0, name)` → requests + maps BAR0 (the current managed API in
   6.18; `pcim_iomap_regions` is deprecated). Returns `IOMEM_ERR_PTR` on failure → `IS_ERR`.
5. BAR2: check its size ≥ `MLINK_LAYOUT_END`, `pcim_request_region`, then
   **`devm_memremap(..., MEMREMAP_WB)`**. It's host RAM, not registers, so map it cacheable
   (write-back). Consequence: CPU caches + store buffers are involved → we need memory
   barriers (`virt_*`) for cross-VM ordering, not `ioread/iowrite` semantics.
6. Pointers into shm: header, AP state block, modem state block; `self`/`peer` by role.
7. `ioread32(IVPosition)` → our peer ID (assigned by ivshmem-server, changes on reconnect).
8. Rings attached (Phase 2).
9. `pci_alloc_irq_vectors(pdev, 4, 4, PCI_IRQ_MSIX)` → exactly 4 vectors or fail. Managed
   because the device was enabled with `pcim_enable_device`.
10. `devm_request_irq()` per vector, each with its own handler and name → visible in
    `/proc/interrupts` as `mlink-data`, `mlink-space`, `mlink-state`, `mlink-test`.
11. netdev registered (Phase 3), header published (AP), own `gen` bumped, own state published,
    debugfs created, **`ready = true` last**, first NAPI poll, link work started.

*What breaks if `ready` were set before the netdev exists:* a DATA interrupt (possible as
soon as the IRQ is requested — the peer may already be kicking) would call `napi_schedule`
on an uninitialised NAPI struct.

---

## 4. Interrupt handlers (hard-IRQ context)

- Hard-IRQ context: interrupts off on this CPU, **cannot sleep** (no mutex, no `GFP_KERNEL`,
  no `msleep`). Keep it short; defer real work (NAPI softirq, workqueue).
- `mlink_test_irq` takes `ktime_get_ns()` **first** — that's the RTT end point.
  - Modem: reflects with `mlink_kick(TEST)` straight from the handler (an MMIO write is fine
    in IRQ context).
  - AP: stores `t_reply`, then `complete()`. `complete()` takes a spinlock and wakes the
    waiter; the waiter reads `t_reply` after `wait_for_completion*()` returns, so the store is
    visible (completion = synchronisation point).
- `irq_count[]` are `atomic_long_t`: incremented on any CPU, read by debugfs.

---

## 5. Shared header and state blocks (`mlink_shm.h`)

- **Single-writer rule:** header = AP only; AP state block = AP only; modem block = modem
  only. Two writers on one field would need cross-VM atomics → avoided by design.
- Each state block is exactly one 64-byte cache line (`__aligned(64)` + `static_assert`), so
  the two writers never share a line (no false sharing, and no torn line ownership).
- `static_assert(offsetof(...))` everywhere: the two VMs might run *different builds*; the
  ABI must be checkable at compile time.
- **Publishing the header (AP):** clear `magic` → `virt_wmb()` → write fields →
  `virt_store_release(&magic)`. **Validating (modem):** `virt_load_acquire(&magic)` → read
  every field **once** into a local → compare against the expected values + CRC.
  - Release/acquire pair: if the modem sees the magic, it sees all fields written before it.
  - Read-once-then-validate: the peer could change a field between two reads (TOCTOU /
    double fetch). Only the local copy is trusted.
- `gen` is bumped on every probe; it survives in shm across reloads. (Phase 4 uses it to
  detect "the peer restarted".)

---

## 6. Peer discovery (`mlink_link_work`, delayed work every 100 ms)

- Workqueue = **process context**: may sleep, take mutexes.
- Modem waits until the header is valid, then publishes its own state.
- Reads peer block: acquire `magic`, then `peer_id`, `gen`. Accepts only `peer_id ≤ 0xFFFF`
  (the doorbell has 16 bits for it) and ≠ own ID — **never trust shm**.
- Re-arms itself unless `dying`. `cancel_delayed_work_sync()` in remove is safe for a
  self-requeueing work: it prevents requeueing while it waits.

*What breaks if the peer ID were read once at probe:* when the modem VM restarts, the server
gives it a new ID; we would keep kicking a dead ID (doorbells to absent peers are silently
dropped).

---

## 7. Doorbell = `iowrite32((peer << 16) | vector, regs + 0x0C)`

QEMU traps the MMIO write (a VM exit), writes the eventfd for (peer, vector), the peer's QEMU
injects MSI-X vector `vector` into the peer guest. One kick ≈ one interrupt on the other side.

---

## 8. debugfs (`/sys/kernel/debug/mlink/`)

- `info`, `irq_counts`: `DEFINE_SHOW_ATTRIBUTE` + `seq_file` — the standard read-only pattern.
- `ring_doorbell`: `simple_open` puts `inode->i_private` (our `mdev`) into
  `file->private_data`; `kstrtou32_from_user` parses user input safely.
- `pingpong`: process context, may sleep. Per round: `reinit_completion` → `t0` → kick TEST
  → `wait_for_completion_timeout(100 ms)`. On a timeout it sleeps another 100 ms so a very
  late reply cannot complete the *next* round (the doorbell carries no sequence number —
  documented limitation). Samples sorted with `sort()`, percentiles by index.
- `debugfs_remove()` waits for in-flight file operations → remove() can't free `mdev` under a
  running pingpong.

---

## 9. Teardown order (`mlink_remove`) — the classic interview question

```
dying = true; wake_up(tx_wq)       # loops in debugfs writers / selftest bail out
debugfs_remove                     # waits for running file ops
cancel_delayed_work_sync           # link work can no longer touch the peer/netdev
state = DOWN; release(magic = 0)   # tell the peer we're gone
ready = false; devm_free_irq × 4   # free_irq waits for running handlers (synchronize_irq)
mlink_net_exit                     # napi_disable → unregister_netdev → free_netdev
(devres after return: MSI-X vectors, BAR2 unmap, BAR0 unmap, disable device, kfree mdev)
```

Rule: **stop everything that can generate new work before freeing what that work uses**.
*What breaks if IRQs were freed by devres after `free_netdev`:* a DATA interrupt in that
window would call `napi_schedule` on freed memory → KASAN use-after-free.

---

## 10. Results you can quote (debug kernel — say so)
- Doorbell RTT (AP → modem → AP, interrupt to interrupt): p50 ≈ 8–12 µs, p99 ≈ 16–26 µs,
  0 lost of 15 000 (`bench/results/p1-doorbell-rtt-debug.txt`).
- 100× insmod/rmmod on each side while the peer sends ~365 000 kicks: KASAN/lockdep clean,
  kmemleak 0.

## Likely quiz questions
1. Why `pci_set_master()` for a device that does no DMA?
2. Why `MEMREMAP_WB` and not `ioremap` for BAR2? Consequence for ordering?
3. What context does `mlink_test_irq` run in? Could it call `mutex_lock`? `msleep`?
4. Why read each header field once into a local?
5. Why must the peer ID be re-read from shm instead of cached at probe?
6. Walk through the teardown order and say what breaks if you swap two steps.
7. What does `devm_` buy you, and when does devres actually free things?
8. Spot-the-bug candidates: `ready = true` moved before `mlink_net_init()`; `free_irq` moved
   after `mlink_net_exit()`; `virt_store_release(&magic)` replaced by `WRITE_ONCE`.
