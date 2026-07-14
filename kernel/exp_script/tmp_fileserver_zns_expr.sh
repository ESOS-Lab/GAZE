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

DEV=(/dev/nvme1n1p1)
DEV_whole=/dev/nvme1n1
RES_FILE=blk
FTRACE_PATH=/sys/kernel/debug/tracing/
OUTPUTDIR="filebench_F2FS_ZNS_"$WORKLOAD"_`date "+%Y%m%d"`_`date "+%H%M"`"
MKBIN="/home/juwon/IFLBA_test/"
FILE_SIZE_COLD=25165824 #24GB, 80% of 30GB partition
#FILESYSTEM=(vanilla)
#FILESYSTEM=(lm)
#FILESYSTEM=(IPLFS_multi_zones)
#FILESYSTEM=(IPLFS_recent)
#FILESYSTEM=(IPLFS_optimized)
FILESYSTEM=(zns)
IO_TYPE=(randwrite)
#	write)   #read randread write randwrite trimwrite)	

NUM_JOBS=(4)

# 4 8)    #1 4 8)
RANDOM_BLOCK_SIZE=(4k) # 4k 8k 16k 32k)
SEQ_BLOCK_SIZE=(4k)   # 64k 256k)

main()
{
	# Create result root directory
	mkdir ${OUTPUTDIR}

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
				${MKBIN}zns_f2fs.sh ${filesys}
				#${MKBIN}ext4.sh ${filesys}
			   echo "==== Format complete ===="


			   # Format Page Cache
			   #blktrace -d $DEV_whole -o ${OUTPUTDIR_FS_JOB}/$RES_FILE$i & PID_BLKTRACE=$!;
			   #sudo iostat -p /dev/nvme0n1 1 > ${OUTPUTDIR_FS_JOB}/iostat.txt & PID_IOSTAT=$!;
			   echo "iostat pid=${PID_IOSTAT}"

		   	   ./record_df.sh ${OUTPUTDIR_FS_JOB}/df_log & PID_DF_LOG=$!;
		   	   ./record_mem.sh ${OUTPUTDIR_FS_JOB}/mem_log & PID_MEM_LOG=$!;
			   #echo 1 > ${FTRACE_PATH}tracing_on
			   # Run
			   echo "==== Run workload ===="
			   #strace -o process_dump/process_dump -ff \ 
			#  sudo ./create_additional_file/createfile -p $MNT -f $FILE_SIZE_COLD
				sudo echo 0 > /proc/sys/kernel/randomize_va_space
			   sync
			   echo 3 > /proc/sys/vm/drop_caches 
			   sudo sysctl kernel.randomize_va_space=0;
			   
			   su root -c 'echo STARTTTTTT > /dev/kmsg'
			   
#sudo gdb --args /home/juwon/filebench/filebench -f $FILEBENCH_PATH;
			   sudo /home/juwon/filebench/filebench -f $FILEBENCH_PATH 1> ${OUTPUTDIR_FS_JOB}/result.txt;
#sudo gdb --args /home/juwon/filebench/filebench -f $FILEBENCH_PATH 1> ${OUTPUTDIR_FS_JOB}/result.txt;
				#sudo /home/nobleminsu/filebench-1.5-alpha3/filebench -f $FILEBENCH_PATH 1> ${OUTPUTDIR_FS_JOB}/result.txt & PID_1=$!;
			   
			   #/home/juwon/fio/bin/fio \
			#100GB is okay
#			   /home/juwon/fio_src/fio \
#				   	    --filename=/mnt/test  \
#				   	    --name test \
#						--rw=${fs}  \
#					    --bs=${block_size} \
#					    --filesize=25GB \
#					    --numjobs=${numjob} \
#						--norandommap \
#						--allow_mounted_write=1	\
#						--random_generator=lfsr \
#					    --group_reporting=1 \
#					    --log_avg_msec=2000\
#					    --write_iops_log=${block_size}\
#					    --write_lat_log=${block_size} \
#					    --fadvise_hint=0 \
#					    --time_based --runtime=600s \
#					    > ${OUTPUTDIR_FS_JOB}/result_${block_size}.txt;
##--time_based --runtime=120s \
#		#			    #--time_based --runtime=300s \
#			   echo 0 > ${FTRACE_PATH}tracing_on

		   	   kill $PID_DF_LOG;
		   	   kill $PID_MEM_LOG;
			   
			   echo $'\n'
			   rm new
			   ln -s ${OUTPUTDIR_FS_JOB} new
			   
#			   cat ${block_size}_lat.*.log | \
#			   awk -F ',' '{print $2}' | sort -n -k 1 > tmp_lat.txt
#			   #awk -F ',' '{print $2}' ${block_size}_lat.1.log | sort -n -k 1 > tmp_lat.txt
#			   mv tmp_lat.txt ${OUTPUTDIR_FS_JOB}/lat_sum_sorted;
#			   #awk -F ',' '{print $2}' ${block_size}_iops.1.log | sort -n -k 1 > tmp_iops.txt
#			   #mv tmp_iops.txt ${OUTPUTDIR_FS_JOB}/iops_${block_size}.log;
#			   mv *.log ${OUTPUTDIR_FS_JOB}/;
#			   python sum.py ${OUTPUTDIR_FS_JOB}/4k_iops. > ${OUTPUTDIR_FS_JOB}/kiops_sum
			   dmesg > ${OUTPUTDIR_FS_JOB}/dmesg


			  number=$(cat ${OUTPUTDIR_FS_JOB}/dmesg | grep STARTT | sed 's/\]//g' |  awk '{print $2}') 

			   cat ${OUTPUTDIR_FS_JOB}/dmesg | grep -e WAF -e "STARTTT" > ${OUTPUTDIR_FS_JOB}/waf_raw

			   cat ${OUTPUTDIR_FS_JOB}/dmesg | grep FSWAF > ${OUTPUTDIR_FS_JOB}/fswaf_only
#				cat ${OUTPUTDIR_FS_JOB}/waf_only | sed 's/\]//g' | sed 's/\[//g' | awk '{print $1, $4}' >${OUTPUTDIR_FS_JOB}/waf_parsed
				cat ${OUTPUTDIR_FS_JOB}/fswaf_only | sed 's/\]//g' | sed 's/\[//g' | awk -v num="$number" '{$1 = $1 - num; print $1, $4}' >${OUTPUTDIR_FS_JOB}/fswaf_parsed

			   cat ${OUTPUTDIR_FS_JOB}/dmesg | grep WAF | grep -v FSWAF > ${OUTPUTDIR_FS_JOB}/waf_only
				cat ${OUTPUTDIR_FS_JOB}/waf_only | sed 's/\]//g' | sed 's/\[//g' | awk -v num="$number" '{print $1 - num, $4}' >${OUTPUTDIR_FS_JOB}/waf_parsed
				rm ${OUTPUTDIR_FS_JOB}/waf_only 

				cat ${OUTPUTDIR_FS_JOB}/dmesg | grep MG_CMD_CNT > ${OUTPUTDIR_FS_JOB}/mgcmd_only
#				cat ${OUTPUTDIR_FS_JOB}/mgcmd_only | sed 's/\]//g' | sed 's/\[//g' | awk '{print $1, $6}' > ${OUTPUTDIR_FS_JOB}/mgcmd_parsed
				cat ${OUTPUTDIR_FS_JOB}/mgcmd_only | sed 's/\]//g' | sed 's/\[//g' | awk -v num="$number" '{$1 = $1 - num; print $1, $6}'  > ${OUTPUTDIR_FS_JOB}/mgcmd_parsed
				rm ${OUTPUTDIR_FS_JOB}/mgcmd_only 

				cat ${OUTPUTDIR_FS_JOB}/dmesg | grep GC_LOG_MEM > ${OUTPUTDIR_FS_JOB}/gclog_mem_only
#				cat ${OUTPUTDIR_FS_JOB}/gclog_mem_only | sed 's/\]//g' | sed 's/\[//g' | awk '{print $1, $4}' > ${OUTPUTDIR_FS_JOB}/gclog_mem_parsed
				cat ${OUTPUTDIR_FS_JOB}/gclog_mem_only | sed 's/\]//g' | sed 's/\[//g' | awk -v num="$number" '{$1 = $1 - num; print $1, $4}'  > ${OUTPUTDIR_FS_JOB}/gclog_mem_parsed
				rm ${OUTPUTDIR_FS_JOB}/gclog_mem_only 

				cat ${OUTPUTDIR_FS_JOB}/dmesg | grep CHIP_UTIL > ${OUTPUTDIR_FS_JOB}/chip_util_raw
#				cat ${OUTPUTDIR_FS_JOB}/chip_util_raw | sed 's/\]//g' | sed 's/\[//g' | awk '{print $1, $4}' > ${OUTPUTDIR_FS_JOB}/chip_util
				cat ${OUTPUTDIR_FS_JOB}/chip_util_raw | sed 's/\]//g' | sed 's/\[//g' | awk -v num="$number" '{$1 = $1 - num; print $1, $4}' > ${OUTPUTDIR_FS_JOB}/chip_util
#${MKBIN}f2fs_rm_umount.sh ${filesys}
			   #./taillat_fb.sh ${OUTPUTDIR_FS_JOB}



#               cat ${OUTPUTDIR_FS_JOB}/dmesg | grep -e WAF -e "lm_write: type: 1 start_lpn:" > ${OUTPUTDIR_FS_JOB}/waf_raw
#			   cat ${OUTPUTDIR_FS_JOB}/dmesg | grep WAF > ${OUTPUTDIR_FS_JOB}/waf_only
#			   cat ${OUTPUTDIR_FS_JOB}/waf_only | sed 's/\]//g' | sed 's/\[//g' | awk '{print $1, $4}' > ${OUTPUTDIR_FS_JOB}/waf_parsed
#			   
#			   rm ${OUTPUTDIR_FS_JOB}/waf_only
#			   cat ${OUTPUTDIR_FS_JOB}/dmesg | grep MG_CMD_CNT > ${OUTPUTDIR_FS_JOB}/mgcmd_only
#			   cat ${OUTPUTDIR_FS_JOB}/mgcmd_only | sed 's/\]//g' | sed 's/\[//g' | awk '{print $1, $6}' > ${OUTPUTDIR_FS_JOB}/mgcmd_parsed
#			   rm ${OUTPUTDIR_FS_JOB}/mgcmd_only
#			   cat ${OUTPUTDIR_FS_JOB}/dmesg | grep GC_LOG_MEM > ${OUTPUTDIR_FS_JOB}/gclog_mem_only
#			   cat ${OUTPUTDIR_FS_JOB}/gclog_mem_only | sed 's/\]//g' | sed 's/\[//g' | awk '{print $1, $4}' > ${OUTPUTDIR_FS_JOB}/gclog_mem_parsed
#			   rm ${OUTPUTDIR_FS_JOB}/gclog_mem_only
			   
			   
			   cat ${OUTPUTDIR_FS_JOB}/result.txt | grep -e Summary > ${OUTPUTDIR_FS_JOB}/result_time
			   cat ${OUTPUTDIR_FS_JOB}/result_time | awk 'BEGIN {t=5} {print t, $6/1000} {t+=5}' > ${OUTPUTDIR_FS_JOB}/kiops_sum
			   rm ${OUTPUTDIR_FS_JOB}/result_time









				#${MKBIN}f2fs_rm_umount.sh ${filesys}
			   #./taillat_fb.sh ${OUTPUTDIR_FS_JOB}
   	  		   echo "fb end";
			   #sudo kill $PID_BLKTRACE;
			   #wait $PID_1;
			   #sudo kill -9 $PID_1;
			   #kill -9 $PID_IOSTAT;
			   
			   #echo "kill end";
			   #wait $PID_BLKTRACE;\
			#    wait $PID_IOSTAT;
			   wait $PID_DF_LOG;
			   wait $PID_MEM_LOG;

			   echo "blkparsing start!";
			   #sudo blkparse -i ${OUTPUTDIR_FS_JOB}/$RES_FILE$i -o ${OUTPUTDIR_FS_JOB}/blkresult$RES_FILE_$RES_FILE$i;\
			   #rm -rf ${OUTPUTDIR_FS_JOB}/*blktrace*
			   #cat ${OUTPUTDIR_FS_JOB}/blkresult$RES_FILE_$RES_FILE$i | grep D | grep W | grep -v Read > ${OUTPUTDIR_FS_JOB}/blkwrite
			   #cat ${OUTPUTDIR_FS_JOB}/blkresult$RES_FILE_$RES_FILE$i | grep 'D   D'  > ${OUTPUTDIR_FS_JOB}/discard
			   #cat ${OUTPUTDIR_FS_JOB}/blkresult$RES_FILE_$RES_FILE$i | grep R | grep -v Read | grep -v Thr  > ${OUTPUTDIR_FS_JOB}/read
			   cat ${OUTPUTDIR_FS_JOB}/mem_log | grep 'Available'  | awk '{a += 1} {print a, $2/1024/1024}' >  ${OUTPUTDIR_FS_JOB}/available_mem_GB_per_sec
			   cat ${OUTPUTDIR_FS_JOB}/mem_log | grep 'Free'  | awk '{a += 1} {print a, $2/1024/1024}' >  ${OUTPUTDIR_FS_JOB}/free_mem_GB_per_sec
               #--fdatasync=1
			   #--iodepth=64
			   #--allow_mounted_write=1

					    #--buffered=1	\
				#		--iodepth=64	\
				cp throughput_waf_fs_fb.gpi  ${OUTPUTDIR_FS_JOB}/        
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





