#!/bin/bash
if [[ $# -ne 3 ]]; then
  echo need 3 params
  exit 1
fi
. ./fio-env.sh

run_test 4 randwrite 64k
run_test 4 write 64k
