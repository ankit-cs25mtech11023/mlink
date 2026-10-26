// SPDX-License-Identifier: GPL-2.0-only
/*
 * mlink network device: raw-IP point-to-point netdev on top of the rings.
 * Modelled on modem-style drivers (mhi_net, rmnet): no L2 header, no ARP.
 */
#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/if_arp.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/u64_stats_sync.h>

#include "mlink.h"
#include "mlink_trace.h"

#define MLINK_MIN_MTU	68
#define MLINK_MAX_MTU	MLINK_SLOT_SIZE

static struct mlink_dev *ndev_to_mdev(struct net_device *ndev)
{
	return *(struct mlink_dev **)netdev_priv(ndev);
}

/* ---- TX ------------------------------------------------------------------ */

static void mlink_tx_drop(struct mlink_dev *mdev, struct sk_buff *skb)
{
	u64_stats_update_begin(&mdev->tx_stats.syncp);
	mdev->tx_stats.dropped++;
	u64_stats_update_end(&mdev->tx_stats.syncp);
	dev_kfree_skb_any(skb);
}

/*
 * Fault injection (P5.2). Called only from xmit, so the counters need no
 * locking. Returns true if the DATA kick must be swallowed.
 */
static bool mlink_fault_kick(struct mlink_dev *mdev)
{
	u32 n = READ_ONCE(mdev->fault.drop_kick);
	u32 us = READ_ONCE(mdev->fault.delay_kick_us);

	if (us) {
		udelay(min(us, 1000U));	/* BH context: busy-wait, bounded */
		mdev->stats.fault_kicks_delayed++;
	}
	if (n && ++mdev->fault.n_kick % n == 0) {
		mdev->stats.fault_kicks_dropped++;
		return true;
	}
	return false;
}

/* Publish 1 of N packets with a length the consumer must reject. */
static u32 mlink_fault_len(struct mlink_dev *mdev, u32 len)
{
	u32 n = READ_ONCE(mdev->fault.corrupt_tx_len);

	if (n && ++mdev->fault.n_len % n == 0) {
		mdev->stats.fault_lens_corrupted++;
		return MLINK_SLOT_SIZE + 1;
	}
	return len;
}

/* Serialized by the core's TX lock: we are the ring's only producer. */
static netdev_tx_t mlink_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct mlink_dev *mdev = ndev_to_mdev(ndev);
	struct mlink_ring *r = &mdev->tx;
	bool safe = !unsafe_no_mb;
	u32 len = skb->len;
	u16 proto;
	int free;

	/* Belt and braces: never touch the ring outside UP (link reset owns it). */
	if (unlikely(!READ_ONCE(mdev->link_up))) {
		netif_stop_queue(ndev);
		mlink_tx_drop(mdev, skb);
		return NETDEV_TX_OK;
	}

	switch (skb->protocol) {
	case htons(ETH_P_IP):
		proto = MLINK_PROTO_IPV4;
		break;
	case htons(ETH_P_IPV6):
		proto = MLINK_PROTO_IPV6;
		break;
	default:
		mlink_tx_drop(mdev, skb);
		return NETDEV_TX_OK;
	}
	if (unlikely(!len || len > MLINK_SLOT_SIZE)) {
		mlink_tx_drop(mdev, skb);
		return NETDEV_TX_OK;
	}

	free = mlink_ring_free(r);
	if (unlikely(free < 0)) {
		/* Impossible index: peer restart or corruption, link work decides. */
		WRITE_ONCE(mdev->tx_broken, true);
		mlink_link_kick_work(mdev);
		netif_stop_queue(ndev);
		mlink_tx_drop(mdev, skb);
		return NETDEV_TX_OK;
	}
	if (unlikely(!free)) {
		/*
		 * Full although the queue was running (e.g. ifup while the ring was
		 * still full). Not steady state: stop, ask for a SPACE kick, and let
		 * the core requeue this skb instead of dropping it.
		 */
		netif_stop_queue(ndev);
		mdev->stats.tx_queue_stops++;
		if (mlink_ring_arm_space(r, safe)) {
			mlink_ring_disarm_space(r);
			netif_wake_queue(ndev);
			mdev->stats.tx_queue_wakes++;
		}
		return NETDEV_TX_BUSY;
	}

	/* Copy (no zero-copy): the peer must never see pointers into our memory. */
	skb_copy_bits(skb, 0, mlink_ring_slot(r), len);
	mlink_ring_publish(r, mlink_fault_len(mdev, len), proto, mdev->tx_seq++);

	u64_stats_update_begin(&mdev->tx_stats.syncp);
	mdev->tx_stats.packets++;
	mdev->tx_stats.bytes += len;
	u64_stats_update_end(&mdev->tx_stats.syncp);
	dev_consume_skb_any(skb);

	if (free == 1) {
		/* That was the last slot: stop, then re-check (SB with the consumer). */
		netif_stop_queue(ndev);
		mdev->stats.tx_queue_stops++;
		if (mlink_ring_arm_space(r, safe)) {
			mlink_ring_disarm_space(r);
			netif_wake_queue(ndev);
			mdev->stats.tx_queue_wakes++;
		}
	}

	/* More packets coming right now: one kick for the whole batch. */
	if (netdev_xmit_more() && !netif_queue_stopped(ndev)) {
		trace_mlink_xmit(len, r->idx, false);
		return NETDEV_TX_OK;
	}
	if (!kick_suppress || mlink_ring_need_kick(r, safe)) {
		if (!mlink_fault_kick(mdev))
			mlink_kick(mdev, MLINK_VEC_DATA);
		mdev->stats.tx_kicks++;
		trace_mlink_xmit(len, r->idx, true);
	} else {
		mdev->stats.tx_kick_suppressed++;
		trace_mlink_xmit(len, r->idx, false);
	}
	return NETDEV_TX_OK;
}

/* SPACE irq (hard-irq context). */
void mlink_net_space(struct mlink_dev *mdev)
{
	struct net_device *ndev = mdev->ndev;

	if (READ_ONCE(mdev->link_up) && netif_queue_stopped(ndev) &&
	    mlink_ring_free(&mdev->tx) > 0) {
		mlink_ring_disarm_space(&mdev->tx);
		netif_wake_queue(ndev);
		mdev->stats.tx_queue_wakes++;
	}
}

/* ---- RX ------------------------------------------------------------------ */

/* NAPI context. d->data points into shared memory; copy first, then inspect. */
void mlink_net_rx(struct mlink_dev *mdev, const struct mlink_rx_desc *d)
{
	struct net_device *ndev = mdev->ndev;
	struct sk_buff *skb;
	u8 ver;

	if (unlikely(!netif_running(ndev)))
		goto drop;
	skb = napi_alloc_skb(&mdev->napi, d->len);
	if (unlikely(!skb))
		goto drop;
	skb_put_data(skb, d->data, d->len);
	trace_mlink_rx(d->len, d->proto, mdev->rx.idx);

	/* Classify from our private copy, never from shm (no double fetch). */
	ver = skb->data[0] >> 4;
	if (ver == 4) {
		skb->protocol = htons(ETH_P_IP);
	} else if (ver == 6) {
		skb->protocol = htons(ETH_P_IPV6);
	} else {
		mdev->stats.rx_proto_errors++;
		dev_kfree_skb_any(skb);
		goto drop;
	}
	skb->dev = ndev;
	skb_reset_network_header(skb);

	u64_stats_update_begin(&mdev->rx_stats.syncp);
	mdev->rx_stats.packets++;
	mdev->rx_stats.bytes += d->len;
	u64_stats_update_end(&mdev->rx_stats.syncp);
	napi_gro_receive(&mdev->napi, skb);
	return;

drop:
	u64_stats_update_begin(&mdev->rx_stats.syncp);
	mdev->rx_stats.dropped++;
	u64_stats_update_end(&mdev->rx_stats.syncp);
}

static int mlink_napi_poll(struct napi_struct *napi, int budget)
{
	struct mlink_dev *mdev = container_of(napi, struct mlink_dev, napi);
	int done;

	mdev->stats.napi_polls++;
	done = mlink_rx_poll(mdev, budget);
	trace_mlink_napi_poll(budget, done);
	if (done == budget) {
		mdev->stats.napi_budget_exhausted++;
		return budget;	/* stay scheduled; the core polls us again */
	}

	if (napi_complete_done(napi, done) && !READ_ONCE(mdev->rx_broken)) {
		/* Going idle: arm the DATA kick, then re-check (lost-wakeup race). */
		if (mlink_ring_arm_kick(&mdev->rx, !unsafe_no_mb)) {
			mlink_ring_disarm_kick(&mdev->rx);
			napi_schedule(napi);
		}
	}
	return done;
}

/* ---- netdev ops ---------------------------------------------------------- */

static int mlink_open(struct net_device *ndev)
{
	struct mlink_dev *mdev = ndev_to_mdev(ndev);

	/* One producer per ring: refuse while a ring selftest owns TX. */
	if (atomic_cmpxchg(&mdev->tx_owner, MLINK_TX_FREE, MLINK_TX_NETDEV) != MLINK_TX_FREE)
		return -EBUSY;
	/* The queue runs only while the link is UP; the link work wakes it. */
	if (READ_ONCE(mdev->link_up))
		netif_start_queue(ndev);
	else
		netif_tx_stop_all_queues(ndev);
	return 0;
}

static int mlink_stop(struct net_device *ndev)
{
	struct mlink_dev *mdev = ndev_to_mdev(ndev);

	/* The core has already waited for in-flight ndo_start_xmit calls. */
	netif_stop_queue(ndev);
	mlink_ring_disarm_space(&mdev->tx);
	atomic_set(&mdev->tx_owner, MLINK_TX_FREE);
	return 0;
}

static void mlink_get_stats64(struct net_device *ndev,
			      struct rtnl_link_stats64 *s)
{
	struct mlink_dev *mdev = ndev_to_mdev(ndev);
	unsigned int start;

	do {
		start = u64_stats_fetch_begin(&mdev->rx_stats.syncp);
		s->rx_packets = mdev->rx_stats.packets;
		s->rx_bytes = mdev->rx_stats.bytes;
		s->rx_dropped = mdev->rx_stats.dropped;
	} while (u64_stats_fetch_retry(&mdev->rx_stats.syncp, start));

	do {
		start = u64_stats_fetch_begin(&mdev->tx_stats.syncp);
		s->tx_packets = mdev->tx_stats.packets;
		s->tx_bytes = mdev->tx_stats.bytes;
		s->tx_dropped = mdev->tx_stats.dropped;
	} while (u64_stats_fetch_retry(&mdev->tx_stats.syncp, start));

	s->rx_errors = READ_ONCE(mdev->stats.rx_len_errors) +
		       READ_ONCE(mdev->stats.rx_proto_errors);
}

static const struct net_device_ops mlink_netdev_ops = {
	.ndo_open = mlink_open,
	.ndo_stop = mlink_stop,
	.ndo_start_xmit = mlink_start_xmit,
	.ndo_get_stats64 = mlink_get_stats64,
};

/* ---- ethtool ------------------------------------------------------------- */

/* Driver counters, shared with debugfs "stats". */
const struct mlink_stat_desc mlink_stat_descs[] = {
#define S(f) { #f, offsetof(struct mlink_stats, f) }
	S(tx_kicks), S(tx_kick_suppressed), S(tx_queue_stops), S(tx_queue_wakes),
	S(tx_space_waits), S(tx_lost_space_wakeups), S(tx_space_timeouts),
	S(rx_msgs), S(napi_polls), S(napi_budget_exhausted), S(rx_len_errors),
	S(rx_proto_errors), S(rx_stalls), S(space_kicks), S(ring_corruptions),
	S(link_resets), S(link_ups), S(hb_saved_by_ring),
	S(fault_kicks_dropped), S(fault_kicks_delayed), S(fault_lens_corrupted),
#undef S
};

const unsigned int mlink_nr_stats = ARRAY_SIZE(mlink_stat_descs);

static void mlink_get_drvinfo(struct net_device *ndev,
			      struct ethtool_drvinfo *info)
{
	struct mlink_dev *mdev = ndev_to_mdev(ndev);

	strscpy(info->driver, KBUILD_MODNAME, sizeof(info->driver));
	strscpy(info->bus_info, pci_name(mdev->pdev), sizeof(info->bus_info));
}

static void mlink_get_ringparam(struct net_device *ndev,
				struct ethtool_ringparam *ring,
				struct kernel_ethtool_ringparam *kring,
				struct netlink_ext_ack *extack)
{
	ring->rx_max_pending = MLINK_RING_ENTRIES;
	ring->tx_max_pending = MLINK_RING_ENTRIES;
	ring->rx_pending = MLINK_RING_ENTRIES;
	ring->tx_pending = MLINK_RING_ENTRIES;
}

static int mlink_get_sset_count(struct net_device *ndev, int sset)
{
	return sset == ETH_SS_STATS ? mlink_nr_stats + 1 : -EOPNOTSUPP;
}

static void mlink_get_strings(struct net_device *ndev, u32 sset, u8 *data)
{
	int i;

	if (sset != ETH_SS_STATS)
		return;
	for (i = 0; i < mlink_nr_stats; i++)
		ethtool_puts(&data, mlink_stat_descs[i].name);
	ethtool_puts(&data, "rx_irqs");
}

static void mlink_get_ethtool_stats(struct net_device *ndev,
				    struct ethtool_stats *stats, u64 *data)
{
	struct mlink_dev *mdev = ndev_to_mdev(ndev);
	int i;

	for (i = 0; i < mlink_nr_stats; i++)
		data[i] = mlink_stat_read(mdev, i);
	data[i] = atomic_long_read(&mdev->irq_count[MLINK_VEC_DATA]);
}

static const struct ethtool_ops mlink_ethtool_ops = {
	.get_drvinfo = mlink_get_drvinfo,
	.get_link = ethtool_op_get_link,
	.get_ringparam = mlink_get_ringparam,
	.get_sset_count = mlink_get_sset_count,
	.get_strings = mlink_get_strings,
	.get_ethtool_stats = mlink_get_ethtool_stats,
};

/* ---- setup / teardown ---------------------------------------------------- */

static void mlink_setup(struct net_device *ndev)
{
	/* Raw IP point-to-point, like mhi_net: no L2 header, no ARP. */
	ndev->header_ops = NULL;
	ndev->type = ARPHRD_RAWIP;
	ndev->hard_header_len = 0;
	ndev->addr_len = 0;
	ndev->flags = IFF_POINTOPOINT | IFF_NOARP;
	ndev->netdev_ops = &mlink_netdev_ops;
	ndev->ethtool_ops = &mlink_ethtool_ops;
	ndev->mtu = ETH_DATA_LEN;
	ndev->min_mtu = MLINK_MIN_MTU;
	ndev->max_mtu = MLINK_MAX_MTU;
	ndev->tx_queue_len = 1000;
}

int mlink_net_init(struct mlink_dev *mdev)
{
	struct net_device *ndev;
	int ret;

	ndev = alloc_netdev(sizeof(struct mlink_dev *), "mlink%d",
			    NET_NAME_ENUM, mlink_setup);
	if (!ndev)
		return -ENOMEM;
	*(struct mlink_dev **)netdev_priv(ndev) = mdev;
	SET_NETDEV_DEV(ndev, &mdev->pdev->dev);
	mdev->ndev = ndev;
	u64_stats_init(&mdev->rx_stats.syncp);
	u64_stats_init(&mdev->tx_stats.syncp);

	netif_napi_add(ndev, &mdev->napi, mlink_napi_poll);
	netif_carrier_off(ndev);	/* on when the peer is found */

	ret = register_netdev(ndev);
	if (ret) {
		netif_napi_del(&mdev->napi);
		free_netdev(ndev);
		mdev->ndev = NULL;
		return ret;
	}
	/* NAPI stays disabled until the link work brings the link UP. */
	return 0;
}

/* Called after the irqs are freed, so nothing can schedule NAPI any more. */
void mlink_net_exit(struct mlink_dev *mdev)
{
	if (!mdev->ndev)
		return;
	if (mdev->link_up) {
		netif_tx_disable(mdev->ndev);
		napi_disable(&mdev->napi);	/* waits for a running poll */
		mdev->link_up = false;
	}
	unregister_netdev(mdev->ndev);
	netif_napi_del(&mdev->napi);
	free_netdev(mdev->ndev);
	mdev->ndev = NULL;
}
