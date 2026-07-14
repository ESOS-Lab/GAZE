#!/bin/bash

# First Column should be [ timestamp]. First line should be first ssd_advance_nand_chip_util NAND_READ. 

kern_log=$1

start_t=$(head -n 1 $kern_log | sed 's/\[//g' | sed 's/\]//g' | awk '{print $9}')
#start_t=$(head -n 1 kern.log | sed 's/\[//g' | sed 's/\]//g' | awk '{print $9}')

linecnt=21388775

echo $start_t


cat $kern_log | grep NAND_WRITE | head -n $linecnt | sed 's/\]//g' | sed 's/\[//g'  | awk -v start_t="$start_t" '{chipno = $5+$7*8; arrival_time = $9 - start_t; buf_avail_time = $11 - start_t; buf_written_t = $13 - start_t; chip_op_stime = $15 - start_t; chip_op_etime = $17 - start_t; print arrival_time, chipno, "\n"buf_avail_time, chipno"\n"}' > new/buf_wait_time_data_WRITE

cat $kern_log | grep NAND_WRITE | head -n $linecnt | sed 's/\]//g' | sed 's/\[//g'  | awk -v start_t="$start_t" '{chipno = $5+$7*8; arrival_time = $9 - start_t; buf_avail_time = $11 - start_t; buf_written_t = $13 - start_t; chip_op_stime = $15 - start_t; chip_op_etime = $17 - start_t; print buf_avail_time, chipno, "\n"buf_written_t, chipno"\n"}' > new/buf_write_time_data_WRITE

cat $kern_log | grep NAND_WRITE | head -n $linecnt | sed 's/\]//g' | sed 's/\[//g'  | awk -v start_t="$start_t" '{chipno = $5+$7*8; arrival_time = $9 - start_t; buf_avail_time = $11 - start_t; buf_written_t = $13 - start_t; chip_op_stime = $15 - start_t; chip_op_etime = $17 - start_t; print buf_written_t, chipno, "\n"chip_op_stime, chipno"\n"}' > new/channel_xfer_time_data_WRITE

cat $kern_log | grep NAND_WRITE | head -n $linecnt | sed 's/\]//g' | sed 's/\[//g'  | awk -v start_t="$start_t" '{chipno = $5+$7*8; arrival_time = $9 - start_t; buf_avail_time = $11 - start_t; buf_written_t = $13 - start_t; chip_op_stime = $15 - start_t; chip_op_etime = $17 - start_t; print chip_op_stime, chipno, "\n"chip_op_etime, chipno"\n"}' > new/chip_write_time_data_WRITE



cat $kern_log | grep NAND_READ | head -n $linecnt | sed 's/\]//g' | sed 's/\[//g'  | awk -v start_t="$start_t" '{chipno = $5+$7*8; arrival_time = $9 - start_t; fw_rd_complete_time = $11 - start_t; chip_op_stime = $13 - start_t; chip_op_etime = $15 - start_t; print arrival_time, chipno, "\n"fw_rd_complete_time, chipno"\n"}' > new/firmware_time_data_READ

cat $kern_log | grep NAND_READ | head -n $linecnt | sed 's/\]//g' | sed 's/\[//g'  | awk -v start_t="$start_t" '{chipno = $5+$7*8; arrival_time = $9 - start_t; fw_rd_complete_time = $11 - start_t; chip_op_stime = $13 - start_t; chip_op_etime = $15 - start_t; print fw_rd_complete_time, chipno, "\n"chip_op_stime, chipno"\n"}' > new/channel_xfer_time_data_READ

cat $kern_log | grep NAND_READ | head -n $linecnt | sed 's/\]//g' | sed 's/\[//g'  | awk -v start_t="$start_t" '{chipno = $5+$7*8; arrival_time = $9 - start_t; fw_rd_complete_time = $11 - start_t; chip_op_stime = $13 - start_t; chip_op_etime = $15 - start_t; print chip_op_stime, chipno, "\n"chip_op_etime, chipno"\n"}' > new/chip_read_time_data_READ

cp plot_chip_util.gpi new/
