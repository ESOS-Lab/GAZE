#!/bin/bash
# make_graph.sh -- regenerate the paper's "Garbage Collection Latency under
# Individual Techniques" figure (ablation_gc_lat_compare.eps; FIO, victim
# zone utilization 40%).
#
# Four bars, one per technique combination:
#   Vanilla    f2fs mode       (serial GC)
#   Page       f2fs_pool mode  (serial GC + GC page pool)
#   Pipe       f2fs_pipe mode  (pipelined GC)
#   Pipe+Page  gaze mode       (pipelined GC + page pool)
#
# Bar value = per-GC average (CP + do_gc) latency at util 40, parsed with
# pipe_gc_lat_breakdown_SOSP/parse_gc_breakdown.sh (serial flavor for the
# two serial-GC modes, pipe flavor for the two pipelined ones). Re-running
# this over the archived run dirs below reproduces the committed
# ablation_total_lat_data to the last decimal.
#
# Usage:
#   ./make_graph.sh     # newest profile FIO run per mode
#                       # (experiment_script/FIO/<mode>_profile_data/fio_*/);
#                       # falls back to the archived paper run for any mode
#                       # without a fresh run, and says which bars are which.
#   VANILLA_RUN=... PAGE_RUN=... PIPE_RUN=... PIPE_PAGE_RUN=... ./make_graph.sh
#                       # explicit run-dir overrides (any subset)
#
# To produce fresh runs (all on the profile kernel, no reboot between modes):
#   kernel/switch_mode.sh f2fs --profile && kernel/build_kernel.sh  # once
#   <reboot into 6.1.0.sosp26_ae_profile>
#   then per mode: experiment_script/select_fs.sh (pick the mode, profiling: y)
#                  + sudo experiment_script/FIO/run_fio.sh
#
# Outputs (this directory): ablation_total_lat_data / .eps / .png

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GAZE_DIR="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
FIO_DATA="$GAZE_DIR/experiment_script/FIO"
PARSER="$SCRIPT_DIR/../pipe_gc_lat_breakdown_SOSP/parse_gc_breakdown.sh"
UTIL="${UTIL:-40}"

log() { echo "[ablation_gc_lat] $*"; }
die() { echo "[ablation_gc_lat] ERROR: $*" >&2; exit 1; }

[[ -x "$PARSER" ]] || die "parser not found: $PARSER"

latest_run() {  # latest_run <mode_data_dir> -> newest fio_* run dir with a dmesg
    local d
    for d in $(ls -1dt "$1"/fio_*/ 2>/dev/null); do
        [[ -f "$d/dmesg" ]] && { echo "${d%/}"; return 0; }
    done
    return 1
}

# resolve_run LABEL OVERRIDE MODE_DATA_DIR ARCHIVE_DIR FLAVOR
# -> sets RUN (run dir) and SRC (fresh|archive|override)
resolve_run() {
    local label="$1" override="$2" fresh_root="$3" archive="$4"
    if [[ -n "$override" ]]; then
        RUN="$override"; SRC="override"
    elif RUN=$(latest_run "$fresh_root"); then
        SRC="fresh"
    else
        RUN="$SCRIPT_DIR/$archive"; SRC="archive (paper run)"
    fi
    [[ -f "$RUN/dmesg" ]] || die "$label: no dmesg in $RUN"
    log "$label: $SRC -> $RUN"
}

# total LABEL RUN FLAVOR -> per-GC average CP + do_gc latency (us)
total() {
    local label="$1" run="$2" flavor="$3" out
    out=$("$PARSER" "$run" "$flavor" "$UTIL") || die "parsing $label run failed: $run"
    echo "$out" | awk '{printf "%.6f", $1 + $7}'
}

resolve_run "Vanilla"   "${VANILLA_RUN:-}"   "$FIO_DATA/f2fs_profile_data" \
    "f2fs_vanilla_profile_fio_exp_output_4k_20251207_0240/randwrite/f2fs_vanilla_profile/1"
V_VANILLA=$(total "Vanilla" "$RUN" serial)

resolve_run "Page"      "${PAGE_RUN:-}"      "$FIO_DATA/f2fs_pool_profile_data" \
    "f2fs_pgpool_profile_fio_exp_output_4k_20251208_1355/randwrite/f2fs_pgpool_profile/1"
V_PAGE=$(total "Page" "$RUN" serial)

resolve_run "Pipe"      "${PIPE_RUN:-}"      "$FIO_DATA/f2fs_pipe_profile_data" \
    "f2fs_4step_profile_fio_exp_output_4k_20251208_0301/f2fs_4step_batchseg_16_profile/1"
V_PIPE=$(total "Pipe" "$RUN" pipe)

resolve_run "Pipe+Page" "${PIPE_PAGE_RUN:-}" "$FIO_DATA/gaze_profile_data" \
    "gaze_4step_profile_fio_exp_output_4k_20251208_0012/randwrite/gaze_4step_profile/1"
V_PIPE_PAGE=$(total "Pipe+Page" "$RUN" pipe)

DATA="$SCRIPT_DIR/ablation_total_lat_data"
{
    echo
    echo "\"Vanilla\" $V_VANILLA 0 0 0"
    echo "\"Page\" 0 $V_PAGE 0 0"
    echo "\"Pipe\" 0 0 $V_PIPE 0"
    echo "\"Pipe+Page\" 0 0 0 $V_PIPE_PAGE"
} > "$DATA"
log "data: $DATA"

( cd "$SCRIPT_DIR" && gnuplot plot_ablation_gc_lat.gpi ) || die "gnuplot failed"
log "eps:  $SCRIPT_DIR/ablation_gc_lat_compare.eps"

if command -v gs >/dev/null 2>&1; then
    gs -dQUIET -dBATCH -dNOPAUSE -dEPSCrop -r150 -sDEVICE=png16m \
       -sOutputFile="$SCRIPT_DIR/ablation_gc_lat_compare.png" \
       "$SCRIPT_DIR/ablation_gc_lat_compare.eps" \
        && log "png:  $SCRIPT_DIR/ablation_gc_lat_compare.png"
fi

# Paper-numbered alias (paper Fig 14: "Garbage Collection Latency under
# Individual Techniques (FIO, victim zone utilization: 40%)").
ALIAS="Fig-14.GC_latency_under_individual_techniques"
cp -f "$SCRIPT_DIR/ablation_gc_lat_compare.eps" "$SCRIPT_DIR/$ALIAS.eps"
[ -f "$SCRIPT_DIR/ablation_gc_lat_compare.png" ] && \
    cp -f "$SCRIPT_DIR/ablation_gc_lat_compare.png" "$SCRIPT_DIR/$ALIAS.png"
log "paper-figure alias: $SCRIPT_DIR/$ALIAS.png"
