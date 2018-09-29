#!/bin/bash
. ./qenv.sh
mount_guest_fs()
{
  check_block ${guestblk}
  check_exec mkfs.${guestfs}
  echo ++mkfs
  dd if=/dev/zero of=${guestblk} bs=1M count=1
  time mkfs.${guestfs} -q ${guestblk}
  mount ${guestblk} /mnt
  mount | grep '/mnt'
}

run_kbuild()
{
  echo =========== $TAG
  mount_guest_fs
  echo ++cp
  time (cp -r /home/wuxb/work/core/linux /mnt/ ; sync)
  cd /mnt/linux
  pwd
  sleep 1
  check_exist PKGBUILD
  echo ++make
  time (makepkg --asroot &>/dev/null; sync)
  ls /mnt/linux/*.pkg.tar.xz
  cd /tmp
  sleep 1
  echo ++rm
  time (rm -rf /mnt/linux; sync)
  umount /mnt
  sleep 1
}

# /dev/sdb /mnt/img
if [[ $# < 2 ]]; then
  echo "usage $0 <image> <fs-type>"
  exit 0
fi
check_exist /home/wuxb/work/core/linux
check_exec makepkg
QCMD=run_kbuild
QI=$1
guestblk=/dev/nbd0
# ext4 zfs btrfs
guestfs=$2

run_selfie
run_qcow2
run_fvd

if [[ ! -b ${QI} ]]; then
  run_raw
  run_qed
fi
if [[ -b ${QI} ]]; then
  guestblk=$QI
  run_host
fi
