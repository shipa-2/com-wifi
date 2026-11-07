#!/bin/bash
# Reproduce plasma-nm: wpa_supplicant with NetworkManager's exact settings
# (taken from the host journal) running at the same time as iwd.
export PATH=/bin:/usr/bin:/sbin
mount -t proc proc /proc; mount -t sysfs sys /sys; mount -t devtmpfs dev /dev 2>/dev/null
mkdir -p /run/dbus /tmp /var/lib/iwd /etc/iwd /dev/pts; ln -s /run /var/run
echo 15 > /proc/sys/kernel/hung_task_timeout_secs; echo 1 > /proc/sys/kernel/hung_task_panic; echo 1 > /proc/sys/kernel/panic_on_oops; echo 1 > /proc/sys/kernel/panic; dmesg -n 7
say() { echo "### $*"; }
for m in $(cat /m/order); do insmod /m/$m 2>/dev/null; done
insmod /m/rfkill.ko; insmod /m/cfg80211.ko; insmod /m/comwifi.ko baud=115200
/bin/comwifi-attach /dev/ttyS1 & ATT=$!
sleep 4
cat > /etc/wpa-nm.conf <<'CONF'
ctrl_interface=/run/wpa
ap_scan=1
network={
  ssid="TestNet"
  scan_ssid=1
  bgscan="simple:30:-70:86400"
  key_mgmt=WPA-PSK WPA-PSK-SHA256
  auth_alg=OPEN
  psk="password123"
}
CONF
nm_like() { # what NetworkManager does around the supplicant
  ip link set wlan0 address 96:19:ad:ab:c2:e1; ip link set wlan0 down; ip link set wlan0 up
  iw dev wlan0 set power_save off; iw dev wlan0 set type managed
  sysctl -q -w net.ipv6.conf.wlan0.disable_ipv6=0 2>/dev/null
}
say "phase A: NM-style wpa_supplicant alone"
nm_like
wpa_supplicant -Dnl80211 -iwlan0 -c /etc/wpa-nm.conf -d > /wpa-a.log 2>&1 & WPA=$!
sleep 25; iw dev wlan0 link; grep -vE "Register frame" /wpa-a.log | grep -E "CTRL-EVENT|Trying to associate|Association request|nl80211: (Connect|Associate|Authenticate|Scan)|SME|connect|failed|EAPOL|RSN|akm|PMK|key_mgmt|drv_flags|capa" | cut -c1-170 | head -45
kill $WPA; sleep 2
say "phase B: iwd AND NM-style wpa_supplicant together (as on the laptop)"
dbus-daemon --config-file=/etc/dbus.conf --nofork --nopidfile > /dbus.log 2>&1 & sleep 1
iwd -d > /iwd.log 2>&1 &
sleep 6
wpa_supplicant -Dnl80211 -iwlan0 -c /etc/wpa-nm.conf -d > /wpa-b.log 2>&1 & WPA=$!
for i in 1 2 3 4 5 6; do nm_like; sleep 5; done
iw dev wlan0 link; grep -vE "Register frame" /wpa-b.log | grep -E "CTRL-EVENT|Trying to associate|Association request|nl80211: (Connect|Associate|Authenticate|Scan)|SME|connect|failed|EAPOL|RSN|akm|PMK|key_mgmt|drv_flags|capa" | cut -c1-170 | head -30
grep -iE "connect|disconnect|error" /iwd.log | tail -8

say "phase C: churn (scan/connect/disconnect/down/up) with both supplicants alive"
echo "### C0: scan then immediately down/up (cfg80211 frees an unfinished scan on DOWN)"
for i in 1 2 3 4 5 6; do iw dev wlan0 scan trigger >/dev/null 2>&1; ip link set wlan0 down; sleep 0.3; ip link set wlan0 up; sleep 1; done
echo "### C0 done"
( sleep 45; echo "### WATCHDOG dump"; echo w > /proc/sysrq-trigger; echo l > /proc/sysrq-trigger; ps -eo pid,stat,wchan:30,cmd | grep -vE "\[|ps -eo" ; ) &
for i in $(seq 1 25); do
  echo "### iter $i"
  echo " a:scan"; iw dev wlan0 scan trigger >/dev/null 2>&1
  echo " b:iwctl-disc"; iwctl station wlan0 disconnect >/dev/null 2>&1
  echo " c:iw-disc"; iw dev wlan0 disconnect >/dev/null 2>&1
  echo " d:iwctl-conn"; iwctl station wlan0 connect TestNet >/dev/null 2>&1 &
  [ $((i % 5)) = 0 ] && { echo " e:link-down"; ip link set wlan0 down; sleep 0.2; ip link set wlan0 up; }
  [ $((i % 7)) = 0 ] && iw dev wlan0 set type managed
  sleep 1
done
sleep 4; iw dev wlan0 link | head -3
say "unload"; kill $WPA; kill $ATT; sleep 3; pkill iwd; sleep 1; rmmod comwifi; say "rmmod rc=$?"
dmesg | grep -iE "BUG|WARN|Oops|KFENCE|lockdep|refcount|corrupt" | head -20
say "DONE"; echo o > /proc/sysrq-trigger; sleep 5
