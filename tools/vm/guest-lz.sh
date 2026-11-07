#!/bin/bash
export PATH=/bin:/usr/bin:/sbin
mount -t proc proc /proc; mount -t sysfs sys /sys; mount -t devtmpfs dev /dev 2>/dev/null
echo 1 > /proc/sys/kernel/panic_on_oops; echo 1 > /proc/sys/kernel/panic; dmesg -n 4
say() { echo "### $*"; }
insmod /m/rfkill.ko; insmod /m/cfg80211.ko; insmod /m/comwifi.ko baud=115200
/bin/comwifi-attach /dev/ttyS1 & ATT=$!; sleep 4
ip link set wlan0 up; iw dev wlan0 connect TestNet; sleep 2; ip addr add 10.9.9.2/24 dev wlan0
C=/sys/class/net/wlan0/comwifi/compress
say "initial"; cat $C
say "zeros payload (very compressible)";  ping -c10 -i0.1 -s 1200 -p 00 10.9.9.1 | tail -3; cat $C
say "text-like payload";                   ping -c10 -i0.1 -s 1200 -p 6162636465666768 10.9.9.1 | tail -3; cat $C
say "default (incrementing) payload";      ping -c10 -i0.1 -s 1200 10.9.9.1 | tail -3; cat $C
say "small packets (below threshold)";     ping -c10 -i0.1 -s 20 10.9.9.1 | tail -3
say "compress OFF";  echo 0 > $C; cat $C; ping -c10 -i0.1 -s 1200 -p 00 10.9.9.1 | tail -3; cat $C
say "compress ON again"; echo 1 > $C; ping -c10 -i0.1 -s 1200 -p 00 10.9.9.1 | tail -3; cat $C
say "burst"; for i in $(seq 1 6); do ping -c60 -i0.01 -s 1400 -p 00 10.9.9.1 > /dev/null 2>&1 & done; sleep 8; ping -c3 10.9.9.1 | tail -2; cat $C
say "stats"; cat /sys/class/net/wlan0/comwifi/stats; ip -s link show wlan0 | tail -4
say "unload"; kill $ATT; sleep 3; rmmod comwifi; say "rmmod rc=$?"
dmesg | grep -iE "BUG|WARN|Oops|lockdep|refcount|corrupt|comwifi:" | head
say "DONE"; echo o > /proc/sysrq-trigger; sleep 5
