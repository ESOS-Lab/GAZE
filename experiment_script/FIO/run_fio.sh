#!/bin/bash
# FIO benchmark — reads FS_MODE from experiment_script/config.sh
# Usage: ./run_fio.sh   (self-elevates: needs root for /sys/block, mount,
#                        sysctl, /dev/kmsg, dmesg -C, etc.)

# Self-elevate so the README convention "./run_fio.sh" (no sudo prefix)
# matches every other user-facing entry point (setup_env.sh, set_config.sh,
# build_kernel.sh, run_experiment.sh — each self-elevates the parts that
# actually need root). -E preserves env vars callers may have set.
if [ "${EUID:-$(id -u)}" -ne 0 ]; then
    exec sudo -E "$0" "$@"
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../../config.sh"
source "$GENERAL_RES/lib.sh"

BLOCK_SIZE="$FIO_BLOCK_SIZE"
IO_TYPE="$FIO_IO_TYPE"
NUM_JOBS="$FIO_NUM_JOBS"
RUNTIME="$FIO_RUNTIME"  # seconds
FILESIZE="$FIO_FILESIZE"
GC_START_SEC="${FIO_GC_START_SEC:-500}"  # fio records stats only after this

TIMESTAMP=$(date "+%Y%m%d_%H%M")
OUTDIR="$SCRIPT_DIR/${FS_NAME}_data/fio_${TIMESTAMP}"

cleanup() {
    echo
    echo "[run_fio] Interrupted — killing fio and tearing down..."
    sudo pkill -TERM -f "$FIO_PATH/fio" 2>/dev/null
    sleep 1
    sudo pkill -KILL -f "$FIO_PATH/fio" 2>/dev/null
    kill "$PID_DF" 2>/dev/null
    "$GENERAL_RES/setup_fs.sh" teardown 2>/dev/null
    exit 130
}
trap cleanup INT TERM

main() {
    export LD_LIBRARY_PATH=/usr/local/sbin:$LD_LIBRARY_PATH

    echo "==== FIO: $FS_NAME | $IO_TYPE | $BLOCK_SIZE | ${RUNTIME}s ===="
    mkdir -p "$OUTDIR"
    write_run_env "$OUTDIR"

    # Mount filesystem
    "$GENERAL_RES/setup_fs.sh" setup || { echo "[run_fio] ABORT: filesystem setup failed -- not running on a stale/absent mount." >&2; exit 1; }

    # setup_fs.sh (re)loaded NVMeVirt and run_nvmevirt.sh may have rewritten
    # ZNS_DEV/ZNS_DEV_NAME in .config (fresh-clone start where set_config.sh
    # wrote the placeholder nvme2n1; the real device is nvme1n1 etc). setup_fs.sh
    # ran in its own subshell so the refresh didn't reach us -- re-source here.
    # Skipping this used to write the scheduler onto /sys/block/<stale>/... (tee
    # ENOENT); the nvme default scheduler "none" then stayed in effect, writes
    # reordered across queues, and ZNS rejected them (WP mismatch -> EIO ->
    # F2FS shutdown). See shival_diff (session 9, 2026-07-13).
    # (Guarded: without .config, config.sh already warned and fell back to
    # placeholder defaults — nothing to refresh.)
    [[ -f "$GAZE_DIR/.config" ]] && source "$GAZE_DIR/.config"

    root_write "/sys/block/${ZNS_DEV_NAME}/queue/scheduler" mq-deadline
    sudo sysctl -w kernel.randomize_va_space=0
    sudo sysctl -w vm.drop_caches=3

    "$GENERAL_RES/record_df.sh" "$OUTDIR/df_log" "$CONV_PART" &
    PID_DF=$!

    dmesg > "$OUTDIR/bef_run_dmesg"
    sudo dmesg -C
    root_write /dev/kmsg STARTTTTTT

    echo "==== Running FIO (stats recorded from ${GC_START_SEC}s = GC onset) ===="
    sudo FIO_GC_START_SEC="$GC_START_SEC" "$FIO_PATH/fio" \
        --filename="$MNT/test" \
        --name=test \
        --rw="$IO_TYPE" \
        --bs="$BLOCK_SIZE" \
        --filesize="$FILESIZE" \
        --numjobs="$NUM_JOBS" \
        --norandommap \
        --allow_mounted_write=1 \
        --random_generator=tausworthe \
        --randseed=1 \
        --group_reporting=1 \
        --log_avg_msec=2000 \
        --write_iops_log="$BLOCK_SIZE" \
        --write_lat_log="$BLOCK_SIZE" \
        --fadvise_hint=0 \
        --percentile_list=90:95:99:99.90:99.95:99.99:99.999:99.9999:99.99999:99.999999 \
        --time_based --runtime="${RUNTIME}s" \
        > "$OUTDIR/result_${BLOCK_SIZE}.txt" &
    wait $!

    kill $PID_DF 2>/dev/null; wait $PID_DF 2>/dev/null

    echo "==== Processing results ===="

    # Latency log
    cat ${BLOCK_SIZE}_lat.*.log | awk -F ',' '{print $2}' | sort -n > "$OUTDIR/lat_sum_sorted"
    mv *.log "$OUTDIR/" 2>/dev/null || true

    # Throughput, averaged over the GC window. fio records samples only
    # after FIO_GC_START_SEC (see benchmarks/fio_src/jw_gc.h), so kiops_sum
    # holds ~(RUNTIME - GC_START)/2 two-second samples; keep the paper
    # pipeline's 5-sample head trim (paper: (1200-500)/2 - 5 = 345).
    python3 "$GENERAL_RES/sum.py" "$OUTDIR/${BLOCK_SIZE}_iops." "$NUM_JOBS" > "$OUTDIR/kiops_sum"
    GC_TAIL=$(( (RUNTIME - GC_START_SEC) / 2 - 5 ))
    [ "$GC_TAIL" -lt 1 ] && GC_TAIL=1
    awk '{b+=1; a+=$2} END {print a/b}' \
        <(tail -n "$GC_TAIL" "$OUTDIR/kiops_sum") > "$OUTDIR/kiops_avg_gc"

    dmesg > "$OUTDIR/dmesg"

    # Teardown
    "$GENERAL_RES/setup_fs.sh" teardown

    # dmesg parsing
    parse_dmesg "$OUTDIR"
    parse_dmesg_fs_specific "$OUTDIR"

    rm -f new; ln -sf "$OUTDIR" new; sudo chown -h "${SUDO_USER:-$USER}" new 2>/dev/null || true
    sudo chown -R "${SUDO_USER:-$USER}" "$OUTDIR" 2>/dev/null || true

    # Profile mode: rebuild the paper's profile figures this run can feed
    # (Fig 5 overall/read/write phase, Fig 14 ablation, Fig 16 sota) and
    # collect the paper-numbered copies into AE_results/profile/. Each
    # figure resolves to the newest profile run per mode (i.e. this run)
    # and falls back to the archived paper run for modes not (re)run yet;
    # AE_results/profile/SOURCES.txt records which is which.
    if [[ "$PROFILE_MODE" == "on" ]]; then
        AE_RESULTS="$GAZE_DIR/experiment_script/AE_results"
        echo "==== Rebuilding profile figures -> AE_results/profile/ ===="
        "$AE_RESULTS/collect_profile_figures.sh" \
            || echo "[run_fio] profile figures not (re)generated -- see message above."
        # Figure rebuilds ran as root here: re-own everything they touched
        # (graph pipelines, AE_results/profile, and the intermediates they
        # wrote back into $OUTDIR) so aec's view_results.sh keeps working.
        sudo chown -R "${SUDO_USER:-$USER}" "$AE_RESULTS/profile_baseline" \
            "$AE_RESULTS/profile" "$OUTDIR" 2>/dev/null || true
    fi

    echo "==== Done. Results in: $OUTDIR ===="
}

main
