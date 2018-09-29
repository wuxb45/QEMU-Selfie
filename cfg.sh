#!/bin/bash
./configure \
  --python=python2 \
  --cc=clang \
  --target-list=x86_64-softmmu \
  --prefix=/home/wuxb/program/qemu \
  --enable-kvm \
  --enable-linux-aio \
  --disable-xen --disable-vnc --disable-vnc-tls --disable-vnc-sasl --disable-vnc-jpeg \
  --disable-vnc-png --disable-vnc-ws --disable-rdma --disable-libssh2 \
  --disable-glusterfs --disable-vhdx --disable-bluez \
  --extra-cflags="-Wno-error" --extra-ldflags="-llz4" \
  #--enable-debug --extra-cflags="-g3 -ggdb -Wno-error" --extra-ldflags="-llz4" \
# end
