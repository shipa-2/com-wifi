#!/bin/bash
# Arm panic capture on the host: systemd-pstore + kdump (kexec crash kernel).
# Idempotent. Run as root.  Needs a reboot afterwards (crashkernel= reservation).
set -euo pipefail
[ "$EUID" = 0 ] || { echo "run with sudo"; exit 1; }

ENTRY=/boot/loader/entries/arch.conf
CRASHK=1G
ROOT_UUID=$(findmnt -no UUID /)

pacman -S --needed --noconfirm kexec-tools makedumpfile

# 1) pstore: archive whatever the firmware/ramoops kept from the last crash
systemctl enable systemd-pstore.service

# 2) kdump
install -d /var/crash
cat > /usr/local/sbin/kdump-save <<'SCRIPT'
#!/bin/bash
# Runs inside the crash kernel: copy the dead kernel's memory + log to disk, reboot.
d=/var/crash/$(date +%F-%H%M%S)
mkdir -p "$d"
makedumpfile --dump-dmesg /proc/vmcore "$d/dmesg.txt" > "$d/makedumpfile.log" 2>&1
makedumpfile -c -d 31 /proc/vmcore "$d/vmcore" >> "$d/makedumpfile.log" 2>&1
# keep only the 3 newest dumps (a vmcore is ~2 GB)
ls -1dt /var/crash/*/ | tail -n +4 | xargs -r rm -rf
sync
systemctl --force --force reboot
SCRIPT
chmod 755 /usr/local/sbin/kdump-save

cat > /etc/systemd/system/kdump-load.service <<UNIT
[Unit]
Description=Load the kdump crash kernel
ConditionPathExists=/sys/kernel/kexec_crash_size

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/bin/kexec -p /boot/vmlinuz-linux --initrd=/boot/initramfs-linux.img --append="root=UUID=$ROOT_UUID rw irqpoweroff nr_cpus=1 reset_devices module_blacklist=amdgpu,radeon systemd.unit=kdump-save.service"
ExecStop=/usr/bin/kexec -p -u

[Install]
WantedBy=multi-user.target
UNIT

cat > /etc/systemd/system/kdump-save.service <<'UNIT'
[Unit]
Description=Save crash dump (crash kernel only)
DefaultDependencies=no
After=local-fs.target
Requires=local-fs.target

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/kdump-save
UNIT
systemctl daemon-reload
systemctl enable kdump-load.service

# 3) reserve memory for the crash kernel (backup first)
mkdir -p /home/shipa/com-wifi-backups
cp -n "$ENTRY" /home/shipa/com-wifi-backups/arch.conf.before-kdump
grep -q crashkernel= "$ENTRY" || sed -i "/^options/ s/\$/ crashkernel=$CRASHK/" "$ENTRY"
echo "--- $ENTRY"; cat "$ENTRY"
echo "OK. Reboot to reserve memory; afterwards: cat /sys/kernel/kexec_crash_size  and  systemctl status kdump-load"
