#!/bin/bash
BENCHMARK_DIR=/mnt
DEV_partition=/dev/nvme0n1p1
DEV_whole=/dev/nvme0n1
DEV_ZNS=/dev/nvme1n1
#DEV_partition=/dev/nvme0n1p1
#DEV_whole=/dev/nvme0n1

sudo rm $BENCHMARK_DIR/* -rf
sudo umount $DEV_partition\

sudo rmmod f2fs
sudo insmod ./mod/f2fs_$1.ko;

#sudo mkfs.f2fs -s 4 -f $DEV_partition;\
#sudo LD_LIBRARY_PATH=$LD_LIBRARY_PATH mkfs.f2fs -s 16 -f $DEV_partition;\
#sudo mkfs.f2fs -m -s 512 -f $DEV_partition;\
#mkfs.f2fs -m -s 512 -f $DEV_partition;\
#sudo LD_LIBRARY_PATH=$LD_LIBRARY_PATH mkfs.f2fs -s 512 -f $DEV_partition;\

# 1 GB zone
sudo LD_LIBRARY_PATH=$LD_LIBRARY_PATH mkfs.f2fs -f -m -s 512 -c $DEV_ZNS  $DEV_partition;\

# 32 MB zone
#sudo LD_LIBRARY_PATH=$LD_LIBRARY_PATH mkfs.f2fs -f -m -s 16 -c $DEV_ZNS  $DEV_partition;\



#sudo mkfs.f2fs -f -z 2 $DEV_partition;\
#sudo /usr/src/linux-5.11/f2fs-tools/mkfs/mkfs.f2fs -f $DEV_partition;\
#sudo mount -t f2fs -o discard $DEV_partition $BENCHMARK_DIR;\
#sudo mount -t f2fs -o mode=lfs $DEV_partition $BENCHMARK_DIR;\
#sudo mount -t f2fs -o mode=lfs $DEV_partition $BENCHMARK_DIR;\
#sudo mount -t f2fs $DEV_partition $BENCHMARK_DIR;\
sudo mount -t f2fs -o nobarrier -o mode=lfs $DEV_partition $BENCHMARK_DIR;\
#sudo mount -t f2fs -o nobarrier -o gc_merge -o mode=lfs $DEV_partition $BENCHMARK_DIR;\
#sudo mount -t f2fs -o mode=lfs $DEV_partition $BENCHMARK_DIR;\
