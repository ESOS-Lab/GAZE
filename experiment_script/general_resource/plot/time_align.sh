#!/bin/bash

MNT=/mnt


DEV=(/dev/nvme2n1p1)
DEV_whole=/dev/nvme2n1
DEV_ZNS_NAME=nvme3n1
DEV_ZNS=/dev/$DEV_ZNS_NAME


CUR_DIR=$(pwd)
FIO_PATH=(/home/juwon/fio_src)
#FILESYSTEM=(f2fs)
#FILESYSTEM=(f2fs_preread_segs_2 f2fs_preread_segs_1 f2fs_preread_segs_256 f2fs_preread_segs_128 f2fs_preread_segs_64 f2fs_preread_segs_32 f2fs_preread_segs_16 f2fs_preread_segs_8 f2fs_preread_segs_4)
FILESYSTEM=(f2fs_preread_segs_512)
IO_TYPE=(randwrite)

NUM_JOBS=(1)
RANDOM_BLOCK_SIZE=(128k) # 4k 8k 16k 32k)
SEQ_BLOCK_SIZE=(4k)   # 64k 256k)

OUTPUTDIR="f2fs_data/fio_f2fs_exp_output_PagePoolCheck_${RANDOM_BLOCK_SIZE}_`date "+%Y%m%d"`_`date "+%H%M"`"

OUTPUTDIR_FS_JOB="."

main()
{

			   number=$(cat ${OUTPUTDIR_FS_JOB}/dmesg | grep STARTT | sed 's/\[//g'  | sed 's/\]//g' |  awk '{print $1}')

				cat ${OUTPUTDIR_FS_JOB}/dmesg | grep print_cur_gc_profiler | grep 'lat summary' > ${OUTPUTDIR_FS_JOB}/gc_per_sec_only
				
#				cat ${OUTPUTDIR_FS_JOB}/dmesg | grep 'pipelining check' | grep 'do_gc_cnt: 1' > ${OUTPUTDIR_FS_JOB}/phase_lat_one_gc
#				echo -e "#time\tp0\tp1\tp2\tp3\tp4" > ${OUTPUTDIR_FS_JOB}/gc_lat_per_sec
				
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

			   cat ${OUTPUTDIR_FS_JOB}/dmesg_aft_umount | grep print_intra_seg | sed 's/\]//g' | sed 's/\[//g' > ${OUTPUTDIR_FS_JOB}/intra_seg_read_lat
			   cat ${OUTPUTDIR_FS_JOB}/dmesg_aft_umount | grep print_intra_zone | sed 's/\]//g' | sed 's/\[//g' > ${OUTPUTDIR_FS_JOB}/intra_zone_info

			   echo "==== End the experiment ===="
}

main               





