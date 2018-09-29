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

run_pm()
{
  echo ========== $TAG
  mount_guest_fs
  sleep 1
  postmark <<EOF
set number 100000
set transactions 1000000
set location /mnt
run
EOF
  sleep 1
  umount /mnt
  sleep 1
}

# /dev/sdb /mnt/img
if [[ $# < 2 ]]; then
  echo "usage $0 <image> <fs-type>"
  exit 0
fi
check_exec postmark
QCMD=run_pm
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
