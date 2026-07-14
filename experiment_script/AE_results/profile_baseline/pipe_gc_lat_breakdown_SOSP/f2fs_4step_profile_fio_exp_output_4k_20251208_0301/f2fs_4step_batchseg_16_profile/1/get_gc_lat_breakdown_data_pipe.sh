#!/bin/bash


FS_NAME="PIPE"
PIPE=0

./parse_pipe.sh do_gc_lat: > do_gc_lat_only
./parse_pipe.sh p3_prerd: preproc_rd: check_cache1: get_free_pg: zerofill: add_cache: pg_others1: submit_rd: > gc_lat_read_phase_only
./parse_pipe.sh preproc_rd: > gc_lat_preread_only
./parse_pipe.sh check_cache1: get_free_pg: zerofill: add_cache: pg_others1: submit_rd: > gc_lat_read_only

./parse_pipe.sh p4_mig: preproc_wrt: check_cache2: wait_rd: pg_others2: bef_write: write: fmap_wrt: pg_wb: alloc_blk: inv_cache: submit_wrt: update_index: wrt_others: > gc_lat_write_phase_only
./parse_pipe.sh p4_mig: preproc_wrt: check_cache2: wait_rd: pg_others2: bef_write: fmap_wrt: pg_wb: > gc_lat_prewrite_only
./parse_pipe.sh p4_mig: alloc_blk: submit_wrt: update_index: > gc_lat_write_only


CP=$(./parse_pipe.sh cp: | awk 'NR==2 { sum=0; for(i=1; i<=NF; i++) sum+=$i; printf "%.6f", sum }')
P0_1_2=$(./parse_pipe.sh p0_1_2: | awk 'NR==2 { sum=0; for(i=1; i<=NF; i++) sum+=$i; printf "%.6f", sum }')

PREREAD_SUM=$(./parse_pipe.sh p3_s1: | awk 'NR==2 { sum=0; for(i=1; i<=NF; i++) sum+=$i; printf "%.6f", sum }')
READ_SUM=$(./parse_pipe.sh p3_s2: | awk 'NR==2 { sum=0; for(i=1; i<=NF; i++) sum+=$i; printf "%.6f", sum }')

PREWRITE_SUM=$(./parse_pipe.sh p4_s1: | awk 'NR==2 { sum=0; for(i=1; i<=NF; i++) sum+=$i; printf "%.6f", sum }')
WRITE_SUM=$(./parse_pipe.sh p4_s2: | awk 'NR==2 { sum=0; for(i=1; i<=NF; i++) sum+=$i; printf "%.6f", sum }')


echo "\"$FS_NAME\" $CP $P0_1_2 $PREREAD_SUM $READ_SUM $PREWRITE_SUM $WRITE_SUM " > four_phase_lat_data
