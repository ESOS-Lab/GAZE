#!/bin/bash
# Refreshes the AE result comparison graphs from whatever benchmark modes
# have been run so far. Safe to call any time; called automatically at the
# end of Fileserver/run_fileserver.sh, YCSB-A/run_ycsba.sh, YCSB-F/run_ycsbf.sh
# and TPC-C/run_tpcc.sh.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

python3 parse_results.py || exit 1

if [[ ! -f throughput.tsv ]]; then
    exit 0
fi

N=$(head -1 throughput.tsv | awk -F'\t' '{print NF}')
for gpi in plot_throughput.gpi plot_avg_lat.gpi plot_tail_lat.gpi; do
    gnuplot -e "N=$N" "$gpi"
done

echo "[update_graph] graphs in $SCRIPT_DIR: throughput.png avg_lat.png tail_lat.png"
