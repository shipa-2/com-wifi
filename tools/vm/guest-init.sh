#!/bin/bash
# runs as PID 1 inside the test VM
export PATH=/bin:/usr/bin:/sbin
mount -t proc proc /proc; mount -t sysfs sys /sys; mount -t devtmpfs dev /dev 2>/dev/null
echo 1 > /proc/sys/kernel/panic_on_oops; echo 1 > /proc/sys/kernel/panic
dmesg -n 7
say() { echo "### $*"; }
say "plumbing"; printf "PLUMB\\000" > /dev/ttyS1; sleep 1
say "load"
insmod /m/rfkill.ko; insmod /m/cfg80211.ko; insmod /m/comwifi.ko baud=${CW_BAUD:-460800}
/bin/comwifi-attach /dev/ttyS1 & ATT=$!; sleep 5; for t in /proc/[0-9]*; do [ "$(cat $t/comm 2>/dev/null)" = comwifi ] && { echo "kthread $t: $(cat $t/stat | cut -d' ' -f3)"; cat $t/stack 2>&1 | head -8; cat $t/wchan; echo; }; done
ip link set wlan0 up; ip -br link; iw dev
say "sysfs"; for f in baud selftest stats; do cat /sys/class/net/wlan0/comwifi/$f; done
say "scan x5"; for i in 1 2 3 4 5; do iw dev wlan0 scan | grep -E "SSID|signal" | head -4; done
say "open connect"; iw dev wlan0 connect TestNet; sleep 2; iw dev wlan0 link
ip addr add 10.9.9.2/24 dev wlan0; ping -c3 -W1 10.9.9.1; ip -s link show wlan0
say "wpa_supplicant (PMK offload path)"; iw dev wlan0 disconnect; sleep 1
wpa_supplicant -Dnl80211 -iwlan0 -c /etc/wpa.conf > /wpa.log 2>&1 & WPA=$!; sleep 8; iw dev wlan0 link; kill $WPA; sleep 1; grep -iE "CTRL-EVENT|offload|connect" /wpa.log | head -8
say "disconnect/connect loop"
for i in 1 2 3 4 5; do iw dev wlan0 disconnect; sleep 0.5; iw dev wlan0 connect TestNet; sleep 0.5; done
say "scan during connect"; iw dev wlan0 scan >/dev/null & P=$!; iw dev wlan0 scan >/dev/null; wait $P
say "parallel stress"
( sleep 45; echo "### WATCHDOG blocked tasks"; echo w > /proc/sysrq-trigger; sleep 1; echo t > /proc/sysrq-trigger ) &
( for i in $(seq 1 30); do iw dev wlan0 scan >/dev/null 2>&1; done ) & S1=$!
( for i in $(seq 1 30); do iw dev wlan0 connect TestNet >/dev/null 2>&1; sleep 0.2; iw dev wlan0 disconnect >/dev/null 2>&1; done ) & S2=$!
( for i in $(seq 1 100); do ping -c1 -W1 -s 1200 10.9.9.1 >/dev/null 2>&1; done ) & S3=$!
wait $S1 $S2 $S3; iw dev wlan0 connect TestNet; sleep 1
say "baud 178500"; echo 178500 > /sys/class/net/wlan0/comwifi/baud; cat /sys/class/net/wlan0/comwifi/baud
say "selftest"; echo 2 > /sys/class/net/wlan0/comwifi/selftest; cat /sys/class/net/wlan0/comwifi/selftest
say "autotune"; echo 1 > /sys/class/net/wlan0/comwifi/autotune; cat /sys/class/net/wlan0/comwifi/autotune
say "traffic burst"; for i in $(seq 1 100); do ping -c1 -W1 -s 1000 10.9.9.1 >/dev/null 2>&1 & done; sleep 3
say "unload"; kill $ATT; sleep 2; rmmod comwifi; say "rmmod rc=$?"
dmesg | grep -iE "comwifi|BUG|WARN|Oops|KFENCE|lockdep|refcount" | tail -30
say "DONE"
echo o > /proc/sysrq-trigger; sleep 5
