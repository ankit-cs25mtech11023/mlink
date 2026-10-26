# mlink — Linux Network Driver for a Shared-Memory Modem Link

A small rebuild of a modem ↔ application-processor data path: an out-of-tree Linux PCI
driver for QEMU's `ivshmem-doorbell` device that exposes a raw-IP point-to-point
`net_device` (`mlink0`) using MSI-X, NAPI and lock-free single-producer/single-consumer
rings in shared memory, running between two KVM guests ("ap" and "modem"), with
heartbeat-based subsystem-restart recovery.

**Status:** work in progress — Phase 0 (environment & bootstrap). No results yet.

## Prior art
- Siemens Jailhouse `ivshmem-net` (out-of-tree shared-memory Ethernet driver)
- Linux `drivers/net/mhi_net.c`, `drivers/net/ethernet/qualcomm/rmnet/`
- Linux `drivers/net/virtio_net.c`

## License
GPL-2.0-only. See [LICENSE](LICENSE).
