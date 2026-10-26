/* SPDX-License-Identifier: GPL-2.0-only */
/* mlink private definitions shared by the driver's .c files. */
#ifndef _MLINK_H
#define _MLINK_H

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/u64_stats_sync.h>
#include <linux/pci.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "mlink_ring.h"
#include "mlink_shm.h"

/* ivshmem-doorbell BAR0 registers */
#define IVSHMEM_INTR_MASK	0x00
#define IVSHMEM_INTR_STATUS	0x04
#define IVSHMEM_IVPOSITION	0x08
#define IVSHMEM_DOORBELL	0x0c

#define MLINK_PEER_POLL_MS	100
#define MLINK_RX_BUDGET		64

/* Module parameters (mlink_main.c) */
extern bool kick_suppress;
extern bool unsafe_no_mb;

enum mlink_role {
	MLINK_ROLE_AP,
	MLINK_ROLE_MODEM,
};

struct mlink_pingpong {
	struct mutex lock;		/* one run at a time */
	struct completion done;		/* completed by the TEST irq handler */
	bool active;			/* a run is waiting for replies */
	u64 t_reply;			/* ktime_get_ns() in the TEST handler */
	/* results of the last run */
	u32 count, lost;
	u64 min, p50, p90, p99, max;
};

/*
 * Driver counters. Each is written from one context only (noted), read
 * racily by debugfs; that is fine for statistics.
 */
struct mlink_stats {
	u64 tx_kicks;			/* producer: DATA kicks sent */
	u64 tx_kick_suppressed;		/* producer: publish without kick */
	u64 tx_space_waits;		/* producer: ring was full */
	u64 tx_lost_space_wakeups;	/* producer: space appeared, no SPACE irq */
	u64 tx_space_timeouts;		/* producer: no space for a whole timeout */
	u64 rx_msgs;			/* consumer work */
	u64 rx_len_errors;		/* consumer work: invalid desc len */
	u64 rx_proto_errors;		/* consumer work: unknown proto */
	u64 rx_stalls;			/* link work: armed but data waiting (lost DATA kick) */
	u64 tx_queue_stops;		/* xmit: ring full, queue stopped */
	u64 tx_queue_wakes;		/* xmit/SPACE irq: queue woken */
	u64 napi_polls;			/* NAPI */
	u64 napi_budget_exhausted;	/* NAPI: poll used its whole budget */
	u64 space_kicks;		/* consumer work: SPACE kicks sent */
	u64 ring_corruptions;		/* impossible prod/cons seen */
};

/* Ring selftest (P2.3): sequence-numbered pseudo-random payloads. */
struct mlink_selftest {
	struct mutex lock;		/* one TX run at a time */
	/* last TX run */
	u32 tx_sent;
	int tx_err;
	u64 tx_ns;
	u64 tx_kicks, tx_suppressed, tx_space_waits, tx_lost_wakeups;
	/* RX verifier state (consumer work only) */
	u32 rx_expect;
	u64 rx_ok, rx_lost, rx_dup_ooo, rx_corrupt;
	u64 rx_first_ns, rx_last_ns;
};

/* netdev packet counters; one writer context each, read under syncp. */
struct mlink_netstats {
	u64 packets;
	u64 bytes;
	u64 dropped;
	struct u64_stats_sync syncp;
};

/* Who may produce on the TX ring (SPSC: exactly one producer at a time). */
enum {
	MLINK_TX_FREE,
	MLINK_TX_NETDEV,
	MLINK_TX_SELFTEST,
};

struct mlink_dev {
	struct pci_dev *pdev;
	enum mlink_role role;
	void __iomem *regs;		/* BAR0 */
	void *shm;			/* BAR2, normal WB memory shared with the peer */
	resource_size_t shm_len;

	struct mlink_shm_hdr *hdr;
	struct mlink_shm_state *self;	/* we are the only writer */
	struct mlink_shm_state *peer;	/* untrusted, read-only for us */

	u32 own_id;			/* our IVPosition */
	u32 peer_id;			/* learned from the peer's state block */
	bool hdr_ok;			/* modem: header validated */
	bool peer_found;
	bool ready;			/* probe finished; irq handlers do nothing until then */
	bool dying;			/* set first in remove() */

	struct mlink_ring tx;		/* we produce */
	struct mlink_ring rx;		/* we consume */
	bool rx_broken;			/* corruption seen; stop consuming (P4 resets) */
	u32 rx_stall_seen;		/* link work: consecutive "armed, pending, no progress" polls */
	u32 rx_stall_idx;		/* link work: rx.idx at the first such poll */
	wait_queue_head_t tx_wq;	/* selftest producer, woken by the SPACE irq */
	atomic_t tx_owner;		/* MLINK_TX_* */
	u32 tx_seq;

	struct net_device *ndev;
	struct napi_struct napi;
	struct mlink_netstats rx_stats;	/* NAPI context */
	struct mlink_netstats tx_stats;	/* xmit context */

	struct delayed_work link_work;	/* peer discovery, later heartbeat (P4) */
	atomic_long_t irq_count[MLINK_NR_VECTORS];
	struct mlink_stats stats;
	struct mlink_pingpong pp;
	struct mlink_selftest st;
	struct dentry *dbg;
};

void mlink_kick(struct mlink_dev *mdev, enum mlink_vector vec);
int mlink_pingpong_run(struct mlink_dev *mdev, u32 n);
void mlink_debugfs_init(struct mlink_dev *mdev);
void mlink_debugfs_exit(struct mlink_dev *mdev);

/* mlink_selftest.c: ring consumer + selftest */
int mlink_rx_poll(struct mlink_dev *mdev, int budget);
int mlink_selftest_tx(struct mlink_dev *mdev, u32 n);

/* mlink_net.c */
int mlink_net_init(struct mlink_dev *mdev);
void mlink_net_exit(struct mlink_dev *mdev);
void mlink_net_rx(struct mlink_dev *mdev, const struct mlink_rx_desc *d);
void mlink_net_space(struct mlink_dev *mdev);

static inline const char *mlink_role_name(enum mlink_role role)
{
	return role == MLINK_ROLE_AP ? "ap" : "modem";
}

#endif /* _MLINK_H */
