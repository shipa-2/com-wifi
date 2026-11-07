#!/usr/bin/env python3
"""Talk to the real firmware from userspace (no kernel module needed).

    probe.py /dev/ttyUSB0 [baud ...]     negotiate each baud in turn, echo-test it
"""
import sys, os, struct, time, random, importlib.util
spec = importlib.util.spec_from_file_location("f", os.path.join(os.path.dirname(__file__), "fake-esp.py"))
f = importlib.util.module_from_spec(spec); spec.loader.exec_module(f)
import serial

class Link:
    def __init__(s, port, baud=115200):
        s.p = serial.Serial(port, baud, timeout=0.05); s.id = 0; s.buf = bytearray()
    def req(s, typ, pl=b"", timeout=0.5):
        s.id = (s.id % 255) + 1
        h = bytes([typ, s.id, 0, 0]) + pl
        s.p.write(f.cobs_enc(h + struct.pack("<H", f.crc16(h))))
        end = time.time() + timeout
        while time.time() < end:
            s.buf += s.p.read(4096)
            while 0 in s.buf:
                i = s.buf.index(0); raw = bytes(s.buf[:i]); del s.buf[:i + 1]
                d = f.cobs_dec(raw) if raw else None
                if not d or len(d) < 6 or f.crc16(d[:-2]) != struct.unpack("<H", d[-2:])[0]: continue
                if d[0] == typ and d[1] == s.id and d[3] & 1: return d[4:-2]
        return None

def echo_test(l, baud, rounds=1):
    ok = bad = lost = 0; t0 = time.time(); nbytes = 0
    for _ in range(rounds):
        for size in (16, 64, 256, 1024, 1500):
            for name, gen in (("zero", lambda n: bytes(n)), ("ff", lambda n: b"\xff" * n),
                              ("ramp", lambda n: bytes(i & 255 for i in range(n))),
                              ("rand", lambda n: random.randbytes(n))):
                pl = gen(size); r = l.req(5, pl, 0.3 + 3 * size * 20 / baud)
                if r is None: lost += 1
                elif r != pl: bad += 1
                else: ok += 1; nbytes += 2 * size
    dt = time.time() - t0
    return ok, bad, lost, nbytes / dt

def main():
    port = sys.argv[1]; targets = [int(x) for x in sys.argv[2:]]
    l = Link(port)
    # firmware may be mid-boot / left on another rate: retry HELLO
    for _ in range(10):
        r = l.req(1, struct.pack("<BBH6sII", 1, 0, 1514, bytes(6), 0, 0))
        if r: break
    else: sys.exit("no HELLO answer at 115200")
    ver, win, mf, mac, fw, maxb = struct.unpack("<BBH6sII", r[:18])
    print(f"HELLO ok: proto {ver} window {win} mac {mac.hex(':')} fw {fw:#x} max_baud {maxb}")
    p = l.req(4); print("PING:", struct.unpack("<III", p) if p else None)
    print("echo @115200 (3 rounds, ok/bad/lost/Bps):", echo_test(l, 115200, 3))
    cur = 115200
    for b in targets:
        r = l.req(3, struct.pack("<I", b), 0.5)
        if not r: print(f"{b}: SET_BAUD no answer"); continue
        req, act, trial = struct.unpack("<III", r)
        print(f"{b}: esp divider gives {act} ({(act-b)/b*100:+.2f}%)")
        l.p.baudrate = b; time.sleep(0.05); l.p.reset_input_buffer(); l.buf.clear()
        ok, bad, lost, bps = echo_test(l, b, 3)
        good = ok and not bad and not lost
        print(f"  echo: ok={ok} bad={bad} lost={lost} ~{bps/1000:.0f} kB/s -> {'PASS' if good else 'FAIL'}")
        if good:
            print("  commit:", l.req(6) is not None); cur = b
        else:
            l.p.baudrate = cur; print(f"  reverting; waiting for ESP trial timeout"); time.sleep(1.8)
            l.p.reset_input_buffer(); l.buf.clear(); print("  ping after revert:", l.req(4) is not None)
if __name__ == "__main__": main()
