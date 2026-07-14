#!/bin/bash
# get_copy_gc_lat_breakdown_data.sh -- ZNS+ (copy-offload) variant of
# get_gc_lat_breakdown_data.sh for the paper's "GC Latency Breakdown across
# Different Filesystems" figure.
#
# Run inside a zns_plus profile-mode benchmark output dir (needs ./dmesg with
# OSDI26_analysis_copy lines + ./parse_copy_lat.sh, both placed here by
# lib.sh). Emits ./four_phase_lat_data, one row consumed by
# AE_results/profile_baseline/sota_gc_lat_SOSP/make_graph.sh.

FS_NAME="${GC_FS_NAME:-ZNS+}"
PIPE=0

./parse_copy_lat.sh p3_prerd: preproc_rd: check_cache1: get_free_pg: zerofill: add_cache: pg_others1: submit_rd: > gc_lat_read_phase_only
./parse_copy_lat.sh preproc_rd: > gc_lat_preread_only
./parse_copy_lat.sh check_cache1: get_free_pg: zerofill: add_cache: pg_others1: submit_rd: > gc_lat_read_only

./parse_copy_lat.sh p4_mig: preproc_wrt: check_cache2: wait_rd: pg_others2: bef_write: write: fmap_wrt: pg_wb: alloc_blk: inv_cache: submit_wrt: update_index: wrt_others: > gc_lat_write_phase_only
./parse_copy_lat.sh p4_mig: preproc_wrt: check_cache2: wait_rd: pg_others2: bef_write: fmap_wrt: pg_wb: > gc_lat_prewrite_only
./parse_copy_lat.sh p4_mig: alloc_blk: submit_wrt: update_index: > gc_lat_write_only

CP=0

P0_1_2=$(./parse_copy_lat.sh p0_1_2: | awk 'NR==2 { sum=0; for(i=1; i<=NF; i++) sum+=$i; printf "%.6f", sum }')

PREREAD_SUM=0
PREREAD_LOCK=0
P3=0

READ_SUM=0
PREREAD_EXCEPT_LOCK=0

PREWRITE_SUM=0
P4=0
WRITE_SUM=0

P3_P4_SUM=$(./parse_copy_lat.sh p3_prerd: p4_mig: | awk 'NR==2 { sum=0; for(i=1; i<=NF; i++) sum+=$i; printf "%.6f", sum }')
COPY=$(./parse_copy_lat.sh submit_copy: | awk 'NR==2 { sum=0; for(i=1; i<=NF; i++) sum+=$i; printf "%.6f", sum }')
PRECOPY_SUM=$(echo "scale=2; $P3_P4_SUM - $COPY" | bc -l)
POST_CP=$(./parse_copy_lat.sh post_copy_cp_lat: | awk 'NR==2 { sum=0; for(i=1; i<=NF; i++) sum+=$i; printf "%.6f", sum }')

echo "\"$FS_NAME\" $CP $P0_1_2 $PREREAD_EXCEPT_LOCK $PREREAD_LOCK $READ_SUM $PREWRITE_SUM $WRITE_SUM $PIPE $PRECOPY_SUM $COPY $POST_CP" > four_phase_lat_data
