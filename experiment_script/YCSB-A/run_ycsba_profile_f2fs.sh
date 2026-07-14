#!/bin/bash

USER=juwon
HOME=/home/${USER}
YCSB=${HOME}/lee/YCSB
MNT=/mnt
DATA=/exp_mysql_data
DEV=(/dev/nvme2n1p1)
DEV_whole=/dev/nvme2n1
DEV_ZNS_NAME=nvme3n1
DEV_ZNS=/dev/$DEV_ZNS_NAME

CUR_DIR=$(pwd)
FILESYSTEM=(f2fs)
OUTPUTDIR="f2fs_data/ycsba_F2FS_exp_output_${FILESYSTEM}_`date "+%Y%m%d"`_`date "+%H%M"`"
RECORDSIZE=(8000)
RECOUNT=(16000000)
OPCOUNT=(10000000)
THREAD=15

main()
{
	# Create Disk Partition
	export LD_LIBRARY_PATH=/usr/local/sbin:$LD_LIBRARY_PATH
	
	echo -e "d\nw" | sudo fdisk ${DEV_whole}
	echo -e "n\n\n\n\n+15G\nY\nw" | sudo fdisk ${DEV_whole}
	
	# Create result root directory
	mkdir -p ${OUTPUTDIR}
	
	# Disable ASLR
	echo 0 > /proc/sys/kernel/randomize_va_space
	
	rs=${RECORDSIZE[i]}
	rs_kb=$(expr ${rs} / 1000)
	fieldlength=$(expr ${rs} / 10)

       	echo $'\n'
       	echo "==== Start experiment of mysql  ===="
       	
	# Format and Mount
    	echo "==== Format $dev on $MNT ===="
	systemctl stop mysql
	
	cd ../general_resource
	./zns.sh ${FILESYSTEM}
	cd "$CUR_DIR"

        echo "==== Format complete ===="

	echo mq-deadline > /sys/block/${DEV_ZNS_NAME}/queue/scheduler
	
	../general_resource/record_df.sh ${OUTPUTDIR}/df_log & PID_DF_LOG=$!;

	cp -rp ${DATA}/cold_20G ${MNT}/
	cp -rp ${DATA}/cold_20G ${MNT}/cold_20G_another
	cp -rp ${DATA}/cold_10G ${MNT}/
	cp -rp ${DATA}/mysql_174G ${MNT}/mysql
	chown -R mysql ${MNT}
	chmod -R 777 ${MNT}
	systemctl start mysql

        echo "==== Run YCSB-A workload ===="
        sync
        echo 3 > /proc/sys/vm/drop_caches
	sudo sysctl kernel.randomize_va_space=0;
	sudo dmesg > ${OUTPUTDIR}/bef_run_dmesg
	sudo dmesg -C
	
	cd ${YCSB}
	su root -c 'echo STARTTTTTT > /dev/kmsg' 
	sudo bin/ycsb run jdbc -P ${CUR_DIR}/workloada -P db.properties \
		       	-p recordcount=${RECOUNT[i]} \
	       		-p fieldlength=${fieldlength} \
			-p operationcount=${OPCOUNT} \
			-p writeallfields=false \
			-threads ${THREAD} \
			-cp mysql-connector-j-8.0.33.jar \
			> ${CUR_DIR}/${OUTPUTDIR}/result.txt

	kill $PID_DF_LOG;

	echo "==== Workload complete ===="
	
	echo "==== Process Result Data ===="
	cd "$CUR_DIR"
	chown -R ${USER} ${OUTPUTDIR}
	echo $'\n'
	rm new
	ln -s ${OUTPUTDIR} new

	dmesg > ${OUTPUTDIR}/dmesg
	../general_resource/f2fs_rm_umount.sh ${filesys}
	echo -e "d\nw" | fdisk ${DEV_whole}

	../general_resource/cp_kern_log.sh
	../general_resource/parse_kern_log.sh > ${OUTPUTDIR}/dmesg_aft_umount

	number=$(cat ${OUTPUTDIR}/dmesg | grep STARTT | sed 's/\[//g'  | sed 's/\]//g' |  awk '{print $1}') 
				
	cat ${OUTPUTDIR}/dmesg | sed 's/\]//g' | sed 's/\[//g' | awk -v num="$number" '{$1 = $1 - num; print $0}' >${OUTPUTDIR}/dmesg_parsed

	cat ${OUTPUTDIR}/dmesg | grep print_cur_gc_profiler | grep 'lat summary' > ${OUTPUTDIR}/gc_per_sec_only
	
	cat ${OUTPUTDIR}/dmesg | grep 'pipelining check' | grep 'do_gc_cnt: 1' > ${OUTPUTDIR}/phase_lat_one_gc
	echo -e "#time\tp0\tp1\tp2\tp3\tp4" > ${OUTPUTDIR}/gc_lat_per_sec
	
	echo -e "#time\tutil\tdo_gc_cnt\tfsgc_lat\tdogc_lat\tcalc_dogc_lat\tcp\tmeta\tfilemap\tpcache\tread\twrite\tothers\t" > ${OUTPUTDIR}/gc_lat_per_sec
	cat ${OUTPUTDIR}/gc_per_sec_only | sed 's/\]//g' | sed 's/\[//g' | sed 's/(//g' | sed 's/)//g' | \
			awk -v num="$number" '{$1 = $1 - num; print $1, $6, $8, $10, $12, $14, $16, $18, $20, $22, $24, $30, $26+$28-$30}' >> ${OUTPUTDIR}/gc_lat_per_sec
	
	echo -e "#time\tutil\tdo_gc_cnt\tfsgc_lat\tdogc_lat\tcalc_dogc_lat\tcp\tmeta\tfilemap\tpcache\tread\twrite\tothers\t" > ${OUTPUTDIR}/gc_lat_per_sec_one_gc
	cat ${OUTPUTDIR}/gc_per_sec_only | sed 's/\]//g' | sed 's/\[//g' | sed 's/(//g' | sed 's/)//g' | \
			awk -v num="$number" '{$1 = $1 - num; print $1, $6, $8, $10, $12, $14, $16, $18, $20, $22, $24, $30, $26+$28-$30}' | awk '{if ($3==1) print $0}' >> ${OUTPUTDIR}/gc_lat_per_sec_one_gc
	
	echo -e "#time\tdo_gc_cnt\tdo_write_page_lat\t_order_lock\t_aloc_block\t_inv_cache\t_submit_pg\t__io_rwsem\t__zone_wait\t__other1\t__submit_previo\t__bio_alloc\t__bio_add\t__submit_write_by_bio_add\t___finish_plug\t___submit_bio\t__cur_zone_write\t__end_write\tupdate_map\t" > ${OUTPUTDIR}/write_detail_lat_per_sec
	cat ${OUTPUTDIR}/dmesg | grep outplace_detail_gc_profiler | sed 's/\]//g' | sed 's/\[//g' | sed 's/(//g' | sed 's/)//g' | \
			awk -v num="$number" '{$1 = $1 - num; print $1, $4, $6, $8, $10, $12, $14, $16, $18, $20, $22, $24, $26, $28, $30, $32, $34, $36, $38}' >> ${OUTPUTDIR}/write_detail_lat_per_sec
	
	echo -e "#time\tdo_gc_cnt\tdo_write_page_lat\t_order_lock\t_aloc_block\t_inv_cache\t_submit_pg\t__io_rwsem\t__zone_wait\t__other1\t__submit_previo\t__bio_alloc\t__bio_add\t__submit_write_by_bio_add\t___finish_plug\t___submit_bio\t__cur_zone_write\t__end_write\tupdate_map\t" > ${OUTPUTDIR}/write_detail_lat_per_sec_one_gc
	cat ${OUTPUTDIR}/dmesg | grep outplace_detail_gc_profiler | sed 's/\]//g' | sed 's/\[//g' | sed 's/(//g' | sed 's/)//g' | \
			awk -v num="$number" '{$1 = $1 - num; print $1, $4, $6, $8, $10, $12, $14, $16, $18, $20, $22, $24, $26, $28, $30, $32, $34, $36, $38}' | awk '{if ($2==1) print $0}' >> ${OUTPUTDIR}/write_detail_lat_per_sec_one_gc
	
	echo -e "#time\tdo_gc_cnt\tsubmit_bio\t_split\t_get_new_reqs\t__merge\t__throttle\t__alloc_requests\t___get_tag\t____io_schedule\t_mq_sched_insert\t" > ${OUTPUTDIR}/submit_bio_detail_lat_per_sec
	cat ${OUTPUTDIR}/dmesg | grep io_profiler | sed 's/\]//g' | sed 's/\[//g' | sed 's/(//g' | sed 's/)//g' | \
			awk -v num="$number" '{$1 = $1 - num; print $1, $4, $6, $8, $10, $12, $14, $16, $18, $20, $22}' >> ${OUTPUTDIR}/submit_bio_detail_lat_per_sec
	
	echo -e "#time\tdo_gc_cnt\tsubmit_bio\t_split\t_get_new_reqs\t__merge\t__throttle\t__alloc_requests\t___get_tag\t____io_schedule\t_mq_sched_insert\t" > ${OUTPUTDIR}/submit_bio_detail_lat_per_sec_one_gc
	cat ${OUTPUTDIR}/dmesg | grep io_profiler | sed 's/\]//g' | sed 's/\[//g' | sed 's/(//g' | sed 's/)//g' | \
			awk -v num="$number" '{$1 = $1 - num; print $1, $4, $6, $8, $10, $12, $14, $16, $18, $20, $22}' | awk '{if ($2==1) print $0}'  >> ${OUTPUTDIR}/submit_bio_detail_lat_per_sec_one_gc

	cat ${OUTPUTDIR}/dmesg_aft_umount | grep print_intra_seg | sed 's/\]//g' | sed 's/\[//g' > ${OUTPUTDIR}/intra_seg_read_lat
	cat ${OUTPUTDIR}/dmesg_aft_umount | grep print_intra_zone | sed 's/\]//g' | sed 's/\[//g' > ${OUTPUTDIR}/intra_zone_info


	cp ../general_resource/plot/*.gpi ${OUTPUTDIR}/
	cp workloada ${OUTPUTDIR}/
	wait $PID_DF_LOG;

	chown -R ${USER} ${OUTPUTDIR}



	echo "==== End the experiment ===="
}
main
