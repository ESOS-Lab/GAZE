#!/bin/bash

# Device
# - /dev/sdb: SAMSUNG 860EVO 500GB
# - /dev/nvme0n1: SAMSUNG 970PRO 512GB
# - /dev/nvme1n1: SAMSUNG 970EVO 500GB
# - /dev/nvme2n1: INTEL 750 


WORKLOAD=fileserver
# WORKLOAD=v-1.5-alpha3armail
FILEBENCH_PATH=/home/juwon/filebench/myworkload/$WORKLOAD.f
MNT=/mnt

DEV=(/dev/nvme0n1p1)
DEV_whole=/dev/nvme0n1
DEV_ZNS=/dev/nvme1n1

RES_FILE=blk
FTRACE_PATH=/sys/kernel/debug/tracing/
funcs_gc=func_ftrace_jwgcread
#funcs_gc=func_ftrace_jwgc
funcs_bio=func_ftrace_bio
#cat ${FTRACE_PATH}available_filter_functions | grep f2fs > ${funcs}
OUTPUTDIR="fio_exp_gc_analy_ZNS_`date "+%Y%m%d"`_`date "+%H%M"`"
MKBIN="/home/juwon/test_fast_gc/"
FILE_SIZE_COLD=25165824 #24GB, 80% of 30GB partition
#FILESYSTEM=(latest_gc_hack_pcache_analysis)
#FILESYSTEM=(latest_gc_hack_timeserial_test)
#FILESYSTEM=(preread_batch_128)
#FILESYSTEM=(preread_batch_512)
#FILESYSTEM=(fmap_once)
FILESYSTEM=(preread_fmap_once_write_nothrottle)
#FILESYSTEM=(vanilla)
IO_TYPE=(randwrite)
#	write)   #read randread write randwrite trimwrite)	

NUM_JOBS=(1)

# 4 8)    #1 4 8)
RANDOM_BLOCK_SIZE=(4k) # 4k 8k 16k 32k)
SEQ_BLOCK_SIZE=(4k)   # 64k 256k)

main()
{
		   OUTPUTDIR_FS_JOB=./

		 	   echo $'\n'
			   echo "==== Start experiment of ${block_size} fio ===="

			   # Format and Mount
			   echo "==== Format $dev on $MNT ===="
			   
			  number=$(cat ${OUTPUTDIR_FS_JOB}/dmesg | grep drop_caches | sed 's/\]//g' | sed 's/\[//g' |  awk '{print $1}') 
				
				cat ${OUTPUTDIR_FS_JOB}/dmesg | grep print_cur_gc_profiler > ${OUTPUTDIR_FS_JOB}/gc_per_sec_only
				
#				cp ${FTRACE_PATH}/trace ${OUTPUTDIR_FS_JOB}/ftrace
				
				echo -e "#time\tutil\tdo_gc_cnt\tfsgc_lat\tdogc_lat\tcalc_dogc_lat\tcp\tmeta\tfilemap\tpcache\tread\twrite\tothers\t" > ${OUTPUTDIR_FS_JOB}/gc_lat_per_sec
				cat ${OUTPUTDIR_FS_JOB}/gc_per_sec_only | sed 's/\]//g' | sed 's/\[//g' | sed 's/(//g' | sed 's/)//g' | \
						awk -v num="$number" '{$1 = $1 - num; print $1, $6, $8, $10, $12, $14, $16, $18, $20, $22, $24, $30, $26+$28-$30}' >> ${OUTPUTDIR_FS_JOB}/gc_lat_per_sec
				
				echo -e "#time\tutil\tdo_gc_cnt\tfsgc_lat\tdogc_lat\tcalc_dogc_lat\tcp\tmeta\tfilemap\tpcache\tread\twrite\tothers\t" > ${OUTPUTDIR_FS_JOB}/gc_lat_per_sec_one_gc
				cat ${OUTPUTDIR_FS_JOB}/gc_per_sec_only | sed 's/\]//g' | sed 's/\[//g' | sed 's/(//g' | sed 's/)//g' | \
						awk -v num="$number" '{$1 = $1 - num; print $1, $6, $8, $10, $12, $14, $16, $18, $20, $22, $24, $30, $26+$28-$30}' | awk '{if ($3==1) print $0}' >> ${OUTPUTDIR_FS_JOB}/gc_lat_per_sec_one_gc
				
				echo -e "#time\tdo_gc_cnt\tdo_write_page_lat\t_order_lock\t_aloc_block\t_inv_cache\t_submit_pg\t__io_rwsem\t__zone_wait\t__other1\t__submit_previo\t__bio_alloc\t__bio_add\t__submit_write_by_bio_add\t___finish_plug\t___submit_bio\t__cur_zone_write\t__end_write\tupdate_map\t" > ${OUTPUTDIR_FS_JOB}/write_detail_lat_per_sec
				cat ${OUTPUTDIR_FS_JOB}/dmesg | grep outplace_detail_gc_profiler | sed 's/\]//g' | sed 's/\[//g' | sed 's/(//g' | sed 's/)//g' | \
						awk -v num="$number" '{$1 = $1 - num; print $1, $4, $6, $8, $10, $12, $14, $16, $18, $20, $22, $24, $26, $28, $30, $32, $34, $36, $38}' >> ${OUTPUTDIR_FS_JOB}/write_detail_lat_per_sec
				
				echo -e "#time\tdo_gc_cnt\tdo_write_page_lat\t_order_lock\t_aloc_block\t_inv_cache\t_submit_pg\t__io_rwsem\t__zone_wait\t__other1\t__submit_previo\t__bio_alloc\t__bio_add\t__submit_write_by_bio_add\t___finish_plug\t___submit_bio\t__cur_zone_write\t__end_write\tupdate_map\t" > ${OUTPUTDIR_FS_JOB}/write_detail_lat_per_sec_one_gc
				cat ${OUTPUTDIR_FS_JOB}/dmesg | grep outplace_detail_gc_profiler | sed 's/\]//g' | sed 's/\[//g' | sed 's/(//g' | sed 's/)//g' | \
						awk -v num="$number" '{$1 = $1 - num; print $1, $4, $6, $8, $10, $12, $14, $16, $18, $20, $22, $24, $26, $28, $30, $32, $34, $36, $38}' | awk '{if ($2==1) print $0}' >> ${OUTPUTDIR_FS_JOB}/write_detail_lat_per_sec_one_gc
				
				echo -e "#time\tdo_gc_cnt\tsubmit_bio\t_split\t_get_new_reqs\t__merge\t__throttle\t__alloc_requests\t___get_tag\t____io_schedule\t_mq_sched_insert\t" > ${OUTPUTDIR_FS_JOB}/submit_bio_detail_lat_per_sec
				cat ${OUTPUTDIR_FS_JOB}/dmesg | grep io_profiler | sed 's/\]//g' | sed 's/\[//g' | sed 's/(//g' | sed 's/)//g' | \
						awk -v num="$number" '{$1 = $1 - num; print $1, $4, $6, $8, $10, $12, $14, $16, $18, $20, $22}' >> ${OUTPUTDIR_FS_JOB}/submit_bio_detail_lat_per_sec
				
				echo -e "#time\tdo_gc_cnt\tsubmit_bio\t_split\t_get_new_reqs\t__merge\t__throttle\t__alloc_requests\t___get_tag\t____io_schedule\t_mq_sched_insert\t" > ${OUTPUTDIR_FS_JOB}/submit_bio_detail_lat_per_sec_one_gc
				cat ${OUTPUTDIR_FS_JOB}/dmesg | grep io_profiler | sed 's/\]//g' | sed 's/\[//g' | sed 's/(//g' | sed 's/)//g' | \
						awk -v num="$number" '{$1 = $1 - num; print $1, $4, $6, $8, $10, $12, $14, $16, $18, $20, $22}' | awk '{if ($2==1) print $0}'  >> ${OUTPUTDIR_FS_JOB}/submit_bio_detail_lat_per_sec_one_gc

			   echo "==== End ===="
#dmesg > ${OUTPUTDIR_FS_JOB}/dmesg_aft_umount
}

main               





