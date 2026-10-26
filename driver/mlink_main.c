// SPDX-License-Identifier: GPL-2.0-only
/*
 * mlink - raw-IP network driver over an ivshmem-doorbell shared-memory link.
 *
 * This file: module parameters, PCI probe/remove, BAR mapping, MSI-X doorbell
 * interrupts, the shared header and state blocks, and peer discovery.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bottom_half.h>
#include <linux/crc32.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/string.h>

#include "mlink.h"

#define CREATE_TRACE_POINTS
#include "mlink_trace.h"

#define MLINK_PCI_DEVICE_ID	0x1110	/* QEMU ivshmem */

static char *role;
module_param(role, charp, 0444);
MODULE_PARM_DESC(role, "Link side: \"ap\" or \"modem\" (required)");

bool kick_suppress = true;
module_param(kick_suppress, bool, 0644);
MODULE_PARM_DESC(kick_suppress, "Kick the peer only when it asked for it (default on)");

bool unsafe_no_mb;
module_param(unsafe_no_mb, bool, 0644);
MODULE_PARM_DESC(unsafe_no_mb, "EXPERIMENT ONLY: drop the kick-path full barriers (P2.4)");

unsigned int hb_ms = 100;
module_param(hb_ms, uint, 0644);
MODULE_PARM_DESC(hb_ms, "Heartbeat / link poll period in ms (default 100)");

unsigned int hb_timeout_ms = 500;
module_param(hb_timeout_ms, uint, 0644);
MODULE_PARM_DESC(hb_timeout_ms, "Peer declared lost after this long without a heartbeat (default 500)");

unsigned int napi_weight = NAPI_POLL_WEIGHT;
module_param(napi_weight, uint, 0444);
MODULE_PARM_DESC(napi_weight, "NAPI poll budget, 1..64 (default 64)");

static enum mlink_role mlink_role;

void mlink_kick(struct mlink_dev *mdev, enum mlink_vector vec)
{
	u32 peer = READ_ONCE(mdev->peer_id);

	trace_mlink_kick(peer, vec);
	/* QEMU signals eventfd[vec] of peer 'peer'; its QEMU raises MSI-X vector vec. */
	iowrite32((peer << 16) | vec, mdev->regs + IVSHMEM_DOORBELL);
}

/* ---- interrupts --------------------------------------------------------- */

/*
 * Handlers are registered before the netdev exists and stay registered
 * briefly after remove() starts, so each one does nothing until 'ready'.
 */
static irqreturn_t mlink_data_irq(int irq, void *data)
{
	struct mlink_dev *mdev = data;

	atomic_long_inc(&mdev->irq_count[MLINK_VEC_DATA]);
	trace_mlink_irq(MLINK_VEC_DATA);
	if (!READ_ONCE(mdev->ready))
		return IRQ_HANDLED;
	/* We are about to poll: further kicks would be wasted interrupts. */
	mlink_ring_disarm_kick(&mdev->rx);
	napi_schedule_irqoff(&mdev->napi);
	return IRQ_HANDLED;
}

static irqreturn_t mlink_space_irq(int irq, void *data)
{
	struct mlink_dev *mdev = data;

	atomic_long_inc(&mdev->irq_count[MLINK_VEC_SPACE]);
	trace_mlink_irq(MLINK_VEC_SPACE);
	if (!READ_ONCE(mdev->ready))
		return IRQ_HANDLED;
	mlink_net_space(mdev);
	wake_up(&mdev->tx_wq);
	return IRQ_HANDLED;
}

/* The peer's state block changed: run the link work now, not in <= hb_ms. */
static irqreturn_t mlink_state_irq(int irq, void *data)
{
	struct mlink_dev *mdev = data;

	atomic_long_inc(&mdev->irq_count[MLINK_VEC_STATE]);
	trace_mlink_irq(MLINK_VEC_STATE);
	if (READ_ONCE(mdev->ready) && !READ_ONCE(mdev->dying))
		mlink_link_kick_work(mdev);
	return IRQ_HANDLED;
}

static irqreturn_t mlink_test_irq(int irq, void *data)
{
	struct mlink_dev *mdev = data;
	u64 now = ktime_get_ns();	/* first thing: this is the RTT end point */

	atomic_long_inc(&mdev->irq_count[MLINK_VEC_TEST]);
	trace_mlink_irq(MLINK_VEC_TEST);
	if (!READ_ONCE(mdev->ready))
		return IRQ_HANDLED;

	if (mdev->role == MLINK_ROLE_MODEM) {
		/* Reflector: kick straight back from hard-irq context. */
		mlink_kick(mdev, MLINK_VEC_TEST);
	} else if (READ_ONCE(mdev->pp.active)) {
		WRITE_ONCE(mdev->pp.t_reply, now);
		/* complete() orders the t_reply store before the waiter wakes. */
		complete(&mdev->pp.done);
	}
	return IRQ_HANDLED;
}

static const irq_handler_t mlink_irq_handlers[MLINK_NR_VECTORS] = {
	[MLINK_VEC_DATA]  = mlink_data_irq,
	[MLINK_VEC_SPACE] = mlink_space_irq,
	[MLINK_VEC_STATE] = mlink_state_irq,
	[MLINK_VEC_TEST]  = mlink_test_irq,
};

static const char * const mlink_irq_names[MLINK_NR_VECTORS] = {
	"mlink-data", "mlink-space", "mlink-state", "mlink-test",
};

/* ---- shared header and state blocks ------------------------------------- */

static void mlink_hdr_expected(struct mlink_shm_hdr *h)
{
	memset(h, 0, sizeof(*h));
	h->magic = MLINK_SHM_MAGIC;
	h->abi_version = MLINK_ABI_VERSION;
	h->ring_entries = MLINK_RING_ENTRIES;
	h->slot_size = MLINK_SLOT_SIZE;
	h->ring_a_off = MLINK_RING_A_OFF;
	h->ring_b_off = MLINK_RING_B_OFF;
	h->layout_crc = crc32_le(~0, (u8 *)h + MLINK_HDR_CRC_START, MLINK_HDR_CRC_LEN);
}

/* True if the shared header matches exactly what this build expects. */
bool mlink_hdr_valid(struct mlink_dev *mdev)
{
	struct mlink_shm_hdr *sh = mdev->hdr;
	struct mlink_shm_hdr want, got;

	mlink_hdr_expected(&want);
	/* Pairs with the release in mlink_hdr_publish(): fields are complete. */
	got.magic = virt_load_acquire(&sh->magic);
	if (got.magic != MLINK_SHM_MAGIC)
		return false;

	/* Untrusted memory: read each field once, then compare the copies. */
	got.abi_version = READ_ONCE(sh->abi_version);
	got.ring_entries = READ_ONCE(sh->ring_entries);
	got.slot_size = READ_ONCE(sh->slot_size);
	got.ring_a_off = READ_ONCE(sh->ring_a_off);
	got.ring_b_off = READ_ONCE(sh->ring_b_off);
	got.layout_crc = READ_ONCE(sh->layout_crc);

	return got.abi_version == want.abi_version &&
	       got.ring_entries == want.ring_entries &&
	       got.slot_size == want.slot_size &&
	       got.ring_a_off == want.ring_a_off &&
	       got.ring_b_off == want.ring_b_off &&
	       got.layout_crc == want.layout_crc;
}

/* AP only: (re)write the header. The modem must never see half of it. */
void mlink_hdr_publish(struct mlink_dev *mdev)
{
	struct mlink_shm_hdr *sh = mdev->hdr;
	struct mlink_shm_hdr want;

	mlink_hdr_expected(&want);
	WRITE_ONCE(sh->magic, 0);
	virt_wmb();	/* invalidate before touching the fields */
	WRITE_ONCE(sh->abi_version, want.abi_version);
	WRITE_ONCE(sh->ring_entries, want.ring_entries);
	WRITE_ONCE(sh->slot_size, want.slot_size);
	WRITE_ONCE(sh->ring_a_off, want.ring_a_off);
	WRITE_ONCE(sh->ring_b_off, want.ring_b_off);
	WRITE_ONCE(sh->layout_crc, want.layout_crc);
	/* All fields become visible before (or with) the magic. */
	virt_store_release(&sh->magic, MLINK_SHM_MAGIC);
}

/* ---- PCI driver ---------------------------------------------------------- */

static int mlink_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct device *dev = &pdev->dev;
	struct mlink_shm_state *ap_st, *modem_st;
	struct mlink_dev *mdev;
	int ret, i;

	mdev = devm_kzalloc(dev, sizeof(*mdev), GFP_KERNEL);
	if (!mdev)
		return -ENOMEM;
	mdev->pdev = pdev;
	mdev->role = mlink_role;
	mutex_init(&mdev->pp.lock);
	init_completion(&mdev->pp.done);
	mutex_init(&mdev->st.lock);
	init_waitqueue_head(&mdev->tx_wq);
	atomic_set(&mdev->tx_owner, MLINK_TX_FREE);
	INIT_DELAYED_WORK(&mdev->link_work, mlink_link_work);

	ret = pcim_enable_device(pdev);
	if (ret)
		return dev_err_probe(dev, ret, "cannot enable device\n");
	/* QEMU delivers MSI-X writes via the bus-master address space. */
	pci_set_master(pdev);

	mdev->regs = pcim_iomap_region(pdev, 0, KBUILD_MODNAME);
	if (IS_ERR(mdev->regs))
		return dev_err_probe(dev, PTR_ERR(mdev->regs), "cannot map BAR0\n");

	mdev->shm_len = pci_resource_len(pdev, 2);
	if (mdev->shm_len < MLINK_LAYOUT_END)
		return dev_err_probe(dev, -EINVAL, "BAR2 too small: %pa bytes\n",
				     &mdev->shm_len);
	ret = pcim_request_region(pdev, 2, KBUILD_MODNAME);
	if (ret)
		return dev_err_probe(dev, ret, "cannot claim BAR2\n");
	/* Host RAM, not device registers: cacheable, ordered with virt_* barriers. */
	mdev->shm = devm_memremap(dev, pci_resource_start(pdev, 2), mdev->shm_len,
				  MEMREMAP_WB);
	if (IS_ERR(mdev->shm))
		return dev_err_probe(dev, PTR_ERR(mdev->shm), "cannot map BAR2\n");

	mdev->hdr = mdev->shm + MLINK_HDR_OFF;
	ap_st = mdev->shm + MLINK_STATE_AP_OFF;
	modem_st = mdev->shm + MLINK_STATE_MODEM_OFF;
	mdev->self = mdev->role == MLINK_ROLE_AP ? ap_st : modem_st;
	mdev->peer = mdev->role == MLINK_ROLE_AP ? modem_st : ap_st;
	mdev->own_id = ioread32(mdev->regs + IVSHMEM_IVPOSITION);

	/* Ring A carries AP -> MODEM, ring B MODEM -> AP. */
	if (mdev->role == MLINK_ROLE_AP) {
		mlink_ring_attach(&mdev->tx, mdev->shm + MLINK_RING_A_OFF, true);
		mlink_ring_attach(&mdev->rx, mdev->shm + MLINK_RING_B_OFF, false);
	} else {
		mlink_ring_attach(&mdev->tx, mdev->shm + MLINK_RING_B_OFF, true);
		mlink_ring_attach(&mdev->rx, mdev->shm + MLINK_RING_A_OFF, false);
	}

	ret = pci_alloc_irq_vectors(pdev, MLINK_NR_VECTORS, MLINK_NR_VECTORS,
				    PCI_IRQ_MSIX);
	if (ret < 0)
		return dev_err_probe(dev, ret, "need %d MSI-X vectors\n",
				     MLINK_NR_VECTORS);
	for (i = 0; i < MLINK_NR_VECTORS; i++) {
		ret = devm_request_irq(dev, pci_irq_vector(pdev, i),
				       mlink_irq_handlers[i], 0,
				       mlink_irq_names[i], mdev);
		if (ret)
			return dev_err_probe(dev, ret, "cannot request %s\n",
					     mlink_irq_names[i]);
	}

	ret = mlink_net_init(mdev);
	if (ret)
		return dev_err_probe(dev, ret, "cannot register netdev\n");
	pci_set_drvdata(pdev, mdev);

	/* The AP owns the layout; the modem starts once it has validated it. */
	if (mdev->role == MLINK_ROLE_AP) {
		if (!mlink_hdr_valid(mdev))
			mlink_hdr_publish(mdev);
		mdev->hdr_ok = true;
		mlink_link_start(mdev);
	}

	mlink_debugfs_init(mdev);
	WRITE_ONCE(mdev->ready, true);
	/* High-priority workers: a heartbeat starved by bulk traffic looks like a dead peer. */
	queue_delayed_work(system_highpri_wq, &mdev->link_work, 0);

	dev_info(dev, "%s: role=%s own_id=%u shm=%pa bytes\n",
		 netdev_name(mdev->ndev), mlink_role_name(mdev->role),
		 mdev->own_id, &mdev->shm_len);
	return 0;
}

static void mlink_remove(struct pci_dev *pdev)
{
	struct mlink_dev *mdev = pci_get_drvdata(pdev);
	int i;

	/* Teardown order: stop producers of work -> tell the peer -> quiesce irqs. */
	WRITE_ONCE(mdev->dying, true);
	wake_up(&mdev->tx_wq);		/* a selftest waiting for space bails out */
	mlink_debugfs_exit(mdev);	/* waits for running pingpong/selftest */
	cancel_delayed_work_sync(&mdev->link_work);
	mlink_link_stop(mdev);	/* state DOWN + STATE kick: fast detection */

	/*
	 * Free the handlers now rather than at devres time: after this no irq
	 * can schedule NAPI or touch the netdev we are about to free.
	 */
	WRITE_ONCE(mdev->ready, false);
	for (i = 0; i < MLINK_NR_VECTORS; i++)
		devm_free_irq(&pdev->dev, pci_irq_vector(pdev, i), mdev);
	mlink_net_exit(mdev);
	dev_info(&pdev->dev, "removed\n");
}

static const struct pci_device_id mlink_pci_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_REDHAT_QUMRANET, MLINK_PCI_DEVICE_ID) },
	{ }
};
MODULE_DEVICE_TABLE(pci, mlink_pci_ids);

static struct pci_driver mlink_pci_driver = {
	.name = KBUILD_MODNAME,
	.id_table = mlink_pci_ids,
	.probe = mlink_probe,
	.remove = mlink_remove,
};

static int __init mlink_init(void)
{
	if (!role) {
		pr_err("role= is required (ap|modem)\n");
		return -EINVAL;
	}
	if (sysfs_streq(role, "ap")) {
		mlink_role = MLINK_ROLE_AP;
	} else if (sysfs_streq(role, "modem")) {
		mlink_role = MLINK_ROLE_MODEM;
	} else {
		pr_err("invalid role=%s (ap|modem)\n", role);
		return -EINVAL;
	}
	return pci_register_driver(&mlink_pci_driver);
}

static void __exit mlink_exit(void)
{
	pci_unregister_driver(&mlink_pci_driver);
}

module_init(mlink_init);
module_exit(mlink_exit);

MODULE_DESCRIPTION("Raw-IP network driver over an ivshmem shared-memory modem link");
MODULE_AUTHOR("Ankit <cs25mtech11023@iith.ac.in>");
MODULE_LICENSE("GPL");
