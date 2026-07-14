#!/bin/bash

MNT=/mnt


DEV=(/dev/nvme2n1p1)
DEV_whole=/dev/nvme2n1
DEV_ZNS_NAME=nvme3n1
DEV_ZNS=/dev/$DEV_ZNS_NAME


CUR_DIR=$(pwd)
FIO_PATH=(/home/juwon/fio_src)
#FILESYSTEM=(fastgc_batch_2 fastgc_batch_1)
#FILESYSTEM=(f2fs_vanilla_semiaggressive_discard_profile)
#FILESYSTEM=(f2fs_vanilla_profile)	# f2fs vanilla
#FILESYSTEM=(f2fs_pgpool_profile)	# GC page pool only
#FILESYSTEM=(f2fs_pgpool_4step_profile)		# pg pool + 4step
#FILESYSTEM=(gns_4step_profile)		# GNS
#FILESYSTEM=(gns_4step_profile f2fs_pgpool_4step_profile)		
#FILESYSTEM=(f2fs_4step_profile)		
FILESYSTEM=(f2fs_4step_batchseg_32_profile f2fs_4step_batchseg_16_profile f2fs_4step_batchseg_8_profile f2fs_4step_batchseg_4_profile f2fs_4step_batchseg_64_profile f2fs_4step_batchseg_128_profile f2fs_4step_batchseg_2_profile f2fs_4step_batchseg_256_profile f2fs_4step_batchseg_512_profile )		
FILESYSTEM_NAME=(OSDI26)
#FILESYSTEM=(f2fs_profile)
#FILESYSTEM=(f2fs)
#FILESYSTEM=(f2fs_gc_req)
#FILESYSTEM=(f2fs_sync_wrt_profile)
#FILESYSTEM=(f2fs_dirty_pg_monitor)
#FILESYSTEM=(f2fs_NB_only)
#FILESYSTEM=(f2fs_NB_page)
#FILESYSTEM=(f2fs_copy)
#FILESYSTEM=(f2fs)
#FILESYSTEM=(f2fs_profile)
#FILESYSTEM=(gns_blk_gra_profile)
#FILESYSTEM=(gns_deep_profile)
#FILESYSTEM=(fast_ver_happy)
#FILESYSTEM=(blk_gra_happy)
#FILESYSTEM=(blk_gra_profile)
#FILESYSTEM=(gns_deep_happy)
#FILESYSTEM=(gns_deep_is_alive_opti_happy)
#FILESYSTEM=(f2fs_test)
#FILESYSTEM=(f2fs_vanilla_profile)
#FILESYSTEM=(gns_deep_aggressive_discard_happy)
#FILESYSTEM=(gns_deep_aggressive_discard_profile)
#FILESYSTEM=(gns_deep_semiaggressive_discard_profile)
#FILESYSTEM=(gns_2steppipe_semiaggressive_discard_profile)
IO_TYPE=(randwrite)

NUM_JOBS=(1)
RANDOM_BLOCK_SIZE=(4k) # 4k 8k 16k 32k)
SEQ_BLOCK_SIZE=(4k)   # 64k 256k)

OUTPUTDIR="${FILESYSTEM_NAME}_data/${FILESYSTEM}_fio_exp_output_${RANDOM_BLOCK_SIZE}_`date "+%Y%m%d"`_`date "+%H%M"`"

main()
{
	# Create Disk Partition
	export LD_LIBRARY_PATH=/usr/local/sbin:$LD_LIBRARY_PATH
	
	#echo -e "d\nw" | sudo fdisk ${DEV_whole}
	#echo -e "n\n\n\n\n+15G\nY\nw" | sudo fdisk ${DEV_whole}
	
	# Create result root directory
	mkdir -p ${OUTPUTDIR}

	# Disable ASLR
	#echo 0 > /proc/sys/kernel/randomize_va_space

	# Create directory for device
	for fs in ${IO_TYPE[@]}
	do
		# Set a filesystem result name
		OUTPUTDIR_FS=${OUTPUTDIR}/${fs}

		# Craete directory for filesystem
		mkdir ${OUTPUTDIR_FS}

 	    case $fs in
		    "write")
				BLOCK_SIZE=${SEQ_BLOCK_SIZE[@]}
			    ;;
			"randwrite")
				BLOCK_SIZE=${RANDOM_BLOCK_SIZE[@]}
				;;
			"trimwrite")
			    BLOCK_SIZE=${SEQ_BLOCK_SIZE[@]}
				;;
			"read")
			    BLOCK_SIZE=${SEQ_BLOCK_SIZE[@]}
				;;
			"randread")
			    BLOCK_SIZE=${RANDOM_BLOCK_SIZE[@]}
				;;
	    esac
	    for filesys in ${FILESYSTEM[@]}
	    	do
		echo -e "d\nw" | sudo fdisk ${DEV_whole}
		echo -e "n\n\n\n\n+15G\nY\nw" | sudo fdisk ${DEV_whole}
		echo 0 > /proc/sys/kernel/randomize_va_space
	    for numjob in ${NUM_JOBS[@]}
		do
		   # Set a number of jobs result name
		   OUTPUTDIR_FS_JOB=${OUTPUTDIR_FS}/${filesys}/${numjob}

		   # Create dirctory for numjob
		   mkdir -p ${OUTPUTDIR_FS_JOB}

		   for block_size in ${BLOCK_SIZE}
		   do
		 	   echo $'\n'
			   echo "==== Start experiment of ${block_size} fio ===="

			   # Format and Mount
			   echo "==== Format $dev on $MNT ===="
				cd ../general_resource
				./zns.sh ${filesys}
				cd "$CUR_DIR"
			   echo "==== Format complete ===="

			   echo mq-deadline > /sys/block/${DEV_ZNS_NAME}/queue/scheduler
		   	   
			   ../general_resource/record_df.sh ${OUTPUTDIR_FS_JOB}/df_log & PID_DF_LOG=$!;
			   
			   # Run
			   echo "==== Run workload ===="
			   #cp run_fio_f2fs.sh ${OUTPUTDIR_FS_JOB}/
			   cp run_fio_deep_pipe.sh ${OUTPUTDIR_FS_JOB}/
			   sync
			   echo 3 > /proc/sys/vm/drop_caches 
			   sudo sysctl kernel.randomize_va_space=0;
			   sudo dmesg > ${OUTPUTDIR_FS_JOB}/bef_run_dmesg
			   sudo dmesg -C
			   su root -c 'echo STARTTTTTT > /dev/kmsg'
			   ${FIO_PATH}/fio \
				   	    --filename=${MNT}/test  \
				   	    --name test \
						--rw=${fs}  \
					    --bs=${block_size} \
					    --filesize=180GB \
					    --numjobs=${numjob} \
						--norandommap \
						--allow_mounted_write=1	\
						--random_generator=tausworthe \
						--randseed=1 \
					    --group_reporting=1 \
					    --log_avg_msec=2000\
					    --write_iops_log=${block_size}\
					    --write_lat_log=${block_size} \
					    --fadvise_hint=0 \
					    --percentile_list=90:95:99:99.90:99.95:99.99:99.999:99.9999:99.99999:99.999999 \
					    --time_based --runtime=1200s \
					    > ${OUTPUTDIR_FS_JOB}/result_${block_size}.txt;

		   	   kill $PID_DF_LOG;
			   
			   echo "==== Workload complete ===="
			   echo "==== Process Result Data ===="
			   echo $'\n'
			   rm new
			   ln -s ${OUTPUTDIR_FS_JOB} new
			   cat ${block_size}_lat.*.log | \
			   awk -F ',' '{print $2}' | sort -n -k 1 > tmp_lat.txt
			   mv tmp_lat.txt ${OUTPUTDIR_FS_JOB}/lat_sum_sorted;
			   mv *.log ${OUTPUTDIR_FS_JOB}/;
			   python ../general_resource/sum.py ${OUTPUTDIR_FS_JOB}/${BLOCK_SIZE}_iops. ${numjob} > ${OUTPUTDIR_FS_JOB}/kiops_sum
			   cat ${OUTPUTDIR_FS_JOB}/kiops_sum | tail -n 345 | awk '{b += 1; a += $2} END {print a/b}' > ${OUTPUTDIR_FS_JOB}/kiops_avg_gc
			   dmesg > ${OUTPUTDIR_FS_JOB}/dmesg
			   ../general_resource/f2fs_rm_umount.sh ${filesys}
			   echo -e "d\nw" | fdisk ${DEV_whole}

			   ../general_resource/cp_kern_log.sh
			   ../general_resource/parse_kern_log.sh > ${OUTPUTDIR_FS_JOB}/dmesg_aft_umount

			   number=$(cat ${OUTPUTDIR_FS_JOB}/dmesg | grep STARTT | sed 's/\[//g' | sed 's/\]//g' |  awk '{print $1}')



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


			   cp ../general_resource/plot/*.gpi ${OUTPUTDIR_FS_JOB}/

			   wait $PID_DF_LOG;

			   chown -R juwon ${OUTPUTDIR}


			   echo "==== End the experiment ===="
		   done
	   	done
		done
	done
	
}

main               





