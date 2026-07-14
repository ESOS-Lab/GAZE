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
#FILESYSTEM=(fastgc)
#FILESYSTEM=(preread_fmap_once_write_nothrottle)
FILESYSTEM=(vanilla)
IO_TYPE=(randwrite)
#	write)   #read randread write randwrite trimwrite)	

#NUM_JOBS=(1)
NUM_JOBS=(16)

# 4 8)    #1 4 8)
RANDOM_BLOCK_SIZE=(4k) # 4k 8k 16k 32k)
SEQ_BLOCK_SIZE=(4k)   # 64k 256k)

main()
{
	# Create result root directory
	mkdir ${OUTPUTDIR}

	echo -e "d\nw" | fdisk ${DEV_whole}
	echo -e "n\n\n\n\n+5G\nY\nw" | fdisk ${DEV_whole}

	# Disable ASLR
	echo 0 > /proc/sys/kernel/randomize_va_space

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


				#dmesg -C
				${MKBIN}zns.sh ${filesys}
				#${MKBIN}ext4.sh ${filesys}
			   echo "==== Format complete ===="

			   # Format Page Cache
#			   blktrace -d $DEV_ZNS -o ${OUTPUTDIR_FS_JOB}/$RES_FILE$i & PID_BLKTRACE=$!;
				#sudo iostat -p /dev/nvme1n1 -d 1 > ${OUTPUTDIR_FS_JOB}/iostat.txt & PID_IOSTAT=$!;
#				echo "iostat pid=${PID_IOSTAT}"

				echo mq-deadline > /sys/block/nvme1n1/queue/scheduler
#				echo none > /sys/block/nvme1n1/queue/scheduler

		   	   ./record_mem.sh ${OUTPUTDIR_FS_JOB}/mem_log & PID_MEM_LOG=$!;
		   	   ./record_df.sh ${OUTPUTDIR_FS_JOB}/df_log & PID_DF_LOG=$!;

##				FTRACE activation 
#			   echo function_graph > ${FTRACE_PATH}current_tracer
#				echo 100000 > ${FTRACE_PATH}buffer_size_kb
#			   echo > ${FTRACE_PATH}trace
#			   echo > ${FTRACE_PATH}set_ftrace_filter
#
#			   input=${funcs_gc}
#				while IFS= read -r line
#				do
##					  echo "$line"| awk '{print $1;}' >> ${FTRACE_PATH}set_ftrace_filter
#					  echo "$line"| awk '{print $1;}' >> ${FTRACE_PATH}set_graph_function
#					  #echo "$line"| awk '{print $1;}' >> ${FTRACE_PATH}set_ftrace_notrace
#					  #echo "$line" >> ${FTRACE_PATH}set_ftrace_filter
#					echo "$line"
#				done < "$input"
#			   
##				input=${funcs_bio}
##				while IFS= read -r line
##				do
##					  echo "$line"| awk '{print $1;}' >> ${FTRACE_PATH}set_ftrace_filter
##					  #echo "$line"| awk '{print $1;}' >> ${FTRACE_PATH}set_ftrace_notrace
##					  #echo "$line" >> ${FTRACE_PATH}set_ftrace_filter
##					echo "$line"
##				done < "$input"
#
#
#			   echo 1 > ${FTRACE_PATH}tracing_on
			   echo 0 > ${FTRACE_PATH}tracing_on

			   # Run
			   echo "==== Run workload ===="
			   #strace -o process_dump/process_dump -ff \ 
#sudo ./create_additional_file/createfile -p $MNT -f $FILE_SIZE_COLD
			   sync
			   echo 3 > /proc/sys/vm/drop_caches 
			   sudo sysctl kernel.randomize_va_space=0;
			   #sudo /home/juwon/filebench/filebench -f $FILEBENCH_PATH 1> ${OUTPUTDIR_FS_JOB}/result.txt;
			   #sudo /home/nobleminsu/filebench-1.5-alpha3/filebench -f $FILEBENCH_PATH 1> ${OUTPUTDIR_FS_JOB}/result.txt & PID_1=$!;
			   sudo dmesg > ${OUTPUTDIR_FS_JOB}/bef_run_dmesg
			   sudo dmesg -C
			  su root -c 'echo STARTTTTTT > /dev/kmsg' 
			   #/home/juwon/fio/bin/fio \
			#100GB is okay
#			   /home/juwon/fio_src/fio \
#				   	    --filename=/mnt/test0  \
#				   	    --name test0 \
#						--rw=${fs}  \
#					    --bs=${block_size} \
#					    --filesize=24GB \
#					    --numjobs=${numjob} \
#						--norandommap \
#						--randseed=1 \
#						--random_generator=lfsr \
#						--allow_mounted_write=1	\
#					    --group_reporting=1 \
#					    --log_avg_msec=2000\
#					    --write_iops_log=${block_size}\
#					    --write_lat_log=${block_size} \
#					    --fadvise_hint=0 \
#					    > ${OUTPUTDIR_FS_JOB}/result_${block_size}.txt;
#			   
#			
#			   cat ${block_size}_lat.*.log | \
#			   awk -F ',' '{print $2}' | sort -n -k 1 > tmp_lat.txt
#			   #awk -F ',' '{print $2}' ${block_size}_lat.1.log | sort -n -k 1 > tmp_lat.txt
#			   mv tmp_lat.txt ${OUTPUTDIR_FS_JOB}/lat_sum_sorted;
#			   #awk -F ',' '{print $2}' ${block_size}_iops.1.log | sort -n -k 1 > tmp_iops.txt
#			   #mv tmp_iops.txt ${OUTPUTDIR_FS_JOB}/iops_${block_size}.log;
#			   mv *.log ${OUTPUTDIR_FS_JOB}/;
#			   python sum.py ${OUTPUTDIR_FS_JOB}/4k_iops. ${numjob} > ${OUTPUTDIR_FS_JOB}/kiops_sum
#				  su root -c 'echo CREATE_DONE > /dev/kmsg' 
				  cp run_fio_zns.sh ${OUTPUTDIR_FS_JOB}/
			   /home/juwon/fio_src/fio \
				   	    --filename=/mnt/test  \
				   	    --name test \
						--rw=${fs}  \
					    --bs=${block_size} \
					    --filesize=65GB \
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
						--time_based --runtime=400s \
					    > ${OUTPUTDIR_FS_JOB}/result_${block_size}.txt;

#					    --filesize=80GB \
#					    --filesize=65GB \
#						--size=300G \
#					    --log_avg_msec=2000\
			   echo 0 > ${FTRACE_PATH}tracing_on
		   	   kill $PID_MEM_LOG;
		   	   kill $PID_DF_LOG;
			   
			   echo $'\n'
			   rm new
			   ln -s ${OUTPUTDIR_FS_JOB} new
			   cat ${block_size}_lat.*.log | \
			   awk -F ',' '{print $2}' | sort -n -k 1 > tmp_lat.txt
			   #awk -F ',' '{print $2}' ${block_size}_lat.1.log | sort -n -k 1 > tmp_lat.txt
			   mv tmp_lat.txt ${OUTPUTDIR_FS_JOB}/lat_sum_sorted;
			   #awk -F ',' '{print $2}' ${block_size}_iops.1.log | sort -n -k 1 > tmp_iops.txt
			   #mv tmp_iops.txt ${OUTPUTDIR_FS_JOB}/iops_${block_size}.log;
			   mv *.log ${OUTPUTDIR_FS_JOB}/;
			   python sum.py ${OUTPUTDIR_FS_JOB}/4k_iops. ${numjob} > ${OUTPUTDIR_FS_JOB}/kiops_sum1
			   
				# for 400 sec
				cat ${OUTPUTDIR_FS_JOB}/kiops_sum1 | tail -n 150 | awk '{b += 1; a += $2} END {print a/b}' > ${OUTPUTDIR_FS_JOB}/kiops_avg_gc
				# for 300 sec
#				cat ${OUTPUTDIR_FS_JOB}/kiops_sum1 | tail -n 100 | awk '{b += 1; a += $2} END {print a/b}' > ${OUTPUTDIR_FS_JOB}/kiops_avg_gc
			   
			   dmesg > ${OUTPUTDIR_FS_JOB}/dmesg
				${MKBIN}f2fs_rm_umount.sh ${filesys}
				echo -e "d\nw" | fdisk ${DEV_whole}
#			   dmesg > ${OUTPUTDIR_FS_JOB}/dmesg_aft_umount
				./cp_kern_log.sh
				./parse_kern_log.sh > ${OUTPUTDIR_FS_JOB}/dmesg_aft_umount
			   
			  number=$(cat ${OUTPUTDIR_FS_JOB}/dmesg | grep drop_caches | sed 's/\]//g' | sed 's/\[//g' |  awk '{print $1}') 

			   cat ${OUTPUTDIR_FS_JOB}/dmesg | grep -e WAF -e "STARTTT" > ${OUTPUTDIR_FS_JOB}/waf_raw

			   cat ${OUTPUTDIR_FS_JOB}/dmesg | grep FSWAF > ${OUTPUTDIR_FS_JOB}/fswaf_only
#				cat ${OUTPUTDIR_FS_JOB}/waf_only | sed 's/\]//g' | sed 's/\[//g' | awk '{print $1, $4}' >${OUTPUTDIR_FS_JOB}/waf_parsed
				cat ${OUTPUTDIR_FS_JOB}/fswaf_only | sed 's/\]//g' | sed 's/\[//g' | awk -v num="$number" '{$1 = $1 - num; print $1, $4}' >${OUTPUTDIR_FS_JOB}/fswaf_parsed

			   cat ${OUTPUTDIR_FS_JOB}/dmesg | grep WAF | grep -v FSWAF > ${OUTPUTDIR_FS_JOB}/waf_only
				cat ${OUTPUTDIR_FS_JOB}/waf_only | sed 's/\]//g' | sed 's/\[//g' | awk -v num="$number" '{print $1 - num, $4}' >${OUTPUTDIR_FS_JOB}/waf_parsed
				rm ${OUTPUTDIR_FS_JOB}/waf_only 
			   
				
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
			   #./taillat_fb.sh ${OUTPUTDIR_FS_JOB}
   	  		   echo "fb end";
#			   sudo kill $PID_BLKTRACE;
			   #wait $PID_1;
			   #sudo kill -9 $PID_1;
#			   kill -9 $PID_IOSTAT;
			   
			   #echo "kill end";
#			   wait $PID_BLKTRACE;\
#			   wait $PID_IOSTAT;
			   wait $PID_MEM_LOG;
			   wait $PID_DF_LOG;

#				echo "blkparsing start!";
#			   sudo blkparse -i ${OUTPUTDIR_FS_JOB}/$RES_FILE$i -o ${OUTPUTDIR_FS_JOB}/blkresult$RES_FILE_$RES_FILE$i;\
#			   rm -rf ${OUTPUTDIR_FS_JOB}/*blktrace*
#			   cat ${OUTPUTDIR_FS_JOB}/blkresult$RES_FILE_$RES_FILE$i | grep D | grep W | grep -v Read > ${OUTPUTDIR_FS_JOB}/blkwrite
#			   cat ${OUTPUTDIR_FS_JOB}/blkresult$RES_FILE_$RES_FILE$i | grep 'D   D'  > ${OUTPUTDIR_FS_JOB}/discard
#			   cat ${OUTPUTDIR_FS_JOB}/blkresult$RES_FILE_$RES_FILE$i | grep R | grep -v Read | grep -v Thr  > ${OUTPUTDIR_FS_JOB}/read
			   cat ${OUTPUTDIR_FS_JOB}/mem_log | grep 'Available'  | awk '{a += 1} {print a, $2/1024/1024}' >  ${OUTPUTDIR_FS_JOB}/available_mem_GB_per_sec
			   cat ${OUTPUTDIR_FS_JOB}/mem_log | grep 'Free'  | awk '{a += 1} {print a, $2/1024/1024}' >  ${OUTPUTDIR_FS_JOB}/free_mem_GB_per_sec
			   cp plot/*.gpi ${OUTPUTDIR_FS_JOB}/
			   
			   chown -R juwon ${OUTPUTDIR}

			   echo "==== Workload complete ===="

			   echo "==== End the experiment ===="
#dmesg > ${OUTPUTDIR_FS_JOB}/dmesg_aft_umount
		   done
	   	done
		done
	done
	
}

main               





