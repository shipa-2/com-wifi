#!/bin/bash
# Run this instead of a bare insmod when testing the driver on a real machine.
# 1. backs up the project, 2. sync(2)s everything to disk, 3. asks you to
# confirm, 4. loads the module and prints what to do next.
# A kernel Oops here is a full panic (panic_on_oops=1): unsynced files can
# come back as zeros after the reboot.
#
#   tools/safe-load.sh [module params, e.g. baud=460800]
#   CW_PORT=/dev/ttyS0 tools/safe-load.sh baud=115200     # real COM port
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
port=${CW_PORT:-/dev/ttyUSB0}
dest=${CW_BACKUP_DIR:-$HOME/com-wifi-backups}; mkdir -p "$dest"
tar --exclude=.pio --exclude='*.o' --exclude='*.ko' -czf "$dest/com-wifi-$(date +%Y%m%d-%H%M%S).tar.gz" -C "$root/.." "$(basename "$root")"
sync; sync
echo "backup written to $dest, filesystems synced."
echo "About to insmod $root/driver/comwifi.ko $* on the REAL kernel ($(uname -r))."
echo "Unsaved work in other programs can be lost if it panics. Close/save them now."
read -r -p "Type YES to load: " a; [ "$a" = YES ] || { echo aborted; exit 1; }
sudo modprobe cfg80211   # dependency; absent when the laptop has no other WiFi
sudo insmod "$root/driver/comwifi.ko" "$@"

cat <<EOF

=====================================================================
 comwifi.ko loaded. Next steps (run them in this order):

 1. Bind the serial port ($port) to the driver. Keep it running:
      sudo $root/tools/comwifi-attach $port &
    (add -f for RTS/CTS flow control if you wired it; the port must
     exist: ls -l $port)

 2. Wait ~3 seconds, then keep NetworkManager away from the new card
    (it fights iwd over wlan0 and spams association attempts):
      nmcli device set wlan0 managed no

 3. Check that the link is up:
      sudo dmesg | tail -8              # "wlan0 up, ESP MAC ..." + "baud ...: ok"
      iw dev
      cat /sys/class/net/wlan0/comwifi/{baud,selftest,stats}
    "tty cannot do N" / "refused, staying put" is normal on a real COM
    port: it only does 115200, the driver stays there by itself.

 4. Connect with iwd:
      rfkill unblock all                 # only if iwctl shows "Powered off"
      iwctl station wlan0 scan
      iwctl station wlan0 get-networks
      iwctl --passphrase='PASSWORD' station wlan0 connect 'SSID'
      iwctl station wlan0 show           # State: connected, IPv4 address
      ping -c3 <your router>

 5. Tune the speed (optional, needs a UART that can go faster than 115200):
      echo 178500 | sudo tee /sys/class/net/wlan0/comwifi/baud
      echo 1      | sudo tee /sys/class/net/wlan0/comwifi/selftest
      echo 1      | sudo tee /sys/class/net/wlan0/comwifi/autotune
      cat /sys/class/net/wlan0/comwifi/autotune

 6. Unload when done:
      sudo pkill comwifi-attach; sleep 2; sudo rmmod comwifi

 If the machine panics: after the reboot your files are in
 $dest, and tell Claude what you were doing.
=====================================================================
EOF
