#!/bin/bash
# Fileserver benchmark — reads FS_MODE from experiment_script/config.sh
# Usage: sudo ./run_fileserver.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../../config.sh"
source "$GENERAL_RES/lib.sh"

TIMESTAMP=$(date "+%Y%m%d_%H%M")
OUTDIR="$SCRIPT_DIR/${FS_NAME}_data/fileserver_${TIMESTAMP}"

cleanup() {
    echo
    echo "[run_fileserver] Interrupted — killing filebench and tearing down..."
    sudo pkill -TERM -f "$FILEBENCH_PATH/filebench" 2>/dev/null
    sleep 1
    sudo pkill -KILL -f "$FILEBENCH_PATH/filebench" 2>/dev/null
    kill "$PID_DF" 2>/dev/null
    "$GENERAL_RES/setup_fs.sh" teardown 2>/dev/null
    exit 130
}
trap cleanup INT TERM

main() {
    export LD_LIBRARY_PATH=/usr/local/sbin:$LD_LIBRARY_PATH

    echo "==== Fileserver: $FS_NAME ===="
    mkdir -p "$OUTDIR"
    write_run_env "$OUTDIR"

    # Mount filesystem
    "$GENERAL_RES/setup_fs.sh" setup || { echo "[run_fileserver] ABORT: filesystem setup failed -- not running on a stale/absent mount." >&2; exit 1; }

    # Re-source .config: setup_fs.sh's run_nvmevirt.sh may have rewritten
    # ZNS_DEV/ZNS_DEV_NAME (fresh-clone placeholder -> real device). Its own
    # subshell refresh didn't reach us; without this the scheduler write below
    # targets a nonexistent /sys/block path (see FIO/run_fio.sh for the full
    # trail — EIO from write reorder on ZNS).
    [[ -f "$GAZE_DIR/.config" ]] && source "$GAZE_DIR/.config"

    root_write "/sys/block/${ZNS_DEV_NAME}/queue/scheduler" mq-deadline
    sudo sysctl -w kernel.randomize_va_space=0
    sudo sysctl -w vm.drop_caches=3

    "$GENERAL_RES/record_df.sh" "$OUTDIR/df_log" "$CONV_PART" &
    PID_DF=$!

    dmesg > "$OUTDIR/bef_run_dmesg"
    sudo dmesg -C
    root_write /dev/kmsg STARTTTTTT

    # Render fileserver.f from the template + current experiment_params.sh
    # values (filebench's own DSL has no env-var substitution, so this is
    # a plain sed pass over @@PLACEHOLDER@@ tokens).
    sed -e "s/@@NFILES@@/$FILESERVER_NFILES/" \
        -e "s/@@MEANDIRWIDTH@@/$FILESERVER_MEANDIRWIDTH/" \
        -e "s/@@NTHREADS@@/$FILESERVER_NTHREADS/" \
        -e "s/@@IOSIZE@@/$FILESERVER_IOSIZE/" \
        -e "s/@@MEANAPPENDSIZE@@/$FILESERVER_MEANAPPENDSIZE/" \
        -e "s/@@RUNTIME@@/$FILESERVER_RUNTIME/" \
        "$SCRIPT_DIR/fileserver.f.template" > "$SCRIPT_DIR/fileserver.f"

    GC_START_SEC="${FILESERVER_GC_START_SEC:-100}"  # GC-window stats start here
    echo "==== Running Fileserver (runtime=${FILESERVER_RUNTIME}s, GC stats from ${GC_START_SEC}s) ===="
    sudo FILEBENCH_GC_START_SEC="$GC_START_SEC" \
        "$FILEBENCH_PATH/filebench" -f "$SCRIPT_DIR/fileserver.f" \
        1> "$OUTDIR/result.txt" &
    wait $!

    kill $PID_DF 2>/dev/null; wait $PID_DF 2>/dev/null

    echo "==== Processing results ===="
    dmesg > "$OUTDIR/dmesg"

    # Teardown
    "$GENERAL_RES/setup_fs.sh" teardown

    # dmesg parsing
    parse_dmesg "$OUTDIR"
    parse_dmesg_fs_specific "$OUTDIR"

    # Fileserver-specific: compute kiops_sum from filebench Summary lines
    grep Summary "$OUTDIR/result.txt" | \
        awk 'BEGIN{t=5} {print t, $6/1000; t+=5}' > "$OUTDIR/kiops_sum"

    cp "$SCRIPT_DIR/fileserver.f" "$OUTDIR/"
    rm -f new; ln -sf "$OUTDIR" new; sudo chown -h "${SUDO_USER:-$USER}" new 2>/dev/null || true
    sudo chown -R "${SUDO_USER:-$USER}" "$OUTDIR" 2>/dev/null || true

    "$GAZE_DIR/experiment_script/graph/AE_results/update_graph.sh"

    echo "==== Done. Results in: $OUTDIR ===="
}

main
