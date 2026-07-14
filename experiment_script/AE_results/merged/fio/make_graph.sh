#!/bin/bash

# KIOPS
GAZE_KIOPS=$(find ./gaze_data/ -name "kiops_sum")
SCALELFS_KIOPS=$(find ./scalelfs_data/ -name "kiops_sum")
ZNS_KIOPS=$(find ./zns_data/ -name "kiops_sum")
IPLFS_KIOPS=$(find ./iplfs_data/ -name "kiops_sum")
PGC_KIOPS=$(find ./pgc_data/ -name "kiops_sum")
COPY_KIOPS=$(find ./copy_data/ -name "kiops_sum")

python3 moving_avg.py "$GAZE_KIOPS" > gaze_kiops_parsed
python3 moving_avg.py "$SCALELFS_KIOPS" > scalelfs_kiops_parsed
python3 moving_avg.py "$ZNS_KIOPS" > zns_kiops_parsed
python3 moving_avg.py "$IPLFS_KIOPS" > iplfs_kiops_parsed
python3 moving_avg.py "$PGC_KIOPS" > pgc_kiops_parsed
python3 moving_avg.py "$COPY_KIOPS" > copy_kiops_parsed


# Latency
GAZE_RESULT=$(find ./gaze_data/ -name "result_4k.txt")
SCALELFS_RESULT=$(find ./scalelfs_data/ -name "result_4k.txt")
ZNS_RESULT=$(find ./zns_data/ -name "result_4k.txt")
IPLFS_RESULT=$(find ./iplfs_data/ -name "result_4k.txt")
PGC_RESULT=$(find ./pgc_data/ -name "result_4k.txt")
COPY_RESULT=$(find ./copy_data/ -name "result_4k.txt")


# Average Latency
SCALELFS_AVG_LAT=$(cat "$SCALELFS_RESULT" | grep avg | grep clat | sed 's/=/ /g' | sed 's/,//g' | awk '{print $8}')
[ -z "$SCALELFS_AVG_LAT" ] && SCALELFS_AVG_LAT=0
GAZE_AVG_LAT=$(cat "$GAZE_RESULT" | grep avg | grep clat | sed 's/=/ /g' | sed 's/,//g' | awk '{print $8}')
[ -z "$GAZE_AVG_LAT" ] && GAZE_AVG_LAT=0
ZNS_AVG_LAT=$(cat "$ZNS_RESULT" | grep avg | grep clat | sed 's/=/ /g' | sed 's/,//g' | awk '{print $8}')
[ -z "$ZNS_AVG_LAT" ] && ZNS_AVG_LAT=0
IPLFS_AVG_LAT=$(cat "$IPLFS_RESULT" | grep avg | grep clat | sed 's/=/ /g' | sed 's/,//g' | awk '{print $8}')
[ -z "$IPLFS_AVG_LAT" ] && IPLFS_AVG_LAT=0
PGC_AVG_LAT=$(cat "$PGC_RESULT" | grep avg | grep clat | sed 's/=/ /g' | sed 's/,//g' | awk '{print $8}')
[ -z "$PGC_AVG_LAT" ] && PGC_AVG_LAT=0
COPY_AVG_LAT=$(cat "$COPY_RESULT" | grep avg | grep clat | sed 's/=/ /g' | sed 's/,//g' | awk '{print $8}')
[ -z "$COPY_AVG_LAT" ] && COPY_AVG_LAT=0

echo -e "steps\tscaleLFS\tGAZE\tzF2FS\tiplfs\tpgc\tcopy\navg\t${SCALELFS_AVG_LAT}\t${GAZE_AVG_LAT}\t${ZNS_AVG_LAT}\t${IPLFS_AVG_LAT}\t${PGC_AVG_LAT}\t${COPY_AVG_LAT}" > avg_lat_data

# 99.9999th Tail Latency
SCALELFS_TAIL_LAT=$(cat "$SCALELFS_RESULT" | grep 99.999900th | sed 's/\[/ /g' | sed 's/\]/ /g' | awk '{print $6}')
[ -z "$SCALELFS_TAIL_LAT" ] && SCALELFS_TAIL_LAT=0
GAZE_TAIL_LAT=$(cat "$GAZE_RESULT" | grep 99.999900th | sed 's/\[/ /g' | sed 's/\]/ /g' | awk '{print $6}')
[ -z "$GAZE_TAIL_LAT" ] && GAZE_TAIL_LAT=0
ZNS_TAIL_LAT=$(cat "$ZNS_RESULT" | grep 99.999900th | sed 's/\[/ /g' | sed 's/\]/ /g' | awk '{print $6}') # multiply by 1000 to make the unit equal with GAZE's and IPLFS's
[ -z "$ZNS_TAIL_LAT" ] && ZNS_TAIL_LAT=0
IPLFS_TAIL_LAT=$(cat "$IPLFS_RESULT" | grep 99.999900th | sed 's/\[/ /g' | sed 's/\]/ /g' | awk '{print $6}') # multiply by 1000 to make the unit equal with GAZE's and IPLFS's
[ -z "$IPLFS_TAIL_LAT" ] && IPLFS_TAIL_LAT=0
PGC_TAIL_LAT=$(cat "$PGC_RESULT" | grep 99.999900th | sed 's/\[/ /g' | sed 's/\]/ /g' | awk '{print $6}') # multiply by 1000 to make the unit equal with GAZE's and IPLFS's
[ -z "$PGC_TAIL_LAT" ] && PGC_TAIL_LAT=0
COPY_TAIL_LAT=$(cat "$COPY_RESULT" | grep 99.999900th | sed 's/\[/ /g' | sed 's/\]/ /g' | awk '{print $6}') # multiply by 1000 to make the unit equal with GAZE's and IPLFS's
[ -z "$COPY_TAIL_LAT" ] && COPY_TAIL_LAT=0
echo -e "steps\tscaleLFS\tGAZE\tzF2FS\tiplfs\tpgc\tcopy\n99.9999\t${SCALELFS_TAIL_LAT}\t${GAZE_TAIL_LAT}\t${ZNS_TAIL_LAT}\t${IPLFS_TAIL_LAT}\t${PGC_TAIL_LAT}\t${COPY_TAIL_LAT}" > 99.9999_lat_data


# Plot graph
# PAPER_ENV (0/1) from build_graphs.sh — .gpi templates use it to keep the
# paper's hardcoded y-range when 1, autoscale when 0. Default 1 so
# standalone runs of this script still reproduce the paper's figures.
: "${PAPER_ENV:=1}"
gnuplot -e "PAPER_ENV=$PAPER_ENV" plot_Fig-17.FIO_throughput.gpi
gnuplot -e "PAPER_ENV=$PAPER_ENV" plot_Supp-b.FIO_99.9999_latency.gpi
gnuplot -e "PAPER_ENV=$PAPER_ENV" plot_Supp-a.FIO_avg_latency.gpi
