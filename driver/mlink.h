/* SPDX-License-Identifier: GPL-2.0-only */
/* mlink private definitions shared by the driver's .c files. */
#ifndef _MLINK_H
#define _MLINK_H

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/ethtool.h>
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
extern unsigned int hb_ms;
extern unsigned int hb_timeout_ms;
extern unsigned int napi_weight;

enum mlink_reset_reason {
	MLINK_RESET_HB_TIMEOUT,
	MLINK_RESET_PEER_GEN,
	MLINK_RESET_PEER_DOWN,
	MLINK_RESET_RING,
	MLINK_NR_RESET_REASONS,
};

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
	u64 ring_corruptions;		/* impossible prod/cons with unchanged peer gen */
	u64 link_resets;		/* link work */
	u64 link_ups;			/* link work */
	u64 hb_saved_by_ring;		/* heartbeat stale but peer ring progress seen */
	u64 hdr_repairs;		/* AP: corrupted shared header rewritten */
	u64 tx_stalls;			/* link work: stopped queue woken (lost SPACE kick) */
	u64 fault_kicks_dropped;	/* P5.2 injected */
	u64 fault_kicks_delayed;	/* P5.2 injected */
	u64 fault_lens_corrupted;	/* P5.2 injected */
	u64 rx_csum_errors;		/* NAPI: invalid CSUM_PARTIAL offsets from the peer */
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
	bool rx_broken;			/* NAPI: impossible index; link work resets */
	bool tx_broken;			/* xmit/selftest: impossible index; link work resets */
	u32 rx_stall_seen;		/* link work: "armed, pending, no progress" polls */
	u32 rx_stall_idx;		/* link work: rx.idx at the first such poll */
	u32 tx_stall_seen;		/* link work: queue stopped with ring space */
	wait_queue_head_t tx_wq;	/* selftest producer, woken by the SPACE irq */
	atomic_t tx_owner;		/* MLINK_TX_* */
	u32 tx_seq;

	struct net_device *ndev;
	struct napi_struct napi;
	struct mlink_netstats rx_stats;	/* NAPI context */
	struct mlink_netstats tx_stats;	/* xmit context */

	/* link state machine (mlink_link.c); written only by the link work */
	struct delayed_work link_work;
	enum mlink_link_state link_state;
	bool link_up;			/* read locklessly by the data path / selftest */
	u32 gen;			/* our current generation */
	u32 peer_gen;			/* peer generation we acknowledged */
	u32 hb_seen;			/* last peer heartbeat value */
	unsigned long hb_changed;	/* jiffies when it last changed */
	ktime_t t_detect;		/* when the last reset was decided */
	u64 last_recover_us;		/* reset decision -> link UP */
	u32 last_hb_age_ms;		/* peer heartbeat age when the last reset was decided */
	u32 max_hb_gap_ms;		/* longest peer heartbeat interval seen while UP */
	u32 peer_activity;		/* fingerprint of peer ring progress (liveness) */
	u64 reset_count[MLINK_NR_RESET_REASONS];
	atomic_long_t irq_count[MLINK_NR_VECTORS];
	struct mlink_stats stats;
	struct mlink_pingpong pp;
	struct mlink_selftest st;
	struct mlink_ctrl *ctrl;	/* control plane (mlink_ctrl.c), refcounted */
	struct dentry *dbg;

	/* Fault injection knobs (debugfs, P5.2); 0 = off. Read in xmit only. */
	struct {
		u32 drop_kick;		/* drop 1 of N DATA kicks */
		u32 delay_kick_us;	/* busy-wait before each DATA kick (<= 1000) */
		u32 corrupt_tx_len;	/* publish 1 of N packets with an invalid len */
		u32 n_kick, n_len;	/* xmit-private counters */
	} fault;
};

/* Named view of struct mlink_stats for ethtool -S and debugfs (mlink_net.c). */
struct mlink_stat_desc {
	char name[ETH_GSTRING_LEN];
	size_t off;
};

extern const struct mlink_stat_desc mlink_stat_descs[];
extern const unsigned int mlink_nr_stats;

static inline u64 mlink_stat_read(struct mlink_dev *mdev, unsigned int i)
{
	return READ_ONCE(*(u64 *)((u8 *)&mdev->stats + mlink_stat_descs[i].off));
}

void mlink_kick(struct mlink_dev *mdev, enum mlink_vector vec);
bool mlink_hdr_valid(struct mlink_dev *mdev);
void mlink_hdr_publish(struct mlink_dev *mdev);

/* mlink_link.c */
void mlink_link_start(struct mlink_dev *mdev);
void mlink_link_stop(struct mlink_dev *mdev);
void mlink_link_work(struct work_struct *work);

/* From irq/softirq context: ask the link work to run now. */
static inline void mlink_link_kick_work(struct mlink_dev *mdev)
{
	mod_delayed_work(system_highpri_wq, &mdev->link_work, 0);
}

int mlink_pingpong_run(struct mlink_dev *mdev, u32 n);
void mlink_debugfs_init(struct mlink_dev *mdev);
void mlink_debugfs_exit(struct mlink_dev *mdev);

/* mlink_selftest.c: ring consumer + selftest */
int mlink_rx_poll(struct mlink_dev *mdev, int budget);
int mlink_selftest_tx(struct mlink_dev *mdev, u32 n);

/* mlink_ctrl.c (P7) */
struct mlink_ctrl;
struct seq_file;
int mlink_ctrl_init(struct mlink_dev *mdev);
void mlink_ctrl_exit(struct mlink_dev *mdev);
void mlink_ctrl_irq(struct mlink_dev *mdev);
void mlink_ctrl_link_reset(struct mlink_dev *mdev);
void mlink_ctrl_link_up(struct mlink_dev *mdev);
void mlink_ctrl_stats(struct mlink_dev *mdev, struct seq_file *s);

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
