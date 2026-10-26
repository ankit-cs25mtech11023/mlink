// SPDX-License-Identifier: GPL-2.0-only
/*
 * mlink debugfs diagnostics: /sys/kernel/debug/mlink/
 *   info           role, IDs, generations, BAR sizes
 *   irq_counts     interrupts received per vector
 *   ring_doorbell  write a vector number -> kick the peer on that vector
 *   pingpong       AP: write N -> N TEST round trips; read -> RTT stats (ns)
 */
#include <linux/debugfs.h>
#include <linux/kstrtox.h>
#include <linux/ktime.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/sort.h>

#include "mlink.h"

#define MLINK_PP_MAX		100000
#define MLINK_PP_TIMEOUT_MS	100

static const char * const mlink_vec_names[MLINK_NR_VECTORS] = {
	"data", "space", "state", "test",
};

static int info_show(struct seq_file *s, void *unused)
{
	struct mlink_dev *mdev = s->private;

	seq_printf(s, "role:       %s\n", mlink_role_name(mdev->role));
	seq_printf(s, "own_id:     %u\n", mdev->own_id);
	seq_printf(s, "hdr_ok:     %d\n", READ_ONCE(mdev->hdr_ok));
	seq_printf(s, "peer_found: %d\n", READ_ONCE(mdev->peer_found));
	seq_printf(s, "peer_id:    %u\n", READ_ONCE(mdev->peer_id));
	seq_printf(s, "own_gen:    %u\n", READ_ONCE(mdev->self->gen));
	seq_printf(s, "peer_gen:   %u\n", READ_ONCE(mdev->peer->gen));
	seq_printf(s, "peer_state: %u\n", READ_ONCE(mdev->peer->state));
	seq_printf(s, "bar0_len:   %llu\n",
		   (u64)pci_resource_len(mdev->pdev, 0));
	seq_printf(s, "shm_len:    %llu\n", (u64)mdev->shm_len);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(info);

static int irq_counts_show(struct seq_file *s, void *unused)
{
	struct mlink_dev *mdev = s->private;
	int i;

	for (i = 0; i < MLINK_NR_VECTORS; i++)
		seq_printf(s, "%-6s %ld\n", mlink_vec_names[i],
			   atomic_long_read(&mdev->irq_count[i]));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(irq_counts);

static ssize_t ring_doorbell_write(struct file *file, const char __user *ubuf,
				   size_t len, loff_t *ppos)
{
	struct mlink_dev *mdev = file->private_data;
	u32 vec;
	int ret;

	ret = kstrtou32_from_user(ubuf, len, 0, &vec);
	if (ret)
		return ret;
	if (vec >= MLINK_NR_VECTORS)
		return -EINVAL;
	if (!READ_ONCE(mdev->peer_found))
		return -ENOTCONN;
	mlink_kick(mdev, vec);
	return len;
}

static const struct file_operations ring_doorbell_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = ring_doorbell_write,
};

static int cmp_u64(const void *a, const void *b)
{
	u64 x = *(const u64 *)a, y = *(const u64 *)b;

	return x < y ? -1 : x > y;
}

/* Value at percentile p of a sorted array of n > 0 samples. */
static u64 pct(const u64 *v, u32 n, u32 p)
{
	return v[(u64)(n - 1) * p / 100];
}

int mlink_pingpong_run(struct mlink_dev *mdev, u32 n)
{
	struct mlink_pingpong *pp = &mdev->pp;
	u32 i, got = 0, lost = 0;
	u64 *rtt;

	if (mdev->role != MLINK_ROLE_AP)
		return -EOPNOTSUPP;	/* the modem only reflects */
	if (!n || n > MLINK_PP_MAX)
		return -EINVAL;
	if (!READ_ONCE(mdev->peer_found))
		return -ENOTCONN;

	rtt = kvmalloc_array(n, sizeof(*rtt), GFP_KERNEL);
	if (!rtt)
		return -ENOMEM;

	mutex_lock(&pp->lock);
	WRITE_ONCE(pp->active, true);
	for (i = 0; i < n && !READ_ONCE(mdev->dying); i++) {
		u64 t0;

		reinit_completion(&pp->done);
		t0 = ktime_get_ns();
		mlink_kick(mdev, MLINK_VEC_TEST);
		if (!wait_for_completion_timeout(&pp->done,
						 msecs_to_jiffies(MLINK_PP_TIMEOUT_MS))) {
			lost++;
			/*
			 * A very late reply could complete the next round early.
			 * Let it arrive now; the doorbell carries no sequence number.
			 */
			msleep(MLINK_PP_TIMEOUT_MS);
			continue;
		}
		rtt[got++] = READ_ONCE(pp->t_reply) - t0;
	}
	WRITE_ONCE(pp->active, false);

	pp->count = got;
	pp->lost = lost;
	if (got) {
		sort(rtt, got, sizeof(*rtt), cmp_u64, NULL);
		pp->min = rtt[0];
		pp->p50 = pct(rtt, got, 50);
		pp->p90 = pct(rtt, got, 90);
		pp->p99 = pct(rtt, got, 99);
		pp->max = rtt[got - 1];
	}
	mutex_unlock(&pp->lock);
	kvfree(rtt);
	return 0;
}

static int pingpong_show(struct seq_file *s, void *unused)
{
	struct mlink_dev *mdev = s->private;
	struct mlink_pingpong *pp = &mdev->pp;

	mutex_lock(&pp->lock);
	seq_printf(s, "count %u lost %u min_ns %llu p50_ns %llu p90_ns %llu p99_ns %llu max_ns %llu\n",
		   pp->count, pp->lost, pp->min, pp->p50, pp->p90, pp->p99,
		   pp->max);
	mutex_unlock(&pp->lock);
	return 0;
}

static int pingpong_open(struct inode *inode, struct file *file)
{
	return single_open(file, pingpong_show, inode->i_private);
}

static ssize_t pingpong_write(struct file *file, const char __user *ubuf,
			      size_t len, loff_t *ppos)
{
	struct mlink_dev *mdev = ((struct seq_file *)file->private_data)->private;
	u32 n;
	int ret;

	ret = kstrtou32_from_user(ubuf, len, 0, &n);
	if (ret)
		return ret;
	ret = mlink_pingpong_run(mdev, n);
	return ret ? ret : len;
}

static const struct file_operations pingpong_fops = {
	.owner = THIS_MODULE,
	.open = pingpong_open,
	.read = seq_read,
	.write = pingpong_write,
	.llseek = seq_lseek,
	.release = single_release,
};

void mlink_debugfs_init(struct mlink_dev *mdev)
{
	/* debugfs errors are not fatal; the calls accept an ERR_PTR parent. */
	mdev->dbg = debugfs_create_dir(KBUILD_MODNAME, NULL);
	debugfs_create_file("info", 0444, mdev->dbg, mdev, &info_fops);
	debugfs_create_file("irq_counts", 0444, mdev->dbg, mdev, &irq_counts_fops);
	debugfs_create_file("ring_doorbell", 0200, mdev->dbg, mdev,
			    &ring_doorbell_fops);
	debugfs_create_file("pingpong", 0600, mdev->dbg, mdev, &pingpong_fops);
}

void mlink_debugfs_exit(struct mlink_dev *mdev)
{
	debugfs_remove(mdev->dbg);	/* waits for in-flight file operations */
}
