#!/bin/bash
# make_graph.sh — regenerate the paper's "GC Latency Breakdown" figure
# (Serial GC vs Pipelined GC, FIO; pipe_gc_lat_breakdown.eps) from
# profile-mode FIO runs.
#
# Usage:
#   ./make_graph.sh                          # auto-pick the LATEST runs
#   ./make_graph.sh SERIAL_RUN_DIR PIPE_RUN_DIR
#   UTIL=40 OUT_DIR=... ./make_graph.sh ...  # optional overrides
#
# Inputs (each must contain a `dmesg` from a profile run):
#   SERIAL_RUN_DIR : FIO run with f2fs_profile.ko       (serial GC)
#   PIPE_RUN_DIR   : FIO run with f2fs_pipe_profile.ko  (pipelined GC)
# Auto-discovery looks for the newest
#   experiment_script/FIO/f2fs_profile_data/fio_*/
#   experiment_script/FIO/f2fs_pipe_profile_data/fio_*/
#
# To produce those runs (each needs the profile kernel):
#   sudo kernel/switch_mode.sh f2fs --profile        # or f2fs_pipe --profile
#   sudo kernel/build_kernel.sh && reboot            # once, into *_profile
#   kernel/build_all_profile_modules.sh              # builds all *_profile.ko
#   experiment_script/select_fs.sh                   # pick f2fs (profiling: y)
#   sudo experiment_script/FIO/run_fio.sh
#   experiment_script/select_fs.sh                   # pick f2fs_pipe (y)
#   sudo experiment_script/FIO/run_fio.sh            # graph auto-generates
#
# Outputs (in OUT_DIR, default: this directory):
#   pipe_gc_lat_breakdown_data / .eps / .png

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GAZE_DIR="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
FIO_DATA="$GAZE_DIR/experiment_script/FIO"
UTIL="${UTIL:-40}"
OUT_DIR="${OUT_DIR:-$SCRIPT_DIR}"
PARSER="$SCRIPT_DIR/parse_gc_breakdown.sh"
GPI="$SCRIPT_DIR/plot_compare_f2fs_pipe_lat_breakdown.gpi"

log() { echo "[gc_breakdown] $*"; }
die() { echo "[gc_breakdown] ERROR: $*" >&2; exit 1; }

latest_run() {  # latest_run <mode_data_dir>  -> newest fio_* run dir with a dmesg
    local d
    for d in $(ls -1dt "$1"/fio_*/ 2>/dev/null); do
        [[ -f "$d/dmesg" ]] && { echo "${d%/}"; return 0; }
    done
    return 1
}

SERIAL_RUN="${1:-}"
PIPE_RUN="${2:-}"

if [[ -z "$SERIAL_RUN" ]]; then
    SERIAL_RUN=$(latest_run "$FIO_DATA/f2fs_profile_data") || {
        log "No f2fs_profile FIO run found ($FIO_DATA/f2fs_profile_data/fio_*/)."
        log "Run FIO in mode 'f2fs' with profiling on first (see header of this script)."
        exit 1
    }
    log "serial run (auto): $SERIAL_RUN"
fi
if [[ -z "$PIPE_RUN" ]]; then
    PIPE_RUN=$(latest_run "$FIO_DATA/f2fs_pipe_profile_data") || {
        log "No f2fs_pipe_profile FIO run found ($FIO_DATA/f2fs_pipe_profile_data/fio_*/)."
        log "Run FIO in mode 'f2fs_pipe' with profiling on first (see header of this script)."
        exit 1
    }
    log "pipe run   (auto): $PIPE_RUN"
fi

read -r S_CP S_P012 S_PR S_RD S_PW S_WR S_DOGC < <("$PARSER" "$SERIAL_RUN" serial "$UTIL") \
    || die "parsing serial run failed: $SERIAL_RUN"
read -r P_CP P_P012 P_PR P_RD P_PW P_WR P_DOGC < <("$PARSER" "$PIPE_RUN" pipe "$UTIL") \
    || die "parsing pipe run failed: $PIPE_RUN"

# The pipelined-GC bar shows total elapsed GC time minus the (serially
# executed) p0-p2 preprocess, which is stacked as its own segment.
P_PIPE=$(echo "scale=6; $P_DOGC - $P_P012" | bc -l)

mkdir -p "$OUT_DIR"
DATA="$OUT_DIR/pipe_gc_lat_breakdown_data"
# Column layout consumed by the .gpi (all us; plotted /1e6 as seconds):
#   label CP PREPROCESS PREREAD READ PREWRITE WRITE PIPELINED_GC
{
    echo
    echo "\"Total\" $S_CP $S_P012 $S_PR $S_RD $S_PW $S_WR 0"
    echo "\"Total\" $P_CP $P_P012 0 0 0 0 $P_PIPE"
    echo "\"Preread\" 0 0 $P_PR 0 0 0 0"
    echo "\"Read\" 0 0 0 $P_RD 0 0 0"
    echo "\"Prewrite\" 0 0 0 0 $P_PW 0 0"
    echo "\"Write\" 0 0 0 0 0 $P_WR 0"
} > "$DATA"
log "data: $DATA"

# The .gpi reads/writes relative paths -> run gnuplot from OUT_DIR.
( cd "$OUT_DIR" && gnuplot "$GPI" ) || die "gnuplot failed"
log "eps:  $OUT_DIR/pipe_gc_lat_breakdown.eps"

if command -v gs >/dev/null 2>&1; then
    gs -dQUIET -dBATCH -dNOPAUSE -dEPSCrop -r150 -sDEVICE=png16m \
       -sOutputFile="$OUT_DIR/pipe_gc_lat_breakdown.png" \
       "$OUT_DIR/pipe_gc_lat_breakdown.eps" \
        && log "png:  $OUT_DIR/pipe_gc_lat_breakdown.png"
fi

# Paper-numbered alias (paper Fig 11: "Serial GC vs. Pipelined GC:
# Garbage Collection Latency Breakdown").
ALIAS="Fig-11.Serial_vs_pipelined_GC_latency_breakdown"
cp -f "$OUT_DIR/pipe_gc_lat_breakdown.eps" "$OUT_DIR/$ALIAS.eps"
[ -f "$OUT_DIR/pipe_gc_lat_breakdown.png" ] && \
    cp -f "$OUT_DIR/pipe_gc_lat_breakdown.png" "$OUT_DIR/$ALIAS.png"
log "paper-figure alias: $OUT_DIR/$ALIAS.png"
