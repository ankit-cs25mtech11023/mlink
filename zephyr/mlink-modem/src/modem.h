/* SPDX-License-Identifier: GPL-2.0-only */
/* State shared by the link/ring core (main.c) and the IP-stack glue (net.c). */
#ifndef MLINK_MODEM_H
#define MLINK_MODEM_H

#include <zephyr/kernel.h>

#include <asm/barrier.h>	/* compat/: virt_* as in Linux */
#include <linux/compiler.h>	/* compat/: READ_ONCE, WRITE_ONCE */

#include "mlink_ring.h"

#define OWN_MTU		1500

struct modem_stats {
	u32 irqs[MLINK_NR_VECTORS];
	u32 rx, tx, icmp, udp, frags, dropped, bad_len, tx_full;
	u32 to_stack, csum_fixed, tx_waits, tx_dropped;
	u32 kicks_data, kicks_space, kicks_state;
	u32 link_ups, link_resets;
};

struct modem {
	const struct device *dev;
	u8 *shm;
	struct mlink_shm_hdr *hdr;
	struct mlink_shm_state *self, *peer;	/* MODEM block, AP block */
	struct mlink_ring tx, rx;		/* ring B (to AP), ring A (from AP) */
	struct mlink_ring ctrl_tx, ctrl_rx;	/* rings D, C: only reset, not served */

	/*
	 * Ring B producers: the poll thread (loopback) or Zephyr net/app threads
	 * via send() (IP stack). The lock keeps ring B single-producer and
	 * excludes the link reset, which zeroes the producer index.
	 */
	struct k_mutex tx_lock;
	struct k_sem space_sem;		/* given on every SPACE doorbell */

	u32 own_id, peer_id;
	bool hdr_ok, peer_found, link_up;
	enum mlink_link_state state;
	u32 gen, peer_gen, tx_seq;
	bool rx_broken, tx_broken;

	u32 hb_seen, peer_activity;
	int64_t hb_changed;

	struct modem_stats st;
};

extern struct modem m;

void modem_kick(enum mlink_vector vec);

#ifdef CONFIG_MLINK_MODEM_NET
/* net.c: hand one validated packet (private copy) to the IP stack. */
bool modem_net_rx(u8 *data, u32 len, const struct mlink_rx_desc *d);
void modem_net_carrier(bool up);
#else
static inline bool modem_net_rx(u8 *data, u32 len, const struct mlink_rx_desc *d)
{
	return false;
}
static inline void modem_net_carrier(bool up) {}
#endif

#endif /* MLINK_MODEM_H */
