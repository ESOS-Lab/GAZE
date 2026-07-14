#!/bin/bash
IFLBA=IFLBA
NORM=NORMAL
NORMAL_PK=NORMAL_printk_GC
WORKLOAD=mobi
FIRST=$NORM
SECOND=$IFLBA
BENCHMARK_DIR=/mnt
DEV_partition=/dev/sda1
DEV_whole=/dev/sda
RES_FILE=_parti20G_file4G_4Grndwrt
RES_DIR=./test_$WORKLOAD/$FIRST$RES_FILE
#FILE_SIZE=5194304 #4GB
#FILE_SIZE=10437184 #10GB
FILE_SIZE=9437184 #10GB
#FILE_SIZE=10240
WRITE_SIZE=4 #4KB

#mkdir -p $RES_DIR
touch $1
for (( ; ; )) 
do
        date +"%s" >> $1
	cat /proc/meminfo | sed -n "2, 3p" >> $1
	sleep 1 
done



