// comwifi firmware: ESP8266 as a UART-attached 802.11 station for Linux.
//
// The ESP does scanning / association / WPA handshake itself (FullMAC); the
// host sees an Ethernet device.  Frames in both directions are raw L2:
//   WiFi -> host : tapped from the lwIP glue (phy_capture) before lwIP sees it
//   host -> WiFi : handed to the STA netif's linkoutput
// The ESP's own IP stack is left unconfigured (no IP, no DHCP) so it never
// answers on behalf of the host; both share one MAC address.

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <string.h>

extern "C" {
#include <user_interface.h>
#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <uart_register.h>
#include <eagle_soc.h>
}

#include "comwifi_proto.h"

#ifndef CW_MAX_BAUD
#define CW_MAX_BAUD 5000000
#endif
#ifndef CW_FW_VERSION
#define CW_FW_VERSION 0x00010000
#endif

// UART RX ring (ISR-fed, owned by the Arduino core). Must hold a full window
// of maximum-size frames so the host can never overrun us.
#define CW_WINDOW        4
#define CW_UART_RX_RING  (CW_WINDOW * (CW_MAX_WIRE) + 512)
// WiFi->host data ring: [u16 len][frame] records.
#define CW_DATA_RING     8192
#define CW_EVQ_LEN       8
#define CW_CONNECT_TIMEOUT_MS 20000

extern "C" void (*phy_capture)(int netif_idx, const char* data, size_t len, int out, int success);

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------

static uint8_t rx_buf[CW_MAX_WIRE];
static unsigned rx_len;
static bool rx_overflow;

static uint8_t tx_raw[CW_MAX_RAW];
static uint8_t tx_wire[CW_MAX_WIRE];

static uint32_t host_caps;       // what the host can decode (HELLO/FEATURES)
static uint8_t dec_buf[CW_MAX_ETH_FRAME];
static uint8_t consumed;       // frames taken off the wire (mod 256) -> hdr.ack
static bool ack_dirty;
static uint32_t last_valid_ms;
static uint32_t cur_baud = CW_BAUD_BOOT;
static uint32_t prev_baud = CW_BAUD_BOOT;   // rate to return to if a trial fails
static bool trial;
static uint32_t trial_deadline_ms;
static uint32_t rx_ok, rx_bad;

// control events to host
struct ev {
    uint8_t type, id, flags, len;
    uint8_t data[48];
};
static ev evq[CW_EVQ_LEN];
static uint8_t ev_head, ev_tail;

// data ring (single producer: phy_capture, single consumer: loop)
static uint8_t data_ring[CW_DATA_RING];
static volatile unsigned dr_head, dr_tail;   // byte offsets

// wifi state
static cw_state link_state = CW_ST_DISCONNECTED;
static uint32_t connect_started_ms;
static uint8_t connect_req_id;
static uint32_t last_status_ms;
static uint8_t last_reason;
static WiFiEventHandler h_conn, h_disc;
static uint32_t ignore_leave_until_ms;   // our own WiFi.disconnect() echoes back as reason 8

// bridge diagnostics (sent to the host as CW_LOG every 2 s while connected)
static uint32_t dbg_tap_in, dbg_tap_out, dbg_tap_rej, dbg_tx_n, dbg_tx_noif, dbg_tx_err;
static int dbg_tx_last_err;

// scan state
static bool scan_running;
static int scan_total = -1;      // -1 none, else networks waiting to be emitted
static int scan_idx;
static uint8_t scan_req_id;

static_assert(sizeof(cw_scan_res) <= 48, "ev too small");
static_assert(sizeof(cw_status) <= 48, "ev too small");
static_assert(sizeof(cw_hello) <= 48, "ev too small");

// ---------------------------------------------------------------------------
// transmit
// ---------------------------------------------------------------------------

// Payload is already at tx_raw + CW_HDR_LEN (l1 bytes): wrap, COBS, write.
static void tx_finish(uint8_t type, uint8_t id, uint8_t flags, unsigned l1)
{
    cw_hdr* h = (cw_hdr*)tx_raw;
    h->type = type;
    h->id = id;
    h->ack = consumed;
    h->flags = flags;
    unsigned n = CW_HDR_LEN + l1;
    uint16_t crc = cw_crc16(tx_raw, n);
    tx_raw[n++] = crc & 0xff;
    tx_raw[n++] = crc >> 8;
    unsigned w = cw_cobs_encode(tx_raw, n, tx_wire);
    Serial.write(tx_wire, w);
    ack_dirty = false;
}

static void tx_frame(uint8_t type, uint8_t id, uint8_t flags, const void* pl1,
                     unsigned l1)
{
    if (l1)
        memcpy(tx_raw + CW_HDR_LEN, pl1, l1);
    tx_finish(type, id, flags, l1);
}

// LZ4 (see proto): only for DATA, only when the host said it can decode, and
// only when the frame really got smaller -- otherwise it goes out unchanged.
static uint16_t lz_ht[1 << 8];
static uint32_t lz_tx_raw, lz_tx_wire, lz_rx_raw, lz_rx_wire;

static void tx_data(const uint8_t* eth, unsigned len)
{
    lz_tx_raw += len;
    if ((host_caps & CW_CAP_LZ4) && len >= 64) {
        uint8_t* dst = tx_raw + CW_HDR_LEN + 2;
        int c = cw_lz4_compress(eth, len, dst, (int)len - 10, lz_ht, 8);
        if (c > 0) {
            tx_raw[CW_HDR_LEN] = len & 0xff;
            tx_raw[CW_HDR_LEN + 1] = len >> 8;
            lz_tx_wire += 2 + c;
            tx_finish(CW_DATA, 0, CW_F_LZ, 2 + c);
            return;
        }
    }
    lz_tx_wire += len;
    tx_frame(CW_DATA, 0, 0, eth, len);
}

static bool ev_push(uint8_t type, uint8_t id, uint8_t flags, const void* d,
                    unsigned len)
{
    uint8_t nx = (ev_head + 1) % CW_EVQ_LEN;
    if (nx == ev_tail)
        return false;
    ev& e = evq[ev_head];
    e.type = type;
    e.id = id;
    e.flags = flags;
    e.len = len;
    memcpy(e.data, d, len);
    ev_head = nx;
    return true;
}

static void push_status(uint8_t id, uint8_t flags)
{
    cw_status st = {};
    st.state = link_state;
    st.reason = last_reason;
    if (link_state == CW_ST_CONNECTED) {
        memcpy(st.bssid, WiFi.BSSID(), 6);
        st.channel = WiFi.channel();
        st.rssi = WiFi.RSSI();
        String s = WiFi.SSID();
        st.ssid_len = s.length() > 32 ? 32 : s.length();
        memcpy(st.ssid, s.c_str(), st.ssid_len);
    }
    ev_push(CW_STATUS, id, flags, &st, sizeof(st));
}

static void set_state(cw_state s, uint8_t reason)
{
    link_state = s;
    last_reason = reason;
    push_status(connect_req_id, 0);
}

// ---------------------------------------------------------------------------
// WiFi -> host data tap
// ---------------------------------------------------------------------------

static inline unsigned dr_free()
{
    return (dr_tail - dr_head - 1) & (CW_DATA_RING - 1);
}

static void dr_put(const uint8_t* p, unsigned n)
{
    unsigned h = dr_head;
    unsigned first = CW_DATA_RING - h;
    if (n <= first) {
        memcpy(data_ring + h, p, n);
    } else {
        memcpy(data_ring + h, p, first);
        memcpy(data_ring, p + first, n - first);
    }
}

static void dr_get(uint8_t* p, unsigned off, unsigned n)
{
    unsigned t = (dr_tail + off) & (CW_DATA_RING - 1);
    unsigned first = CW_DATA_RING - t;
    if (n <= first) {
        memcpy(p, data_ring + t, n);
    } else {
        memcpy(p, data_ring + t, first);
        memcpy(p + first, data_ring, n - first);
    }
}

static void wifi_rx_tap(int netif_idx, const char* data, size_t len, int out,
                        int success)
{
    (void)success;
    if (out) {
        dbg_tap_out++;
        return;
    }
    if (netif_idx != 0 || link_state != CW_ST_CONNECTED) {
        dbg_tap_rej++;
        return;
    }
    dbg_tap_in++;
    if (len < 14 || len > CW_MAX_ETH_FRAME)
        return;
    if (dr_free() < len + 2)
        return;                       // drop; TCP will back off
    uint8_t hdr2[2] = { (uint8_t)len, (uint8_t)(len >> 8) };
    dr_put(hdr2, 2);
    dr_head = (dr_head + 2) & (CW_DATA_RING - 1);
    dr_put((const uint8_t*)data, len);
    dr_head = (dr_head + len) & (CW_DATA_RING - 1);
}

// ---------------------------------------------------------------------------
// host -> WiFi
// ---------------------------------------------------------------------------

static netif* sta_netif()
{
    for (netif* n = netif_list; n; n = n->next)
        if (n->num == 0)
            return n;
    return nullptr;
}

static void wifi_tx(const uint8_t* eth, unsigned len)
{
    if (link_state != CW_ST_CONNECTED || len < 14)
        return;
    netif* n = sta_netif();
    if (!n || !n->linkoutput) {
        dbg_tx_noif++;
        return;
    }
    // PBUF_LINK keeps the headroom the SDK uses to prepend the 802.11 header
    pbuf* p = pbuf_alloc(PBUF_LINK, len, PBUF_RAM);
    if (!p)
        return;
    memcpy(p->payload, eth, len);
    dbg_tx_n++;
    int err = n->linkoutput(n, p);
    if (err) {
        dbg_tx_err++;
        dbg_tx_last_err = err;
    }
    pbuf_free(p);
}

// ---------------------------------------------------------------------------
// request handlers
// ---------------------------------------------------------------------------

static void do_connect(const cw_connect_req& r, uint8_t id)
{
    char ssid[33];
    memcpy(ssid, r.ssid, r.ssid_len);
    ssid[r.ssid_len] = 0;
    char psk[65];
    const char* pw = nullptr;
    if (r.has_psk) {
        memcpy(psk, r.psk_hex, 64);
        psk[64] = 0;
        pw = psk;
    }
    connect_req_id = id;
    connect_started_ms = millis();
    last_reason = 0;
    link_state = CW_ST_CONNECTING;
    ignore_leave_until_ms = millis() + 2000;
    WiFi.disconnect(false);
    // The lwIP glue only brings the STA netif (and with it the raw L2 path in
    // both directions) up once it has an address.  Give it a throw-away
    // link-local one; the host never sees or uses it.
    WiFi.config(IPAddress(169, 254, 190, 1), IPAddress(169, 254, 190, 2),
                IPAddress(255, 255, 255, 0));
    WiFi.begin(ssid, pw, r.channel, r.bssid_set ? r.bssid : nullptr, true);
    push_status(id, 0);
}

static void do_scan(const cw_scan_req& r, uint8_t id)
{
    if (scan_running || scan_total >= 0) {
        cw_scan_done busy = { 1, 0 };
        ev_push(CW_SCAN_DONE, id, CW_F_RESP, &busy, sizeof(busy));
        return;
    }
    char ssid[33];
    memcpy(ssid, r.ssid, r.ssid_len);
    ssid[r.ssid_len] = 0;
    scan_req_id = id;
    scan_running = true;
    WiFi.scanNetworks(true, true, r.channel, r.ssid_len ? (uint8_t*)ssid : nullptr);
}

static void scan_poll()
{
    if (scan_running) {
        int n = WiFi.scanComplete();
        if (n == WIFI_SCAN_RUNNING)
            return;
        scan_running = false;
        if (n < 0) {
            cw_scan_done d = { 1, 0 };
            ev_push(CW_SCAN_DONE, scan_req_id, CW_F_RESP, &d, sizeof(d));
            return;
        }
        scan_total = n;
        scan_idx = 0;
    }
    while (scan_total >= 0 && scan_idx < scan_total) {
        cw_scan_res r = {};
        String ssid;
        uint8_t enc;
        int32_t rssi;
        uint8_t* bssid;
        int32_t ch;
        bool hidden;
        WiFi.getNetworkInfo(scan_idx, ssid, enc, rssi, bssid, ch, hidden);
        memcpy(r.bssid, bssid, 6);
        r.channel = ch;
        r.rssi = rssi;
        r.ssid_len = hidden ? 0 : (ssid.length() > 32 ? 32 : ssid.length());
        memcpy(r.ssid, ssid.c_str(), r.ssid_len);
        switch (enc) {
        case ENC_TYPE_NONE: r.auth = CW_AUTH_OPEN; r.pairwise = r.group = CW_CIPHER_NONE; break;
        case ENC_TYPE_WEP:  r.auth = CW_AUTH_WEP;  r.pairwise = r.group = CW_CIPHER_WEP; break;
        case ENC_TYPE_TKIP: r.auth = CW_AUTH_WPA_PSK; r.pairwise = r.group = CW_CIPHER_TKIP; break;
        case ENC_TYPE_CCMP: r.auth = CW_AUTH_WPA2_PSK; r.pairwise = r.group = CW_CIPHER_CCMP; break;
        default:            r.auth = CW_AUTH_WPA_WPA2_PSK; r.pairwise = CW_CIPHER_TKIP_CCMP; r.group = CW_CIPHER_TKIP; break;
        }
        if (!ev_push(CW_SCAN_RES, scan_req_id, 0, &r, sizeof(r)))
            return;                   // queue full, continue next loop
        scan_idx++;
    }
    if (scan_total >= 0) {
        cw_scan_done d = { 0, (uint8_t)scan_total };
        if (ev_push(CW_SCAN_DONE, scan_req_id, CW_F_RESP, &d, sizeof(d))) {
            WiFi.scanDelete();
            scan_total = -1;
        }
    }
}

static void change_baud(uint32_t baud)
{
    Serial.flush();
    delayMicroseconds(200);
    Serial.updateBaudRate(baud);
    cur_baud = baud;
    rx_len = 0;
    rx_overflow = false;
}

static void handle_frame(uint8_t* buf, unsigned n)
{
    int len = cw_cobs_decode(buf, n);
    if (len < (int)(CW_HDR_LEN + CW_CRC_LEN)) {
        rx_bad++;
        return;
    }
    unsigned body = len - CW_CRC_LEN;
    uint16_t crc = buf[body] | (buf[body + 1] << 8);
    if (crc != cw_crc16(buf, body)) {
        rx_bad++;
        return;
    }
    rx_ok++;

    last_valid_ms = millis();
    if (trial)                        // trial ends after TRIAL_MS of *silence*
        trial_deadline_ms = last_valid_ms + CW_TRIAL_MS;
    const cw_hdr* h = (const cw_hdr*)buf;
    const uint8_t* pl = buf + CW_HDR_LEN;
    unsigned pl_len = body - CW_HDR_LEN;

    switch (h->type) {
    case CW_HELLO: {
        cw_hello r = {};
        r.version = CW_PROTO_VERSION;
        r.window = CW_WINDOW;
        r.max_frame = CW_MAX_ETH_FRAME;
        WiFi.macAddress(r.mac);
        r.fw_version = CW_FW_VERSION;
        r.max_baud = CW_MAX_BAUD;
        r.caps = CW_CAP_LZ4;
        host_caps = 0;                // until this HELLO says otherwise
        if (pl_len >= sizeof(cw_hello)) {
            cw_hello hh;
            memcpy(&hh, pl, sizeof(hh));
            host_caps = hh.caps;
        }
        ev_push(CW_HELLO, h->id, CW_F_RESP, &r, sizeof(r));
        break;
    }
    case CW_FEATURES:
        if (pl_len >= sizeof(cw_features)) {
            cw_features f;
            memcpy(&f, pl, sizeof(f));
            host_caps = f.caps;
            ev_push(CW_FEATURES, h->id, CW_F_RESP, &f, sizeof(f));
        }
        break;
    case CW_DATA:
        if (h->flags & CW_F_LZ) {
            if (pl_len < 2 + 1)
                break;
            unsigned raw = pl[0] | (pl[1] << 8);
            if (raw < 14 || raw > CW_MAX_ETH_FRAME)
                break;
            int d = cw_lz4_decompress(pl + 2, pl_len - 2, dec_buf, sizeof(dec_buf));
            if (d != (int)raw) {
                rx_bad++;
                break;
            }
            lz_rx_raw += raw;
            lz_rx_wire += pl_len;
            wifi_tx(dec_buf, raw);
        } else {
            lz_rx_raw += pl_len;
            lz_rx_wire += pl_len;
            wifi_tx(pl, pl_len);
        }
        break;
    case CW_PING: {
        cw_ping_rsp r = { rx_ok, rx_bad, cur_baud };
        ev_push(CW_PING, h->id, CW_F_RESP, &r, sizeof(r));
        break;
    }
    case CW_BAUD_TEST:
        // echo straight away; bypass the event queue (payload up to 1.5 KB)
        tx_frame(CW_BAUD_TEST, h->id, CW_F_RESP, pl, pl_len);
        break;
    case CW_BAUD_COMMIT:
        trial = false;
        ev_push(CW_BAUD_COMMIT, h->id, CW_F_RESP, nullptr, 0);
        break;
    case CW_SET_BAUD: {
        if (pl_len < sizeof(cw_set_baud))
            break;
        uint32_t b;
        memcpy(&b, pl, 4);
        if (b < CW_BAUD_MIN || b > CW_MAX_BAUD)
            b = cur_baud;             // refuse: answer with the unchanged rate
        uint32_t div = 80000000UL / b;           // core truncates, so do we
        cw_set_baud_rsp r = { b, 80000000UL / div, CW_TRIAL_MS };
        // answer at the old rate (rx_pump already counted this frame)
        tx_frame(CW_SET_BAUD, h->id, CW_F_RESP, &r, sizeof(r));
        if (b != cur_baud) {
            prev_baud = cur_baud;
            change_baud(b);
            trial = true;
            trial_deadline_ms = millis() + CW_TRIAL_MS;
        }
        return;
    }
    case CW_SCAN_REQ:
        if (pl_len >= sizeof(cw_scan_req))
            do_scan(*(const cw_scan_req*)pl, h->id);
        break;
    case CW_CONNECT_REQ:
        if (pl_len >= sizeof(cw_connect_req)) {
            cw_connect_req r;
            memcpy(&r, pl, sizeof(r));
            if (r.ssid_len && r.ssid_len <= 32)
                do_connect(r, h->id);
        }
        break;
    case CW_DISCONNECT_REQ:
        WiFi.disconnect(false);
        connect_req_id = h->id;
        set_state(CW_ST_DISCONNECTED, CW_REASON_LOCAL);
        break;
    case CW_GET_STATUS:
        push_status(h->id, CW_F_RESP);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// main loop pieces
// ---------------------------------------------------------------------------

static void rx_pump()
{
    uint8_t tmp[256];
    int avail;
    while ((avail = Serial.available()) > 0) {
        size_t n = Serial.readBytes((char*)tmp, avail > (int)sizeof(tmp) ? sizeof(tmp) : avail);
        const uint8_t* p = tmp;
        const uint8_t* end = tmp + n;
        while (p < end) {
            const uint8_t* z = (const uint8_t*)memchr(p, 0, end - p);
            unsigned seg = (z ? z : end) - p;
            if (!rx_overflow) {
                if (rx_len + seg > sizeof(rx_buf))
                    rx_overflow = true;
                else {
                    memcpy(rx_buf + rx_len, p, seg);
                    rx_len += seg;
                }
            }
            if (!z)
                break;
            p = z + 1;
            if (rx_len || rx_overflow) {
                consumed++;
                ack_dirty = true;
                if (!rx_overflow)
                    handle_frame(rx_buf, rx_len);
            }
            rx_len = 0;
            rx_overflow = false;
        }
    }
}

static void tx_pump()
{
    // control events first
    while (ev_tail != ev_head) {
        ev& e = evq[ev_tail];
        tx_frame(e.type, e.id, e.flags, e.data, e.len);
        ev_tail = (ev_tail + 1) % CW_EVQ_LEN;
        if (Serial.availableForWrite() < 64)
            break;
    }
    // then received WiFi frames
    unsigned budget = 4;
    while (dr_tail != dr_head && budget--) {
        uint8_t l[2];
        dr_get(l, 0, 2);
        unsigned len = l[0] | (l[1] << 8);
        static uint8_t frame[CW_MAX_ETH_FRAME];
        dr_get(frame, 2, len);
        dr_tail = (dr_tail + 2 + len) & (CW_DATA_RING - 1);
        tx_data(frame, len);
    }
    if (ack_dirty)
        tx_frame(CW_NOP, 0, 0, nullptr, 0);
}

static void link_watchdog()
{
    uint32_t now = millis();
    // lost host while running fast (e.g. host rebooted): fall back so a
    // fresh driver can find us at the boot rate.
    if (trial && (int32_t)(now - trial_deadline_ms) >= 0) {
        // host never confirmed the new rate: go back to the last good one
        trial = false;
        change_baud(prev_baud);
        last_valid_ms = now;
    }
    if (cur_baud != CW_BAUD_BOOT && now - last_valid_ms > CW_LINK_TIMEOUT_MS) {
        trial = false;
        change_baud(CW_BAUD_BOOT);
        last_valid_ms = now;
    }
    if (link_state == CW_ST_CONNECTING && now - connect_started_ms > CW_CONNECT_TIMEOUT_MS) {
        WiFi.disconnect(false);
        set_state(CW_ST_DISCONNECTED, CW_REASON_NO_AP);
    }
    if (link_state == CW_ST_CONNECTED && now - last_status_ms > 2000) {
        last_status_ms = now;
        push_status(0, 0);            // carries fresh RSSI
        char msg[48];
        int m = snprintf(msg, sizeof(msg), "in%u out%u rej%u tx%u noif%u err%u/%d",
                         (unsigned)dbg_tap_in, (unsigned)dbg_tap_out, (unsigned)dbg_tap_rej,
                         (unsigned)dbg_tx_n, (unsigned)dbg_tx_noif, (unsigned)dbg_tx_err, dbg_tx_last_err);
        ev_push(CW_LOG, 0, 0, msg, m);
        m = snprintf(msg, sizeof(msg), "lz tx %u>%u rx %u<%u",
                     (unsigned)lz_tx_raw, (unsigned)lz_tx_wire,
                     (unsigned)lz_rx_raw, (unsigned)lz_rx_wire);
        ev_push(CW_LOG, 0, 0, msg, m);
    }
}

#ifdef CW_HWFLOW
static void hwflow_enable()
{
    // GPIO13 = U0CTS, GPIO15 = U0RTS (function 4 on both)
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTCK_U, 4);
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTDO_U, 4);
    // deassert RTS when RX FIFO holds >= 96 bytes (FIFO is 128)
    SET_PERI_REG_BITS(UART_CONF1(0), UART_RX_FLOW_THRHD, 96, UART_RX_FLOW_THRHD_S);
    SET_PERI_REG_MASK(UART_CONF1(0), UART_RX_FLOW_EN);
    SET_PERI_REG_MASK(UART_CONF0(0), UART_TX_FLOW_EN);
}
#endif

void setup()
{
    Serial.setRxBufferSize(CW_UART_RX_RING);
    Serial.begin(CW_BAUD_BOOT);
    Serial.setDebugOutput(false);
#ifdef CW_HWFLOW
    hwflow_enable();
#endif

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    WiFi.setAutoConnect(false);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    wifi_station_dhcpc_stop();
    WiFi.disconnect(false);

    h_conn = WiFi.onStationModeConnected([](const WiFiEventStationModeConnected&) {
        if (link_state == CW_ST_CONNECTING)
            set_state(CW_ST_CONNECTED, 0);
    });
    h_disc = WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected& e) {
        if (e.reason == WIFI_DISCONNECT_REASON_ASSOC_LEAVE &&
            (int32_t)(millis() - ignore_leave_until_ms) < 0)
            return;                   // stale echo of our own disconnect
        if (link_state != CW_ST_DISCONNECTED)
            set_state(CW_ST_DISCONNECTED, (uint8_t)e.reason);
    });

    phy_capture = wifi_rx_tap;
    last_valid_ms = millis();
}

void loop()
{
    rx_pump();
    scan_poll();
    link_watchdog();
    tx_pump();
    yield();
}
