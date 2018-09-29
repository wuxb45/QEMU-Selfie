#!/bin/bash
. ./qenv.sh
mount_guest_fs()
{
  check_block ${guestblk}
  check_exec mkfs.${guestfs}
  echo ++mkfs
  dd if=/dev/zero of=${guestblk} bs=1M count=1
  mkfs.${guestfs} -q ${guestblk}
  mount ${guestblk} /mnt
  mount | grep '/mnt'
}

run_dd()
{
  echo ========== $TAG
  mount_guest_fs
  sleep 1
  time dd if=/dev/zero of=/mnt/zero bs=1M count=8192
  sleep 1
  time sync
  sleep 1
  echo 1 > /proc/sys/vm/drop_caches
  time dd if=/mnt/zero of=/dev/null bs=1M count=8192
  sync
  sleep 1
  umount /mnt
  sleep 1
}

# /dev/sdb /mnt/img
if [[ $# < 2 ]]; then
  echo "usage $0 <image> <fs-type>"
  exit 0
fi
QCMD=run_dd
QI=$1
# ext4 zfs btrfs
guestfs=$2

guestblk=/dev/nbd0
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
