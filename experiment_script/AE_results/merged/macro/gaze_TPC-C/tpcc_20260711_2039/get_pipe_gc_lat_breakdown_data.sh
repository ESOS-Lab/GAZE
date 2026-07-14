#!/bin/bash

#./parse.sh p3_prerd: preproc_rd: check_cache1: get_free_pg: zerofill: add_cache: pg_others1: submit_rd: > gc_lat_read_phase
#./parse_pipe_lat.sh p4_mig: preproc_wrt: check_cache2: wait_rd: fmap_wrt: alloc_blk: submit_wrt: update_index: > gc_lat_write_phase 

#./parse_pipe_lat.sh p3_s1: p3_s2: check_cache1: get_free_pg: zerofill: add_cache: pg_others1: submit_rd:  preproc_rd: check_cache1: get_free_pg: zerofill: add_cache: pg_others1: submit_rd: > gc_lat_read_phase

./parse_pipe_lat.sh p3_s1: p3_wrt_buf: p3_s2: p3_rd_buf: check_cache1: get_free_pg: zerofill: add_cache: pg_others1: submit_rd: wake_writer: p4_s1: preproc_wrt: check_cache2: wait_rd: fmap_wrt: p4_wrt_buf_n_wake: p4_s2: p4_rd_buf: alloc_blk: submit_wrt: update_index: do_gc_lat: > gc_pipe_total

./parse_pipe_lat.sh empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty:  do_gc_lat: > gc_pipe_gc_total_only

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

./parse_pipe_lat.sh p3_s1: p3_wrt_buf: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: > gc_lat_preread 
#./parse_pipe_lat.sh p3_s1: p3_wrt_buf: > gc_lat_preread 
./parse_pipe_lat.sh empty: empty: p3_s2: p3_rd_buf: check_cache1: get_free_pg: zerofill: add_cache: pg_others1: submit_rd: wake_writer: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: > gc_lat_read
#./parse_pipe_lat.sh empty: empty: p3_s2: p3_rd_buf: check_cache1: get_free_pg: zerofill: add_cache: pg_others1: submit_rd: wake_writer: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: > gc_lat_read

./parse_pipe_lat.sh empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: p4_s1: preproc_wrt: check_cache2: wait_rd: fmap_wrt: p4_wrt_buf_n_wake: empty: empty: empty: empty: empty: empty: > gc_lat_prewrite
#./parse_pipe_lat.sh empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: p4_s1: preproc_wrt: check_cache2: wait_rd: fmap_wrt: p4_wrt_buf_n_wake: empty: empty: empty: empty: empty: > gc_lat_prewrite
./parse_pipe_lat.sh empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: p4_s2: p4_rd_buf: alloc_blk: submit_wrt: update_index: empty: > gc_lat_write
#./parse_pipe_lat.sh empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: empty: p4_s2: p4_rd_buf: alloc_blk: submit_wrt: update_index: > gc_lat_write


#./parse_pipe_lat.sh p3_prerd: preproc_rd: check_cache1: get_free_pg: zerofill: add_cache: pg_others1: submit_rd: > gc_lat_read_phase_only
#./parse_pipe_lat.sh preproc_rd: > gc_lat_preread_only
#./parse_pipe_lat.sh check_cache1: get_free_pg: zerofill: add_cache: pg_others1: submit_rd: > gc_lat_read_only

./parse_pipe_lat.sh p4_mig: preproc_wrt: check_cache2: wait_rd: pg_others2: bef_write: fmap_wrt: pg_wb: alloc_blk: submit_wrt: update_index: > gc_lat_write_phase_only
./parse_pipe_lat.sh p4_mig: preproc_wrt: check_cache2: wait_rd: pg_others2: bef_write: fmap_wrt: pg_wb: > gc_lat_prewrite_only
./parse_pipe_lat.sh p4_mig: alloc_blk: submit_wrt: update_index: > gc_lat_write_only


