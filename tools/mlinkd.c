// SPDX-License-Identifier: GPL-2.0-only
/*
 * mlinkd - the "modem" side control daemon (P7.4). Serves requests arriving
 * on /dev/mlink_ctrl in the modem guest:
 *   GET_LINK_INFO -> link state, MTU, peer generation
 *   GET_STATS     -> mlink0 packet/byte counters
 *   SET_DATA_CALL -> bring mlink0 up/down (the "data call"), answer, then
 *                    send an IND_DATA_CALL indication
 * Survives SSR: while the link is down, reads fail with ENOTCONN / poll
 * reports POLLHUP; the daemon waits and carries on when the link is back.
 *
 * Usage (in the modem guest): mlinkd [-v]
 */
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "mlink_ctrl_lib.h"

static int verbose;

static uint64_t sysfs_u64(const char *path)
{
	unsigned long long v = 0;
	FILE *f = fopen(path, "r");

	if (f) {
		if (fscanf(f, "%llu", &v) != 1)
			v = 0;
		fclose(f);
	}
	return v;
}

static int data_call_state(void)
{
	struct ifreq ifr = { 0 };
	int s = socket(AF_INET, SOCK_DGRAM, 0), up = 0;

	strncpy(ifr.ifr_name, "mlink0", IFNAMSIZ - 1);
	if (s >= 0 && ioctl(s, SIOCGIFFLAGS, &ifr) == 0)
		up = !!(ifr.ifr_flags & IFF_UP);
	if (s >= 0)
		close(s);
	return up;
}

static int set_data_call(int up)
{
	struct ifreq ifr = { 0 };
	int s = socket(AF_INET, SOCK_DGRAM, 0), ret = -errno;

	if (s < 0)
		return ret;
	strncpy(ifr.ifr_name, "mlink0", IFNAMSIZ - 1);
	ret = ioctl(s, SIOCGIFFLAGS, &ifr);
	if (ret == 0) {
		if (up)
			ifr.ifr_flags |= IFF_UP;
		else
			ifr.ifr_flags &= ~IFF_UP;
		ret = ioctl(s, SIOCSIFFLAGS, &ifr);
	}
	if (ret)
		ret = -errno;
	close(s);
	return ret;
}

static void send_msg(int fd, struct msgbuf *m)
{
	if (write(fd, m->raw, msg_size(m)) < 0 && verbose)
		perror("mlinkd: write");
}

static void serve(int fd, const struct msgbuf *req, size_t len)
{
	struct msgbuf resp;

	msg_init(&resp, req->h.msg_id, MLINK_CTRL_RESP, req->h.txn_id, 0);
	switch (req->h.msg_id) {
	case MLINK_MSG_GET_LINK_INFO: {
		struct mlink_ctrl_info info;
		uint32_t mtu = sysfs_u64("/sys/class/net/mlink0/mtu");

		if (ioctl(fd, MLINK_IOC_GET_INFO, &info) == 0) {
			tlv_put(&resp, MLINK_TLV_LINK_STATE, &info.link_state, 4);
			tlv_put(&resp, MLINK_TLV_PEER_GEN, &info.peer_gen, 4);
		}
		tlv_put(&resp, MLINK_TLV_MTU, &mtu, 4);
		break;
	}
	case MLINK_MSG_GET_STATS: {
		static const struct { uint8_t t; const char *f; } st[] = {
			{ MLINK_TLV_RX_PKTS, "rx_packets" }, { MLINK_TLV_TX_PKTS, "tx_packets" },
			{ MLINK_TLV_RX_BYTES, "rx_bytes" }, { MLINK_TLV_TX_BYTES, "tx_bytes" },
		};
		char path[96];
		size_t i;

		for (i = 0; i < sizeof(st) / sizeof(st[0]); i++) {
			uint64_t v;

			snprintf(path, sizeof(path), "/sys/class/net/mlink0/statistics/%s", st[i].f);
			v = sysfs_u64(path);
			tlv_put(&resp, st[i].t, &v, 8);
		}
		break;
	}
	case MLINK_MSG_SET_DATA_CALL: {
		const uint8_t *want = tlv_get(req, len, MLINK_TLV_DATA_CALL, 1);
		struct msgbuf ind;
		uint8_t now;

		if (!want) {
			resp.h.result = -EINVAL;
			break;
		}
		resp.h.result = set_data_call(*want);
		now = data_call_state();
		tlv_put(&resp, MLINK_TLV_DATA_CALL, &now, 1);
		send_msg(fd, &resp);
		/* Unsolicited: every AP client learns the new state. */
		msg_init(&ind, MLINK_MSG_IND_DATA_CALL, MLINK_CTRL_IND, 0, 0);
		tlv_put(&ind, MLINK_TLV_DATA_CALL, &now, 1);
		send_msg(fd, &ind);
		if (verbose)
			printf("mlinkd: data call -> %s\n", now ? "up" : "down");
		return;
	}
	default:
		resp.h.result = -EOPNOTSUPP;
	}
	send_msg(fd, &resp);
}

int main(int argc, char **argv)
{
	struct msgbuf m;
	int fd;

	verbose = argc > 1 && !strcmp(argv[1], "-v");
	fd = open("/dev/mlink_ctrl", O_RDWR);
	if (fd < 0) {
		perror("/dev/mlink_ctrl");
		return 1;
	}
	fprintf(stderr, "mlinkd: serving /dev/mlink_ctrl\n");
	for (;;) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		ssize_t n;

		if (poll(&p, 1, 1000) < 0)
			continue;
		if (p.revents & POLLERR)	/* device gone (rmmod / unbind) */
			break;
		n = read(fd, m.raw, sizeof(m.raw));
		if (n < 0) {
			if (errno == ENOTCONN || errno == EAGAIN || errno == EINTR) {
				usleep(100000);		/* SSR in progress: wait */
				continue;
			}
			perror("mlinkd: read");
			break;
		}
		if ((size_t)n < sizeof(m.h) || m.h.type != MLINK_CTRL_REQ)
			continue;
		if (verbose)
			printf("mlinkd: req msg 0x%04x txn %u\n", m.h.msg_id, m.h.txn_id);
		serve(fd, &m, n);
	}
	close(fd);
	return 0;
}
