#!/bin/bash
fioparam="--status-interval=30 --size=4G"
IMGCAP=2G

. ./fenv.sh

run_all $1 $2 1 randwrite 64k
run_all $1 $2 1 write 64k
