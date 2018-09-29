#!/bin/bash

. ./qenv.sh

ts=$(date +%s)
statfio="00-fio-${ts}-$$.log"

runfio()
{
  local pt=
  local fulltag="${TAG}-${nj}-${rw}-${bs}"
  if [[ -n ${hostblk} && -b /dev/${hostblk} ]]; then
    blktrace -a issue -a complete -a flush /dev/${hostblk} &
    pt=$!
  fi

  echo "============================== ${fulltag}" >> 00-run_fio.log
  RT=${rt} NJ=${nj} DEV=${dev} RW=${rw} BS=${bs} INC=8G fio ${fioparam} --section=job0 fio-job0.fio >/tmp/fiolog
  if [[ $? != 0 ]]; then
    echo --------run-fio returned none zero? "$*" >> 00-run_fio.log
  fi

  if [[ -n $pt ]]; then
    kill -SIGINT $pt
    wait $pt
    blkparse ${hostblk} > 11-${fulltag}-trace.txt
  fi
  cat /tmp/fiolog >> 00-run_fio.log
  grep '^  WRITE' /tmp/fiolog | sed -e "s/^/${fulltag} /" >>${statfio}
}

dummy()
{
  echo ====
  echo QI $QI
  echo QCMD $QCMD
  echo TAG $TAG
  echo nj $nj
  echo rw $rw
  echo bs $bs
  echo hostblk $hostblk
  echo dev $dev
  echo statfio $statfio
}

# $1 image file -- /dev/sdb /mnt/img
# $2 runtime -- 60
# $3 nr_jobs -- 1 16
# $4 rw      -- write randwrite
# $5 bs      -- 64k 1024k 128M 1G
# [blk] -- sda sdb md0
run_all()
{
  QI=$1
  QCMD=runfio

  rt=$2
  nj=$3
  rw=$4
  bs=$5
  echo ========== $(date +"%F %T") >>${statfio}
  dev=/dev/nbd0
  run_selfie
  run_qcow2
  run_fvd
  if [[ ! -b ${QI} ]]; then
    run_raw
    run_qed
  fi
  if [[ -b ${QI} ]]; then
    dev=$QI
    run_host
  fi
}

if [[ $# < 2 ]]; then
  echo "usage: $0 <image> <runtime> [<blk>]"
  exit 1
fi
if [[ -n $3 ]]; then
  hostblk=$3
  check_exist /dev/${hostblk}
fi
