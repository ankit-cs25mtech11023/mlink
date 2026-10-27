// SPDX-License-Identifier: GPL-2.0-only
/*
 * IP-stack mode (P8.3, plan.md D20): the ring pair becomes a Zephyr network
 * interface. DUMMY_L2 means "no link-layer header", i.e. raw IP like Linux's
 * ARPHRD_RAWIP on the AP side (and mhi_net); the net core picks IPv4/IPv6 from
 * the version nibble. On top of it: UDP echo :7, TCP echo :7, TCP discard :9.
 */
#include <zephyr/net/dummy.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>

#include "modem.h"

#define MODEM_IPV4	"10.77.0.2"
#define MODEM_NETMASK	"255.255.255.0"
#define TX_WAIT_MS	100
#define TX_MAX_WAITS	20	/* then drop: TCP retransmits, UDP is lossy anyway */

static struct net_if *mlink_iface;

/* ---- RX: ring A -> stack (poll thread) ----------------------------------- */

/*
 * The AP sent CHECKSUM_PARTIAL (D17): the L4 checksum field holds only the
 * pseudo-header seed. Zephyr verifies L4 checksums, so finish the job the
 * way skb_checksum_help() does. start/offset come from shared memory: bound
 * them against this packet before use.
 */
static bool csum_complete(u8 *data, u32 len, const struct mlink_rx_desc *d)
{
	u32 start = d->csum_start, off = d->csum_offset, sum = 0, i;
	u16 csum;

	if (start >= len || off > len - start || len - start - off < 2)
		return false;
	for (i = start; i + 1 < len; i += 2)
		sum += sys_get_be16(&data[i]);
	if (i < len)
		sum += data[i] << 8;	/* odd length: pad with a zero byte */
	sum = (sum & 0xffff) + (sum >> 16);
	sum = (sum & 0xffff) + (sum >> 16);
	csum = ~sum;
	sys_put_be16(csum ? csum : 0xffff, &data[start + off]);
	return true;
}

bool modem_net_rx(u8 *data, u32 len, const struct mlink_rx_desc *d)
{
	struct net_pkt *pkt;

	if (!mlink_iface)
		return false;
	if (d->flags & MLINK_DESC_F_CSUM_PARTIAL) {
		if (!csum_complete(data, len, d))
			return false;
		m.st.csum_fixed++;
	}
	pkt = net_pkt_rx_alloc_with_buffer(mlink_iface, len, NET_AF_UNSPEC, 0, K_NO_WAIT);
	if (!pkt)
		return false;
	if (net_pkt_write(pkt, data, len) || net_recv_data(mlink_iface, pkt) < 0) {
		net_pkt_unref(pkt);
		return false;
	}
	return true;
}

void modem_net_carrier(bool up)
{
	if (!mlink_iface)
		return;
	if (up)
		net_if_carrier_on(mlink_iface);
	else
		net_if_carrier_off(mlink_iface);
}

/* ---- TX: stack -> ring B (any net/app thread) ---------------------------- */

static int mlink_net_send(const struct device *dev, struct net_pkt *pkt)
{
	struct mlink_tx_meta meta = {
		.proto = net_pkt_family(pkt) == NET_AF_INET6 ? MLINK_PROTO_IPV6 : MLINK_PROTO_IPV4,
	};
	size_t len = net_pkt_get_len(pkt);
	int ret = -ENETDOWN, waits = 0;

	ARG_UNUSED(dev);
	if (!len || len > MLINK_SLOT_SIZE)
		return -EMSGSIZE;

	k_mutex_lock(&m.tx_lock, K_FOREVER);
	for (;;) {
		int n;

		if (!READ_ONCE(m.link_up))
			break;
		n = mlink_ring_free(&m.tx);
		if (n < 0) {
			WRITE_ONCE(m.tx_broken, true);	/* the link tick resets */
			ret = -EIO;
			break;
		}
		if (n > 0) {
			net_pkt_cursor_init(pkt);
			ret = net_pkt_read(pkt, mlink_ring_slot(&m.tx), len);
			if (ret)
				break;
			mlink_ring_publish(&m.tx, len, &meta, m.tx_seq++);
			m.st.tx++;
			if (mlink_ring_need_kick(&m.tx, true))
				modem_kick(MLINK_VEC_DATA);
			break;
		}
		/* Full: ask for a SPACE kick, then re-check (SB pattern). */
		if (mlink_ring_arm_space(&m.tx, true)) {
			mlink_ring_disarm_space(&m.tx);
			continue;
		}
		if (++waits > TX_MAX_WAITS) {
			ret = -ENOBUFS;
			break;
		}
		m.st.tx_waits++;
		/* Sleep unlocked: the link reset needs tx_lock. The timeout covers a lost kick. */
		k_mutex_unlock(&m.tx_lock);
		k_sem_take(&m.space_sem, K_MSEC(TX_WAIT_MS));
		k_mutex_lock(&m.tx_lock, K_FOREVER);
	}
	k_mutex_unlock(&m.tx_lock);
	if (ret)
		m.st.tx_dropped++;
	return ret;
}

/* ---- interface ----------------------------------------------------------- */

static void mlink_iface_init(struct net_if *iface)
{
	static const u8 lladdr[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x02 };
	struct net_in_addr addr, mask;

	net_if_set_link_addr(iface, (u8 *)lladdr, sizeof(lladdr), NET_LINK_DUMMY);
	net_addr_pton(NET_AF_INET, MODEM_IPV4, &addr);
	net_addr_pton(NET_AF_INET, MODEM_NETMASK, &mask);
	net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0);
	net_if_ipv4_set_netmask_by_addr(iface, &addr, &mask);
	/* No carrier until the mlink handshake says UP. */
	net_if_carrier_off(iface);
	mlink_iface = iface;
}

static int mlink_dev_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

static const struct dummy_api mlink_net_api = {
	.iface_api.init = mlink_iface_init,
	.send = mlink_net_send,
};

NET_DEVICE_INIT(mlink, "mlink0", mlink_dev_init, NULL, NULL, NULL,
		CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &mlink_net_api, DUMMY_L2,
		NET_L2_GET_CTX_TYPE(DUMMY_L2), OWN_MTU);

/* ---- services ------------------------------------------------------------ */

static int listen_on(int type, int proto, u16 port)
{
	struct net_sockaddr_in a = {
		.sin_family = NET_AF_INET,
		.sin_port = net_htons(port),
	};
	int s = zsock_socket(NET_AF_INET, type, proto);

	if (s < 0 || zsock_bind(s, (struct net_sockaddr *)&a, sizeof(a)) < 0 ||
	    (type == NET_SOCK_STREAM && zsock_listen(s, 1) < 0)) {
		printk("mlink-modem: port %u: socket setup failed (%d)\n", port, errno);
		return -1;
	}
	return s;
}

static void udp_echo(void *p1, void *p2, void *p3)
{
	static u8 buf[OWN_MTU];
	struct net_sockaddr_in from;
	net_socklen_t flen;
	int s = listen_on(NET_SOCK_DGRAM, NET_IPPROTO_UDP, 7);
	ssize_t n;

	while (s >= 0) {
		flen = sizeof(from);
		n = zsock_recvfrom(s, buf, sizeof(buf), 0, (struct net_sockaddr *)&from, &flen);
		if (n > 0)
			zsock_sendto(s, buf, n, 0, (struct net_sockaddr *)&from, flen);
	}
}

/* One client at a time: echo (port 7) or discard with a byte count (port 9). */
static void tcp_server(void *port_arg, void *p2, void *p3)
{
	static u8 bufs[2][2048];
	u16 port = (uintptr_t)port_arg;
	u8 *buf = bufs[port == 9];
	int s = listen_on(NET_SOCK_STREAM, NET_IPPROTO_TCP, port);

	while (s >= 0) {
		int c = zsock_accept(s, NULL, NULL);
		int64_t t0 = k_uptime_get();
		size_t total = 0;
		ssize_t n;

		if (c < 0)
			continue;
		while ((n = zsock_recv(c, buf, sizeof(buf), 0)) > 0) {
			total += n;
			for (ssize_t off = 0; port == 7 && off < n;) {
				ssize_t w = zsock_send(c, buf + off, n - off, 0);

				if (w <= 0) {
					n = -1;
					break;
				}
				off += w;
			}
			if (n < 0)
				break;
		}
		printk("mlink-modem: tcp :%u client done: %zu bytes in %lld ms\n", port, total,
		       k_uptime_get() - t0);
		zsock_close(c);
	}
}

K_THREAD_DEFINE(udp_echo_tid, 2048, udp_echo, NULL, NULL, NULL, 7, 0, 0);
K_THREAD_DEFINE(tcp_echo_tid, 3072, tcp_server, (void *)7, NULL, NULL, 7, 0, 0);
K_THREAD_DEFINE(tcp_discard_tid, 3072, tcp_server, (void *)9, NULL, NULL, 7, 0, 0);
