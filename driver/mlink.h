/* SPDX-License-Identifier: GPL-2.0-only */
/* mlink private definitions shared by the driver's .c files. */
#ifndef _MLINK_H
#define _MLINK_H

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/workqueue.h>

#include "mlink_shm.h"

/* ivshmem-doorbell BAR0 registers */
#define IVSHMEM_INTR_MASK	0x00
#define IVSHMEM_INTR_STATUS	0x04
#define IVSHMEM_IVPOSITION	0x08
#define IVSHMEM_DOORBELL	0x0c

#define MLINK_PEER_POLL_MS	100

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
	bool dying;			/* set first in remove(); irq handlers check it */

	struct delayed_work link_work;	/* peer discovery, later heartbeat (P4) */
	atomic_long_t irq_count[MLINK_NR_VECTORS];
	struct mlink_pingpong pp;
	struct dentry *dbg;
};

void mlink_kick(struct mlink_dev *mdev, enum mlink_vector vec);
int mlink_pingpong_run(struct mlink_dev *mdev, u32 n);
void mlink_debugfs_init(struct mlink_dev *mdev);
void mlink_debugfs_exit(struct mlink_dev *mdev);

static inline const char *mlink_role_name(enum mlink_role role)
{
	return role == MLINK_ROLE_AP ? "ap" : "modem";
}

#endif /* _MLINK_H */
