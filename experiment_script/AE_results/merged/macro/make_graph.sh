#!/bin/bash

OUTPUT_THROUGHPUT=throughput
OUTPUT_AVG_LAT=avg_lat
OUTPUT_TAIL_LAT=99.99_lat

Target="99.99PercentileLatency"
FTARGET=22
#===============================================================================================#
# Fileserver Data
gaze_FILESERVER=$(find ./gaze_Fileserver -name "result.txt")
IPLFS_FILESERVER=$(find ./iplfs_Fileserver -name "result.txt")
ZNS_FILESERVER=$(find ./zns_Fileserver -name "result.txt")
F2FS_FILESERVER=$(find ./f2fs_Fileserver -name "result.txt")
PGC_FILESERVER=$(find ./pgc_Fileserver -name "result.txt")
COPY_FILESERVER=$(find ./copy_Fileserver -name "result.txt")

if [ -s "$gaze_FILESERVER" ]; then
	gaze_FILESERVER_THROUGHPUT=$(cat "$gaze_FILESERVER"  | grep 'GC IO Summary' | awk '{print $7/1000}')
	gaze_FILESERVER_AVG_LAT=$(cat "$gaze_FILESERVER"  | grep 'GC append' | sed 's/ms/ /g' | awk '{print $6}')
	gaze_FILESERVER_TAIL_LAT=$(cat "$gaze_FILESERVER"  | grep 'GC append' | awk '{print $'"$FTARGET"'/1000/1000}')
else
	gaze_FILESERVER_THROUGHPUT=0
	gaze_FILESERVER_AVG_LAT=0
	gaze_FILESERVER_TAIL_LAT=0
fi

if [ -s "$IPLFS_FILESERVER" ]; then
	IPLFS_FILESERVER_THROUGHPUT=$(cat "$IPLFS_FILESERVER"  | grep 'GC IO Summary' | awk '{print $7/1000}')
	IPLFS_FILESERVER_AVG_LAT=$(cat "$IPLFS_FILESERVER"  | grep 'GC append' | sed 's/ms/ /g' | awk '{print $6}')
	IPLFS_FILESERVER_TAIL_LAT=$(cat "$IPLFS_FILESERVER"  | grep 'GC append' | awk '{print $'"$FTARGET"'/1000/1000}')
else
	IPLFS_FILESERVER_THROUGHPUT=0
	IPLFS_FILESERVER_AVG_LAT=0
	IPLFS_FILESERVER_TAIL_LAT=0
fi

if [ -s "$ZNS_FILESERVER" ]; then
	ZNS_FILESERVER_THROUGHPUT=$(cat "$ZNS_FILESERVER"  | grep 'GC IO Summary' | awk '{print $7/1000}')
	ZNS_FILESERVER_AVG_LAT=$(cat "$ZNS_FILESERVER"  | grep 'GC append' | sed 's/ms/ /g' | awk '{print $6}')
	ZNS_FILESERVER_TAIL_LAT=$(cat "$ZNS_FILESERVER"  | grep 'GC append' | awk '{print $'"$FTARGET"'/1000/1000}')
else
	ZNS_FILESERVER_THROUGHPUT=0
	ZNS_FILESERVER_AVG_LAT=0
	ZNS_FILESERVER_TAIL_LAT=0
fi

if [ -s "$F2FS_FILESERVER" ]; then
	F2FS_FILESERVER_THROUGHPUT=$(cat "$F2FS_FILESERVER"  | grep 'GC IO Summary' | awk '{print $7/1000}')
	F2FS_FILESERVER_AVG_LAT=$(cat "$F2FS_FILESERVER"  | grep 'GC append' | sed 's/ms/ /g' | awk '{print $6}')
	F2FS_FILESERVER_TAIL_LAT=$(cat "$F2FS_FILESERVER"  | grep 'GC append' | awk '{print $'"$FTARGET"'/1000/1000}')
else
	F2FS_FILESERVER_THROUGHPUT="N/A"
	F2FS_FILESERVER_AVG_LAT=0
	F2FS_FILESERVER_TAIL_LAT=0
fi


if [ -s "$PGC_FILESERVER" ]; then
	PGC_FILESERVER_THROUGHPUT=$(cat "$PGC_FILESERVER"  | grep 'GC IO Summary' | awk '{print $7/1000}')
	PGC_FILESERVER_AVG_LAT=$(cat "$PGC_FILESERVER"  | grep 'GC append' | sed 's/ms/ /g' | awk '{print $6}')
	PGC_FILESERVER_TAIL_LAT=$(cat "$PGC_FILESERVER"  | grep 'GC append' | awk '{print $'"$FTARGET"'/1000/1000}')
else
	PGC_FILESERVER_THROUGHPUT="N/A"
	PGC_FILESERVER_AVG_LAT=0
	PGC_FILESERVER_TAIL_LAT=0
fi

if [ -s "$COPY_FILESERVER" ]; then
	COPY_FILESERVER_THROUGHPUT=$(cat "$COPY_FILESERVER"  | grep 'GC IO Summary' | awk '{print $7/1000}')
	COPY_FILESERVER_AVG_LAT=$(cat "$COPY_FILESERVER"  | grep 'GC append' | sed 's/ms/ /g' | awk '{print $6}')
	COPY_FILESERVER_TAIL_LAT=$(cat "$COPY_FILESERVER"  | grep 'GC append' | awk '{print $'"$FTARGET"'/1000/1000}')
else
	COPY_FILESERVER_THROUGHPUT="N/A"
	COPY_FILESERVER_AVG_LAT=0
	COPY_FILESERVER_TAIL_LAT=0
fi

#===============================================================================================#
# YCSB-A Data

gaze_YCSBA=$(find ./gaze_YCSB-A -name "result.txt")
IPLFS_YCSBA=$(find ./iplfs_YCSB-A -name "result.txt")
ZNS_YCSBA=$(find ./zns_YCSB-A -name "result.txt")
F2FS_YCSBA=$(find ./f2fs_YCSB-A -name "result.txt")
PGC_YCSBA=$(find ./pgc_YCSB-A -name "result.txt")
COPY_YCSBA=$(find ./copy_YCSB-A -name "result.txt")



if [ -s "$gaze_YCSBA" ]; then
	gaze_YCSBA_THROUGHPUT=$(cat "$gaze_YCSBA"  | grep Throughput | awk '{print $3/1000}')
	gaze_YCSBA_AVG_LAT_UPDATE=$(cat "$gaze_YCSBA" | grep UPDATE | grep Average | awk '{print $3/1000}')
	gaze_YCSBA_AVG_LAT_READ=$(cat "$gaze_YCSBA" | grep READ | grep Average | awk '{print $3/1000}')
	gaze_YCSBA_TAIL_LAT_UPDATE=$(cat "$gaze_YCSBA" | grep UPDATE | grep "$Target" | awk '{print $3/1000/1000}')
	gaze_YCSBA_TAIL_LAT_READ=$(cat "$gaze_YCSBA" | grep READ | grep "$Target" | awk '{print $3/1000/1000}')
else
	gaze_YCSBA_THROUGHPUT=0
	gaze_YCSBA_AVG_LAT_UPDATE=0
	gaze_YCSBA_AVG_LAT_READ=0
	gaze_YCSBA_TAIL_LAT_UPDATE=0
	gaze_YCSBA_TAIL_LAT_READ=0
fi

if [ -s "$IPLFS_YCSBA" ]; then
	IPLFS_YCSBA_THROUGHPUT=$(cat "$IPLFS_YCSBA"  | grep Throughput | awk '{print $3/1000}')
	IPLFS_YCSBA_AVG_LAT_UPDATE=$(cat "$IPLFS_YCSBA" | grep UPDATE | grep Average | awk '{print $3/1000}')
	IPLFS_YCSBA_AVG_LAT_READ=$(cat "$IPLFS_YCSBA" | grep READ | grep Average | awk '{print $3/1000}')
	IPLFS_YCSBA_TAIL_LAT_UPDATE=$(cat "$IPLFS_YCSBA" | grep UPDATE | grep "$Target" | awk '{print $3/1000/1000}')
	IPLFS_YCSBA_TAIL_LAT_READ=$(cat "$IPLFS_YCSBA" | grep READ | grep "$Target" | awk '{print $3/1000/1000}')
else
	IPLFS_YCSBA_THROUGHPUT=0
	IPLFS_YCSBA_AVG_LAT_UPDATE=0
	IPLFS_YCSBA_AVG_LAT_READ=0
	IPLFS_YCSBA_TAIL_LAT_UPDATE=0
	IPLFS_YCSBA_TAIL_LAT_READ=0
fi

if [ -s "$ZNS_YCSBA" ]; then
	ZNS_YCSBA_THROUGHPUT=$(cat "$ZNS_YCSBA"  | grep Throughput | awk '{print $3/1000}')
	ZNS_YCSBA_AVG_LAT_UPDATE=$(cat "$ZNS_YCSBA" | grep UPDATE | grep Average | awk '{print $3/1000}')
	ZNS_YCSBA_AVG_LAT_READ=$(cat "$ZNS_YCSBA" | grep READ | grep Average | awk '{print $3/1000}')
	ZNS_YCSBA_TAIL_LAT_UPDATE=$(cat "$ZNS_YCSBA" | grep UPDATE | grep "$Target" | awk '{print $3/1000/1000}')
	ZNS_YCSBA_TAIL_LAT_READ=$(cat "$ZNS_YCSBA" | grep READ | grep "$Target" | awk '{print $3/1000/1000}')
else
	ZNS_YCSBA_THROUGHPUT=0
	ZNS_YCSBA_AVG_LAT_UPDATE=0
	ZNS_YCSBA_AVG_LAT_READ=0
	ZNS_YCSBA_TAIL_LAT_UPDATE=0
	ZNS_YCSBA_TAIL_LAT_READ=0
fi

if [ -s "$F2FS_YCSBA" ]; then
	F2FS_YCSBA_THROUGHPUT=$(cat "$F2FS_YCSBA"  | grep Throughput | awk '{print $3/1000}')
	F2FS_YCSBA_AVG_LAT_UPDATE=$(cat "$F2FS_YCSBA" | grep UPDATE | grep Average | awk '{print $3/1000}')
	F2FS_YCSBA_AVG_LAT_READ=$(cat "$F2FS_YCSBA" | grep READ | grep Average | awk '{print $3/1000}')
	F2FS_YCSBA_TAIL_LAT_UPDATE=$(cat "$F2FS_YCSBA" | grep UPDATE | grep "$Target" | awk '{print $3/1000/1000}')
	F2FS_YCSBA_TAIL_LAT_READ=$(cat "$F2FS_YCSBA" | grep READ | grep "$Target" | awk '{print $3/1000/1000}')
else
	F2FS_YCSBA_THROUGHPUT=0
	F2FS_YCSBA_AVG_LAT_UPDATE=0
	F2FS_YCSBA_AVG_LAT_READ=0
	F2FS_YCSBA_TAIL_LAT_UPDATE=0
	F2FS_YCSBA_TAIL_LAT_READ=0
fi

if [ -s "$PGC_YCSBA" ]; then
	PGC_YCSBA_THROUGHPUT=$(cat "$PGC_YCSBA"  | grep Throughput | awk '{print $3/1000}')
	PGC_YCSBA_AVG_LAT_UPDATE=$(cat "$PGC_YCSBA" | grep UPDATE | grep Average | awk '{print $3/1000}')
	PGC_YCSBA_AVG_LAT_READ=$(cat "$PGC_YCSBA" | grep READ | grep Average | awk '{print $3/1000}')
	PGC_YCSBA_TAIL_LAT_UPDATE=$(cat "$PGC_YCSBA" | grep UPDATE | grep "$Target" | awk '{print $3/1000/1000}')
	PGC_YCSBA_TAIL_LAT_READ=$(cat "$PGC_YCSBA" | grep READ | grep "$Target" | awk '{print $3/1000/1000}')
else
	PGC_YCSBA_THROUGHPUT=0
	PGC_YCSBA_AVG_LAT_UPDATE=0
	PGC_YCSBA_AVG_LAT_READ=0
	PGC_YCSBA_TAIL_LAT_UPDATE=0
	PGC_YCSBA_TAIL_LAT_READ=0
fi

if [ -s "$COPY_YCSBA" ]; then
	COPY_YCSBA_THROUGHPUT=$(cat "$COPY_YCSBA"  | grep Throughput | awk '{print $3/1000}')
	COPY_YCSBA_AVG_LAT_UPDATE=$(cat "$COPY_YCSBA" | grep UPDATE | grep Average | awk '{print $3/1000}')
	COPY_YCSBA_AVG_LAT_READ=$(cat "$COPY_YCSBA" | grep READ | grep Average | awk '{print $3/1000}')
	COPY_YCSBA_TAIL_LAT_UPDATE=$(cat "$COPY_YCSBA" | grep UPDATE | grep "$Target" | awk '{print $3/1000/1000}')
	COPY_YCSBA_TAIL_LAT_READ=$(cat "$COPY_YCSBA" | grep READ | grep "$Target" | awk '{print $3/1000/1000}')
else
	COPY_YCSBA_THROUGHPUT=0
	COPY_YCSBA_AVG_LAT_UPDATE=0
	COPY_YCSBA_AVG_LAT_READ=0
	COPY_YCSBA_TAIL_LAT_UPDATE=0
	COPY_YCSBA_TAIL_LAT_READ=0
fi

#===============================================================================================#
# YCSB-F Data
gaze_YCSBF=$(find ./gaze_YCSB-F -name "result.txt")
IPLFS_YCSBF=$(find ./iplfs_YCSB-F -name "result.txt")
ZNS_YCSBF=$(find ./zns_YCSB-F -name "result.txt")
F2FS_YCSBF=$(find ./f2fs_YCSB-F -name "result.txt")
PGC_YCSBF=$(find ./pgc_YCSB-F -name "result.txt")
COPY_YCSBF=$(find ./copy_YCSB-F -name "result.txt")

if [ -s "$gaze_YCSBF" ]; then
	gaze_YCSBF_THROUGHPUT=$(cat "$gaze_YCSBF"  | grep Throughput | awk '{print $3/1000}')
	gaze_YCSBF_AVG_LAT_UPDATE=$(cat "$gaze_YCSBF" | grep UPDATE | grep Average | awk '{print $3/1000}')
	gaze_YCSBF_AVG_LAT_READ=$(cat "$gaze_YCSBF" | grep READ | grep -v MODIFY | grep Average | awk '{print $3/1000}')
	gaze_YCSBF_TAIL_LAT_UPDATE=$(cat "$gaze_YCSBF" | grep UPDATE | grep "$Target" | awk '{print $3/1000/1000}')
	gaze_YCSBF_TAIL_LAT_READ=$(cat "$gaze_YCSBF" | grep READ | grep -v MODIFY | grep "$Target" | awk '{print $3/1000/1000}')
else
	gaze_YCSBF_THROUGHPUT=0
	gaze_YCSBF_AVG_LAT_UPDATE=0
	gaze_YCSBF_AVG_LAT_READ=0
	gaze_YCSBF_TAIL_LAT_UPDATE=0
	gaze_YCSBF_TAIL_LAT_READ=0
fi

if [ -s "$IPLFS_YCSBF" ]; then
	IPLFS_YCSBF_THROUGHPUT=$(cat "$IPLFS_YCSBF"  | grep Throughput | awk '{print $3/1000}')
	IPLFS_YCSBF_AVG_LAT_UPDATE=$(cat "$IPLFS_YCSBF" | grep UPDATE | grep Average | awk '{print $3/1000}')
	IPLFS_YCSBF_AVG_LAT_READ=$(cat "$IPLFS_YCSBF" | grep READ | grep -v MODIFY | grep Average | awk '{print $3/1000}')
	IPLFS_YCSBF_TAIL_LAT_UPDATE=$(cat "$IPLFS_YCSBF" | grep UPDATE | grep "$Target" | awk '{print $3/1000/1000}')
	IPLFS_YCSBF_TAIL_LAT_READ=$(cat "$IPLFS_YCSBF" | grep READ | grep -v MODIFY | grep "$Target" | awk '{print $3/1000/1000}')
else
	IPLFS_YCSBF_THROUGHPUT=0
	IPLFS_YCSBF_AVG_LAT_UPDATE=0
	IPLFS_YCSBF_AVG_LAT_READ=0
	IPLFS_YCSBF_TAIL_LAT_UPDATE=0
	IPLFS_YCSBF_TAIL_LAT_READ=0
fi

if [ -s "$ZNS_YCSBF" ]; then
	ZNS_YCSBF_THROUGHPUT=$(cat "$ZNS_YCSBF"  | grep Throughput | awk '{print $3/1000}')
	ZNS_YCSBF_AVG_LAT_UPDATE=$(cat "$ZNS_YCSBF" | grep UPDATE | grep Average | awk '{print $3/1000}')
	ZNS_YCSBF_AVG_LAT_READ=$(cat "$ZNS_YCSBF" | grep READ | grep -v MODIFY | grep Average | awk '{print $3/1000}')
	ZNS_YCSBF_TAIL_LAT_UPDATE=$(cat "$ZNS_YCSBF" | grep UPDATE | grep "$Target" | awk '{print $3/1000/1000}')
	ZNS_YCSBF_TAIL_LAT_READ=$(cat "$ZNS_YCSBF" | grep READ | grep -v MODIFY | grep "$Target" | awk '{print $3/1000/1000}')
else
	ZNS_YCSBF_THROUGHPUT=0
	ZNS_YCSBF_AVG_LAT_UPDATE=0
	ZNS_YCSBF_AVG_LAT_READ=0
	ZNS_YCSBF_TAIL_LAT_UPDATE=0
	ZNS_YCSBF_TAIL_LAT_READ=0
fi

if [ -s "$F2FS_YCSBF" ]; then
	F2FS_YCSBF_THROUGHPUT=$(cat "$F2FS_YCSBF"  | grep Throughput | awk '{print $3/1000}')
	F2FS_YCSBF_AVG_LAT_UPDATE=$(cat "$F2FS_YCSBF" | grep UPDATE | grep Average | awk '{print $3/1000}')
	F2FS_YCSBF_AVG_LAT_READ=$(cat "$F2FS_YCSBF" | grep READ | grep -v MODIFY | grep Average | awk '{print $3/1000}')
	F2FS_YCSBF_TAIL_LAT_UPDATE=$(cat "$F2FS_YCSBF" | grep UPDATE | grep "$Target" | awk '{print $3/1000/1000}')
	F2FS_YCSBF_TAIL_LAT_READ=$(cat "$F2FS_YCSBF" | grep READ | grep -v MODIFY | grep "$Target" | awk '{print $3/1000/1000}')
else
	F2FS_YCSBF_THROUGHPUT=0
	F2FS_YCSBF_AVG_LAT_UPDATE=0
	F2FS_YCSBF_AVG_LAT_READ=0
	F2FS_YCSBF_TAIL_LAT_UPDATE=0
	F2FS_YCSBF_TAIL_LAT_READ=0
fi

if [ -s "$PGC_YCSBF" ]; then
	PGC_YCSBF_THROUGHPUT=$(cat "$PGC_YCSBF"  | grep Throughput | awk '{print $3/1000}')
	PGC_YCSBF_AVG_LAT_UPDATE=$(cat "$PGC_YCSBF" | grep UPDATE | grep Average | awk '{print $3/1000}')
	PGC_YCSBF_AVG_LAT_READ=$(cat "$PGC_YCSBF" | grep READ | grep -v MODIFY | grep Average | awk '{print $3/1000}')
	PGC_YCSBF_TAIL_LAT_UPDATE=$(cat "$PGC_YCSBF" | grep UPDATE | grep "$Target" | awk '{print $3/1000/1000}')
	PGC_YCSBF_TAIL_LAT_READ=$(cat "$PGC_YCSBF" | grep READ | grep -v MODIFY | grep "$Target" | awk '{print $3/1000/1000}')
else
	PGC_YCSBF_THROUGHPUT=0
	PGC_YCSBF_AVG_LAT_UPDATE=0
	PGC_YCSBF_AVG_LAT_READ=0
	PGC_YCSBF_TAIL_LAT_UPDATE=0
	PGC_YCSBF_TAIL_LAT_READ=0
fi

if [ -s "$COPY_YCSBF" ]; then
	COPY_YCSBF_THROUGHPUT=$(cat "$COPY_YCSBF"  | grep Throughput | awk '{print $3/1000}')
	COPY_YCSBF_AVG_LAT_UPDATE=$(cat "$COPY_YCSBF" | grep UPDATE | grep Average | awk '{print $3/1000}')
	COPY_YCSBF_AVG_LAT_READ=$(cat "$COPY_YCSBF" | grep READ | grep -v MODIFY | grep Average | awk '{print $3/1000}')
	COPY_YCSBF_TAIL_LAT_UPDATE=$(cat "$COPY_YCSBF" | grep UPDATE | grep "$Target" | awk '{print $3/1000/1000}')
	COPY_YCSBF_TAIL_LAT_READ=$(cat "$COPY_YCSBF" | grep READ | grep -v MODIFY | grep "$Target" | awk '{print $3/1000/1000}')
else
	COPY_YCSBF_THROUGHPUT=0
	COPY_YCSBF_AVG_LAT_UPDATE=0
	COPY_YCSBF_AVG_LAT_READ=0
	COPY_YCSBF_TAIL_LAT_UPDATE=0
	COPY_YCSBF_TAIL_LAT_READ=0
fi

#===============================================================================================#
# TPC-C Data
gaze_TPCC=$(find ./gaze_TPC-C -name "result.txt")
IPLFS_TPCC=$(find ./iplfs_TPC-C -name "result.txt")
ZNS_TPCC=$(find ./zns_TPC-C -name "result.txt")
F2FS_TPCC=$(find ./f2fs_TPC-C -name "result.txt")
PGC_TPCC=$(find ./pgc_TPC-C -name "result.txt")
COPY_TPCC=$(find ./copy_TPC-C -name "result.txt")

if [ -s "$gaze_TPCC" ]; then
	gaze_TPCC_THROUGHPUT=$(cat "$gaze_TPCC"  | grep trx | sed 's/,//g' | awk '{print $3/$1}')
	gaze_TPCC_AVG_LAT=$(cat "$gaze_TPCC"  | awk '/Raw Results2/ {exit} {print}' | grep '\[1\]' | awk '{print $7}')
	gaze_TPCC_TAIL_LAT=$(cat "$gaze_TPCC"  | grep trx | sed 's/,//g' | awk '{print $13/1000}')
else
	gaze_TPCC_THROUGHPUT=0
	gaze_TPCC_AVG_LAT=0
	gaze_TPCC_TAIL_LAT=0
fi

if [ -s "$IPLFS_TPCC" ]; then
	IPLFS_TPCC_THROUGHPUT=$(cat "$IPLFS_TPCC"  | grep trx | sed 's/,//g' | awk '{print $3/$1}')
	IPLFS_TPCC_AVG_LAT=$(cat "$IPLFS_TPCC"  | awk '/Raw Results2/ {exit} {print}' | grep '\[1\]' | awk '{print $7}')
	IPLFS_TPCC_TAIL_LAT=$(cat "$IPLFS_TPCC"  | grep trx | sed 's/,//g' | awk '{print $13/1000}')
else
	IPLFS_TPCC_THROUGHPUT=0
	IPLFS_TPCC_AVG_LAT=0
	IPLFS_TPCC_TAIL_LAT=0
fi

if [ -s "$ZNS_TPCC" ]; then
	ZNS_TPCC_THROUGHPUT=$(cat "$ZNS_TPCC"  | grep trx | sed 's/,//g' | awk '{print $3/$1}')
	ZNS_TPCC_AVG_LAT=$(cat "$ZNS_TPCC"  | awk '/Raw Results2/ {exit} {print}' | grep '\[1\]' | awk '{print $7}')
	ZNS_TPCC_TAIL_LAT=$(cat "$ZNS_TPCC"  | grep trx | sed 's/,//g' | awk '{print $13/1000}')
else
	ZNS_TPCC_THROUGHPUT=0
	ZNS_TPCC_AVG_LAT=0
	ZNS_TPCC_TAIL_LAT=0
fi

if [ -s "$F2FS_TPCC" ]; then
	F2FS_TPCC_THROUGHPUT=$(cat "$F2FS_TPCC"  | grep trx | sed 's/,//g' | awk '{print $3/$1}')
	F2FS_TPCC_AVG_LAT=$(cat "$F2FS_TPCC"  | awk '/Raw Results2/ {exit} {print}' | grep '\[1\]' | awk '{print $7}')
	F2FS_TPCC_TAIL_LAT=$(cat "$F2FS_TPCC"  | grep trx | sed 's/,//g' | awk '{print $13/1000}')
else
	F2FS_TPCC_THROUGHPUT=0
	F2FS_TPCC_AVG_LAT=0
	F2FS_TPCC_TAIL_LAT=0
fi

if [ -s "$PGC_TPCC" ]; then
	PGC_TPCC_THROUGHPUT=$(cat "$PGC_TPCC"  | grep trx | sed 's/,//g' | awk '{print $3/$1}')
	PGC_TPCC_AVG_LAT=$(cat "$PGC_TPCC"  | awk '/Raw Results2/ {exit} {print}' | grep '\[1\]' | awk '{print $7}')
	PGC_TPCC_TAIL_LAT=$(cat "$PGC_TPCC"  | grep trx | sed 's/,//g' | awk '{print $13/1000}')
else
	PGC_TPCC_THROUGHPUT=0
	PGC_TPCC_AVG_LAT=0
	PGC_TPCC_TAIL_LAT=0
fi

if [ -s "$COPY_TPCC" ]; then
	COPY_TPCC_THROUGHPUT=$(cat "$COPY_TPCC"  | grep trx | sed 's/,//g' | awk '{print $3/$1}')
	COPY_TPCC_AVG_LAT=$(cat "$COPY_TPCC"  | awk '/Raw Results2/ {exit} {print}' | grep '\[1\]' | awk '{print $7}')
	COPY_TPCC_TAIL_LAT=$(cat "$COPY_TPCC"  | grep trx | sed 's/,//g' | awk '{print $13/1000}')
else
	COPY_TPCC_THROUGHPUT=0
	COPY_TPCC_AVG_LAT=0
	COPY_TPCC_TAIL_LAT=0
fi


#===============================================================================================#
# Plot Throughput Graph

echo -e "#throughput" > $OUTPUT_THROUGHPUT
echo -e "Xsteps\tIPLFS\tGAZE\tempty\tzF2FS\tPGC\tCOPY" >> $OUTPUT_THROUGHPUT
#echo -e "Xsteps\tIPLFS\tGAZE\tzF2FS\tF2FS\tPGC\tCOPY" >> $OUTPUT_THROUGHPUT
echo -e "Fileserver\\\n(KOPS)\t${IPLFS_FILESERVER_THROUGHPUT}\t${gaze_FILESERVER_THROUGHPUT}\t${ZNS_FILESERVER_THROUGHPUT}\t${F2FS_FILESERVER_THROUGHPUT}\t${PGC_FILESERVER_THROUGHPUT}\t${COPY_FILESERVER_THROUGHPUT}" >> $OUTPUT_THROUGHPUT
echo -e "YCSB-A\\\n(KOPS)\t${IPLFS_YCSBA_THROUGHPUT}\t${gaze_YCSBA_THROUGHPUT}\t${ZNS_YCSBA_THROUGHPUT}\t${F2FS_YCSBA_THROUGHPUT}\t${PGC_YCSBA_THROUGHPUT}\t${COPY_YCSBA_THROUGHPUT}" >> $OUTPUT_THROUGHPUT
echo -e "YCSB-F\\\n(KOPS)\t${IPLFS_YCSBF_THROUGHPUT}\t${gaze_YCSBF_THROUGHPUT}\t${ZNS_YCSBF_THROUGHPUT}\t${F2FS_YCSBF_THROUGHPUT}\t${PGC_YCSBF_THROUGHPUT}\t${COPY_YCSBF_THROUGHPUT}" >> $OUTPUT_THROUGHPUT
echo -e "TPC-C\\\n(Tx/sec)\t${IPLFS_TPCC_THROUGHPUT}\t${gaze_TPCC_THROUGHPUT}\t${ZNS_TPCC_THROUGHPUT}\t${F2FS_TPCC_THROUGHPUT}\t${PGC_TPCC_THROUGHPUT}\t${COPY_TPCC_THROUGHPUT}" >> $OUTPUT_THROUGHPUT

# PAPER_ENV (0/1) from build_graphs.sh — .gpi templates use it to keep
# the paper's hardcoded y-range when 1, autoscale when 0. Default 1 so
# standalone invocations still reproduce the paper's figures.
: "${PAPER_ENV:=1}"
gnuplot -e "PAPER_ENV=$PAPER_ENV" plot_Fig-18.Macrobenchmark_throughput.gpi

#===============================================================================================#
# Plot Average Latency Graph

echo -e "Xsteps\tIPLFS\tGAZE\tzF2FS\tF2FS\tPGC\tCOPY" > $OUTPUT_AVG_LAT
echo -e "Fileserver\t${IPLFS_FILESERVER_AVG_LAT}\t${gaze_FILESERVER_AVG_LAT}\t${ZNS_FILESERVER_AVG_LAT}\t${F2FS_FILESERVER_AVG_LAT}\t${PGC_FILESERVER_AVG_LAT}\t${COPY_FILESERVER_AVG_LAT}\t#append (msec)" >> $OUTPUT_AVG_LAT
echo -e "YCSB-A\t${IPLFS_YCSBA_AVG_LAT_UPDATE}\t${gaze_YCSBA_AVG_LAT_UPDATE}\t${ZNS_YCSBA_AVG_LAT_UPDATE}\t${F2FS_YCSBA_AVG_LAT_UPDATE}\t${PGC_YCSBA_AVG_LAT_UPDATE}\t${COPY_YCSBA_AVG_LAT_UPDATE}\t#update (msec)" >> $OUTPUT_AVG_LAT
echo -e "YCSB-F\t${IPLFS_YCSBF_AVG_LAT_UPDATE}\t${gaze_YCSBF_AVG_LAT_UPDATE}\t${ZNS_YCSBF_AVG_LAT_UPDATE}\t${F2FS_YCSBF_AVG_LAT_UPDATE}\t${PGC_YCSBF_AVG_LAT_UPDATE}\t${COPY_YCSBF_AVG_LAT_UPDATE}\t#update (msec)" >> $OUTPUT_AVG_LAT
echo -e "YCSB-A\t${IPLFS_YCSBA_AVG_LAT_READ}\t${gaze_YCSBA_AVG_LAT_READ}\t${ZNS_YCSBA_AVG_LAT_READ}\t${F2FS_YCSBA_AVG_LAT_READ}\t${PGC_YCSBA_AVG_LAT_READ}\t${COPY_YCSBA_AVG_LAT_READ}\t#read (msec)" >> $OUTPUT_AVG_LAT
echo -e "YCSB-F\t${IPLFS_YCSBF_AVG_LAT_READ}\t${gaze_YCSBF_AVG_LAT_READ}\t${ZNS_YCSBF_AVG_LAT_READ}\t${F2FS_YCSBF_AVG_LAT_READ}\t${PGC_YCSBF_AVG_LAT_READ}\t${COPY_YCSBF_AVG_LAT_READ}\t#read (msec)" >> $OUTPUT_AVG_LAT
echo -e "TPC-C\t${IPLFS_TPCC_AVG_LAT}\t${gaze_TPCC_AVG_LAT}\t${ZNS_TPCC_AVG_LAT}\t${F2FS_TPCC_AVG_LAT}\t${PGC_TPCC_AVG_LAT}\t${COPY_TPCC_AVG_LAT}" >> $OUTPUT_AVG_LAT

gnuplot -e "PAPER_ENV=$PAPER_ENV" plot_Supp-c.Macrobenchmark_avg_latency.gpi

# Plot 99.99th Latency Graph
echo -e "Xsteps\tIPLFS\tGAZE\tzF2FS\tF2FS\tPGC\tCOPY" > $OUTPUT_TAIL_LAT
echo -e "Fileserver\t${IPLFS_FILESERVER_TAIL_LAT}\t${gaze_FILESERVER_TAIL_LAT}\t${ZNS_FILESERVER_TAIL_LAT}\t${F2FS_FILESERVER_TAIL_LAT}\t${PGC_FILESERVER_TAIL_LAT}\t${COPY_FILESERVER_TAIL_LAT}\t#append (msec)" >> $OUTPUT_TAIL_LAT
echo -e "YCSB-A\t${IPLFS_YCSBA_TAIL_LAT_UPDATE}\t${gaze_YCSBA_TAIL_LAT_UPDATE}\t${ZNS_YCSBA_TAIL_LAT_UPDATE}\t${F2FS_YCSBA_TAIL_LAT_UPDATE}\t${PGC_YCSBA_TAIL_LAT_UPDATE}\t${COPY_YCSBA_TAIL_LAT_UPDATE}\t#update (msec)" >> $OUTPUT_TAIL_LAT
echo -e "YCSB-F\t${IPLFS_YCSBF_TAIL_LAT_UPDATE}\t${gaze_YCSBF_TAIL_LAT_UPDATE}\t${ZNS_YCSBF_TAIL_LAT_UPDATE}\t${F2FS_YCSBF_TAIL_LAT_UPDATE}\t${PGC_YCSBF_TAIL_LAT_UPDATE}\t${COPY_YCSBF_TAIL_LAT_UPDATE}\t#update (msec)" >> $OUTPUT_TAIL_LAT
echo -e "YCSB-A\t${IPLFS_YCSBA_TAIL_LAT_READ}\t${gaze_YCSBA_TAIL_LAT_READ}\t${ZNS_YCSBA_TAIL_LAT_READ}\t${F2FS_YCSBA_TAIL_LAT_READ}\t${PGC_YCSBA_TAIL_LAT_READ}\t${COPY_YCSBA_TAIL_LAT_READ}\t#read (msec)" >> $OUTPUT_TAIL_LAT
echo -e "YCSB-F\t${IPLFS_YCSBF_TAIL_LAT_READ}\t${gaze_YCSBF_TAIL_LAT_READ}\t${ZNS_YCSBF_TAIL_LAT_READ}\t${F2FS_YCSBF_TAIL_LAT_READ}\t${PGC_YCSBF_TAIL_LAT_READ}\t${COPY_YCSBF_TAIL_LAT_READ}\t#read (msec)" >> $OUTPUT_TAIL_LAT
echo -e "TPC-C\t${IPLFS_TPCC_TAIL_LAT}\t${gaze_TPCC_TAIL_LAT}\t${ZNS_TPCC_TAIL_LAT}\t${F2FS_TPCC_TAIL_LAT}\t${PGC_TPCC_TAIL_LAT}\t${COPY_TPCC_TAIL_LAT}\t# (sec)" >> $OUTPUT_TAIL_LAT

gnuplot -e "PAPER_ENV=$PAPER_ENV" plot_Supp-d.Macrobenchmark_tail_latency.gpi

