#!/bin/bash
# collect_profile_figures.sh -- rebuild the profile figures (paper Fig 5,
# 14, 16) and collect their paper-numbered copies here, under
# AE_results/profile/, so every evaluator-facing result lives in AE_results.
#
# Usage:
#   ./collect_profile_figures.sh          # newest fresh profile run per mode,
#                                         # paper archive for the rest
#                                         #   -> AE_results/profile/
#   ./collect_profile_figures.sh --paper  # paper baseline: force every bar to
#                                         # the archived SOSP26 paper run
#                                         #   -> AE_results/profile/paper_baseline/
#
# Each figure's data source (fresh run vs. paper archive) is recorded in
# SOURCES.txt next to the figures. The figure pipelines themselves (plot
# templates, parsers, archived paper runs) live in AE_results/profile_baseline/
# script only orchestrates them and collects the outputs.

set -u
AE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
G="$AE_DIR/profile_baseline"
FIO_DATA="$(cd "$AE_DIR/../FIO" && pwd)"

PAPER=0
[ "${1:-}" = "--paper" ] && PAPER=1

if [ "$PAPER" -eq 1 ]; then
    DEST="$AE_DIR/profile/paper_baseline"
else
    DEST="$AE_DIR/profile"
fi
mkdir -p "$DEST"
LOG="$DEST/SOURCES.txt"
: > "$LOG"
{
    echo "# How each figure below was produced ($(date '+%Y-%m-%d %H:%M'))"
    [ "$PAPER" -eq 1 ] && echo "# --paper: every bar forced to the archived SOSP26 paper run."
    echo "#"
} >> "$LOG"

run_and_log() {  # run_and_log <make_graph invocation...>
    local out
    out=$("$@" 2>&1) || { echo "$out" >&2; return 1; }
    echo "$out"
    # keep the resolution lines (which run each bar came from)
    echo "$out" | grep -E "row:|fresh|archive|paper mode|override|scanning:" >> "$LOG"
}

echo "==== Fig 14 (ablation_gc_lat_SOSP) ===="
if [ "$PAPER" -eq 1 ]; then
    A="$G/ablation_gc_lat_SOSP"
    VANILLA_RUN="$A/f2fs_vanilla_profile_fio_exp_output_4k_20251207_0240/randwrite/f2fs_vanilla_profile/1" \
    PAGE_RUN="$A/f2fs_pgpool_profile_fio_exp_output_4k_20251208_1355/randwrite/f2fs_pgpool_profile/1" \
    PIPE_RUN="$A/f2fs_4step_profile_fio_exp_output_4k_20251208_0301/f2fs_4step_batchseg_16_profile/1" \
    PIPE_PAGE_RUN="$A/gaze_4step_profile_fio_exp_output_4k_20251208_0012/randwrite/gaze_4step_profile/1" \
        run_and_log "$G/ablation_gc_lat_SOSP/make_graph.sh"
else
    run_and_log "$G/ablation_gc_lat_SOSP/make_graph.sh"
fi

echo "==== Fig 16 (sota_gc_lat_SOSP) ===="
if [ "$PAPER" -eq 1 ]; then
    S="$G/sota_gc_lat_SOSP"
    F2FS_RUN="$S/f2fs" PGC_RUN="$S/pgc" ZNS_PLUS_RUN="$S/copy" GAZE_RUN="$S/gaze" \
        run_and_log "$S/make_graph.sh"
else
    run_and_log "$G/sota_gc_lat_SOSP/make_graph.sh"
fi

echo "==== Fig 5 (vanilla_gc_latency_breakdown_accurate) ===="
FIG5_RUN=""
if [ "$PAPER" -eq 0 ]; then
    for r in $(ls -1dt "$FIO_DATA/f2fs_profile_data"/fio_*/ 2>/dev/null); do
        [ -f "$r/gc_lat_read_phase_only" ] && { FIG5_RUN="${r%/}"; break; }
    done
fi
run_and_log "$G/vanilla_gc_latency_breakdown_accurate/make_graph.sh" $FIG5_RUN

# ---- collect the paper-numbered copies --------------------------------------
V="$G/vanilla_gc_latency_breakdown_accurate"
cp -f "$V"/Fig-05a.GC_latency_breakdown.Overall_GC.*        "$DEST/"
cp -f "$V"/Fig-05b.GC_latency_breakdown.GC_Read_Phase.*     "$DEST/"
cp -f "$V"/Fig-05c.GC_latency_breakdown.GC_Write_Phase.*    "$DEST/"
cp -f "$G"/ablation_gc_lat_SOSP/Fig-14.GC_latency_under_individual_techniques.* "$DEST/"
cp -f "$G"/sota_gc_lat_SOSP/Fig-16.GC_latency_breakdown_across_filesystems.*    "$DEST/"
# Fig 16's legend is a separate eps in the paper
cp -f "$G"/sota_gc_lat_SOSP/keys/sota_gc_lat_breakdown_keys.eps \
      "$DEST/Fig-16-legend.sota_gc_lat_breakdown_keys.eps"
if command -v gs >/dev/null 2>&1; then
    gs -dQUIET -dBATCH -dNOPAUSE -dEPSCrop -r150 -sDEVICE=png16m \
       -sOutputFile="$DEST/Fig-16-legend.sota_gc_lat_breakdown_keys.png" \
       "$DEST/Fig-16-legend.sota_gc_lat_breakdown_keys.eps" >/dev/null 2>&1 || true
fi

echo "---------------------------------------"
echo "collected into: $DEST"
ls -1 "$DEST" | sed 's/^/  /'
