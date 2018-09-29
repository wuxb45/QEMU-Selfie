#!/bin/bash

qemupath="/home/wuxb/work/selfie"
fvdpath="/home/wuxb/work/fvd"

qnbd=${qemupath}/qemu-nbd
qimg=${qemupath}/qemu-img

fnbd=${fvdpath}/qemu-nbd
fimg=${fvdpath}/qemu-img

check_exec()
{
  if [[ -z $(which ${1}) ]]; then
    echo "need ${1}"
    exit 1
  fi
}

check_exist()
{
  if [[ ! -e ${1} ]]; then
    echo "file does not exist: ${1}"
    exit 1
  fi
}

check_block()
{
  if [[ ! -b ${1} ]]; then
    echo "block device does not exist: ${1}"
    exit 1
  fi
}

check_exist $qemupath
check_exist $qnbd
check_exist $qimg
check_exist $fvdpath
check_exist $fnbd
check_exist $fimg
check_exist /dev/nbd0
IMGCAP=${IMGCAP:-280G}

# plug a image on /dev/nbd0
# $1: format
# $2: image file
plug_qimg()
{
  local format=$1
  local imgfile=$2
  sleep 1; sync
  ${qnbd} -c /dev/nbd0 -f ${format} --cache=none --aio=native ${imgfile} >>00-trash.log
  if [[ $? != 0 ]]; then
    echo plug ${format} ${imgfile} on /dev/nbd0 failed
    exit 1
  fi
  sleep 1; sync
  echo 'deadline' > /sys/block/nbd0/queue/scheduler
}

# $1 image file
plug_raw()
{
  local imgfile=$1
  ${qimg} create -f raw ${imgfile} 280G >>00-trash.log
  plug_qimg raw ${imgfile}
}

plug_selfie()
{
  local imgfile=$1
  ${qimg} create -f selfie -o init=zero,zone_size=64M,cluster_size=64k ${imgfile} 280G >>00-trash.log
  plug_qimg selfie ${imgfile}
}

plug_qcow2()
{
  local imgfile=$1
  ${qimg} create -f qcow2 -o compat=1.1,lazy_refcounts=on,preallocation=metadata ${imgfile} 280G >>00-trash.log
  plug_qimg qcow2 ${imgfile}
}

plug_qed()
{
  local imgfile=$1
  ${qimg} create -f qed ${imgfile} 280G >>00-trash.log
  plug_qimg qed ${imgfile}
}

plug_fvd()
{
  local imgfile=$1
  ${fimg} create -f fvd ${imgfile} 280G >>00-trash.log
  sleep 1; sync
  ${fnbd} -c /dev/nbd0 -n ${imgfile} >>00-trash.log
  if [[ $? != 0 ]]; then
    echo plug fvd ${imgfile} on /dev/nbd0 failed
    exit 1
  fi
  sleep 1; sync
  echo 'deadline' > /sys/block/nbd0/queue/scheduler
}

unplug_q()
{
  local imgfile=$1
  sleep 1; sync
  while [[ -n $(pgrep qemu-nbd) ]]; do
    ${qnbd} -d /dev/nbd0 >>00-trash.log
    if [[ $? != 0 ]]; then
      echo unplug_q failed
      exit 1
    fi
    sleep 1
  done
  if [[ ! -b ${imgfile} ]]; then
    rm -f ${imgfile}
  fi
  sleep 1; sync
  echo 1 > /proc/sys/vm/drop_caches
}

unplug_f()
{
  local imgfile=$1
  sleep 1; sync
  while [[ -n $(pgrep qemu-nbd) ]]; do
    ${fnbd} -d /dev/nbd0 >>00-trash.log
    if [[ $? != 0 ]]; then
      echo unplug_f failed
      exit 1
    fi
    sleep 1
  done
  if [[ ! -b ${imgfile} ]]; then
    rm -f ${imgfile}
  fi
  sleep 1; sync
  echo 1 > /proc/sys/vm/drop_caches
}

run_host()
{
  TAG=hst
  $QCMD "$@"
}

run_raw()
{
  TAG=raw
  plug_raw $QI
  $QCMD "$@"
  unplug_q $QI
}

run_selfie()
{
  TAG=selfie
  plug_selfie $QI
  $QCMD "$@"
  unplug_q $QI
}

run_qcow2()
{
  TAG=qc2
  plug_qcow2 $QI
  $QCMD "$@"
  unplug_q $QI
}

run_qed()
{
  TAG=qed
  plug_qed $QI
  $QCMD "$@"
  unplug_q $QI
}

run_fvd()
{
  TAG=fvd
  plug_fvd $QI
  $QCMD "$@"
  unplug_f $QI
}
