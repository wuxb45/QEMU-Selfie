#!/bin/bash

. ./fenv.sh

for xrw in write randwrite; do
  for xbs in 64k 512k 4096k; do
    for xth in 1 2 4 8 16 32; do
      dd if=/dev/zero of=/dev/sdb bs=1M count=1
      sleep 1
      mkfs.ext4 /dev/sdb
      mount /dev/sdb /mnt
      run_all $1 $2 $xth $xrw $xbs
      umount /mnt
    done
  done
done
