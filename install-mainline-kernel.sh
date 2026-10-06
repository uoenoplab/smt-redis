#!/bin/bash
# Install mainline 6.17.8 (kernel.ubuntu.com/mainline, built 2025-11-13) on an Ubuntu 24.04
# CloudLab node, make it the default grub entry and add mitigations=off. Run as root on every
# node, then reboot:  sudo bash install-mainline-kernel.sh && sudo reboot
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
# CloudLab's emulab-ipod-dkms won't build on 6.17.8 and breaks the kernel postinst
# (leaves linux-image half-configured, no initrd). We don't need it — drop it first.
apt-get -y purge emulab-ipod-dkms || true
cd /tmp
for f in linux-headers-6.17.8-061708_6.17.8-061708.202511132139_all.deb \
         linux-headers-6.17.8-061708-generic_6.17.8-061708.202511132139_amd64.deb \
         linux-image-unsigned-6.17.8-061708-generic_6.17.8-061708.202511132139_amd64.deb \
         linux-modules-6.17.8-061708-generic_6.17.8-061708.202511132139_amd64.deb; do
  wget -q -c "https://kernel.ubuntu.com/mainline/v6.17.8/amd64/$f" -O "$f"
done
dpkg -i linux-headers-6.17.8-061708_6.17.8-061708.202511132139_all.deb \
        linux-headers-6.17.8-061708-generic_6.17.8-061708.202511132139_amd64.deb \
        linux-modules-6.17.8-061708-generic_6.17.8-061708.202511132139_amd64.deb \
        linux-image-unsigned-6.17.8-061708-generic_6.17.8-061708.202511132139_amd64.deb || true
dpkg --configure -a          # finish configuring: regenerates initrd + grub (ipod now gone)
apt-get -y -f install
test -e /boot/initrd.img-6.17.8-061708-generic  # fail loudly if the initramfs still didn't build
sed -i 's/^GRUB_DEFAULT=.*/GRUB_DEFAULT=saved/' /etc/default/grub
# Benchmark methodology (upstream Homa convention, install_homa warns otherwise):
# disable Meltdown/Spectre mitigations for representative syscall-heavy numbers.
grep -q 'mitigations=off' /etc/default/grub || \
  sed -i 's/^GRUB_CMDLINE_LINUX_DEFAULT="\(.*\)"/GRUB_CMDLINE_LINUX_DEFAULT="\1 mitigations=off"/' /etc/default/grub
update-grub
sub=$(awk -F"'" '/^submenu /{print $4; exit}' /boot/grub/grub.cfg)
ent=$(awk -F"'" '/menuentry / && /6.17.8-061708-generic/ && !/recovery/{print $4; exit}' /boot/grub/grub.cfg)
grub-set-default "${sub}>${ent}"
echo "grub default -> ${sub}>${ent}"
