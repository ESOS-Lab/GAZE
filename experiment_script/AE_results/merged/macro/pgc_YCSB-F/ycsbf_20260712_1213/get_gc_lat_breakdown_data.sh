#!/bin/bash



./parse_lat.sh p3_prerd: preproc_rd: check_cache1: get_free_pg: zerofill: add_cache: pg_others1: submit_rd: > gc_lat_read_phase_only
./parse_lat.sh preproc_rd: > gc_lat_preread_only
./parse_lat.sh check_cache1: get_free_pg: zerofill: add_cache: pg_others1: submit_rd: > gc_lat_read_only

# JW include others overhead in prewrite. 
./parse_lat.sh p4_mig: preproc_wrt: check_cache2: wait_rd: pg_others2: bef_write: dirty_page: dirty_postproc: write: fmap_wrt: pg_wb: alloc_blk: inv_cache: submit_wrt: update_index: wrt_others: > gc_lat_write_phase_only 
./parse_lat.sh p4_mig: preproc_wrt: check_cache2: wait_rd: pg_others2: bef_write: fmap_wrt: pg_wb: > gc_lat_prewrite_only
./parse_lat.sh p4_mig: alloc_blk: submit_wrt: update_index: > gc_lat_write_only
#./parse_lat.sh p4_mig: preproc_wrt: check_cache2: wait_rd: fmap_wrt: alloc_blk: submit_wrt: update_index: > gc_lat_write_phase 
#./parse_lat.sh p4_mig: preproc_wrt: check_cache2: wait_rd: fmap_wrt: > gc_lat_prewrite
#./parse_lat.sh p4_mig: alloc_blk: submit_wrt: update_index: > gc_lat_write
