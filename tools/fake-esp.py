#!/usr/bin/env python3
"""Fake ESP8266 for exercising the comwifi driver without hardware.

    ./fake-esp.py [--ceiling 3000000] [--max-baud 5000000] [--esp-clock 80000000]
    # prints /dev/pts/N ; then, as root:  insmod driver/comwifi.ko; tools/comwifi-attach /dev/pts/N

Speaks the real protocol (HELLO, PING, SET_BAUD trial/commit, BAUD_TEST echo,
scan, connect, DATA loopback).  A pty has no real baud rate, so the "line" is
simulated: any BAUD_TEST sent while the negotiated rate is above --ceiling is
answered with a corrupted payload, and rates the ESP divider cannot hit within
3 % are refused, which is what the driver's autotune has to find.
"""
import argparse, os, pty, struct, sys, time, select, tty

def crc16(b):
    c = 0xFFFF
    for x in b:
        c ^= x << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c

def cobs_enc(d):
    out = bytearray(); idx = 0; out.append(0); code = 1
    for b in d:
        if b == 0:
            out[idx] = code; idx = len(out); out.append(0); code = 1
        else:
            out.append(b); code += 1
            if code == 0xFF:
                out[idx] = code; idx = len(out); out.append(0); code = 1
    out[idx] = code
    return bytes(out) + b"\0"

def cobs_dec(d):
    out = bytearray(); i = 0
    while i < len(d):
        code = d[i]; i += 1
        if code == 0 or i + code - 1 > len(d): return None
        out += d[i:i + code - 1]; i += code - 1
        if code != 0xFF and i < len(d): out.append(0)
    return bytes(out)


def _put_len(out, v):
    while v >= 255: out.append(255); v -= 255
    out.append(v)

def lz4_compress(src, cap):
    """Same algorithm/limits as cw_lz4_compress in proto/comwifi_proto.h. None = did not fit."""
    n = len(src); out = bytearray(); anchor = 0; ip = 1; ht = {}
    if n >= 13:
        while ip <= n - 12:
            seq = bytes(src[ip:ip + 4]); ref = ht.get(seq); ht[seq] = ip
            if ref is None or ip - ref > 65535: ip += 1; continue
            mp, rp = ip + 4, ref + 4
            while mp < n - 5 and src[mp] == src[rp]: mp += 1; rp += 1
            lit, mlen = ip - anchor, mp - ip - 4
            tok = bytearray([min(lit, 15) << 4 | min(mlen, 15)])
            if lit >= 15: _put_len(tok, lit - 15)
            tok += src[anchor:ip] + struct.pack("<H", ip - ref)
            if mlen >= 15: _put_len(tok, mlen - 15)
            out += tok; ip = anchor = mp
    lit = n - anchor
    out.append(min(lit, 15) << 4)
    if lit >= 15: _put_len(out, lit - 15)
    out += src[anchor:]
    return bytes(out) if len(out) <= cap else None

def lz4_decompress(src, cap):
    out = bytearray(); ip = 0
    while ip < len(src):
        tok = src[ip]; ip += 1; lit = tok >> 4
        if lit == 15:
            while True:
                b = src[ip]; ip += 1; lit += b
                if b != 255: break
        out += src[ip:ip + lit]; ip += lit
        if ip >= len(src): break
        off = src[ip] | src[ip + 1] << 8; ip += 2; ml = tok & 15
        if ml == 15:
            while True:
                b = src[ip]; ip += 1; ml += b
                if b != 255: break
        ml += 4
        for _ in range(ml): out.append(out[-off])
    assert len(out) <= cap
    return bytes(out)

def inet_csum(b):
    if len(b) % 2: b += b"\0"
    s = sum(struct.unpack(f">{len(b)//2}H", b))
    while s >> 16: s = (s & 0xffff) + (s >> 16)
    return ~s & 0xffff

GW_IP, GW_MAC = bytes([10, 9, 9, 1]), bytes.fromhex("02aabbccdd01")

def gateway(frame):
    """Answer ARP / ICMP echo for 10.9.9.1; return reply Ethernet frame or None."""
    et = frame[12:14]
    if et == b"\x08\x06" and frame[20:22] == b"\x00\x01" and frame[38:42] == GW_IP:       # ARP request
        sha, spa = frame[22:28], frame[28:32]
        return frame[6:12] + GW_MAC + b"\x08\x06" + bytes.fromhex("0001080006040002") + GW_MAC + GW_IP + sha + spa
    if et == b"\x08\x00" and frame[23] == 1 and frame[30:34] == GW_IP and frame[34] == 8:  # ICMP echo
        ihl = (frame[14] & 15) * 4; icmp = bytearray(frame[14 + ihl:]); icmp[0] = 0; icmp[2:4] = b"\0\0"
        icmp[2:4] = struct.pack(">H", inet_csum(bytes(icmp)))
        ip = bytearray(frame[14:14 + ihl]); ip[8] = 64; ip[10:12] = b"\0\0"; ip[12:16], ip[16:20] = frame[30:34], frame[26:30]
        ip[10:12] = struct.pack(">H", inet_csum(bytes(ip)))
        return frame[6:12] + GW_MAC + b"\x08\x00" + bytes(ip) + bytes(icmp)
    return None

(HELLO, DATA, SET_BAUD, PING, BAUD_TEST, BAUD_COMMIT, FEATURES) = 1, 2, 3, 4, 5, 6, 7
SCAN_REQ, SCAN_RES, SCAN_DONE, CONNECT_REQ, DISC_REQ, STATUS, GET_STATUS = 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16
RESP = 1
BOOT = 115200

class Esp:
    def __init__(s, a):
        s.a = a; s.baud = BOOT; s.prev = BOOT; s.trial = False; s.deadline = 0
        s.consumed = 0; s.ok = 0; s.bad = 0; s.host_caps = 0; s.lz = [0, 0, 0, 0]; s.last = time.time()
        if a.tty:                       # attach to an existing tty (e.g. qemu -serial pty)
            s.m = s.sl = os.open(a.tty, os.O_RDWR | os.O_NOCTTY); tty.setraw(s.m)
        else:
            s.m, s.sl = pty.openpty(); tty.setraw(s.m); tty.setraw(s.sl)
        s.buf = bytearray(); s.connected = False

    def send(s, typ, id_=0, flags=0, pl=b""):
        h = bytes([typ, id_, s.consumed & 0xFF, flags]) + pl
        os.write(s.m, cobs_enc(h + struct.pack("<H", crc16(h))))

    def send_data(s, eth):
        if s.host_caps & 1 and not s.a.no_lz and len(eth) >= 64:
            c = lz4_compress(eth, len(eth) - 10)
            if c is not None:
                s.lz[2] += len(eth); s.lz[3] += 2 + len(c)
                return s.send(DATA, 0, 2, struct.pack("<H", len(eth)) + c)
        s.lz[2] += len(eth); s.lz[3] += len(eth)
        s.send(DATA, 0, 0, eth)

    def div_actual(s, b): return s.a.esp_clock // (s.a.esp_clock // b)

    def handle(s, raw):
        if s.a.verbose: print(f"rx {len(raw)}B: {raw[:12].hex()}", file=sys.stderr, flush=True)
        s.consumed += 1
        d = cobs_dec(raw)
        if d is None or len(d) < 6 or crc16(d[:-2]) != struct.unpack("<H", d[-2:])[0]:
            s.bad += 1; print("bad frame", file=sys.stderr); return
        s.ok += 1; s.last = time.time()
        typ, id_, _, _ = d[:4]; pl = d[4:-2]
        if typ == HELLO:
            s.host_caps = struct.unpack("<I", pl[18:22])[0] if len(pl) >= 22 else 0
            caps = 0 if s.a.no_lz else 1
            hello = struct.pack("<BBH6sIII", 1, 4, 1514, bytes.fromhex("02deadbeef01"), 0x10000, s.a.max_baud, caps)
            s.send(HELLO, id_, RESP, hello[:18] if s.a.no_lz else hello)     # --no-lz = a true v1 firmware (no .caps field)
        elif typ == FEATURES:
            s.host_caps = struct.unpack("<I", pl[:4])[0]; s.send(FEATURES, id_, RESP, pl[:4])
            print(f"esp: host lz decode = {bool(s.host_caps & 1)}")
        elif typ == PING:
            s.send(PING, id_, RESP, struct.pack("<III", s.ok, s.bad, s.baud))
        elif typ == SET_BAUD:
            (b,) = struct.unpack("<I", pl[:4])
            if b < 9600 or b > s.a.max_baud: b = s.baud
            act = s.div_actual(b)
            s.send(SET_BAUD, id_, RESP, struct.pack("<III", b, act, 1500))
            if b != s.baud:
                s.prev, s.baud, s.trial, s.deadline = s.baud, b, True, time.time() + 1.5
                print(f"esp: trial {b} (divider gives {act})")
        elif typ == BAUD_TEST:
            if s.baud > s.a.ceiling: pl = bytes(x ^ 0x5A for x in pl[:1]) + pl[1:]  # line noise
            if s.baud > s.a.ceiling * 1.0 and s.a.drop: return
            s.send(BAUD_TEST, id_, RESP, pl)
        elif typ == BAUD_COMMIT:
            s.trial = False; s.send(BAUD_COMMIT, id_, RESP); print(f"esp: committed {s.baud}")
        elif typ == DATA:
            raw = pl
            if d[3] & 2:                                # CW_F_LZ
                (n,) = struct.unpack("<H", pl[:2]); raw = lz4_decompress(pl[2:], 1514)
                assert len(raw) == n, "LZ length mismatch"
            s.lz[0] += len(raw); s.lz[1] += len(pl)
            rep = gateway(raw)
            if rep is None: rep = raw                   # anything else: loop back
            s.send_data(rep)
        elif typ == SCAN_REQ:
            for n, (ssid, ch, rssi, auth) in enumerate([(b"TestNet", 6, -48, 3), (b"OpenCafe", 1, -70, 0)]):
                s.send(SCAN_RES, id_, 0, struct.pack("<6sBbBBBB32s", bytes([2, 0, 0, 0, 0, n + 1]), ch, rssi, auth,
                                                      3 if auth else 0, 3 if auth else 0, len(ssid), ssid))
            s.send(SCAN_DONE, id_, RESP, struct.pack("<BB", 0, 2))
        elif typ == CONNECT_REQ:
            n = pl[0]; ssid = pl[1:1 + n]
            print(f"esp: connect {ssid!r} psk={'yes' if pl[1+32+1+6+1] else 'no'}")
            if s.a.connect_fail:
                s.connected = False; s.status(id_, 202)       # SDK: auth failed
            else:
                s.connected = True; s.status(id_)
        elif typ == DISC_REQ:
            s.connected = False; s.status(id_, 3)
        elif typ == GET_STATUS:
            s.status(id_)

    def status(s, id_, reason=0):
        ssid = b"TestNet"
        s.send(STATUS, id_, 0, struct.pack("<BB6sBbB32s", 2 if s.connected else 0, reason,
               bytes([2, 0, 0, 0, 0, 1]), 6, -48, len(ssid), ssid))

    def run(s):
        print(s.a.tty or os.ttyname(s.sl), flush=True)
        while True:
            r, _, _ = select.select([s.m], [], [], 0.1)
            if r:
                chunk = os.read(s.m, 65536)
                if s.a.verbose: print(f"chunk {len(chunk)}B {chunk[:16].hex()}", file=sys.stderr, flush=True)
                for byte in chunk:
                    if byte == 0:
                        if s.buf: s.handle(bytes(s.buf))
                        s.buf.clear()
                    else: s.buf.append(byte)
            now = time.time()
            if s.trial and now > s.deadline:
                s.trial = False; s.baud = s.prev; print(f"esp: trial expired -> back to {s.baud}")
            if s.baud != BOOT and now - s.last > 3.0:
                s.baud = BOOT; s.trial = False; print("esp: link silent -> boot baud")

if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--ceiling", type=int, default=3_000_000, help="rates above this corrupt data")
    p.add_argument("--max-baud", type=int, default=5_000_000)
    p.add_argument("--esp-clock", type=int, default=80_000_000)
    p.add_argument("--connect-fail", action="store_true", help="answer every CONNECT_REQ with a failure")
    p.add_argument("--no-lz", action="store_true", help="pretend to be a v1 firmware without LZ4")
    p.add_argument("-v", "--verbose", action="store_true")
    p.add_argument("--tty", help="use this existing tty instead of creating a pty")
    p.add_argument("--drop", action="store_true", help="drop instead of corrupt above ceiling")
    Esp(p.parse_args()).run()
