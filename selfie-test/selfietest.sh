#!/bin/bash
# clean
rm -f /tmp/selfie-debug.log
umount /dev/nbd0
./qemu-nbd -d /dev/nbd0
./qemu-nbd -d /dev/nbd0

rm /tmp/selfie-debug.log
# do it
./qemu-img create -f selfie selfie 8G
./qemu-nbd -d /dev/nbd0
./qemu-nbd -d /dev/nbd0
./qemu-nbd -c /dev/nbd0 selfie
mkfs.xfs /dev/nbd0
mount /dev/nbd0 /mnt
cp -r /usr/share /mnt/
sync
umount /mnt
sync
./qemu-nbd -d /dev/nbd0
./qemu-nbd -d /dev/nbd0

# open again
./qemu-nbd -c /dev/nbd0 selfie
mount /dev/nbd0 /mnt
