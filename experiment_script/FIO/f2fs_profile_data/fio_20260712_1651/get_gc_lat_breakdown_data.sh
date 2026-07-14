#!/bin/bash
# get_gc_lat_breakdown_data.sh -- per-phase GC latency averages for the paper's
# "GC Latency Breakdown across Different Filesystems" figure (serial-GC modes:
# zF2FS / P-GC; ZNS+ has get_copy_gc_lat_breakdown_data.sh, pipelined modes
# have get_pipe_gc_lat_breakdown_data.sh).
#
# Run inside a profile-mode benchmark output dir (needs ./dmesg with
# OSDI26_analysis lines + ./parse_lat.sh, both placed here by lib.sh).
# Emits ./four_phase_lat_data, one row consumed by
# graph/OSDI26/sota_gc_lat_SOSP/make_graph.sh.
#
# FS_NAME (the figure's x-axis label) is derived from the run path;
# override with:  GC_FS_NAME="..." ./get_gc_lat_breakdown_data.sh

if [[ -n "${GC_FS_NAME:-}" ]]; then
    FS_NAME="$GC_FS_NAME"
else
    case "$PWD" in
        *pgc*)             FS_NAME="P-GC" ;;
        *copy*|*zns_plus*) FS_NAME="ZNS+" ;;
        *f2fs*)            FS_NAME="zF2FS" ;;
        *)                 FS_NAME="FS" ;;
    esac
fi
PIPE=0

./parse_lat.sh do_gc_lat: > do_gc_lat_only
./parse_lat.sh p3_prerd: preproc_rd: check_cache1: get_free_pg: zerofill: add_cache: pg_others1: submit_rd: > gc_lat_read_phase_only
./parse_lat.sh preproc_rd: > gc_lat_preread_only
./parse_lat.sh check_cache1: get_free_pg: zerofill: add_cache: pg_others1: submit_rd: > gc_lat_read_only

./parse_lat.sh p4_mig: preproc_wrt: check_cache2: wait_rd: pg_others2: bef_write: write: fmap_wrt: pg_wb: alloc_blk: inv_cache: submit_wrt: update_index: wrt_others: > gc_lat_write_phase_only
./parse_lat.sh p4_mig: preproc_wrt: check_cache2: wait_rd: pg_others2: bef_write: fmap_wrt: pg_wb: > gc_lat_prewrite_only
./parse_lat.sh p4_mig: alloc_blk: submit_wrt: update_index: > gc_lat_write_only


CP=$(./parse_lat.sh cp: | awk 'NR==2 { sum=0; for(i=1; i<=NF; i++) sum+=$i; printf "%.6f", sum }')
P0_1_2=$(./parse_lat.sh p0_1_2: | awk 'NR==2 { sum=0; for(i=1; i<=NF; i++) sum+=$i; printf "%.6f", sum }')

PREREAD_SUM=$(./parse_lat.sh preproc_rd: | awk 'NR==2 { sum=0; for(i=1; i<=NF; i++) sum+=$i; printf "%.6f", sum }')
PREREAD_LOCK=$(./parse_lat.sh bef_rd: | awk 'NR==2 { sum=0; for(i=1; i<=NF; i++) sum+=$i; printf "%.6f", sum }')
P3=$(./parse_lat.sh p3_prerd: | awk 'NR==2 { sum=0; for(i=1; i<=NF; i++) sum+=$i; printf "%.6f", sum }')

READ_SUM=$(echo "scale=2; $P3 - $PREREAD_SUM" | bc -l)
PREREAD_EXCEPT_LOCK=$(echo "scale=2; $PREREAD_SUM - $PREREAD_LOCK" | bc -l)

PREWRITE_SUM=$(./parse_lat.sh preproc_wrt: check_cache2: wait_rd: pg_others2: bef_write: fmap_wrt: pg_wb: | awk 'NR==2 { sum=0; for(i=1; i<=NF; i++) sum+=$i; printf "%.6f", sum }')
P4=$(./parse_lat.sh p4_mig: | awk 'NR==2 { sum=0; for(i=1; i<=NF; i++) sum+=$i; printf "%.6f", sum }')
WRITE_SUM=$(echo "scale=2; $P4 - $PREWRITE_SUM" | bc -l)


P3_P4_SUM=0
COPY=0
PRECOPY_SUM=0
POST_CP=0


echo "\"$FS_NAME\" $CP $P0_1_2 $PREREAD_EXCEPT_LOCK $PREREAD_LOCK $READ_SUM $PREWRITE_SUM $WRITE_SUM $PIPE $PRECOPY_SUM $COPY $POST_CP" > four_phase_lat_data
