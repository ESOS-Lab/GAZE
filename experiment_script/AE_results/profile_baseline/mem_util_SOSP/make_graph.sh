#!/bin/bash
# make_graph.sh -- regenerate the paper's memory-utilization figures
#   mem_utilization_f2fs.eps    ("Memory Utilization and FIO Throughput in F2FS")
#   mem_utilization_gaze.eps  ("Memory Utilization ... in Selective Blocking", GAZE)
# (the paper's *_rev.eps files are renamed copies of these outputs)
#
# Usage:
#   ./make_graph.sh                                   # reference (paper) data
#   ./make_graph.sh --f2fs RUN_DIR --gaze RUN_DIR     # fresh profile runs
#   RAM_GB=64 SOFT_PCT=10 HARD_PCT=20 ./make_graph.sh ...   # optional knobs
#
# A RUN_DIR is a FIO run output directory (experiment_script/FIO/<fs>_data/
# fio_<ts>/) from a run on the profile kernel (switch_mode.sh <mode> --profile
# builds APP_WRITE_PROFILE in, which emits the dirty-page samples). The run
# dir must contain kiops_sum plus a kernel log: raw_kern_log is preferred,
# falling back to dmesg (the dmesg ring buffer may have wrapped on long runs
# -- prefer raw_kern_log for full coverage).
#
# With no arguments the committed paper data are used:
#   dirty-page logs : reference/dirty_page/{f2fs,gaze}_log
#   throughput      : reference/{f2fs,gaze}_kiops_sum
#
# The GC-onset marker ("GC starts.") is taken from the first GC-profiler
# line (print_cur_gc_profiler / OSDI26_analysis / pipe_OSDI26 /
# paper_analysis) in the same log; it is omitted if none is found.

set -eu
cd "$(dirname "${BASH_SOURCE[0]}")"

RAM_GB="${RAM_GB:-64}"
SOFT_PCT="${SOFT_PCT:-10}"
HARD_PCT="${HARD_PCT:-20}"

REF_DIR="reference/dirty_page"
F2FS_RUN=""
GAZE_RUN=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --f2fs) F2FS_RUN="$2"; shift 2 ;;
        --gaze) GAZE_RUN="$2"; shift 2 ;;
        *) echo "usage: $0 [--f2fs RUN_DIR] [--gaze RUN_DIR]" >&2; exit 1 ;;
    esac
done

# pick_log RUN_DIR -> echoes the kernel log to parse
pick_log() {
    local d="$1"
    if   [[ -f "$d/raw_kern_log" ]]; then echo "$d/raw_kern_log"
    elif [[ -f "$d/dmesg" ]];        then echo "$d/dmesg"
    else echo ""; fi
}

# build_one NAME LOG KIOPS_FILE -> parses + plots mem_utilization_<NAME>.eps
build_one() {
    local name="$1" log="$2" kiops="$3"
    [[ -f "$log" ]]   || { echo "[make_graph] ERROR: log not found: $log" >&2; exit 1; }
    [[ -f "$kiops" ]] || { echo "[make_graph] ERROR: kiops_sum not found: $kiops" >&2; exit 1; }

    ./parse_dirty_pg.sh "$log" > "${name}_parsed.dat"
    # 20 sec moving average (10 samples x 2 s), same as the paper's
    # throughput smoothing (seungwon_support/gnuplot/parsing/moving_avg.py)
    awk '{v[NR]=$2; s+=$2; if (NR>10) s-=v[NR-10]; n=(NR<10?NR:10);
          printf "%.3f %.3f\n", $1, s/n}' "$kiops" > "${name}_kiops_avg.dat"
    kiops="${name}_kiops_avg.dat"
    [[ -s "${name}_parsed.dat" ]] || {
        echo "[make_graph] ERROR: no dirty-page samples in $log" >&2
        echo "[make_graph]   (was this run made on a --profile kernel? APP_WRITE_PROFILE" >&2
        echo "[make_graph]    emits the jw_balance_dirty_pages... lines)" >&2
        exit 1
    }

    local t0 gc_raw gcstart
    t0=$(awk 'NR==1{print $1}' "${name}_parsed.dat")
    gc_raw=$(sed -E 's/^.* kernel: //' "$log" |
        sed -nE 's/^\[ *([0-9.]+)\] .*(print_cur_gc_profiler|OSDI26_analysis|pipe_OSDI26|paper_analysis).*/\1/p' |
        head -n 1)
    if [[ -n "$gc_raw" ]]; then
        gcstart=$(echo "$gc_raw - $t0" | bc -l)
        echo "[make_graph] $name: t0=$t0 GC starts at +${gcstart}s"
    else
        gcstart=-1
        echo "[make_graph] $name: t0=$t0, no GC-profiler line found -- omitting 'GC starts.' marker"
    fi

    gnuplot -e "DAT='${name}_parsed.dat'; KIOPS='$kiops'; OUT='mem_utilization_${name}.eps'; \
                T0=$t0; GCSTART=$gcstart; RAM_GB=$RAM_GB; SOFTPCT=$SOFT_PCT; HARDPCT=$HARD_PCT" \
            plot_mem_util.gpi
    echo "[make_graph] -> mem_utilization_${name}.eps"
}

if [[ -n "$F2FS_RUN" ]]; then
    build_one f2fs "$(pick_log "$F2FS_RUN")" "$F2FS_RUN/kiops_sum"
else
    build_one f2fs "$REF_DIR/f2fs_log" reference/f2fs_kiops_sum
fi

if [[ -n "$GAZE_RUN" ]]; then
    build_one gaze "$(pick_log "$GAZE_RUN")" "$GAZE_RUN/kiops_sum"
else
    build_one gaze "$REF_DIR/gaze_log" reference/gaze_kiops_sum
fi

# Paper-numbered aliases (paper Fig 6: "Memory Utilization and FIO
# Throughput in F2FS"; paper Fig 13: same, in Selective Blocking / GAZE).
BASE_DIR="$PWD"   # the script cd'd into its own directory at the top
for pair in "f2fs:Fig-06.Memory_util_and_FIO_throughput.F2FS" \
            "gaze:Fig-13.Memory_util_and_FIO_throughput.GAZE"; do
    src="mem_utilization_${pair%%:*}"; dst="${pair#*:}"
    [ -f "$BASE_DIR/$src.eps" ] && cp -f "$BASE_DIR/$src.eps" "$BASE_DIR/$dst.eps"
    if command -v gs >/dev/null 2>&1 && [ -f "$BASE_DIR/$dst.eps" ]; then
        gs -dQUIET -dBATCH -dNOPAUSE -dEPSCrop -r150 -sDEVICE=png16m \
           -sOutputFile="$BASE_DIR/$dst.png" "$BASE_DIR/$dst.eps" >/dev/null 2>&1 || true
    fi
done

echo "[make_graph] Done."
