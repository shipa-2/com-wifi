/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * comwifi wire protocol, shared by the Linux driver and the ESP8266 firmware.
 *
 * Framing:  COBS( hdr | payload | crc16 ) 0x00
 *   - COBS keeps overhead at <= 1 byte per 254 and makes 0x00 an unambiguous
 *     frame delimiter, so resync after line noise / reset costs one frame.
 *   - crc16 is CRC-16/CCITT-FALSE over hdr+payload, little-endian.
 *
 * Flow control (host -> ESP):
 *   Every non-empty frame the host sends consumes one slot of a window of
 *   `window` frames announced in HELLO.  The ESP reports a free-running 8-bit
 *   counter of frames it has taken off the wire in hdr.ack (corrupt frames
 *   count too, so the window can never leak).  The host may have at most
 *   `window` frames unacknowledged.  ESP -> host needs no window: the host
 *   side has megabytes of buffer, the ESP drops when its own queue is full.
 *
 * All multi-byte fields are little-endian.
 */
#ifndef COMWIFI_PROTO_H
#define COMWIFI_PROTO_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/string.h>
#else
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t s8;
#endif

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "comwifi wire structs are used in native byte order; little-endian hosts only"
#endif

#define CW_PROTO_VERSION	1
#define CW_MTU			1500
#define CW_MAX_ETH_FRAME	1514			/* no FCS */
#define CW_HDR_LEN		4
#define CW_CRC_LEN		2
#define CW_MAX_RAW		(CW_HDR_LEN + CW_MAX_ETH_FRAME + CW_CRC_LEN)
/* worst-case COBS expansion: 1 lead byte + 1 per 254, + trailing delimiter */
#define CW_MAX_WIRE		(CW_MAX_RAW + CW_MAX_RAW / 254 + 3)

#define CW_BAUD_BOOT		115200

enum cw_type {
	CW_NOP		= 0x00,	/* pure ack */
	CW_HELLO	= 0x01,	/* both ways; ESP answers with identity */
	CW_DATA		= 0x02,	/* one Ethernet frame */
	CW_SET_BAUD	= 0x03,	/* host->ESP request, ESP echoes then switches */
	CW_PING		= 0x04,	/* keep-alive; ESP falls back to boot baud when
				 * it hears nothing valid for CW_LINK_TIMEOUT_MS.
				 * Response carries struct cw_ping_rsp. */
	CW_BAUD_TEST	= 0x05,	/* host->ESP: payload is echoed back verbatim */
	CW_BAUD_COMMIT	= 0x06,	/* host->ESP: keep the trial baud, echo */
	CW_FEATURES	= 0x07,	/* host->ESP: struct cw_features, echoed back */

	CW_SCAN_REQ	= 0x10,	/* host->ESP */
	CW_SCAN_RES	= 0x11,	/* ESP->host, one per BSS */
	CW_SCAN_DONE	= 0x12,	/* ESP->host */
	CW_CONNECT_REQ	= 0x13,	/* host->ESP */
	CW_DISCONNECT_REQ = 0x14,	/* host->ESP */
	CW_STATUS	= 0x15,	/* ESP->host, link state; also answers GET_STATUS */
	CW_GET_STATUS	= 0x16,	/* host->ESP */

	CW_LOG		= 0x7f,	/* ESP->host, ASCII */
};

#define CW_LINK_TIMEOUT_MS	3000
/*
 * Baud negotiation:
 *   1. host sends SET_BAUD{baud}; ESP answers at the OLD rate with
 *      cw_set_baud_rsp (the baud its divider really gives), then switches
 *      and starts a trial of CW_TRIAL_MS.
 *   2. host switches its tty, runs BAUD_TEST echo rounds.
 *   3. all passed -> BAUD_COMMIT, trial ends.  Anything else -> host goes
 *      back to the old rate; the ESP does the same on its own when the trial
 *      expires without a COMMIT.
 */
#define CW_TRIAL_MS		1500
#define CW_BAUD_MIN		9600

/* hdr.flags */
#define CW_F_RESP		0x01
/*
 * DATA frame payload is [u16 raw_len][LZ4 block] instead of the raw Ethernet
 * frame.  Set per frame, only when it actually got smaller.
 */
#define CW_F_LZ			0x02

/* capability bits, exchanged in HELLO and changed later with CW_FEATURES */
#define CW_CAP_LZ4		0x00000001	/* "I can decode LZ4 DATA frames" */

struct cw_hdr {
	u8 type;
	u8 id;		/* request id, echoed in responses */
	u8 ack;		/* ESP->host: frames consumed so far (mod 256) */
	u8 flags;
} __attribute__((packed));

struct cw_hello {
	u8  version;
	u8  window;		/* host->ESP frame window (ESP->host field) */
	u16 max_frame;		/* largest Ethernet frame accepted */
	u8  mac[6];		/* station MAC (ESP->host) */
	u32 fw_version;
	u32 max_baud;		/* highest baud the firmware is willing to run */
	u32 caps;		/* CW_CAP_*; both directions.  Absent (zero) in v1 replies */
} __attribute__((packed));
#define CW_HELLO_V1_LEN		18	/* size without .caps */

struct cw_features {
	u32 caps;		/* what the sender of this message can decode */
} __attribute__((packed));

struct cw_set_baud {
	u32 baud;
} __attribute__((packed));

struct cw_set_baud_rsp {
	u32 requested;
	u32 actual;		/* 80 MHz / floor(80 MHz / requested) */
	u32 trial_ms;
} __attribute__((packed));

struct cw_ping_rsp {
	u32 rx_ok;		/* valid frames received */
	u32 rx_bad;		/* CRC/COBS/overflow failures */
	u32 baud;		/* current ESP baud (as requested) */
} __attribute__((packed));

struct cw_scan_req {
	u8 ssid_len;		/* 0 = broadcast scan */
	u8 ssid[32];
	u8 channel;		/* 0 = all */
} __attribute__((packed));

enum cw_auth {
	CW_AUTH_OPEN	= 0,
	CW_AUTH_WEP	= 1,
	CW_AUTH_WPA_PSK	= 2,
	CW_AUTH_WPA2_PSK = 3,
	CW_AUTH_WPA_WPA2_PSK = 4,
};

enum cw_cipher {
	CW_CIPHER_NONE	= 0,
	CW_CIPHER_WEP	= 1,
	CW_CIPHER_TKIP	= 2,
	CW_CIPHER_CCMP	= 3,
	CW_CIPHER_TKIP_CCMP = 4,	/* mixed */
};

struct cw_scan_res {
	u8 bssid[6];
	u8 channel;
	s8 rssi;
	u8 auth;		/* enum cw_auth */
	u8 pairwise;		/* enum cw_cipher */
	u8 group;		/* enum cw_cipher */
	u8 ssid_len;
	u8 ssid[32];
} __attribute__((packed));

struct cw_scan_done {
	u8 status;		/* 0 ok, else aborted/failed */
	u8 count;
} __attribute__((packed));

struct cw_connect_req {
	u8 ssid_len;
	u8 ssid[32];
	u8 bssid_set;
	u8 bssid[6];
	u8 channel;		/* 0 = any */
	u8 has_psk;
	u8 psk_hex[64];		/* 256-bit PMK as 64 hex chars (WPA/WPA2-PSK) */
} __attribute__((packed));

enum cw_state {
	CW_ST_DISCONNECTED = 0,
	CW_ST_CONNECTING   = 1,
	CW_ST_CONNECTED    = 2,
};

/*
 * cw_status.reason: raw 802.11 reason code, or an ESP8266 SDK code
 * (200+) when the failure happened before a real reason existed.
 */
#define CW_REASON_UNSPECIFIED	1
#define CW_REASON_LOCAL		3	/* deauth because we asked for it */
#define CW_REASON_NO_AP		201	/* SDK: SSID not found */
#define CW_REASON_BAD_AUTH	202	/* SDK: auth failed (wrong key) */
#define CW_REASON_ASSOC_FAIL	203
#define CW_REASON_HS_TIMEOUT	204	/* SDK: 4-way handshake timeout */

struct cw_status {
	u8 state;		/* enum cw_state */
	u8 reason;
	u8 bssid[6];
	u8 channel;
	s8 rssi;
	u8 ssid_len;
	u8 ssid[32];
} __attribute__((packed));

/* ---------------------------------------------------------------------- */
/* helpers shared by both sides                                           */
/* ---------------------------------------------------------------------- */

#ifdef __KERNEL__
#define CW_INLINE static inline
#else
#define CW_INLINE static inline __attribute__((unused))
#endif

/* CRC-16/CCITT-FALSE, nibble-wise: no big table (matters on the ESP). */
CW_INLINE u16 cw_crc16(const u8 *p, unsigned int n)
{
	static const u16 t[16] = {
		0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
		0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef,
	};
	u16 crc = 0xffff;

	while (n--) {
		crc = (crc << 4) ^ t[(crc >> 12) ^ (*p >> 4)];
		crc = (crc << 4) ^ t[(crc >> 12) ^ (*p++ & 0x0f)];
	}
	return crc;
}

/*
 * COBS-encode `n` bytes of `in` to `out` and append the 0x00 delimiter.
 * `out` needs n + n/254 + 3 bytes.  Returns encoded length incl. delimiter.
 */
CW_INLINE unsigned int cw_cobs_encode(const u8 *in, unsigned int n, u8 *out)
{
	u8 *code_p = out++;
	u8 code = 1;
	u8 *start = code_p;

	while (n--) {
		u8 b = *in++;

		if (b == 0) {
			*code_p = code;
			code_p = out++;
			code = 1;
		} else {
			*out++ = b;
			if (++code == 0xff) {
				*code_p = code;
				code_p = out++;
				code = 1;
			}
		}
	}
	*code_p = code;
	*out++ = 0;
	return (unsigned int)(out - start);
}

/*
 * Decode `n` COBS bytes (delimiter NOT included) in place.
 * Returns decoded length or -1 on malformed input.
 */
CW_INLINE int cw_cobs_decode(u8 *buf, unsigned int n)
{
	unsigned int r = 0, w = 0;

	while (r < n) {
		u8 code = buf[r++];
		unsigned int i;

		if (code == 0 || r + code - 1 > n)
			return -1;
		for (i = 1; i < code; i++)
			buf[w++] = buf[r++];
		if (code != 0xff && r < n)
			buf[w++] = 0;
	}
	return (int)w;
}

/* ---------------------------------------------------------------------- */
/* LZ4 block codec (format-compatible with the reference implementation)   */
/*                                                                          */
/* Tiny on purpose: the ESP8266 has ~512 bytes to spare for the hash table. */
/* No unaligned loads (Xtensa traps on them), no allocation.                */
/* ---------------------------------------------------------------------- */

#define CW_LZ4_MINMATCH		4
#define CW_LZ4_MFLIMIT		12	/* last match must start this far from the end */
#define CW_LZ4_LASTLIT		5	/* and the last bytes are always literals */
#define CW_LZ4_MIN_INPUT	(CW_LZ4_MFLIMIT + 1)

CW_INLINE u32 cw_lz4_load32(const u8 *p)
{
	u32 v;

	memcpy(&v, p, 4);
	return v;
}

CW_INLINE u8 *cw_lz4_put_len(u8 *op, unsigned int v)
{
	while (v >= 255) {
		*op++ = 255;
		v -= 255;
	}
	*op++ = (u8)v;
	return op;
}

/*
 * Compress `n` bytes.  `ht` must hold (1 << hbits) u16 entries.  Returns the
 * compressed size, or 0 when it does not fit into `cap` (callers treat that
 * as "send it uncompressed").
 */
CW_INLINE int cw_lz4_compress(const u8 *src, int n, u8 *dst, int cap, u16 *ht,
			      int hbits)
{
	const u8 *ip = src, *anchor = src, *iend = src + n;
	const u8 *mflimit = iend - CW_LZ4_MFLIMIT;
	const u8 *matchlimit = iend - CW_LZ4_LASTLIT;
	u8 *op = dst, *oend = dst + cap;
	unsigned int lit;
	int i;

	for (i = 0; i < (1 << hbits); i++)
		ht[i] = 0;

	if (n >= CW_LZ4_MIN_INPUT) {
		ip++;				/* position 0 can never match anything */
		while (ip <= mflimit) {
			u32 seq = cw_lz4_load32(ip);
			unsigned int h = (seq * 2654435761u) >> (32 - hbits);
			const u8 *ref = src + ht[h];

			ht[h] = (u16)(ip - src);
			if (ref >= ip || (ip - ref) > 65535 ||
			    cw_lz4_load32(ref) != seq) {
				ip++;
				continue;
			}
			{
				const u8 *mp = ip + CW_LZ4_MINMATCH;
				const u8 *rp = ref + CW_LZ4_MINMATCH;
				unsigned int mlen, off = (unsigned int)(ip - ref);
				u8 *token;

				while (mp < matchlimit && *mp == *rp) {
					mp++;
					rp++;
				}
				lit = (unsigned int)(ip - anchor);
				mlen = (unsigned int)(mp - ip) - CW_LZ4_MINMATCH;
				if (op + 1 + lit / 255 + 1 + lit + 2 + mlen / 255 + 1 > oend)
					return 0;
				token = op++;
				if (lit >= 15) {
					*token = 15 << 4;
					op = cw_lz4_put_len(op, lit - 15);
				} else {
					*token = (u8)(lit << 4);
				}
				memcpy(op, anchor, lit);
				op += lit;
				*op++ = (u8)off;
				*op++ = (u8)(off >> 8);
				if (mlen >= 15) {
					*token |= 15;
					op = cw_lz4_put_len(op, mlen - 15);
				} else {
					*token |= (u8)mlen;
				}
				ip = mp;
				anchor = ip;
			}
		}
	}

	lit = (unsigned int)(iend - anchor);
	if (op + 1 + lit / 255 + 1 + lit > oend)
		return 0;
	if (lit >= 15) {
		*op++ = 15 << 4;
		op = cw_lz4_put_len(op, lit - 15);
	} else {
		*op++ = (u8)(lit << 4);
	}
	memcpy(op, anchor, lit);
	op += lit;
	return (int)(op - dst);
}

/* Returns the decompressed size, or -1 on any malformed/oversized input. */
CW_INLINE int cw_lz4_decompress(const u8 *src, int n, u8 *dst, int cap)
{
	const u8 *ip = src, *iend = src + n;
	u8 *op = dst, *oend = dst + cap;

	while (ip < iend) {
		unsigned int token = *ip++, lit = token >> 4, ml, off;
		const u8 *m;
		u8 b;

		if (lit == 15) {
			do {
				if (ip >= iend)
					return -1;
				b = *ip++;
				lit += b;
			} while (b == 255);
		}
		if (lit > (unsigned int)(iend - ip) || lit > (unsigned int)(oend - op))
			return -1;
		memcpy(op, ip, lit);
		op += lit;
		ip += lit;
		if (ip >= iend)
			break;			/* last sequence carries literals only */
		if (iend - ip < 2)
			return -1;
		off = ip[0] | ((unsigned int)ip[1] << 8);
		ip += 2;
		if (!off || off > (unsigned int)(op - dst))
			return -1;
		ml = token & 15;
		if (ml == 15) {
			do {
				if (ip >= iend)
					return -1;
				b = *ip++;
				ml += b;
			} while (b == 255);
		}
		ml += CW_LZ4_MINMATCH;
		if (ml > (unsigned int)(oend - op))
			return -1;
		m = op - off;
		while (ml--)			/* byte-wise: source may overlap dest */
			*op++ = *m++;
	}
	return (int)(op - dst);
}


#endif /* COMWIFI_PROTO_H */
