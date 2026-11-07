#!/bin/bash
# One-shot: build driver + tools, load the module with autotune (tests every
# baud above 115200 and settles on the fastest reliable one), attach the
# serial port, and bring wlan0 up. Still asks for a YES before insmod on the
# real kernel (see tools/safe-load.sh) - that confirmation is intentional,
# a panic here is panic_on_oops=1 and can lose unsynced work elsewhere.
#
#   tools/quickstart.sh                              # bring the link up only
#   CW_PORT=/dev/ttyS0 tools/quickstart.sh           # real COM port, no autotune (it only does 115200)
#   SSID=myssid PASSWORD=secret tools/quickstart.sh  # also join a network with iwd
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
port=${CW_PORT:-/dev/ttyUSB0}

echo "== building kernel module =="
make -C "$root/driver"

echo "== building comwifi-attach =="
make -C "$root/tools"

echo "== loading driver (autotune=1 unless CW_PORT looks like a plain COM port) =="
case "$port" in
    /dev/ttyS*) "$root/tools/safe-load.sh" ;;
    *)          "$root/tools/safe-load.sh" autotune=1 ;;
esac

echo "== attaching $port =="
sudo "$root/tools/comwifi-attach" "$port" &
attach_pid=$!
sleep 3

nmcli device set wlan0 managed no 2>/dev/null || true
rfkill unblock all 2>/dev/null || true

echo "== link status =="
sudo dmesg | tail -8
cat /sys/class/net/wlan0/comwifi/baud /sys/class/net/wlan0/comwifi/selftest 2>/dev/null || true

if [ -n "$SSID" ]; then
    echo "== connecting to $SSID with iwd =="
    iwctl station wlan0 scan
    sleep 2
    iwctl --passphrase="$PASSWORD" station wlan0 connect "$SSID"
    iwctl station wlan0 show
else
    cat <<EOF

Link is up but not joined to a network. Connect manually:
  iwctl station wlan0 scan
  iwctl station wlan0 get-networks
  iwctl --passphrase='PASSWORD' station wlan0 connect 'SSID'
  iwctl station wlan0 show

comwifi-attach is running in the background (pid $attach_pid). Tear down with:
  sudo pkill comwifi-attach; sleep 2; sudo rmmod comwifi
EOF
fi
