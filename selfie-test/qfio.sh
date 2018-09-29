#!/bin/bash

. ./fenv.sh

for xrw in write randwrite; do
  for xbs in 64k 512k 4096k; do
    for xth in 1 2 4 8 16 32; do
      run_all $1 $2 $xth $xrw $xbs
    done
  done
done
