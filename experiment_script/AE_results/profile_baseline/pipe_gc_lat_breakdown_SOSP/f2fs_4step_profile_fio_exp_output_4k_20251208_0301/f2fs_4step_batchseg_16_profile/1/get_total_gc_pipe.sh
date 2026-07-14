#!/bin/bash


./parse_pipe.sh cp: do_gc_lat: > cp_and_do_gc_lat_only


GC_TOTAL=$(./parse_pipe.sh cp: do_gc_lat: | awk 'NR==2 { sum=0; for(i=1; i<=NF; i++) sum+=$i; printf "%.6f", sum }')

echo "$GC_TOTAL" > total_gc_lat_data
