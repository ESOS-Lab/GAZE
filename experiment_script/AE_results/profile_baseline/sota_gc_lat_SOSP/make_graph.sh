#!/bin/bash
# make_graph.sh -- regenerate the paper's "Garbage Collection Latency
# Breakdown across Different Filesystems" figure (sota_gc_lat_graph.eps;
# legend: keys/sota_gc_lat_breakdown_keys.eps).
#
# For each of the four filesystems, the run dir is resolved as:
#   1. env override            (F2FS_RUN / PGC_RUN / ZNS_PLUS_RUN / GAZE_RUN)
#   2. newest profile FIO run  (experiment_script/FIO/<mode>_profile_data/fio_*/)
#   3. the archived paper run staged under f2fs/ pgc/ copy/ gaze/ (README.md)
# and the choice is printed, so you always know which bars are fresh vs.
# paper-archive. In the resolved run dir it runs the get_*.sh (placed there
# by lib.sh at benchmark teardown) to produce four_phase_lat_data, then
# merges the four rows and plots.

BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GAZE_DIR="$(cd "$BASE_DIR/../../../.." && pwd)"
FIO_DATA="$GAZE_DIR/experiment_script/FIO"
cd "$BASE_DIR" || exit 1

OUTPUT_FILE="sota_gc_lat_data"
HEADER="#FS_NAME CP P0_1_2 PREREAD_EXCEPT_LOCK PREREAD_LOCK READ_SUM PREWRITE_SUM WRITE_SUM PIPE PRECOPY_SUM COPY POST_CP"

echo "$HEADER" > "$OUTPUT_FILE"

DIRS=("f2fs" "pgc" "copy" "gaze")

latest_run() {  # latest_run <mode_data_dir> -> newest fio_* run dir with a dmesg
    local d
    for d in $(ls -1dt "$1"/fio_*/ 2>/dev/null); do
        [ -f "$d/dmesg" ] && { echo "${d%/}"; return 0; }
    done
    return 1
}

# staged-dir name -> FS mode whose fresh runs to look for
mode_of() { case "$1" in copy) echo zns_plus ;; *) echo "$1" ;; esac; }
override_of() {
    case "$1" in
        f2fs) echo "${F2FS_RUN:-}" ;;     pgc)  echo "${PGC_RUN:-}"  ;;
        copy) echo "${ZNS_PLUS_RUN:-}" ;; gaze) echo "${GAZE_RUN:-}" ;;
    esac
}

for staged in "${DIRS[@]}"; do
    OVERRIDE=$(override_of "$staged")
    if [ -n "$OVERRIDE" ]; then
        root_dir="$OVERRIDE"
        echo "scanning: $root_dir (override) ..."
    elif root_dir=$(latest_run "$FIO_DATA/$(mode_of "$staged")_profile_data"); then
        echo "scanning: $root_dir (fresh run) ..."
    elif [ -d "$staged" ]; then
        root_dir="$staged"
        echo "scanning: $root_dir (archive/paper run) ..."
    else
        echo "ERROR: no fresh $(mode_of "$staged") profile run and no staged directory '$staged'." >&2
        continue
    fi

    # Pick the parser for this run. Pipelined runs get both the standard and
    # the pipe get-script copied in by lib.sh -- the pipe one wins; likewise
    # the copy (ZNS+) variant wins over the standard one.
    SCRIPT_FILE=""
    for name in get_pipe_gc_lat_breakdown_data.sh \
                get_copy_gc_lat_breakdown_data.sh \
                get_gc_lat_breakdown_data.sh; do
        mapfile -t FOUND < <(find "$root_dir" -type f -name "$name")
        if [ "${#FOUND[@]}" -gt 1 ]; then
            echo "ERROR: more than one $name under $root_dir -- keep exactly one run dir per filesystem:" >&2
            printf '  %s\n' "${FOUND[@]}" >&2
            SCRIPT_FILE=""
            break
        elif [ "${#FOUND[@]}" -eq 1 ]; then
            SCRIPT_FILE="${FOUND[0]}"
            break
        fi
    done

    if [ -n "$SCRIPT_FILE" ]; then
        SCRIPT_DIR=$(dirname "$SCRIPT_FILE")
        SCRIPT_NAME=$(basename "$SCRIPT_FILE")
        echo "running: $SCRIPT_NAME (in $SCRIPT_DIR)"
        (
            cd "$SCRIPT_DIR" || exit
            chmod +x "$SCRIPT_NAME"
            ./"$SCRIPT_NAME"
        )
    else
        echo "NOTE: no get_*_gc_lat_breakdown_data.sh under $root_dir (using any existing four_phase_lat_data)."
    fi

    FOUND_FILE=$(find "$root_dir" -type f -name "four_phase_lat_data" | head -n 1)
    if [ -n "$FOUND_FILE" ]; then
        cat "$FOUND_FILE" >> "$OUTPUT_FILE"
        echo "added: $FOUND_FILE"
    else
        echo "WARNING: no four_phase_lat_data under $root_dir -- its bar will be missing." >&2
    fi
done

gnuplot plot_sota_gc_lat.gpi

if command -v gs >/dev/null 2>&1; then
    gs -dQUIET -dBATCH -dNOPAUSE -dEPSCrop -r150 -sDEVICE=png16m \
       -sOutputFile="$BASE_DIR/sota_gc_lat_graph.png" \
       "$BASE_DIR/sota_gc_lat_graph.eps" \
        && echo "png : $BASE_DIR/sota_gc_lat_graph.png"
fi

echo "---------------------------------------"
echo "done: $OUTPUT_FILE -> sota_gc_lat_graph.eps"

# Paper-numbered alias (paper Fig 16: "Garbage Collection Latency Breakdown
# across Different Filesystems (FIO, Victim Zone Utilization: 40%)";
# legend: keys/sota_gc_lat_breakdown_keys.eps).
ALIAS="Fig-16.GC_latency_breakdown_across_filesystems"
cp -f "$BASE_DIR/sota_gc_lat_graph.eps" "$BASE_DIR/$ALIAS.eps"
[ -f "$BASE_DIR/sota_gc_lat_graph.png" ] && \
    cp -f "$BASE_DIR/sota_gc_lat_graph.png" "$BASE_DIR/$ALIAS.png"
echo "paper-figure alias: $BASE_DIR/$ALIAS.png"
