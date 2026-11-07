#!/bin/bash
# Real ESP8266 via USB passthrough (CH340 -> guest ch341 driver) + real iwd with autoconnect.
export PATH=/bin:/usr/bin:/sbin
mount -t proc proc /proc; mount -t sysfs sys /sys; mount -t devtmpfs dev /dev 2>/dev/null
mkdir -p /run/dbus /tmp /var/lib/iwd /etc/iwd /dev/pts; ln -s /run /var/run
echo 1 > /proc/sys/kernel/panic_on_oops; echo 1 > /proc/sys/kernel/panic; dmesg -n 7
say() { echo "### $*"; }
say "modules"; for m in $(cat /m/order); do insmod /m/$m 2>/dev/null; done
insmod /m/rfkill.ko; insmod /m/cfg80211.ko; insmod /m/ch341.ko
for i in $(seq 1 20); do [ -e /dev/ttyUSB0 ] && break; sleep 1; done; ls -la /dev/ttyUSB0
insmod /m/comwifi.ko baud=${CW_BAUD:-460800}
/bin/comwifi-attach /dev/ttyUSB0 & ATT=$!
dbus-daemon --config-file=/etc/dbus.conf --nofork --nopidfile > /dbus.log 2>&1 & sleep 1
iwd -d > /iwd.log 2>&1 &
sleep 12
say "driver state"; dmesg | grep -i comwifi; cat /sys/class/net/wlan0/comwifi/baud /sys/class/net/wlan0/comwifi/selftest /sys/class/net/wlan0/comwifi/stats 2>&1
say "scan"; timeout 10 iwctl station wlan0 scan; sleep 8; timeout 10 iwctl station wlan0 get-networks | head -12
say "wait for autoconnect (wrong passphrase => failure path)"; sleep 40
say "station"; timeout 10 iwctl station wlan0 show; timeout 10 iwctl known-networks list
say "iwd log (connect-related)"; grep -iE "connect|fail|handshake|auth|error|disconnect" /iwd.log | tail -25
say "IP + real traffic"; ip -br addr show wlan0; ip route
GW=$(ip route | awk '/default/{print $3}'); echo "gateway: $GW"
if [ -n "$GW" ]; then
  ping -c10 -W2 -s 56 $GW | tail -3
  ping -c100 -i 0.02 -W2 -s 1400 $GW | tail -3
  ping -c5 -W2 1.1.1.1 | tail -3
  ( for i in 1 2 3 4; do ping -c150 -i 0.01 -W1 -s 1200 $GW >/dev/null 2>&1 & done; wait ) ; echo burst-done
fi
ip -s link show wlan0; cat /sys/class/net/wlan0/comwifi/stats
say "driver after"; dmesg | grep -iE "comwifi|BUG|WARN|Oops|refcount|corrupt" | tail -15; cat /sys/class/net/wlan0/comwifi/stats
say "unload"; kill $ATT; sleep 3; pkill iwd; sleep 1; rmmod comwifi; say "rmmod rc=$?"
dmesg | grep -iE "BUG|WARN|Oops|lockdep|refcount|corrupt" | head
say "DONE"; echo o > /proc/sysrq-trigger; sleep 5
