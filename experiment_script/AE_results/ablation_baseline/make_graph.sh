#!/bin/bash
# make_graph.sh -- regenerate the paper's Fig 15 "FIO Throughput under
# Individual Techniques" (ablation study): per-technique-combination mean
# and stddev of FIO 4k randwrite KIOPS over the GC window, plus the legend.
#
# Eight technique combinations, one FS mode each:
#   Vanilla=f2fs  Pg=f2fs_pool  Pipe=f2fs_pipe  NB=f2fs_sb
#   Pipe+Pg=f2fs_pool_pipe  NB+Pg=f2fs_pool_sb  NB+Pipe=f2fs_sb_pipe
#   NB+Pipe+Pg=gaze     (NB = selective blocking, Pg = GC page pool)
# (The paper's ver3 figure draws 6 of the 8 bars; the data files carry all 8.)
#
# Each bar resolves to the newest valid (non-profile) FIO run of its mode
# under experiment_script/FIO/<mode>_data/, falling back to the paper's
# recorded value (committed result_4k_avg/stddev.dat, raw samples in
# parsed/). Provenance is printed and written to SOURCES.txt:
#   [NEW] = generated on this machine, [OLD] = SOSP26 paper data.
#
# Stats: mean / sample stddev over the GC window = the last GC_TAIL
# (default 345, the paper's (1200-500)/2-5) samples of the run's kiops_sum
# -- the same window kiops_avg_gc uses.
#
# Outputs (this directory): result_4k_avg.dat / result_4k_stddev.dat,
# real_plot/{overall_bar_avg_ver3,overall_bar_stddev_ver3,
# legend_only_hwanseok_ver3}.eps (+ .png), and the paper-numbered aliases
# Fig-15.FIO_throughput_under_individual_techniques.{avg,stddev,legend}.*

set -u
BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GAZE_DIR="$(cd "$BASE_DIR/../../.." && pwd)"
FIO_DATA="$GAZE_DIR/experiment_script/FIO"
GC_TAIL="${GC_TAIL:-345}"
cd "$BASE_DIR" || exit 1

log() { echo "[ablation_tput] $*"; }

# column order of the paper's data files (fields 2..9)
LABELS=("NB+Pipe+Pg" "NB+Pipe" "NB+Pg" "NB" "Pipe+Pg" "Pipe" "Pg" "Vanilla")
MODES=( gaze f2fs_sb_pipe f2fs_pool_sb f2fs_sb f2fs_pool_pipe f2fs_pipe f2fs_pool f2fs )

# paper values (the committed data shipped with the artifact)
read -r -a PAPER_AVG    <<< "$(awk '/^mean/   {for(i=2;i<=9;i++) printf "%s ", $i}' result_4k_avg.dat)"
read -r -a PAPER_STDDEV <<< "$(awk '/^stddev/ {for(i=2;i<=9;i++) printf "%s ", $i}' result_4k_stddev.dat)"

latest_run() {  # latest_run <mode_data_dir> -> newest fio_* run with kiops data
    local d
    for d in $(ls -1dt "$1"/fio_*/ 2>/dev/null); do
        [ -s "$d/kiops_sum" ] && { echo "${d%/}"; return 0; }
    done
    return 1
}

SRC="$BASE_DIR/SOURCES.txt"
{
    echo "# Data source per bar — $(date '+%Y-%m-%d %H:%M')"
    echo "# [NEW] = generated on this machine, [OLD] = SOSP26 paper data"
} > "$SRC"

AVG_ROW="mean"
STD_ROW="stddev"
for i in "${!MODES[@]}"; do
    mode="${MODES[$i]}"; label="${LABELS[$i]}"
    if run=$(latest_run "$FIO_DATA/${mode}_data"); then
        stats=$(tail -n "$GC_TAIL" "$run/kiops_sum" | awk '
            {n++; s+=$2; ss+=$2*$2}
            END{m=s/n; printf "%.6f %.6f", m, (n>1)? sqrt((ss-n*m*m)/(n-1)) : 0}')
        AVG_ROW="$AVG_ROW $(echo "$stats" | cut -d' ' -f1)"
        STD_ROW="$STD_ROW $(echo "$stats" | cut -d' ' -f2)"
        log "$label ($mode): fresh -> $run"
        echo "$label ($mode): [NEW] $run" >> "$SRC"
    else
        AVG_ROW="$AVG_ROW ${PAPER_AVG[$i]}"
        STD_ROW="$STD_ROW ${PAPER_STDDEV[$i]}"
        log "$label ($mode): archive (paper run) -> result_4k_avg.dat column $((i+2))"
        echo "$label ($mode): [OLD] $BASE_DIR/parsed/ (paper samples; committed result_4k_*.dat)" >> "$SRC"
    fi
done

HEADER="#sample  NB+Pipe+Pg     NB+Pipe        NB+Pg          NB             Pipe+Pg        Pipe           Pg             Vanilla"
{ echo "$HEADER"; echo "$AVG_ROW"; } > result_4k_avg.dat
{ echo "$HEADER"; echo "$STD_ROW"; } > result_4k_stddev.dat

( cd plot_file && gnuplot plot_avg.gp && gnuplot plot_stddev.gp && gnuplot plot_legend4.gp ) \
    || { echo "[ablation_tput] ERROR: gnuplot failed" >&2; exit 1; }

ALIAS="Fig-15.FIO_throughput_under_individual_techniques"
declare -A OUT=(
    [overall_bar_avg_ver3]="$ALIAS.avg"
    [overall_bar_stddev_ver3]="$ALIAS.stddev"
    [legend_only_hwanseok_ver3]="$ALIAS.legend"
)
for src in "${!OUT[@]}"; do
    cp -f "real_plot/$src.eps" "$BASE_DIR/${OUT[$src]}.eps"
    if command -v gs >/dev/null 2>&1; then
        gs -dQUIET -dBATCH -dNOPAUSE -dEPSCrop -r150 -sDEVICE=png16m \
           -sOutputFile="$BASE_DIR/${OUT[$src]}.png" "real_plot/$src.eps" \
            && log "png: ${OUT[$src]}.png"
    fi
done

echo "---------------------------------------"
log "done: result_4k_avg.dat / result_4k_stddev.dat -> $ALIAS.{avg,stddev,legend}"
