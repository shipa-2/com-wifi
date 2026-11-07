#!/bin/bash
# Boot the driver in a throw-away QEMU guest talking to tools/fake-esp.py.
# Usage: tools/vm/run.sh [workdir]     (the host kernel is untouched)
set -e
here=$(cd "$(dirname "$0")" && pwd); root=$here/../..
W=${1:-$(mktemp -d)}; mkdir -p "$W/rd"/{bin,m,proc,sys,dev,lib,lib64,usr/lib}
K=$(uname -r); M=/usr/lib/modules/$K
for m in kernel/net/rfkill/rfkill kernel/net/wireless/cfg80211; do zstd -dqf $M/$m.ko.zst -o "$W/rd/m/$(basename $m).ko"; done
cp "$root/driver/comwifi.ko" "$W/rd/m/"; cp "$root/tools/comwifi-attach" "$W/rd/bin/"
mkdir -p "$W/rd/etc"; printf 'network={\n ssid="TestNet"\n psk="password123"\n key_mgmt=WPA-PSK\n}\n' > "$W/rd/etc/wpa.conf"
cp /usr/lib/iwd/iwd "$W/rd/bin/iwd"
mkdir -p "$W/rd/var/lib/iwd"; printf '[Security]\nPassphrase=password123\n' > "$W/rd/var/lib/iwd/TestNet.psk"
printf '[General]\nEnableNetworkConfiguration=false\n' > "$W/rd/etc/iwd.main.conf"; mkdir -p "$W/rd/etc/iwd"; cp "$W/rd/etc/iwd.main.conf" "$W/rd/etc/iwd/main.conf"
printf "root:x:0:0:root:/:/bin/bash\nmessagebus:x:81:81::/:/bin/false\n" > "$W/rd/etc/passwd"; printf "root:x:0:\nmessagebus:x:81:\n" > "$W/rd/etc/group"
cat > "$W/rd/etc/dbus.conf" <<'XML'
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN" "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig><type>system</type><listen>unix:path=/run/dbus/system_bus_socket</listen>
<policy context="default"><allow send_destination="*" eavesdrop="true"/><allow eavesdrop="true"/><allow own="*"/><allow user="*"/></policy></busconfig>
XML
# crypto modules iwd needs (AF_ALG), in dependency order
: > "$W/rd/m/order"; n=0
for mod in algif_hash algif_skcipher algif_rng algif_aead cmac ccm md4 des_generic libarc4 arc4 ecb cbc; do
  modprobe -S $K --show-depends $mod 2>/dev/null | awk '$1=="insmod"{print $2}' | while read f; do
    b=$(basename $f .ko.zst); grep -qx "$b.ko" "$W/rd/m/order" || { zstd -dqf $f -o "$W/rd/m/$b.ko" && echo "$b.ko" >> "$W/rd/m/order"; }
  done
done
if [ -n "$HW" ]; then
  zstd -dqf $M/kernel/drivers/usb/serial/ch341.ko.zst -o "$W/rd/m/ch341.ko"
  printf '[General]\nEnableNetworkConfiguration=true\n[Network]\nNameResolvingService=none\n' > "$W/rd/etc/iwd/main.conf"
  rm -f "$W/rd/var/lib/iwd/TestNet.psk"
  printf '[Security]\nPassphrase=%s\n' "${HW_PASS:-wrong-password-on-purpose}" > "$W/rd/var/lib/iwd/=${HW_SSID_HEX:-f09fa68a322e3447}.psk"
fi
cp "$here/${GUEST:-guest-init.sh}" "$W/rd/init"; chmod +x "$W/rd/init"
for b in awk iwctl dbus-daemon wpa_supplicant bash iw ip ping dmesg insmod rmmod mount sleep cat ls seq pkill grep head tail cut sed mkdir ln tr timeout sysctl wait echo; do
  p=$(command -v $b 2>/dev/null) || continue; [ -f "$p" ] && cp -L "$p" "$W/rd/bin/"; done
for b in $W/rd/bin/*; do ldd "$b" 2>/dev/null | grep -oE '/[^ ]+' ; done | sort -u | while read l; do mkdir -p "$W/rd$(dirname $l)"; cp -L "$l" "$W/rd$l"; done
(cd "$W/rd" && find . | cpio -o -H newc 2>/dev/null | gzip > "$W/initrd.gz")
: > "$W/console.log"
if [ -n "$HW" ]; then
  sudo -n qemu-system-x86_64 -enable-kvm -m 512 -smp 2 -kernel /boot/vmlinuz-linux -initrd "$W/initrd.gz" \
    -append "console=ttyS0 rdinit=/init panic=1 loglevel=7 slub_debug=FZPU panic_on_warn=0" -display none -no-reboot -monitor none \
    -serial file:"$W/console.log" -device qemu-xhci,id=xhci -device usb-host,bus=xhci.0,vendorid=0x1a86,productid=0x7523 \
    > "$W/qemu.err" 2>&1 &
  Q=$!; E=
else
  qemu-system-x86_64 -enable-kvm -m 512 -smp 2 -kernel /boot/vmlinuz-linux -initrd "$W/initrd.gz" \
  -append "console=ttyS0 rdinit=/init panic=1 loglevel=7 slub_debug=FZPU kfence.sample_interval=1 panic_on_warn=0" -display none -no-reboot -monitor none \
  -serial file:"$W/console.log" -serial pty > "$W/qemu.err" 2>&1 &
Q=$!; sleep 1
TTY=$(grep -oE '/dev/pts/[0-9]+' "$W/qemu.err" | head -1)
[ -n "$TTY" ] || { echo "qemu did not report a pty:"; cat "$W/qemu.err"; kill $Q; exit 1; }
python3 "$root/tools/fake-esp.py" -v ${ESP_ARGS} --tty "$TTY" > "$W/esp.log" 2>&1 &
E=$!
fi
for i in $(seq 1 ${VM_TIMEOUT:-120}); do kill -0 $Q 2>/dev/null || break; sleep 1; done
if [ -n "$HW" ]; then sudo -n pkill -f "qemu-system-x86_64.*$W" || true; else kill $Q $E 2>/dev/null || true; fi
echo "== console ($W/console.log)"; cat "$W/console.log"; echo "== fake esp"; tail -15 "$W/esp.log"
