#!/bin/bash
# Real iwd + dbus in the guest, with a known network -> autoconnect, as on the laptop.
export PATH=/bin:/usr/bin:/sbin
mount -t proc proc /proc; mount -t sysfs sys /sys; mount -t devtmpfs dev /dev 2>/dev/null
mkdir -p /run/dbus /tmp /var/lib/iwd /etc/iwd /dev/pts; ln -s /run /var/run
echo 1 > /proc/sys/kernel/panic_on_oops; echo 1 > /proc/sys/kernel/panic; dmesg -n 7
say() { echo "### $*"; }
say "crypto modules"; for m in $(cat /m/order); do insmod /m/$m 2>/dev/null; done
insmod /m/rfkill.ko; insmod /m/cfg80211.ko; insmod /m/comwifi.ko baud=115200
/bin/comwifi-attach /dev/ttyS1 & ATT=$!
dbus-daemon --config-file=/etc/dbus.conf --nofork --nopidfile > /dbus.log 2>&1 & sleep 1
iwd -d > /iwd.log 2>&1 &
say "wait for autoconnect"; sleep 25
say "iwd.log head"; head -40 /iwd.log; say "dbus.log"; head -10 /dbus.log
timeout 10 iwctl device list; timeout 10 iwctl station wlan0 show; timeout 10 iwctl known-networks list
ip -br link; iw dev wlan0 link
say "iwd log tail"; tail -25 /iwd.log 2>/dev/null || grep -v '^$' /iwd.log | head -0
say "traffic"; ip addr add 10.9.9.2/24 dev wlan0; ping -c5 -W1 -s 800 10.9.9.1; ip -s link show wlan0
say "stress: timeout 10 iwctl reconnect + scan loops (40s)"
( for i in $(seq 1 15); do timeout 10 iwctl station wlan0 scan; sleep 1; done ) & A=$!
( for i in $(seq 1 8); do timeout 10 iwctl station wlan0 disconnect; sleep 2; timeout 10 iwctl station wlan0 connect TestNet; sleep 2; done ) & B=$!
( ping -c60 -i0.2 -W1 -s 1200 10.9.9.1 >/dev/null 2>&1 ) & C=$!
( sleep 70; echo "### WATCHDOG"; echo w > /proc/sysrq-trigger; sleep 1; echo t > /proc/sysrq-trigger ) &
wait $A $B $C
timeout 10 iwctl station wlan0 show
say "unload"; kill $ATT; sleep 3; pkill iwd; sleep 1; rmmod comwifi; say "rmmod rc=$?"
dmesg | grep -iE "BUG|WARN|Oops|KFENCE|lockdep|refcount|corrupt" | head -20
say "DONE"; echo o > /proc/sysrq-trigger; sleep 5
