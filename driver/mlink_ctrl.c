// SPDX-License-Identifier: GPL-2.0-only
/*
 * mlink control plane (P7): /dev/mlink_ctrl, a QMI-like request/response/
 * indication channel over two small control rings (design.md, D18).
 *
 * Lifetime: the misc device's open files can outlive the PCI device (sysfs
 * unbind), so the control state is refcounted (kref, one ref per open file +
 * one for the device) and every file operation checks 'dead' under 'lock'
 * before touching shared memory or registers.
 *
 * Locking: one mutex serializes everything here: the control TX ring
 * producer (any number of writers -> still a single producer), the RX
 * consumer work, the transaction table and the file list. Control traffic
 * is low-rate, so simplicity wins over concurrency.
 */
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "mlink.h"
#include "mlink_ctrl.h"

#define MLINK_CTRL_RXQ_MAX	64	/* per open file; oldest dropped beyond */
#define MLINK_CTRL_TXNS		32	/* outstanding requests (AP) */

struct mlink_ctrl_buf {
	struct list_head node;
	u32 len;
	u8 data[];
};

struct mlink_ctrl_file {
	struct list_head node;		/* in ctrl->files */
	struct mlink_ctrl *ctrl;
	struct list_head rxq;		/* struct mlink_ctrl_buf */
	unsigned int rxq_len;
	wait_queue_head_t wq;
};

struct mlink_ctrl_txn {
	u32 txn_id;			/* 0 = free slot */
	u16 msg_id;
	struct mlink_ctrl_file *owner;
};

struct mlink_ctrl {
	struct kref ref;
	struct mutex lock;		/* everything below, incl. the TX ring producer */
	bool dead;			/* device gone: files get -ENODEV */
	bool up;			/* link UP: channel usable */
	struct mlink_dev *mdev;		/* valid only while !dead */
	struct miscdevice misc;
	struct list_head files;
	struct mlink_ring tx, rx;
	struct work_struct rx_work;
	wait_queue_head_t space_wq;	/* writers waiting for TX ring space */
	struct mlink_ctrl_txn txns[MLINK_CTRL_TXNS];
	u32 next_txn;
	/* counters (debugfs "link" shows them) */
	u64 tx_msgs, rx_msgs, rx_dropped, rx_invalid;
};

static void mlink_ctrl_free(struct kref *ref)
{
	kfree(container_of(ref, struct mlink_ctrl, ref));
}

/* Queue a copy of a message to one file. Called with ctrl->lock held. */
static void mlink_ctrl_enqueue(struct mlink_ctrl_file *f, const void *msg, u32 len)
{
	struct mlink_ctrl_buf *b;

	if (f->rxq_len >= MLINK_CTRL_RXQ_MAX) {
		/* Bounded: a reader that never reads cannot pin unlimited memory. */
		b = list_first_entry(&f->rxq, struct mlink_ctrl_buf, node);
		list_del(&b->node);
		kfree(b);
		f->rxq_len--;
		f->ctrl->rx_dropped++;
	}
	b = kmalloc(struct_size(b, data, len), GFP_KERNEL);
	if (!b) {
		f->ctrl->rx_dropped++;
		return;
	}
	b->len = len;
	memcpy(b->data, msg, len);
	list_add_tail(&b->node, &f->rxq);
	f->rxq_len++;
	wake_up_interruptible(&f->wq);
}

static void mlink_ctrl_broadcast(struct mlink_ctrl *c, const void *msg, u32 len)
{
	struct mlink_ctrl_file *f;

	list_for_each_entry(f, &c->files, node)
		mlink_ctrl_enqueue(f, msg, len);
}

/* ---- receive (process context work) ---------------------------------------- */

/* Route one validated message. Called with ctrl->lock held. */
static void mlink_ctrl_route(struct mlink_ctrl *c, const u8 *msg, u32 len)
{
	const struct mlink_ctrl_hdr *h = (const void *)msg;
	int i;

	if (c->mdev->role == MLINK_ROLE_AP) {
		if (h->type == MLINK_CTRL_IND) {
			mlink_ctrl_broadcast(c, msg, len);
			return;
		}
		if (h->type == MLINK_CTRL_RESP && h->txn_id) {
			for (i = 0; i < MLINK_CTRL_TXNS; i++) {
				struct mlink_ctrl_txn *t = &c->txns[i];

				if (t->txn_id != h->txn_id)
					continue;
				mlink_ctrl_enqueue(t->owner, msg, len);
				t->txn_id = 0;
				return;
			}
		}
	} else if (h->type == MLINK_CTRL_REQ) {
		/* Modem: requests go to the daemon(s) holding the device open. */
		mlink_ctrl_broadcast(c, msg, len);
		return;
	}
	c->rx_invalid++;	/* unknown txn, or a type this role never receives */
}

static void mlink_ctrl_rx_work(struct work_struct *work)
{
	struct mlink_ctrl *c = container_of(work, struct mlink_ctrl, rx_work);
	u8 *msg = kmalloc(MLINK_CTRL_MSG_MAX, GFP_KERNEL);
	struct mlink_rx_desc d;
	int n = 0, ret;

	if (!msg)
		return;
	mutex_lock(&c->lock);
	while (!c->dead && c->up) {
		const struct mlink_ctrl_hdr *h = (const void *)msg;

		ret = mlink_ring_peek(&c->rx, &d);
		if (!ret)
			break;
		if (ret == -EIO) {
			/* Restart or corruption: the link work decides and resets. */
			WRITE_ONCE(c->mdev->rx_broken, true);
			mlink_link_kick_work(c->mdev);
			break;
		}
		if (ret > 0 && d.proto == MLINK_PROTO_CTRL &&
		    d.len >= sizeof(*h)) {
			/* Copy first, then validate only our copy (no double fetch). */
			memcpy(msg, d.data, d.len);
			if (sizeof(*h) + h->len == d.len &&
			    h->type <= MLINK_CTRL_IND) {
				c->rx_msgs++;
				mlink_ctrl_route(c, msg, d.len);
			} else {
				c->rx_invalid++;
			}
		} else {
			c->rx_invalid++;
		}
		mlink_ring_consume(&c->rx);
		n++;
	}
	if (n) {
		mlink_ring_release(&c->rx);
		/* Low rate: always kick, so a writer waiting for space wakes up. */
		mlink_kick(c->mdev, MLINK_VEC_CTRL);
	}
	mutex_unlock(&c->lock);
	kfree(msg);
}

/* CTRL vector (hard-IRQ): new messages and/or space freed by the peer. */
void mlink_ctrl_irq(struct mlink_dev *mdev)
{
	struct mlink_ctrl *c = mdev->ctrl;

	queue_work(system_highpri_wq, &c->rx_work);
	wake_up_interruptible(&c->space_wq);
}

/* ---- link state hooks (called from the link work) -------------------------- */

/*
 * Link reset: fail every pending request with -ENOTCONN, hang up readers and
 * zero the control-ring fields we own (the caller has bumped gen already).
 */
void mlink_ctrl_link_reset(struct mlink_dev *mdev)
{
	struct mlink_ctrl *c = mdev->ctrl;
	struct mlink_ctrl_hdr h = { .type = MLINK_CTRL_RESP, .result = -ENOTCONN };
	struct mlink_ctrl_file *f;
	int i;

	mutex_lock(&c->lock);
	c->up = false;
	for (i = 0; i < MLINK_CTRL_TXNS; i++) {
		struct mlink_ctrl_txn *t = &c->txns[i];

		if (!t->txn_id)
			continue;
		h.msg_id = t->msg_id;
		h.txn_id = t->txn_id;
		mlink_ctrl_enqueue(t->owner, &h, sizeof(h));
		t->txn_id = 0;
	}
	mlink_ring_reset_producer(&c->tx);
	mlink_ring_reset_consumer(&c->rx);
	list_for_each_entry(f, &c->files, node)
		wake_up_interruptible(&f->wq);	/* readers see POLLHUP */
	mutex_unlock(&c->lock);
	wake_up_interruptible(&c->space_wq);
}

void mlink_ctrl_link_up(struct mlink_dev *mdev)
{
	struct mlink_ctrl *c = mdev->ctrl;
	struct mlink_ctrl_file *f;

	mutex_lock(&c->lock);
	c->up = true;
	list_for_each_entry(f, &c->files, node)
		wake_up_interruptible(&f->wq);
	mutex_unlock(&c->lock);
	wake_up_interruptible(&c->space_wq);
	queue_work(system_highpri_wq, &c->rx_work);	/* anything already queued */
}

/* ---- file operations ---------------------------------------------------------- */

static int mlink_ctrl_open(struct inode *inode, struct file *file)
{
	struct mlink_ctrl *c = container_of(file->private_data, struct mlink_ctrl, misc);
	struct mlink_ctrl_file *f;

	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	INIT_LIST_HEAD(&f->rxq);
	init_waitqueue_head(&f->wq);
	f->ctrl = c;

	mutex_lock(&c->lock);
	if (c->dead) {
		mutex_unlock(&c->lock);
		kfree(f);
		return -ENODEV;
	}
	kref_get(&c->ref);
	list_add_tail(&f->node, &c->files);
	mutex_unlock(&c->lock);
	file->private_data = f;
	return 0;
}

static int mlink_ctrl_release(struct inode *inode, struct file *file)
{
	struct mlink_ctrl_file *f = file->private_data;
	struct mlink_ctrl *c = f->ctrl;
	struct mlink_ctrl_buf *b, *tmp;
	int i;

	mutex_lock(&c->lock);
	list_del(&f->node);
	for (i = 0; i < MLINK_CTRL_TXNS; i++)	/* responses would have no reader */
		if (c->txns[i].txn_id && c->txns[i].owner == f)
			c->txns[i].txn_id = 0;
	mutex_unlock(&c->lock);
	list_for_each_entry_safe(b, tmp, &f->rxq, node)
		kfree(b);
	kfree(f);
	kref_put(&c->ref, mlink_ctrl_free);
	return 0;
}

static ssize_t mlink_ctrl_read(struct file *file, char __user *ubuf, size_t len,
			       loff_t *ppos)
{
	struct mlink_ctrl_file *f = file->private_data;
	struct mlink_ctrl *c = f->ctrl;
	struct mlink_ctrl_buf *b;
	ssize_t ret;

	for (;;) {
		mutex_lock(&c->lock);
		if (c->dead) {
			mutex_unlock(&c->lock);
			return -ENODEV;
		}
		if (!list_empty(&f->rxq))
			break;
		/* Queued messages (incl. -ENOTCONN responses) are delivered first. */
		if (!c->up) {
			mutex_unlock(&c->lock);
			return -ENOTCONN;
		}
		mutex_unlock(&c->lock);
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (wait_event_interruptible(f->wq, !list_empty(&f->rxq) ||
					     !READ_ONCE(c->up) || READ_ONCE(c->dead)))
			return -ERESTARTSYS;
	}

	b = list_first_entry(&f->rxq, struct mlink_ctrl_buf, node);
	if (len < b->len) {
		ret = -EMSGSIZE;	/* message stays queued */
	} else {
		list_del(&b->node);
		f->rxq_len--;
		ret = b->len;
	}
	mutex_unlock(&c->lock);
	if (ret > 0) {
		if (copy_to_user(ubuf, b->data, b->len))
			ret = -EFAULT;
		kfree(b);
	}
	return ret;
}

/* Record a request's owner; returns the new txn_id or 0 if the table is full. */
static u32 mlink_ctrl_new_txn(struct mlink_ctrl *c, struct mlink_ctrl_file *f, u16 msg_id)
{
	int i;

	for (i = 0; i < MLINK_CTRL_TXNS; i++) {
		struct mlink_ctrl_txn *t = &c->txns[i];

		if (t->txn_id)
			continue;
		if (!++c->next_txn)	/* 0 means "free" */
			c->next_txn = 1;
		t->txn_id = c->next_txn;
		t->msg_id = msg_id;
		t->owner = f;
		return t->txn_id;
	}
	return 0;
}

static ssize_t mlink_ctrl_write(struct file *file, const char __user *ubuf, size_t len,
				loff_t *ppos)
{
	struct mlink_ctrl_file *f = file->private_data;
	struct mlink_ctrl *c = f->ctrl;
	const struct mlink_tx_meta meta = { .proto = MLINK_PROTO_CTRL };
	struct mlink_ctrl_hdr *h;
	ssize_t ret;
	u8 *msg;
	int free;

	if (len < sizeof(*h) || len > MLINK_CTRL_MSG_MAX)
		return -EINVAL;
	msg = memdup_user(ubuf, len);
	if (IS_ERR(msg))
		return PTR_ERR(msg);
	h = (void *)msg;
	if (sizeof(*h) + h->len != len || h->type > MLINK_CTRL_IND) {
		ret = -EINVAL;
		goto out;
	}

	for (;;) {
		mutex_lock(&c->lock);
		if (c->dead) {
			ret = -ENODEV;
			goto unlock;
		}
		if (!c->up) {
			ret = -ENOTCONN;
			goto unlock;
		}
		free = mlink_ring_free(&c->tx);
		if (free < 0) {
			WRITE_ONCE(c->mdev->tx_broken, true);
			mlink_link_kick_work(c->mdev);
			ret = -EIO;
			goto unlock;
		}
		if (free)
			break;
		mutex_unlock(&c->lock);
		if (file->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			goto out;
		}
		/* The consumer kicks CTRL after every batch: no lost-wakeup race. */
		if (wait_event_interruptible_timeout(c->space_wq,
						     mlink_ring_free(&c->tx) || !READ_ONCE(c->up) ||
						     READ_ONCE(c->dead), HZ) < 0) {
			ret = -ERESTARTSYS;
			goto out;
		}
	}

	if (c->mdev->role == MLINK_ROLE_AP) {
		/* The AP only originates requests; the kernel owns txn ids. */
		if (h->type != MLINK_CTRL_REQ) {
			ret = -EINVAL;
			goto unlock;
		}
		h->txn_id = mlink_ctrl_new_txn(c, f, h->msg_id);
		if (!h->txn_id) {
			ret = -EBUSY;
			goto unlock;
		}
	} else if (h->type == MLINK_CTRL_REQ) {
		ret = -EINVAL;	/* the modem answers and indicates, it doesn't ask */
		goto unlock;
	}

	memcpy(mlink_ring_slot(&c->tx), msg, len);
	mlink_ring_publish(&c->tx, len, &meta, h->txn_id);
	mlink_kick(c->mdev, MLINK_VEC_CTRL);
	c->tx_msgs++;
	ret = len;
unlock:
	mutex_unlock(&c->lock);
out:
	kfree(msg);
	return ret;
}

static __poll_t mlink_ctrl_poll(struct file *file, poll_table *wait)
{
	struct mlink_ctrl_file *f = file->private_data;
	struct mlink_ctrl *c = f->ctrl;
	__poll_t mask = 0;

	poll_wait(file, &f->wq, wait);
	poll_wait(file, &c->space_wq, wait);
	mutex_lock(&c->lock);
	if (c->dead)
		mask = EPOLLHUP | EPOLLERR;
	else if (!c->up)
		mask = EPOLLHUP;
	else if (mlink_ring_free(&c->tx) > 0)
		mask = EPOLLOUT | EPOLLWRNORM;
	if (!list_empty(&f->rxq))
		mask |= EPOLLIN | EPOLLRDNORM;
	mutex_unlock(&c->lock);
	return mask;
}

static long mlink_ctrl_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct mlink_ctrl_file *f = file->private_data;
	struct mlink_ctrl *c = f->ctrl;
	struct mlink_ctrl_info info = {};

	if (cmd != MLINK_IOC_GET_INFO)
		return -ENOTTY;
	mutex_lock(&c->lock);
	if (c->dead) {
		mutex_unlock(&c->lock);
		return -ENODEV;
	}
	info.role = c->mdev->role;
	info.link_state = READ_ONCE(c->mdev->link_state);
	info.gen = READ_ONCE(c->mdev->gen);
	info.peer_gen = READ_ONCE(c->mdev->peer_gen);
	info.own_id = c->mdev->own_id;
	info.peer_id = READ_ONCE(c->mdev->peer_id);
	mutex_unlock(&c->lock);
	return copy_to_user((void __user *)arg, &info, sizeof(info)) ? -EFAULT : 0;
}

static const struct file_operations mlink_ctrl_fops = {
	.owner = THIS_MODULE,
	.open = mlink_ctrl_open,
	.release = mlink_ctrl_release,
	.read = mlink_ctrl_read,
	.write = mlink_ctrl_write,
	.poll = mlink_ctrl_poll,
	.unlocked_ioctl = mlink_ctrl_ioctl,
	.llseek = noop_llseek,
};

/* ---- device lifetime ------------------------------------------------------------ */

int mlink_ctrl_init(struct mlink_dev *mdev)
{
	struct mlink_ctrl *c;
	int ret;

	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	kref_init(&c->ref);
	mutex_init(&c->lock);
	INIT_LIST_HEAD(&c->files);
	INIT_WORK(&c->rx_work, mlink_ctrl_rx_work);
	init_waitqueue_head(&c->space_wq);
	c->mdev = mdev;
	/* Ring C carries AP -> MODEM, ring D MODEM -> AP (like A/B for data). */
	mlink_ring_attach(&c->tx, mdev->shm + (mdev->role == MLINK_ROLE_AP ?
			  MLINK_CTRL_C_OFF : MLINK_CTRL_D_OFF),
			  MLINK_CTRL_ENTRIES, MLINK_CTRL_SLOT_SIZE);
	mlink_ring_attach(&c->rx, mdev->shm + (mdev->role == MLINK_ROLE_AP ?
			  MLINK_CTRL_D_OFF : MLINK_CTRL_C_OFF),
			  MLINK_CTRL_ENTRIES, MLINK_CTRL_SLOT_SIZE);

	c->misc.minor = MISC_DYNAMIC_MINOR;
	c->misc.name = "mlink_ctrl";
	c->misc.fops = &mlink_ctrl_fops;
	c->misc.mode = 0600;
	mdev->ctrl = c;
	ret = misc_register(&c->misc);
	if (ret) {
		mdev->ctrl = NULL;
		kfree(c);
	}
	return ret;
}

/* After the irqs are freed: no new rx_work can be queued. */
void mlink_ctrl_exit(struct mlink_dev *mdev)
{
	struct mlink_ctrl *c = mdev->ctrl;
	struct mlink_ctrl_file *f;

	if (!c)
		return;
	misc_deregister(&c->misc);	/* no new opens */
	cancel_work_sync(&c->rx_work);
	mutex_lock(&c->lock);
	c->dead = true;			/* open files: -ENODEV from now on */
	c->up = false;
	c->mdev = NULL;
	list_for_each_entry(f, &c->files, node)
		wake_up_interruptible(&f->wq);
	mutex_unlock(&c->lock);
	wake_up_interruptible(&c->space_wq);
	mdev->ctrl = NULL;
	kref_put(&c->ref, mlink_ctrl_free);	/* the last file frees it */
}

void mlink_ctrl_stats(struct mlink_dev *mdev, struct seq_file *s)
{
	struct mlink_ctrl *c = mdev->ctrl;

	if (!c)
		return;
	mutex_lock(&c->lock);
	seq_printf(s, "ctrl_up:         %d\n", c->up);
	seq_printf(s, "ctrl_tx_msgs:    %llu\n", c->tx_msgs);
	seq_printf(s, "ctrl_rx_msgs:    %llu\n", c->rx_msgs);
	seq_printf(s, "ctrl_rx_dropped: %llu\n", c->rx_dropped);
	seq_printf(s, "ctrl_rx_invalid: %llu\n", c->rx_invalid);
	mutex_unlock(&c->lock);
}
