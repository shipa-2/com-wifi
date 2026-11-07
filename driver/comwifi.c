// SPDX-License-Identifier: GPL-2.0
/*
 * comwifi: FullMAC cfg80211 driver for an ESP8266 attached to a serial port.
 *
 * Transport is a tty line discipline (N_DEVELOPMENT), so it works on any tty:
 * 16550 COM port, USB-UART bridge, ... Attach with tools/comwifi-attach.
 *
 * The ESP does scan / associate / WPA2 handshake itself.  iwd hands us the PMK
 * in NL80211_CMD_CONNECT (4-way-handshake offload), we pass it on as hex.
 *
 * A single kthread owns link bring-up, baud negotiation, self-tests and the
 * keep-alive.  Everything it needs from the wire it gets through cw_req(),
 * a synchronous request/response primitive completed from the RX path.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/tty.h>
#include <linux/tty_ldisc.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/random.h>
#include <linux/hex.h>
#include <linux/ktime.h>
#include <linux/rtnetlink.h>
#include <net/cfg80211.h>

#include "../proto/comwifi_proto.h"

#define DRV "comwifi"

static unsigned int baud = 2000000;
module_param(baud, uint, 0644);
MODULE_PARM_DESC(baud, "target serial rate negotiated after link-up (any value the tty and ESP can do, e.g. 178500)");

static bool compress = true;
module_param(compress, bool, 0644);
MODULE_PARM_DESC(compress, "LZ4-compress Ethernet frames on the serial link when the ESP supports it");

static bool autotune;
module_param(autotune, bool, 0644);
MODULE_PARM_DESC(autotune, "search for the fastest reliable baud right after link-up");

#define TXQ_MAX		128
#define TXQ_LOW		32
#define EVQ_MAX		256
#define MAX_REQ_RESP	(CW_MAX_ETH_FRAME + 8)

struct cw_cb {
	u8 type, id, flags;
};
#define CW_CB(skb) ((struct cw_cb *)(skb)->cb)

enum tune_op { TUNE_NONE, TUNE_SET, TUNE_TEST, TUNE_AUTO };

struct cw_testres {
	u32 ok, bad, lost;
	u64 bytes;
	u32 ms;
};

struct cw_ndev {
	struct cw *cw;
};

struct cw {
	struct tty_struct *tty;
	struct wiphy *wiphy;
	struct wireless_dev wdev;
	struct net_device *ndev;
	bool registered;
	bool dead;

	/* rx framing */
	u8 rx_buf[CW_MAX_WIRE];
	unsigned int rx_len;
	bool rx_bad;

	/* tx */
	struct workqueue_struct *wq;
	struct work_struct tx_work, ev_work;
	struct sk_buff_head ctl_q, data_q, ev_q;
	u8 tx_raw[CW_MAX_RAW];
	u8 tx_wire[CW_MAX_WIRE];
	unsigned int tx_len, tx_off;

	/* window */
	spinlock_t ctr_lock;
	u8 sent, acked, window;
	bool synced;
	bool paused;

	/* synchronous requests */
	struct mutex req_lock;
	spinlock_t pend_lock;
	struct {
		bool active;
		u8 id, type;
		u8 *buf;
		unsigned int cap;
		int len;
		u8 ack;
		struct completion done;
	} pend;
	u8 next_id;

	/* link thread / tuning */
	struct task_struct *thread;
	wait_queue_head_t thread_wq;
	bool link_up;
	u8 mac[6];
	u32 cur_baud, host_baud, esp_baud, esp_max_baud;
	struct mutex tune_lock;
	struct {
		enum tune_op op;
		u32 arg;
		int result;
		struct completion done;
	} tune;
	char report[256];
	struct cw_testres last_test;
	u32 esp_rx_ok, esp_rx_bad;
	u32 rx_frames, rx_crc_err;
	unsigned long last_rx;

	/* LZ4 (shared codec in proto/) */
	bool lz_want;			/* we can decode / are willing to send LZ */
	bool lz_tx;			/* ... and the ESP can decode, so we send LZ */
	u32 esp_caps;
	u16 lz_ht[1 << 10];
	u8 tx_src[CW_MAX_ETH_FRAME];	/* linearized copy for the compressor */
	u8 rx_dec[CW_MAX_ETH_FRAME];
	u64 lz_tx_raw, lz_tx_wire, lz_rx_raw, lz_rx_wire;

	/* cfg80211 state */
	struct ieee80211_channel channels[13];
	struct ieee80211_rate rates[12];
	struct ieee80211_supported_band band;
	spinlock_t scan_lock;		/* protects scan_req handover */
	struct cfg80211_scan_request *scan_req;
	struct delayed_work scan_timeout;
	bool connecting, connected;
	u8 ssid[32];
	u8 ssid_len;
	bool want_psk;
	u8 bssid[6];
	s8 rssi;
};

static const void *const cw_privid = &cw_privid;

/* ===================================================================== */
/* TX                                                                     */
/* ===================================================================== */

static void cw_kick(struct cw *cw)
{
	if (!cw->dead)
		queue_work(cw->wq, &cw->tx_work);
}

static int cw_queue_ctl(struct cw *cw, u8 type, u8 id, u8 flags,
			const void *pl, unsigned int len)
{
	struct sk_buff *skb = alloc_skb(len, GFP_KERNEL);

	if (!skb)
		return -ENOMEM;
	if (len)
		skb_put_data(skb, pl, len);
	CW_CB(skb)->type = type;
	CW_CB(skb)->id = id;
	CW_CB(skb)->flags = flags;
	skb_queue_tail(&cw->ctl_q, skb);
	cw_kick(cw);
	return 0;
}

static bool cw_window_open(struct cw *cw)
{
	bool open;

	spin_lock_bh(&cw->ctr_lock);
	open = !cw->paused && (u8)(cw->sent - cw->acked) < cw->window;
	spin_unlock_bh(&cw->ctr_lock);
	return open;
}

static void cw_tx_work(struct work_struct *w)
{
	struct cw *cw = container_of(w, struct cw, tx_work);
	struct tty_struct *tty = cw->tty;

	while (!cw->dead) {
		struct sk_buff *skb;
		bool is_data = false;
		struct cw_hdr *h;
		u16 crc;
		unsigned int n, plen;
		int r;

		if (cw->tx_off < cw->tx_len) {
			r = tty->ops->write(tty, cw->tx_wire + cw->tx_off,
					    cw->tx_len - cw->tx_off);
			if (r > 0)
				cw->tx_off += r;
			if (cw->tx_off < cw->tx_len) {
				set_bit(TTY_DO_WRITE_WAKEUP, &tty->flags);
				return;
			}
		}

		skb = skb_dequeue(&cw->ctl_q);
		if (!skb && cw_window_open(cw)) {
			skb = skb_dequeue(&cw->data_q);
			is_data = skb != NULL;
		}
		if (!skb)
			return;

		if (skb->len > CW_MAX_ETH_FRAME) {	/* cannot happen; be safe */
			consume_skb(skb);
			continue;
		}
		h = (struct cw_hdr *)cw->tx_raw;
		h->type = is_data ? CW_DATA : CW_CB(skb)->type;
		h->id = is_data ? 0 : CW_CB(skb)->id;
		h->ack = 0;
		h->flags = is_data ? 0 : CW_CB(skb)->flags;
		plen = skb->len;
		/* skb may be non-linear: copy by offset, never trust ->data */
		if (is_data && cw->lz_tx && skb->len >= 64) {
			int c;

			if (skb_copy_bits(skb, 0, cw->tx_src, skb->len)) {
				consume_skb(skb);
				continue;
			}
			c = cw_lz4_compress(cw->tx_src, skb->len,
					    cw->tx_raw + CW_HDR_LEN + 2,
					    skb->len - 10, cw->lz_ht, 10);
			if (c > 0) {
				cw->tx_raw[CW_HDR_LEN] = skb->len & 0xff;
				cw->tx_raw[CW_HDR_LEN + 1] = skb->len >> 8;
				h->flags = CW_F_LZ;
				plen = 2 + c;
			} else {
				memcpy(cw->tx_raw + CW_HDR_LEN, cw->tx_src, skb->len);
			}
		} else if (skb_copy_bits(skb, 0, cw->tx_raw + CW_HDR_LEN, skb->len)) {
			consume_skb(skb);
			continue;
		}
		if (is_data) {
			cw->lz_tx_raw += skb->len;
			cw->lz_tx_wire += plen;
		}
		n = CW_HDR_LEN + plen;
		crc = cw_crc16(cw->tx_raw, n);
		cw->tx_raw[n++] = crc & 0xff;
		cw->tx_raw[n++] = crc >> 8;
		cw->tx_len = cw_cobs_encode(cw->tx_raw, n, cw->tx_wire);
		cw->tx_off = 0;

		spin_lock_bh(&cw->ctr_lock);
		cw->sent++;
		spin_unlock_bh(&cw->ctr_lock);

		if (is_data && cw->ndev) {
			/* per-cpu stats want preemption off */
			local_bh_disable();
			dev_sw_netstats_tx_add(cw->ndev, 1, skb->len);
			local_bh_enable();
			if (netif_queue_stopped(cw->ndev) &&
			    skb_queue_len(&cw->data_q) < TXQ_LOW)
				netif_wake_queue(cw->ndev);
		}
		consume_skb(skb);
	}
}

static void cw_write_wakeup(struct tty_struct *tty)
{
	struct cw *cw = tty->disc_data;

	clear_bit(TTY_DO_WRITE_WAKEUP, &tty->flags);
	if (cw)
		cw_kick(cw);
}

/*
 * Send a request and wait for the matching response.  Returns the response
 * payload length (copied to `resp`, truncated to `cap`) or -ETIMEDOUT.
 */
static int cw_req(struct cw *cw, u8 type, const void *pl, unsigned int len,
		  void *resp, unsigned int cap, unsigned int timeout_ms,
		  u8 *ack)
{
	u8 id;
	int r;

	mutex_lock(&cw->req_lock);
	id = ++cw->next_id;
	if (!id)
		id = ++cw->next_id;

	spin_lock_bh(&cw->pend_lock);
	cw->pend.active = true;
	cw->pend.id = id;
	cw->pend.type = type;
	cw->pend.buf = resp;
	cw->pend.cap = cap;
	cw->pend.len = -1;
	reinit_completion(&cw->pend.done);
	spin_unlock_bh(&cw->pend_lock);

	r = cw_queue_ctl(cw, type, id, 0, pl, len);
	if (!r && !wait_for_completion_timeout(&cw->pend.done,
					       msecs_to_jiffies(timeout_ms)))
		r = -ETIMEDOUT;

	spin_lock_bh(&cw->pend_lock);
	cw->pend.active = false;
	cw->pend.buf = NULL;
	if (!r) {
		r = cw->pend.len;
		if (ack)
			*ack = cw->pend.ack;
	}
	spin_unlock_bh(&cw->pend_lock);
	mutex_unlock(&cw->req_lock);
	return r;
}

/* ===================================================================== */
/* RX                                                                     */
/* ===================================================================== */

static void cw_rx_data(struct cw *cw, const u8 *eth, unsigned int len)
{
	struct net_device *ndev = cw->ndev;
	struct sk_buff *skb;

	if (!ndev || !cw->connected || len < ETH_HLEN || len > CW_MAX_ETH_FRAME)
		return;
	skb = netdev_alloc_skb_ip_align(ndev, len);
	if (!skb) {
		dev_core_stats_rx_dropped_inc(ndev);
		return;
	}
	skb_put_data(skb, eth, len);
	skb->protocol = eth_type_trans(skb, ndev);
	local_bh_disable();
	dev_sw_netstats_rx_add(ndev, len);
	local_bh_enable();
	netif_rx(skb);
}

static bool cw_is_async(u8 type)
{
	return type == CW_SCAN_RES || type == CW_SCAN_DONE ||
	       type == CW_STATUS || type == CW_LOG;
}

static void cw_rx_frame(struct cw *cw, u8 *buf, unsigned int n)
{
	const struct cw_hdr *h;
	const u8 *pl;
	unsigned int body, pl_len;
	bool kick = false;
	int len;
	u16 crc;

	len = cw_cobs_decode(buf, n);
	if (len < (int)(CW_HDR_LEN + CW_CRC_LEN)) {
		cw->rx_crc_err++;
		return;
	}
	body = len - CW_CRC_LEN;
	crc = buf[body] | (buf[body + 1] << 8);
	if (crc != cw_crc16(buf, body)) {
		cw->rx_crc_err++;
		return;
	}
	cw->rx_frames++;
	cw->last_rx = jiffies;

	h = (const struct cw_hdr *)buf;
	pl = buf + CW_HDR_LEN;
	pl_len = body - CW_HDR_LEN;

	/* window accounting */
	spin_lock_bh(&cw->ctr_lock);
	if (!cw->synced || (u8)(cw->sent - h->ack) > cw->window + 8)
		cw->sent = h->ack;	/* (re)sync to the ESP's counter */
	if (cw->acked != h->ack) {
		cw->acked = h->ack;
		kick = true;
	}
	spin_unlock_bh(&cw->ctr_lock);
	if (kick)
		cw_kick(cw);

	if (h->type == CW_DATA) {
		if (h->flags & CW_F_LZ) {
			unsigned int raw;
			int d;

			if (pl_len < 3)
				return;
			raw = pl[0] | (pl[1] << 8);
			if (raw < ETH_HLEN || raw > CW_MAX_ETH_FRAME)
				return;
			d = cw_lz4_decompress(pl + 2, pl_len - 2, cw->rx_dec,
					      sizeof(cw->rx_dec));
			if (d != (int)raw) {
				cw->rx_crc_err++;
				return;
			}
			cw->lz_rx_raw += raw;
			cw->lz_rx_wire += pl_len;
			cw_rx_data(cw, cw->rx_dec, raw);
		} else {
			cw->lz_rx_raw += pl_len;
			cw->lz_rx_wire += pl_len;
			cw_rx_data(cw, pl, pl_len);
		}
		return;
	}

	if (h->flags & CW_F_RESP) {
		spin_lock_bh(&cw->pend_lock);
		if (cw->pend.active && cw->pend.id == h->id &&
		    cw->pend.type == h->type) {
			if (cw->pend.buf)
				memcpy(cw->pend.buf, pl,
				       min(pl_len, cw->pend.cap));
			cw->pend.len = pl_len;
			cw->pend.ack = h->ack;
			cw->pend.active = false;
			complete(&cw->pend.done);
			spin_unlock_bh(&cw->pend_lock);
			return;
		}
		spin_unlock_bh(&cw->pend_lock);
	}

	if (cw_is_async(h->type) && skb_queue_len(&cw->ev_q) < EVQ_MAX) {
		struct sk_buff *skb = alloc_skb(1 + pl_len, GFP_ATOMIC);

		if (!skb)
			return;
		CW_CB(skb)->type = h->type;
		CW_CB(skb)->id = h->id;
		CW_CB(skb)->flags = h->flags;
		skb_put_data(skb, pl, pl_len);
		skb_queue_tail(&cw->ev_q, skb);
		queue_work(cw->wq, &cw->ev_work);
	}
}

static size_t cw_receive_buf2(struct tty_struct *tty, const u8 *cp,
			      const u8 *fp, size_t count)
{
	struct cw *cw = tty->disc_data;
	size_t i = 0;

	if (!cw || cw->dead)
		return count;

	while (i < count) {
		const u8 *z = memchr(cp + i, 0, count - i);
		size_t seg = (z ? z : cp + count) - (cp + i);

		if (fp) {
			size_t k;

			for (k = 0; k < seg; k++)
				if (fp[i + k])
					cw->rx_bad = true;
		}
		if (!cw->rx_bad) {
			if (cw->rx_len + seg > sizeof(cw->rx_buf))
				cw->rx_bad = true;
			else {
				memcpy(cw->rx_buf + cw->rx_len, cp + i, seg);
				cw->rx_len += seg;
			}
		}
		i += seg;
		if (!z)
			break;
		i++;			/* delimiter */
		if (cw->rx_bad)
			cw->rx_crc_err++;
		else if (cw->rx_len)
			cw_rx_frame(cw, cw->rx_buf, cw->rx_len);
		cw->rx_len = 0;
		cw->rx_bad = false;
	}
	return count;
}

/* ===================================================================== */
/* baud, self-test, negotiation                                           */
/* ===================================================================== */

static int cw_tty_set_baud(struct cw *cw, u32 rate, u32 *actual)
{
	struct tty_struct *tty = cw->tty;
	struct ktermios kt;
	int r;

	down_read(&tty->termios_rwsem);
	kt = tty->termios;
	up_read(&tty->termios_rwsem);
	tty_termios_encode_baud_rate(&kt, rate, rate);
	r = tty_set_termios(tty, &kt);
	if (actual)
		*actual = tty_get_baud_rate(tty);
	return r;
}

static bool cw_rate_close(u32 a, u32 b, unsigned int permille)
{
	u64 d = a > b ? a - b : b - a;

	return d * 1000 <= (u64)b * permille;
}

enum { PAT_ZERO, PAT_FF, PAT_55, PAT_AA, PAT_RAMP, PAT_RAND, PAT_N };

static void cw_fill_pattern(u8 *p, unsigned int len, int pat)
{
	unsigned int i;

	switch (pat) {
	case PAT_ZERO: memset(p, 0x00, len); break;
	case PAT_FF:   memset(p, 0xff, len); break;
	case PAT_55:   memset(p, 0x55, len); break;
	case PAT_AA:   memset(p, 0xaa, len); break;
	case PAT_RAMP: for (i = 0; i < len; i++) p[i] = i; break;
	default:       get_random_bytes(p, len); break;
	}
}

/*
 * Echo test at the current rate.  Covers both directions, COBS with
 * worst-case 0x00 / 0xFF runs, CRC, and frame sizes up to a full MTU.
 */
static void cw_selftest(struct cw *cw, bool quick, unsigned int rounds,
			struct cw_testres *res)
{
	static const unsigned short sizes_full[] = { 16, 64, 256, 1024, 1500 };
	static const unsigned short sizes_quick[] = { 64, 512, 1500 };
	static const int quick_pats[] = { PAT_ZERO, PAT_FF, PAT_RAND };
	const unsigned short *sizes = quick ? sizes_quick : sizes_full;
	unsigned int nsz = quick ? ARRAY_SIZE(sizes_quick) : ARRAY_SIZE(sizes_full);
	unsigned int npat = quick ? 3 : PAT_N;
	u8 *tx, *rx;
	ktime_t t0;
	unsigned int rd, si, pi;

	memset(res, 0, sizeof(*res));
	tx = kmalloc(CW_MAX_ETH_FRAME, GFP_KERNEL);
	rx = kmalloc(MAX_REQ_RESP, GFP_KERNEL);
	if (!tx || !rx) {
		kfree(tx);
		kfree(rx);
		res->lost = 1;
		return;
	}

	t0 = ktime_get();
	for (rd = 0; rd < rounds; rd++) {
		for (si = 0; si < nsz; si++) {
			for (pi = 0; pi < npat; pi++) {
				unsigned int len = sizes[si];
				u32 wire_ms = (u32)div64_u64(2ULL * len * 10 * 1000,
							     max(cw->cur_baud, 1200u));
				int r;

				cw_fill_pattern(tx, len,
						quick ? quick_pats[pi] : pi);
				r = cw_req(cw, CW_BAUD_TEST, tx, len, rx,
					   MAX_REQ_RESP, 150 + 3 * wire_ms, NULL);
				if (r < 0)
					res->lost++;
				else if (r != len || memcmp(tx, rx, len))
					res->bad++;
				else {
					res->ok++;
					res->bytes += 2 * len;
				}
			}
		}
	}
	res->ms = ktime_ms_delta(ktime_get(), t0);
	kfree(tx);
	kfree(rx);
}

static bool cw_test_passed(const struct cw_testres *r)
{
	return r->ok && !r->bad && !r->lost;
}

static void cw_pause(struct cw *cw, bool on)
{
	spin_lock_bh(&cw->ctr_lock);
	cw->paused = on;
	spin_unlock_bh(&cw->ctr_lock);
	if (!on)
		cw_kick(cw);
}

static int cw_ping(struct cw *cw)
{
	struct cw_ping_rsp p;
	int r = cw_req(cw, CW_PING, NULL, 0, &p, sizeof(p), 400, NULL);

	if (r < (int)sizeof(p))
		return r < 0 ? r : -EPROTO;
	cw->esp_rx_ok = p.rx_ok;
	cw->esp_rx_bad = p.rx_bad;
	return 0;
}

/*
 * Move the whole link to `target`.  On any failure both ends end up on the
 * previous rate again (the ESP reverts by itself after CW_TRIAL_MS).
 */
static int cw_negotiate(struct cw *cw, u32 target, bool quick,
			struct cw_testres *res)
{
	struct cw_set_baud sb = { .baud = target };
	struct cw_set_baud_rsp rsp;
	struct cw_testres tr;
	u32 prev = cw->cur_baud, host_actual = 0, scratch;
	int r;

	if (target == prev)
		return 0;
	if (target < CW_BAUD_MIN || target > cw->esp_max_baud)
		return -ERANGE;

	cw_pause(cw, true);

	/* can our tty do it at all? */
	r = cw_tty_set_baud(cw, target, &host_actual);
	cw_tty_set_baud(cw, prev, &scratch);
	if (r || !cw_rate_close(host_actual, target, 30)) {
		pr_info(DRV ": tty cannot do %u (gives %u)\n", target,
			host_actual);
		r = -EOPNOTSUPP;
		goto out;
	}

	r = cw_req(cw, CW_SET_BAUD, &sb, sizeof(sb), &rsp, sizeof(rsp), 400, NULL);
	if (r < (int)sizeof(rsp)) {
		r = -EIO;
		goto out;
	}
	if (rsp.requested != target || !cw_rate_close(rsp.actual, target, 30)) {
		r = -EINVAL;
		goto revert_wait;	/* ESP may or may not have moved */
	}
	/* ESP is now on `actual`, in trial mode */
	cw_tty_set_baud(cw, target, &host_actual);
	cw->host_baud = host_actual;
	cw->esp_baud = rsp.actual;
	cw->cur_baud = target;

	cw_selftest(cw, quick, 1, &tr);
	if (res)
		*res = tr;
	if (cw_test_passed(&tr)) {
		r = cw_req(cw, CW_BAUD_COMMIT, NULL, 0, NULL, 0, 400, NULL);
		if (r >= 0) {
			r = 0;
			goto out;
		}
	}
	r = -EIO;

revert_wait:
	cw_tty_set_baud(cw, prev, &cw->host_baud);
	cw->cur_baud = prev;
	spin_lock_bh(&cw->ctr_lock);
	cw->synced = false;		/* re-sync counters from next frame */
	spin_unlock_bh(&cw->ctr_lock);
	msleep(CW_TRIAL_MS + 250);
	if (cw_ping(cw)) {
		pr_warn(DRV ": lost the ESP after a failed trial, will re-HELLO\n");
		r = -ENOLINK;
	}
	spin_lock_bh(&cw->ctr_lock);
	cw->synced = true;
	spin_unlock_bh(&cw->ctr_lock);
out:
	cw_pause(cw, false);
	return r;
}

static int cw_do_autotune(struct cw *cw, u32 limit)
{
	static const u32 grid[] = { 230400, 460800, 921600, 1500000, 2000000,
				    3000000, 4000000, 5000000 };
	struct cw_testres tr = {};
	u32 good = cw->cur_baud, bad = 0, lo = 0, hi = 0, mid, fin;
	int i, r;

	if (!limit || limit > cw->esp_max_baud)
		limit = cw->esp_max_baud;

	for (i = 0; i < ARRAY_SIZE(grid); i++) {
		if (grid[i] <= good)
			continue;
		if (grid[i] > limit)
			break;
		r = cw_negotiate(cw, grid[i], false, &tr);
		if (r == -ENOLINK)
			return r;
		if (r == -EOPNOTSUPP)	/* the serial port itself is the limit */
			goto done;
		if (r) {
			bad = grid[i];
			break;
		}
		good = grid[i];
	}
	if (!bad && limit > good) {
		r = cw_negotiate(cw, limit, false, &tr);
		if (r == -ENOLINK)
			return r;
		if (r == -EOPNOTSUPP)
			goto done;
		if (r)
			bad = limit;
		else
			good = limit;
	}

	/* bisect between last good and first bad down to ~1 % */
	lo = good;
	hi = bad;
	while (hi && hi - lo > lo / 100 && hi - lo > 1000) {
		mid = (lo + hi) / 2;
		mid -= mid % 100;
		r = cw_negotiate(cw, mid, false, &tr);
		if (r == -ENOLINK)
			return r;
		if (r)
			hi = mid;
		else
			lo = mid;
	}

	/* settle 2 % below the edge unless the edge is just the user's cap */
	fin = hi ? lo - lo / 50 : lo;
	fin -= fin % 100;
	if (fin != cw->cur_baud) {
		r = cw_negotiate(cw, fin, false, &tr);
		if (r == -ENOLINK)
			return r;
		if (r && cw->cur_baud != lo)
			cw_negotiate(cw, lo, false, &tr);
	}

done:
	if (!lo)
		lo = good;
	cw_selftest(cw, false, 2, &tr);
	cw->last_test = tr;
	scnprintf(cw->report, sizeof(cw->report),
		  "autotune: edge ok=%u fail=%u -> running at %u (esp %u), "
		  "final test ok=%u bad=%u lost=%u ~%u B/s\n",
		  lo, hi, cw->cur_baud, cw->esp_baud, tr.ok, tr.bad,
		  tr.lost, tr.ms ? (u32)div_u64(tr.bytes * 1000, tr.ms) : 0);
	return 0;
}

/* ===================================================================== */
/* cfg80211                                                               */
/* ===================================================================== */

static int cw_ies_build(u8 *ie, u8 ssid_len, const u8 *ssid, u8 chan, u8 auth,
			u8 pairwise, u8 group)
{
	static const u8 rates[] = { 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 };
	u8 *p = ie;

	*p++ = WLAN_EID_SSID;
	*p++ = ssid_len;
	memcpy(p, ssid, ssid_len);
	p += ssid_len;
	*p++ = WLAN_EID_SUPP_RATES;
	*p++ = sizeof(rates);
	memcpy(p, rates, sizeof(rates));
	p += sizeof(rates);
	*p++ = WLAN_EID_DS_PARAMS;
	*p++ = 1;
	*p++ = chan;

	if (auth == CW_AUTH_WPA2_PSK || auth == CW_AUTH_WPA_WPA2_PSK) {
		u8 *len;

		*p++ = WLAN_EID_RSN;
		len = p++;
		*p++ = 1; *p++ = 0;				/* version */
		memcpy(p, "\x00\x0f\xac", 3); p += 3;		/* group */
		*p++ = group == CW_CIPHER_CCMP ? 4 : 2;
		if (pairwise == CW_CIPHER_TKIP_CCMP) {
			*p++ = 2; *p++ = 0;
			memcpy(p, "\x00\x0f\xac\x02", 4); p += 4;
			memcpy(p, "\x00\x0f\xac\x04", 4); p += 4;
		} else {
			*p++ = 1; *p++ = 0;
			memcpy(p, "\x00\x0f\xac", 3); p += 3;
			*p++ = pairwise == CW_CIPHER_TKIP ? 2 : 4;
		}
		*p++ = 1; *p++ = 0;				/* AKM: PSK */
		memcpy(p, "\x00\x0f\xac\x02", 4); p += 4;
		*p++ = 0; *p++ = 0;				/* caps */
		*len = p - len - 1;
	}
	if (auth == CW_AUTH_WPA_PSK || auth == CW_AUTH_WPA_WPA2_PSK) {
		u8 *len;

		*p++ = WLAN_EID_VENDOR_SPECIFIC;
		len = p++;
		memcpy(p, "\x00\x50\xf2\x01", 4); p += 4;	/* WPA OUI+type */
		*p++ = 1; *p++ = 0;
		memcpy(p, "\x00\x50\xf2\x02", 4); p += 4;	/* group TKIP */
		*p++ = 1; *p++ = 0;
		memcpy(p, "\x00\x50\xf2\x02", 4); p += 4;	/* pairwise TKIP */
		*p++ = 1; *p++ = 0;
		memcpy(p, "\x00\x50\xf2\x02", 4); p += 4;	/* AKM PSK */
		*len = p - len - 1;
	}
	return p - ie;
}

static void cw_inform(struct cw *cw, const u8 *bssid, u8 chan, s8 rssi,
		      u8 ssid_len, const u8 *ssid, u8 auth, u8 pw, u8 grp)
{
	struct ieee80211_channel *c;
	struct cfg80211_bss *bss;
	u8 ie[160];
	int n;

	c = ieee80211_get_channel(cw->wiphy,
				  ieee80211_channel_to_frequency(chan, NL80211_BAND_2GHZ));
	if (!c)
		return;
	n = cw_ies_build(ie, ssid_len, ssid, chan, auth, pw, grp);
	bss = cfg80211_inform_bss(cw->wiphy, c, CFG80211_BSS_FTYPE_PRESP, bssid,
				  ktime_get_boottime_ns() / 1000,
				  WLAN_CAPABILITY_ESS |
				  (auth != CW_AUTH_OPEN ? WLAN_CAPABILITY_PRIVACY : 0),
				  100, ie, n, DBM_TO_MBM(rssi), GFP_KERNEL);
	if (bss)
		cfg80211_put_bss(cw->wiphy, bss);
}

/*
 * Complete the pending scan exactly once, whoever gets here first (ESP's
 * SCAN_DONE, the timeout, link loss, unload).  Calling cfg80211_scan_done()
 * twice for one request is a use-after-free.
 */
static void cw_scan_finish(struct cw *cw, bool aborted)
{
	struct cfg80211_scan_info info = { .aborted = aborted };
	struct cfg80211_scan_request *req;

	spin_lock_bh(&cw->scan_lock);
	req = cw->scan_req;
	cw->scan_req = NULL;
	spin_unlock_bh(&cw->scan_lock);
	if (req)
		cfg80211_scan_done(req, &info);
}

static void cw_scan_timeout(struct work_struct *w)
{
	struct cw *cw = container_of(w, struct cw, scan_timeout.work);

	cw_scan_finish(cw, true);
}

static int cw_cfg_scan(struct wiphy *wiphy, struct cfg80211_scan_request *rq)
{
	struct cw *cw = wiphy_priv(wiphy);
	struct cw_scan_req sr = {};
	int r;

	if (!cw->link_up)
		return -ENETDOWN;
	if (rq->n_ssids && rq->ssids[0].ssid_len) {
		sr.ssid_len = min_t(u8, rq->ssids[0].ssid_len, 32);
		memcpy(sr.ssid, rq->ssids[0].ssid, sr.ssid_len);
	}
	if (rq->n_channels == 1)
		sr.channel = rq->channels[0]->hw_value;

	spin_lock_bh(&cw->scan_lock);
	if (cw->scan_req) {
		spin_unlock_bh(&cw->scan_lock);
		return -EBUSY;
	}
	cw->scan_req = rq;
	spin_unlock_bh(&cw->scan_lock);

	r = cw_queue_ctl(cw, CW_SCAN_REQ, ++cw->next_id, 0, &sr, sizeof(sr));
	if (r) {
		spin_lock_bh(&cw->scan_lock);
		cw->scan_req = NULL;
		spin_unlock_bh(&cw->scan_lock);
		return r;
	}
	schedule_delayed_work(&cw->scan_timeout, 15 * HZ);
	return 0;
}

static void cw_cfg_abort_scan(struct wiphy *wiphy, struct wireless_dev *wdev)
{
	struct cw *cw = wiphy_priv(wiphy);

	cancel_delayed_work(&cw->scan_timeout);
	cw_scan_finish(cw, true);
}

static int cw_cfg_connect(struct wiphy *wiphy, struct net_device *dev,
			  struct cfg80211_connect_params *sme)
{
	struct cw *cw = wiphy_priv(wiphy);
	struct cw_connect_req cr = {};
	int i, r;

	if (!cw->link_up)
		return -ENETDOWN;
	if (!sme->ssid_len || sme->ssid_len > 32)
		return -EINVAL;
	if (sme->privacy && !sme->crypto.n_akm_suites)
		return -EOPNOTSUPP;	/* WEP */
	if (sme->crypto.wpa_versions & ~(NL80211_WPA_VERSION_1 | NL80211_WPA_VERSION_2))
		return -EOPNOTSUPP;
	/*
	 * wpa_supplicant lists every AKM the profile *allows* (NetworkManager:
	 * PSK and PSK-SHA256); the AP picks.  The ESP only does plain PSK, so
	 * that one must be among them, the others are simply never chosen.
	 */
	if (sme->crypto.n_akm_suites) {
		bool psk = false;

		for (i = 0; i < sme->crypto.n_akm_suites; i++)
			psk |= sme->crypto.akm_suites[i] == WLAN_AKM_SUITE_PSK;
		if (!psk || !sme->crypto.psk)
			return -EOPNOTSUPP;	/* needs 4-way offload with a PMK */
	}

	cr.ssid_len = sme->ssid_len;
	memcpy(cr.ssid, sme->ssid, sme->ssid_len);
	if (sme->bssid) {
		cr.bssid_set = 1;
		memcpy(cr.bssid, sme->bssid, 6);
	}
	if (sme->channel)
		cr.channel = sme->channel->hw_value;
	if (sme->crypto.psk) {
		cr.has_psk = 1;
		bin2hex((char *)cr.psk_hex, sme->crypto.psk, 32);
	}

	memcpy(cw->ssid, sme->ssid, sme->ssid_len);
	cw->ssid_len = sme->ssid_len;
	cw->want_psk = cr.has_psk;
	cw->connecting = true;
	r = cw_queue_ctl(cw, CW_CONNECT_REQ, ++cw->next_id, 0, &cr, sizeof(cr));
	if (r)
		cw->connecting = false;
	memzero_explicit(&cr, sizeof(cr));
	return r;
}

static int cw_cfg_disconnect(struct wiphy *wiphy, struct net_device *dev,
			     u16 reason)
{
	struct cw *cw = wiphy_priv(wiphy);
	bool was_connected = cw->connected, was_connecting = cw->connecting;
	int r = cw_queue_ctl(cw, CW_DISCONNECT_REQ, ++cw->next_id, 0, NULL, 0);

	/* cfg80211 keeps the connection until the driver reports it gone;
	 * the ESP's own confirmation arrives later and is then a no-op. */
	cw->connected = cw->connecting = false;
	if (was_connected) {
		netif_carrier_off(dev);
		cfg80211_disconnected(dev, reason, NULL, 0, true, GFP_KERNEL);
	} else if (was_connecting) {
		cfg80211_connect_result(dev, NULL, NULL, 0, NULL, 0,
					WLAN_STATUS_UNSPECIFIED_FAILURE, GFP_KERNEL);
	}
	return r;
}

static int cw_cfg_get_station(struct wiphy *wiphy, struct wireless_dev *wdev,
			      const u8 *mac, struct station_info *si)
{
	struct cw *cw = wiphy_priv(wiphy);

	if (!cw->connected || !ether_addr_equal(mac, cw->bssid))
		return -ENOENT;
	si->filled |= BIT_ULL(NL80211_STA_INFO_SIGNAL);
	si->signal = cw->rssi;
	return 0;
}

static int cw_cfg_change_iface(struct wiphy *wiphy, struct net_device *dev,
			       enum nl80211_iftype type, struct vif_params *p)
{
	return type == NL80211_IFTYPE_STATION ? 0 : -EOPNOTSUPP;
}

static void cw_cfg_mgmt_regs(struct wiphy *wiphy, struct wireless_dev *wdev,
			     struct mgmt_frame_regs *upd)
{
}

static const struct cfg80211_ops cw_cfg_ops = {
	.scan = cw_cfg_scan,
	.abort_scan = cw_cfg_abort_scan,
	.connect = cw_cfg_connect,
	.disconnect = cw_cfg_disconnect,
	.get_station = cw_cfg_get_station,
	.change_virtual_intf = cw_cfg_change_iface,
	.update_mgmt_frame_registrations = cw_cfg_mgmt_regs,
};

static const u32 cw_ciphers[] = { WLAN_CIPHER_SUITE_TKIP, WLAN_CIPHER_SUITE_CCMP };

static void cw_init_band(struct cw *cw)
{
	static const u16 br[12] = { 10, 20, 55, 110, 60, 90, 120, 180, 240, 360, 480, 540 };
	int i;

	for (i = 0; i < 13; i++) {
		cw->channels[i].band = NL80211_BAND_2GHZ;
		cw->channels[i].center_freq = 2412 + 5 * i;
		cw->channels[i].hw_value = i + 1;
		cw->channels[i].max_power = 20;
	}
	for (i = 0; i < 12; i++)
		cw->rates[i].bitrate = br[i];
	cw->band.band = NL80211_BAND_2GHZ;
	cw->band.channels = cw->channels;
	cw->band.n_channels = 13;
	cw->band.bitrates = cw->rates;
	cw->band.n_bitrates = 12;
	cw->wiphy->bands[NL80211_BAND_2GHZ] = &cw->band;
}

/* ===================================================================== */
/* async events from the ESP                                              */
/* ===================================================================== */

static void cw_on_status(struct cw *cw, const struct cw_status *st)
{
	struct net_device *ndev = cw->ndev;

	if (!ndev)
		return;
	cw->rssi = st->rssi;

	if (st->state == CW_ST_CONNECTED) {
		if (cw->connecting) {
			struct cfg80211_bss *bss;

			memcpy(cw->bssid, st->bssid, 6);
			bss = cfg80211_get_bss(cw->wiphy, NULL, st->bssid, cw->ssid,
					       cw->ssid_len, IEEE80211_BSS_TYPE_ESS,
					       IEEE80211_PRIVACY_ANY);
			if (bss)
				cfg80211_put_bss(cw->wiphy, bss);
			else
				cw_inform(cw, st->bssid, st->channel, st->rssi,
					  cw->ssid_len, cw->ssid,
					  cw->want_psk ? CW_AUTH_WPA2_PSK : CW_AUTH_OPEN,
					  CW_CIPHER_CCMP, CW_CIPHER_CCMP);
			cw->connecting = false;
			cw->connected = true;
			netif_carrier_on(ndev);
			cfg80211_connect_result(ndev, st->bssid, NULL, 0, NULL, 0,
						WLAN_STATUS_SUCCESS, GFP_KERNEL);
		} else if (cw->connected) {
			memcpy(cw->bssid, st->bssid, 6);
		}
		return;
	}

	if (st->state == CW_ST_DISCONNECTED) {
		if (cw->connecting) {
			cw->connecting = false;
			cfg80211_connect_result(ndev, NULL, NULL, 0, NULL, 0,
						WLAN_STATUS_UNSPECIFIED_FAILURE,
						GFP_KERNEL);
		} else if (cw->connected) {
			cw->connected = false;
			netif_carrier_off(ndev);
			cfg80211_disconnected(ndev, st->reason, NULL, 0,
					      st->reason == CW_REASON_LOCAL,
					      GFP_KERNEL);
		}
	}
}

static void cw_ev_work(struct work_struct *w)
{
	struct cw *cw = container_of(w, struct cw, ev_work);
	struct sk_buff *skb;

	while ((skb = skb_dequeue(&cw->ev_q))) {
		u8 type = CW_CB(skb)->type;

		switch (type) {
		case CW_SCAN_RES:
			if (cw->scan_req && skb->len >= sizeof(struct cw_scan_res)) {
				const struct cw_scan_res *r = (void *)skb->data;

				cw_inform(cw, r->bssid, r->channel, r->rssi,
					  min_t(u8, r->ssid_len, 32), r->ssid,
					  r->auth, r->pairwise, r->group);
			}
			break;
		case CW_SCAN_DONE:
			cancel_delayed_work(&cw->scan_timeout);
			cw_scan_finish(cw, false);
			break;
		case CW_STATUS:
			if (skb->len >= sizeof(struct cw_status))
				cw_on_status(cw, (void *)skb->data);
			break;
		case CW_LOG:
			pr_info(DRV ": esp: %.*s\n", skb->len, skb->data);
			break;
		}
		consume_skb(skb);
	}
}

/* ===================================================================== */
/* netdev                                                                 */
/* ===================================================================== */

static int cw_ndo_open(struct net_device *dev)
{
	netif_start_queue(dev);
	return 0;
}

static int cw_ndo_stop(struct net_device *dev)
{
	struct cw *cw = ((struct cw_ndev *)netdev_priv(dev))->cw;

	netif_stop_queue(dev);
	/*
	 * cfg80211 runs its NETDEV_DOWN handler right after this and frees an
	 * unfinished scan request itself (with a WARN).  If we still held it,
	 * a later SCAN_DONE/timeout would call cfg80211_scan_done() on freed
	 * memory.  So the scan ends here, aborted.
	 */
	cancel_delayed_work_sync(&cw->scan_timeout);
	cw_scan_finish(cw, true);
	return 0;
}

static netdev_tx_t cw_ndo_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct cw *cw = ((struct cw_ndev *)netdev_priv(dev))->cw;

	if (!cw->connected || skb->len > CW_MAX_ETH_FRAME || skb->len < ETH_HLEN) {
		dev_core_stats_tx_dropped_inc(dev);
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	skb_orphan(skb);
	skb_queue_tail(&cw->data_q, skb);
	if (skb_queue_len(&cw->data_q) >= TXQ_MAX)
		netif_stop_queue(dev);
	cw_kick(cw);
	return NETDEV_TX_OK;
}

/* No ndo_set_mac_address: the ESP transmits with its own station MAC and a
 * ToDS frame cannot carry any other source address. */
static const struct net_device_ops cw_netdev_ops = {
	.ndo_open = cw_ndo_open,
	.ndo_stop = cw_ndo_stop,
	.ndo_start_xmit = cw_ndo_xmit,
	.ndo_get_stats64 = dev_get_tstats64,
	.ndo_validate_addr = eth_validate_addr,
};

/* ===================================================================== */
/* sysfs: /sys/class/net/wlanN/comwifi/{baud,selftest,autotune,stats}      */
/* ===================================================================== */

static struct cw *cw_from_dev(struct device *d)
{
	return ((struct cw_ndev *)netdev_priv(to_net_dev(d)))->cw;
}

static int cw_tune_run(struct cw *cw, enum tune_op op, u32 arg)
{
	int r;

	if (mutex_lock_interruptible(&cw->tune_lock))
		return -EINTR;
	if (!cw->link_up) {
		r = -ENOLINK;
		goto out;
	}
	reinit_completion(&cw->tune.done);
	cw->tune.arg = arg;
	cw->tune.result = -EINTR;
	WRITE_ONCE(cw->tune.op, op);
	wake_up(&cw->thread_wq);
	if (!wait_for_completion_timeout(&cw->tune.done, 180 * HZ))
		r = -ETIMEDOUT;
	else
		r = cw->tune.result;
out:
	mutex_unlock(&cw->tune_lock);
	return r;
}

static ssize_t baud_show(struct device *d, struct device_attribute *a, char *buf)
{
	struct cw *cw = cw_from_dev(d);

	return sysfs_emit(buf, "requested=%u host_actual=%u esp_actual=%u esp_max=%u\n",
			  cw->cur_baud, cw->host_baud, cw->esp_baud,
			  cw->esp_max_baud);
}

static ssize_t baud_store(struct device *d, struct device_attribute *a,
			  const char *buf, size_t n)
{
	struct cw *cw = cw_from_dev(d);
	u32 v;
	int r = kstrtou32(buf, 0, &v);

	if (r)
		return r;
	r = cw_tune_run(cw, TUNE_SET, v);
	return r ? r : n;
}
static DEVICE_ATTR_RW(baud);

static ssize_t selftest_show(struct device *d, struct device_attribute *a, char *buf)
{
	struct cw *cw = cw_from_dev(d);
	const struct cw_testres *t = &cw->last_test;

	return sysfs_emit(buf, "%s at %u: ok=%u bad=%u lost=%u %llu bytes in %u ms (~%u B/s echo)\n",
			  cw_test_passed(t) ? "PASS" : "FAIL", cw->cur_baud,
			  t->ok, t->bad, t->lost, t->bytes, t->ms,
			  t->ms ? (u32)div_u64(t->bytes * 1000, t->ms) : 0);
}

static ssize_t selftest_store(struct device *d, struct device_attribute *a,
			      const char *buf, size_t n)
{
	struct cw *cw = cw_from_dev(d);
	u32 rounds = 3;
	int r;

	if (kstrtou32(buf, 0, &rounds) || !rounds || rounds > 1000)
		rounds = 3;
	r = cw_tune_run(cw, TUNE_TEST, rounds);
	return r ? r : n;
}
static DEVICE_ATTR_RW(selftest);

static ssize_t autotune_show(struct device *d, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "%s", cw_from_dev(d)->report);
}

static ssize_t autotune_store(struct device *d, struct device_attribute *a,
			      const char *buf, size_t n)
{
	struct cw *cw = cw_from_dev(d);
	u32 limit = 0;
	int r;

	if (kstrtou32(buf, 0, &limit) || limit == 1)
		limit = 0;
	r = cw_tune_run(cw, TUNE_AUTO, limit);
	return r ? r : n;
}
static DEVICE_ATTR_RW(autotune);

static ssize_t compress_show(struct device *d, struct device_attribute *a, char *buf)
{
	struct cw *cw = cw_from_dev(d);
	u64 tr = cw->lz_tx_raw, tw = cw->lz_tx_wire, rr = cw->lz_rx_raw, rw = cw->lz_rx_wire;

	return sysfs_emit(buf,
			  "lz4=%s (esp %s) tx %llu -> %llu bytes (%llu%%)  rx %llu <- %llu bytes (%llu%%)\n",
			  cw->lz_want ? "on" : "off",
			  (cw->esp_caps & CW_CAP_LZ4) ? "supports it" : "does not support it",
			  tr, tw, tr ? div64_u64(tw * 100, tr) : 100,
			  rr, rw, rr ? div64_u64(rw * 100, rr) : 100);
}

static ssize_t compress_store(struct device *d, struct device_attribute *a,
			      const char *buf, size_t n)
{
	struct cw *cw = cw_from_dev(d);
	struct cw_features f;
	bool on;
	int r = kstrtobool(buf, &on);

	if (r)
		return r;
	f.caps = on ? CW_CAP_LZ4 : 0;
	if (cw->link_up && (cw->esp_caps & CW_CAP_LZ4)) {
		/* tell the ESP whether it may keep sending us LZ frames */
		r = cw_req(cw, CW_FEATURES, &f, sizeof(f), NULL, 0, 400, NULL);
		if (r < 0)
			return r;
	}
	cw->lz_want = on;
	cw->lz_tx = on && (cw->esp_caps & CW_CAP_LZ4);
	return n;
}
static DEVICE_ATTR_RW(compress);

static ssize_t stats_show(struct device *d, struct device_attribute *a, char *buf)
{
	struct cw *cw = cw_from_dev(d);

	return sysfs_emit(buf, "host_rx_frames=%u host_rx_bad=%u esp_rx_ok=%u esp_rx_bad=%u window=%u\n",
			  cw->rx_frames, cw->rx_crc_err, cw->esp_rx_ok,
			  cw->esp_rx_bad, cw->window);
}
static DEVICE_ATTR_RO(stats);

static struct attribute *cw_attrs[] = {
	&dev_attr_baud.attr, &dev_attr_selftest.attr,
	&dev_attr_autotune.attr, &dev_attr_stats.attr, &dev_attr_compress.attr, NULL,
};
static const struct attribute_group cw_attr_group = {
	.name = "comwifi",
	.attrs = cw_attrs,
};

/* ===================================================================== */
/* registration                                                           */
/* ===================================================================== */

static int cw_register(struct cw *cw)
{
	struct net_device *ndev;
	int err;

	memcpy(cw->wiphy->perm_addr, cw->mac, 6);
	err = wiphy_register(cw->wiphy);
	if (err)
		return err;

	ndev = alloc_netdev(sizeof(struct cw_ndev), "wlan%d", NET_NAME_ENUM,
			    ether_setup);
	if (!ndev) {
		wiphy_unregister(cw->wiphy);
		return -ENOMEM;
	}
	((struct cw_ndev *)netdev_priv(ndev))->cw = cw;
	cw->wdev.wiphy = cw->wiphy;
	cw->wdev.iftype = NL80211_IFTYPE_STATION;
	cw->wdev.netdev = ndev;
	memcpy(cw->wdev.address, cw->mac, 6);
	ndev->ieee80211_ptr = &cw->wdev;
	ndev->netdev_ops = &cw_netdev_ops;
	ndev->needs_free_netdev = true;
	ndev->pcpu_stat_type = NETDEV_PCPU_STAT_TSTATS;
	ndev->mtu = CW_MTU;
	ndev->max_mtu = CW_MTU;
	ndev->sysfs_groups[0] = &cw_attr_group;
	eth_hw_addr_set(ndev, cw->mac);
	netif_carrier_off(ndev);

	rtnl_lock();
	err = register_netdevice(ndev);
	rtnl_unlock();
	if (err) {
		free_netdev(ndev);
		wiphy_unregister(cw->wiphy);
		return err;
	}
	cw->ndev = ndev;
	cw->registered = true;
	return 0;
}

static void cw_link_lost(struct cw *cw)
{
	cw->link_up = false;
	if (cw->ndev) {
		netif_stop_queue(cw->ndev);
		if (cw->connecting || cw->connected) {
			bool was_conn = cw->connected;

			cw->connecting = cw->connected = false;
			netif_carrier_off(cw->ndev);
			if (was_conn)
				cfg80211_disconnected(cw->ndev, WLAN_REASON_UNSPECIFIED,
						      NULL, 0, true, GFP_KERNEL);
			else
				cfg80211_connect_result(cw->ndev, NULL, NULL, 0, NULL, 0,
							WLAN_STATUS_UNSPECIFIED_FAILURE,
							GFP_KERNEL);
		}
	}
	cancel_delayed_work_sync(&cw->scan_timeout);
	cw_scan_finish(cw, true);
	skb_queue_purge(&cw->data_q);
}

static int cw_hello(struct cw *cw)
{
	struct cw_hello h = { .version = CW_PROTO_VERSION,
			      .max_frame = CW_MAX_ETH_FRAME,
			      .caps = compress ? CW_CAP_LZ4 : 0 };
	struct cw_hello r = {};
	u8 ack;
	int n;

	spin_lock_bh(&cw->ctr_lock);
	cw->synced = false;
	spin_unlock_bh(&cw->ctr_lock);

	n = cw_req(cw, CW_HELLO, &h, sizeof(h), &r, sizeof(r), 500, &ack);
	if (n < CW_HELLO_V1_LEN)	/* v1 peers have no .caps: stays 0 */
		return n < 0 ? n : -EPROTO;
	if (r.version != CW_PROTO_VERSION) {
		pr_err(DRV ": protocol version %u, expected %u\n", r.version,
		       CW_PROTO_VERSION);
		return -EPROTONOSUPPORT;
	}
	memcpy(cw->mac, r.mac, 6);
	cw->esp_max_baud = r.max_baud;
	cw->esp_caps = r.caps;
	cw->lz_want = compress;
	cw->lz_tx = compress && (r.caps & CW_CAP_LZ4);
	spin_lock_bh(&cw->ctr_lock);
	cw->window = clamp_t(u8, r.window, 1, 16);
	cw->sent = cw->acked = ack;
	cw->synced = true;
	spin_unlock_bh(&cw->ctr_lock);
	return 0;
}

static void cw_run_tune(struct cw *cw)
{
	enum tune_op op = READ_ONCE(cw->tune.op);
	struct cw_testres tr;
	int r = 0;

	cw->report[0] = 0;
	switch (op) {
	case TUNE_SET:
		r = cw_negotiate(cw, cw->tune.arg, false, &tr);
		if (!r)
			cw->last_test = tr;
		break;
	case TUNE_TEST:
		cw_pause(cw, true);
		cw_selftest(cw, false, cw->tune.arg, &cw->last_test);
		cw_pause(cw, false);
		r = cw_test_passed(&cw->last_test) ? 0 : -EIO;
		break;
	case TUNE_AUTO:
		r = cw_do_autotune(cw, cw->tune.arg);
		break;
	default:
		break;
	}
	cw->tune.op = TUNE_NONE;
	cw->tune.result = r;
	complete(&cw->tune.done);
}

static int cw_thread(void *data)
{
	struct cw *cw = data;
	unsigned int missed = 0;

	while (!kthread_should_stop()) {
		if (!cw->link_up) {
			cw_tty_set_baud(cw, CW_BAUD_BOOT, &cw->host_baud);
			cw->cur_baud = cw->esp_baud = CW_BAUD_BOOT;
			if (cw_hello(cw)) {
				msleep_interruptible(500);
				continue;
			}
			if (!cw->registered) {
				int err = cw_register(cw);

				if (err) {
					pr_err(DRV ": register failed: %d\n", err);
					msleep_interruptible(2000);
					continue;
				}
				pr_info(DRV ": %s up, ESP MAC %pM, esp max baud %u\n",
					cw->ndev->name, cw->mac, cw->esp_max_baud);
			}
			cw->link_up = true;
			missed = 0;
			if (cw->ndev)
				netif_wake_queue(cw->ndev);

			if (autotune) {
				cw_do_autotune(cw, 0);
				pr_info(DRV ": %s", cw->report);
			} else if (baud > CW_BAUD_BOOT) {
				struct cw_testres tr;
				int r = cw_negotiate(cw, baud, false, &tr);

				if (!r)
					cw->last_test = tr;
				pr_info(DRV ": baud %u: %s (now %u)\n", baud,
					r ? "refused, staying put" : "ok",
					cw->cur_baud);
			}
		}

		wait_event_interruptible_timeout(cw->thread_wq,
				kthread_should_stop() ||
				READ_ONCE(cw->tune.op) != TUNE_NONE, HZ);
		if (kthread_should_stop())
			break;
		if (READ_ONCE(cw->tune.op) != TUNE_NONE) {
			cw_run_tune(cw);
			missed = 0;
		} else if (cw_ping(cw)) {
			if (++missed >= 3) {
				pr_warn(DRV ": ESP stopped answering, re-establishing\n");
				cw_link_lost(cw);
			}
		} else {
			missed = 0;
		}
	}
	return 0;
}

/* ===================================================================== */
/* line discipline                                                        */
/* ===================================================================== */

static int cw_ldisc_open(struct tty_struct *tty)
{
	struct wiphy *wiphy;
	struct cw *cw;

	if (tty->disc_data)
		return -EEXIST;
	if (!tty->ops->write)
		return -EOPNOTSUPP;
	if (!capable(CAP_NET_ADMIN))
		return -EPERM;

	/* Adapter is powered from DTR/RTS, not a separate supply: assert both
	 * before anything tries to talk to the ESP so it has power first. */
	if (tty->ops->tiocmset)
		tty->ops->tiocmset(tty, TIOCM_DTR | TIOCM_RTS, 0);

	wiphy = wiphy_new(&cw_cfg_ops, sizeof(*cw));
	if (!wiphy)
		return -ENOMEM;
	cw = wiphy_priv(wiphy);
	cw->wiphy = wiphy;
	cw->tty = tty;
	cw->window = 1;
	cw->esp_max_baud = CW_BAUD_BOOT;
	spin_lock_init(&cw->ctr_lock);
	spin_lock_init(&cw->pend_lock);
	spin_lock_init(&cw->scan_lock);
	mutex_init(&cw->req_lock);
	mutex_init(&cw->tune_lock);
	init_completion(&cw->pend.done);
	init_completion(&cw->tune.done);
	init_waitqueue_head(&cw->thread_wq);
	skb_queue_head_init(&cw->ctl_q);
	skb_queue_head_init(&cw->data_q);
	skb_queue_head_init(&cw->ev_q);
	INIT_WORK(&cw->tx_work, cw_tx_work);
	INIT_WORK(&cw->ev_work, cw_ev_work);
	INIT_DELAYED_WORK(&cw->scan_timeout, cw_scan_timeout);
	cw->wq = alloc_workqueue(DRV, WQ_UNBOUND | WQ_MEM_RECLAIM, 0);
	if (!cw->wq) {
		wiphy_free(wiphy);
		return -ENOMEM;
	}

	wiphy->privid = cw_privid;
	wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);
	wiphy->max_scan_ssids = 1;
	wiphy->max_scan_ie_len = 255;
	wiphy->signal_type = CFG80211_SIGNAL_TYPE_MBM;
	wiphy->cipher_suites = cw_ciphers;
	wiphy->n_cipher_suites = ARRAY_SIZE(cw_ciphers);
	wiphy_ext_feature_set(wiphy, NL80211_EXT_FEATURE_4WAY_HANDSHAKE_STA_PSK);
	cw_init_band(cw);

	tty->disc_data = cw;
	tty->receive_room = 65536;

	cw->thread = kthread_run(cw_thread, cw, DRV);
	if (IS_ERR(cw->thread)) {
		int err = PTR_ERR(cw->thread);

		tty->disc_data = NULL;
		destroy_workqueue(cw->wq);
		wiphy_free(wiphy);
		return err;
	}
	return 0;
}

static void cw_ldisc_close(struct tty_struct *tty)
{
	struct cw *cw = tty->disc_data;

	if (!cw)
		return;
	if (tty->ops->tiocmset)
		tty->ops->tiocmset(tty, 0, TIOCM_DTR | TIOCM_RTS);
	cw->dead = true;
	tty->disc_data = NULL;
	kthread_stop(cw->thread);
	complete_all(&cw->pend.done);
	cancel_delayed_work_sync(&cw->scan_timeout);
	cancel_work_sync(&cw->tx_work);
	cancel_work_sync(&cw->ev_work);
	destroy_workqueue(cw->wq);
	cw_scan_finish(cw, true);

	if (cw->registered) {
		rtnl_lock();
		unregister_netdevice(cw->ndev);	/* cfg80211 notifier tears down wdev */
		rtnl_unlock();
		wiphy_unregister(cw->wiphy);
	}
	skb_queue_purge(&cw->ctl_q);
	skb_queue_purge(&cw->data_q);
	skb_queue_purge(&cw->ev_q);
	wiphy_free(cw->wiphy);
}

static int cw_ldisc_ioctl(struct tty_struct *tty, unsigned int cmd,
			  unsigned long arg)
{
	return n_tty_ioctl_helper(tty, cmd, arg);
}

static struct tty_ldisc_ops cw_ldisc = {
	.owner = THIS_MODULE,
	.num = N_DEVELOPMENT,
	.name = DRV,
	.open = cw_ldisc_open,
	.close = cw_ldisc_close,
	.ioctl = cw_ldisc_ioctl,
	.receive_buf2 = cw_receive_buf2,
	.write_wakeup = cw_write_wakeup,
};

static int __init cw_init(void)
{
	int r = tty_register_ldisc(&cw_ldisc);

	if (r)
		pr_err(DRV ": cannot register ldisc: %d\n", r);
	return r;
}

static void __exit cw_exit(void)
{
	tty_unregister_ldisc(&cw_ldisc);
}

module_init(cw_init);
module_exit(cw_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ESP8266-over-UART FullMAC WiFi driver");
MODULE_ALIAS_LDISC(N_DEVELOPMENT);
