# com-wifi

An ESP8266 used as a FullMAC WiFi NIC for a laptop with no WiFi of its own,
attached over a plain UART (serial port). A Linux `cfg80211` driver binds to
the serial port as a tty line discipline, so standard tools (`iwd`/`iwctl`,
NetworkManager) see it as an ordinary wireless interface.

```
┌──────────────┐   UART (COBS+CRC16 framed)   ┌───────────────┐
│   ESP8266    │ ───────────────────────────► │  comwifi.ko   │ ── wlan0 ── iwd
│  (firmware)  │ ◄─────────────────────────── │ (line discipl.)│
└──────────────┘                              └───────────────┘
```

- `proto/` — the wire protocol shared by both sides: COBS framing, CRC16,
  a small credit window, and a baud-negotiation handshake
  (`SET_BAUD`/`BAUD_TEST`/`COMMIT`).
- `firmware/` — PlatformIO/Arduino-ESP8266 firmware. Builds for `d1_mini`
  (any 4MB ESP8266 module works).
- `driver/` — `comwifi.c`, the out-of-tree kernel module.
- `tools/` — `comwifi-attach` (binds a tty to the driver), `safe-load.sh`
  (backup + confirm + insmod), `quickstart.sh` (does all of the above in one
  shot), `probe.py`/`fake-esp.py` (talk to the firmware from userspace, no
  kernel module needed), `vm/` (QEMU test harness).

## Hardware

Flash the ESP8266 over its own USB port first (or a plain USB-serial
adapter), then move it to the host link:

```
ESP8266 TX (GPIO1) ──► MAX3232 TTL RX
ESP8266 RX (GPIO3) ◄── MAX3232 TTL TX
ESP8266 GND        ──  MAX3232 GND  (shared ground domain)
MAX3232 DB9         ──  host's RS-232 / COM port
```

The ESP8266's GPIOs are 3.3V logic and **not 5V tolerant** — always go
through a proper level shifter (e.g. MAX3232) to a real RS-232 port, never
direct. If a board's level-shifter has its own onboard 5V→3.3V regulator
(e.g. an AMS1117), feed it 5V, not 3.3V.

If you're powering the adapter from a separate USB supply while the DB9 is
also connected to a docked host, make sure both share the same ground
domain (same dock, or the same machine) — two different grounds tied
together through the board's GND trace is how you burn a through-hole pad.

A plain 16550 COM port usually only negotiates 115200; the driver detects
this and stays there rather than failing.

## 32-bit hosts

Everything here builds and runs on 32-bit x86 as-is - the driver's 64-bit
counters go through `div64_u64`/`div_u64` rather than plain division, which
is what plain 64-bit arithmetic needs on a 32-bit kernel. `quickstart.sh`
and `driver/Makefile` both check for kernel headers matching the *running*
32-bit kernel before building and fail with a clear message if they're
missing, instead of a wall of linker errors.

## Build + install + test speed + bring the link up

```bash
tools/quickstart.sh
```

This builds the kernel module and `comwifi-attach`, loads the module with
baud autotune enabled (probes every baud above 115200 and settles on the
fastest one the link survives), attaches the serial port, and brings
`wlan0` up. It still asks for a `YES` confirmation before `insmod` on your
real kernel — that's intentional, a panic here is unrecoverable mid-session
(`panic_on_oops=1`), so it backs up the project and `sync`s disk first.

Options:

```bash
CW_PORT=/dev/ttyUSB0 tools/quickstart.sh                 # default port
CW_PORT=/dev/ttyS0 tools/quickstart.sh                    # real COM port (no autotune, stays at 115200)
SSID=myssid PASSWORD=secret tools/quickstart.sh           # also join a network via iwd
```

Without `SSID`/`PASSWORD` it leaves the link up and prints the manual
`iwctl` steps to scan and connect.

### Test the firmware alone, no kernel module

```bash
python3 tools/probe.py /dev/ttyUSB0 115200 230400 460800
```

Negotiates each baud in turn and runs an echo-test — useful for checking a
new adapter or cable before touching the kernel driver at all.

### Manual control, once the module is loaded

```bash
cat /sys/class/net/wlan0/comwifi/{baud,selftest,stats}
echo 1 | sudo tee /sys/class/net/wlan0/comwifi/autotune
sudo pkill comwifi-attach; sleep 2; sudo rmmod comwifi
```

## VM test harness

`tools/vm/` runs the same driver/firmware combo against a fake ESP8266
(`fake-esp.py`) inside QEMU, including a NetworkManager+iwd stress test
(`guest-nm.sh`) — use this to reproduce anything risky before trying it on
real hardware.
