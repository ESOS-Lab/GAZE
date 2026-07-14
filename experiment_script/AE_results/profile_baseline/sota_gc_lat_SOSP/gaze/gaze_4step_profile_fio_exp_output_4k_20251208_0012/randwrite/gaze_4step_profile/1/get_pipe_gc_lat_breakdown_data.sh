#!/bin/bash


FS_NAME="GAZE"


#./parse.sh p3_prerd: preproc_rd: check_cache1: get_free_pg: zerofill: add_cache: pg_others1: submit_rd: > gc_lat_read_phase
#./parse_pipe.sh p4_mig: preproc_wrt: check_cache2: wait_rd: fmap_wrt: alloc_blk: submit_wrt: update_index: > gc_lat_write_phase 

#./parse_pipe.sh p3_s1: p3_s2: check_cache1: get_free_pg: zerofill: add_cache: pg_others1: submit_rd:  preproc_rd: check_cache1: get_free_pg: zerofill: add_cache: pg_others1: submit_rd: > gc_lat_read_phase

./parse_pipe.sh p3_s1: p3_wrt_buf: p3_s2: p3_rd_buf: check_cache1: get_free_pg: zerofill: add_cache: pg_others1: submit_rd: wake_writer: p4_s1: preproc_wrt: check_cache2: wait_rd: fmap_wrt: p4_wrt_buf_n_wake: p4_s2: p4_rd_buf: alloc_blk: submit_wrt: update_index: > gc_pipe_total

./parse_pipe.sh empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty:  do_gc_lat: > gc_pipe_gc_total_only

#22 in total
#
#2
#"2"->20
#
#9
#2->"9"->11
#
#6
#11->"6"->5
#
#5
#17->"5"

./parse_pipe.sh p3_s1: p3_wrt_buf: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: > gc_lat_preread 
#./parse_pipe.sh p3_s1: p3_wrt_buf: > gc_lat_preread 
./parse_pipe.sh empty: empty: p3_s2: p3_rd_buf: check_cache1: get_free_pg: zerofill: add_cache: pg_others1: submit_rd: wake_writer: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: > gc_lat_read
#./parse_pipe.sh empty: empty: p3_s2: p3_rd_buf: check_cache1: get_free_pg: zerofill: add_cache: pg_others1: submit_rd: wake_writer: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: > gc_lat_read

./parse_pipe.sh empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: p4_s1: preproc_wrt: check_cache2: wait_rd: fmap_wrt: p4_wrt_buf_n_wake: empty: empty: empty: empty: empty: > gc_lat_prewrite
#./parse_pipe.sh empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: p4_s1: preproc_wrt: check_cache2: wait_rd: fmap_wrt: p4_wrt_buf_n_wake: empty: empty: empty: empty: empty: > gc_lat_prewrite
./parse_pipe.sh empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: p4_s2: p4_rd_buf: alloc_blk: submit_wrt: update_index: > gc_lat_write
#./parse_pipe.sh empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: p4_s2: p4_rd_buf: alloc_blk: submit_wrt: update_index: > gc_lat_write






CP=$(./parse_pipe.sh cp: | awk 'NR==2 { sum=0; for(i=1; i<=NF; i++) sum+=$i; printf "%.6f", sum }')
P0_1_2=$(./parse_pipe.sh p0_1_2: | awk 'NR==2 { sum=0; for(i=1; i<=NF; i++) sum+=$i; printf "%.6f", sum }')
DO_GC_LAT=$(./parse_pipe.sh do_gc_lat: | awk 'NR==2 { sum=0; for(i=1; i<=NF; i++) sum+=$i; printf "%.6f", sum }')
PIPE=$(echo "scale=2; $DO_GC_LAT - $P0_1_2" | bc -l)

PREREAD_SUM=0
PREREAD_LOCK=0
P3=0

READ_SUM=0
PREREAD_EXCEPT_LOCK=0

PREWRITE_SUM=0
P4=0
WRITE_SUM=0

P3_P4_SUM=0
COPY=0
PRECOPY_SUM=0
POST_CP=0


echo "\"$FS_NAME\" $CP $P0_1_2 $PREREAD_EXCEPT_LOCK $PREREAD_LOCK $READ_SUM $PREWRITE_SUM $WRITE_SUM $PIPE $PRECOPY_SUM $COPY $POST_CP" > four_phase_lat_data
