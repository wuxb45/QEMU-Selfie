#!/bin/bash
. ./qenv.sh
mount_guest_fs()
{
  check_block ${guestblk}
  check_exec mkfs.${guestfs}
  echo ++mkfs
  time mkfs.${guestfs} ${guestblk}
  sleep 1
  mount ${guestblk} /mnt
  mount | grep '/mnt'
  sleep 1
}

run_fb()
{
  echo ========== $TAG $wlname $(date +"%F %T")
  mount_guest_fs
  filebench <<EOF
load $wlname
set \$dir=/mnt
run 120
EOF
  sleep 1
  rm -rf /mnt/*
  sync
  umount /mnt
  sleep 1
}

if [[ $# < 2 ]]; then
  echo "usage $0 <image> <fs-type>"
  exit 0
fi
check_exec filebench
QCMD=run_fb
# /dev/sdb /e/img
QI=$1
# ext4 zfs btrfs
guestfs=$2

for wl in fivestreamwrite randomwrite singlestreamwrite varmail oltp netsfs fileserver; do
  wlname=$wl
  echo ======++++++ $wlname
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
    dd if=/dev/zero of=${guestblk} bs=1M count=10
    sync
    run_host
  fi
done
