#!/bin/bash
# YCSB-A benchmark — reads FS_MODE from experiment_script/config.sh
# Usage: sudo ./run_ycsba.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../../config.sh"
source "$GENERAL_RES/lib.sh"
source "$GENERAL_RES/cold_data_lib.sh"

# This benchmark copies preloaded MySQL/cold data in before running; if it
# isn't there (fresh machine / different scale), refuse up front with the
# generation instructions instead of failing mid-setup.
ycsb_cold_data_ok || { echo "[run_ycsba] SKIPPED: cold data missing (see above)." >&2; exit 1; }

RECORDSIZE="$YCSBA_RECORDSIZE"
RECOUNT="$YCSBA_RECORDCOUNT"
OPCOUNT="$YCSBA_OPERATIONCOUNT"
THREAD="$YCSBA_THREADS"

TIMESTAMP=$(date "+%Y%m%d_%H%M")
OUTDIR="$SCRIPT_DIR/${FS_NAME}_data/ycsba_${TIMESTAMP}"

cleanup() {
    echo
    echo "[run_ycsba] Interrupted — killing ycsb and tearing down..."
    sudo pkill -TERM -f "bin/ycsb.sh" 2>/dev/null
    sleep 1
    sudo pkill -KILL -f "bin/ycsb.sh" 2>/dev/null
    kill "$PID_DF" 2>/dev/null
    sudo systemctl stop mysql 2>/dev/null
    "$GENERAL_RES/setup_fs.sh" teardown 2>/dev/null
    exit 130
}
trap cleanup INT TERM

main() {
    export LD_LIBRARY_PATH=/usr/local/sbin:$LD_LIBRARY_PATH

    local fieldlength=$(( RECORDSIZE / 10 ))

    echo "==== YCSB-A: $FS_NAME ===="
    mkdir -p "$OUTDIR"
    write_run_env "$OUTDIR"

    # Stop MySQL and mount fresh filesystem
    sudo systemctl stop mysql
    "$GENERAL_RES/setup_fs.sh" setup || { echo "[run_ycsba] ABORT: filesystem setup failed -- not running on a stale/absent mount." >&2; exit 1; }

    # Re-source .config: setup_fs.sh's run_nvmevirt.sh may have rewritten
    # ZNS_DEV/ZNS_DEV_NAME (fresh-clone placeholder -> real device). Its own
    # subshell refresh didn't reach us; without this the scheduler write below
    # targets a nonexistent /sys/block path (see FIO/run_fio.sh for the full
    # trail — EIO from write reorder on ZNS).
    [[ -f "$GAZE_DIR/.config" ]] && source "$GAZE_DIR/.config"

    root_write "/sys/block/${ZNS_DEV_NAME}/queue/scheduler" mq-deadline
    sudo sysctl -w kernel.randomize_va_space=0

    "$GENERAL_RES/record_df.sh" "$OUTDIR/df_log" "$CONV_PART" &
    PID_DF=$!

    # Copy cold data
    echo "==== Copying cold data ===="
    sudo cp -rp "$MYSQL_DATA_PATH/$COLD_20G_NAME" "$MNT/"
    sudo cp -rp "$MYSQL_DATA_PATH/$COLD_20G_NAME" "$MNT/cold_20G_another"
    sudo cp -rp "$MYSQL_DATA_PATH/$COLD_10G_NAME" "$MNT/"
    sudo cp -rp "$MYSQL_DATA_PATH/$YCSB_COLD_DB" "$MNT/mysql"
    sudo chown -R mysql "$MNT"
    sudo chmod -R 777 "$MNT"
    sudo systemctl start mysql

    echo "==== Running YCSB-A ===="
    sync
    sudo sysctl -w vm.drop_caches=3
    dmesg > "$OUTDIR/bef_run_dmesg"
    sudo dmesg -C
    root_write /dev/kmsg STARTTTTTT

    cd "$YCSB_PATH"
    sudo bin/ycsb.sh run jdbc \
        -P "$SCRIPT_DIR/workloada" \
        -P db.properties \
        -p recordcount="$RECOUNT" \
        -p fieldlength="$fieldlength" \
        -p operationcount="$OPCOUNT" \
        -p writeallfields=false \
        -p status.interval=60 \
        -threads "$THREAD" \
        -s \
        > "$OUTDIR/result.txt" 2> "$OUTDIR/ycsb_status.log" &
    wait $!
    cd "$SCRIPT_DIR"

    kill $PID_DF 2>/dev/null; wait $PID_DF 2>/dev/null

    echo "==== Processing results ===="
    dmesg > "$OUTDIR/dmesg"
    sed 's/\]//g;s/\[//g' "$OUTDIR/dmesg" | \
        awk -v n="$(grep STARTT "$OUTDIR/dmesg" | sed 's/\[//g;s/\]//g' | awk '{print $1}')" \
        '{$1=$1-n; print}' > "$OUTDIR/dmesg_parsed"

    # Teardown
    sudo systemctl stop mysql
    "$GENERAL_RES/setup_fs.sh" teardown

    # dmesg parsing
    parse_dmesg "$OUTDIR"
    parse_dmesg_fs_specific "$OUTDIR"

    cp "$SCRIPT_DIR/workloada" "$OUTDIR/"
    rm -f new; ln -sf "$OUTDIR" new; sudo chown -h "${SUDO_USER:-$USER}" new 2>/dev/null || true
    sudo chown -R "${SUDO_USER:-$USER}" "$OUTDIR" 2>/dev/null || true

    "$GAZE_DIR/experiment_script/graph/AE_results/update_graph.sh"

    echo "==== Done. Results in: $OUTDIR ===="
}

main
