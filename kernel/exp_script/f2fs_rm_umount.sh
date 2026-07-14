#!/bin/bash
BENCHMARK_DIR=/mnt
DEV_partition=/dev/nvme1n1p1
DEV_partition0=/dev/nvme0n1p1
DEV_whole=/dev/nvme0n1

sudo rm $BENCHMARK_DIR/* -rf
#sudo umount $DEV_partition\
#sudo umount $DEV_partition0\

sudo umount $BENCHMARK_DIR

#sudo rmmod f2fs

