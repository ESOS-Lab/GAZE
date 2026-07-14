#!/bin/bash
# TPC-C benchmark — reads FS_MODE from experiment_script/config.sh
# Usage: sudo ./run_tpcc.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../../config.sh"
source "$GENERAL_RES/lib.sh"
source "$GENERAL_RES/cold_data_lib.sh"

# This benchmark copies preloaded MySQL/cold data in before running; if it
# isn't there (fresh machine / different scale), refuse up front with the
# generation instructions instead of failing mid-setup.
tpcc_cold_data_ok || { echo "[run_tpcc] SKIPPED: cold data missing (see above)." >&2; exit 1; }

TIMESTAMP=$(date "+%Y%m%d_%H%M")
OUTDIR="$SCRIPT_DIR/${FS_NAME}_data/tpcc_${TIMESTAMP}"

cleanup() {
    echo
    echo "[run_tpcc] Interrupted — killing tpcc_start and tearing down..."
    pkill -TERM -f "$TPCC_PATH/tpcc_start" 2>/dev/null
    sleep 1
    pkill -KILL -f "$TPCC_PATH/tpcc_start" 2>/dev/null
    kill "$PID_DF" 2>/dev/null
    sudo systemctl stop mysql 2>/dev/null
    "$GENERAL_RES/setup_fs.sh" teardown 2>/dev/null
    exit 130
}
trap cleanup INT TERM

main() {
    export LD_LIBRARY_PATH=/usr/local/sbin:$LD_LIBRARY_PATH

    echo "==== TPC-C: $FS_NAME ===="
    mkdir -p "$OUTDIR"
    write_run_env "$OUTDIR"

    # Stop MySQL and mount fresh filesystem
    sudo systemctl stop mysql
    "$GENERAL_RES/setup_fs.sh" setup || { echo "[run_tpcc] ABORT: filesystem setup failed -- not running on a stale/absent mount." >&2; exit 1; }

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
    sudo cp -rp "$MYSQL_DATA_PATH/$COLD_40G_NAME" "$MNT/"
    sudo cp -rp "$MYSQL_DATA_PATH/$COLD_20G_NAME" "$MNT/"
    sudo cp -rp "$MYSQL_DATA_PATH/$TPCC_COLD_DB" "$MNT/mysql"
    sudo chown -R mysql "$MNT"
    sudo chmod -R 777 "$MNT"
    sudo systemctl start mysql

    echo "==== Running TPC-C ===="
    sync
    sudo sysctl -w vm.drop_caches=3
    dmesg > "$OUTDIR/bef_run_dmesg"
    sudo dmesg -C
    root_write /dev/kmsg STARTTTTTT

    cd "$TPCC_PATH"
    # -i (report interval) must be <= -l (measured duration): tpcc_start only
    # tallies/prints results at each -i boundary, so if -i > -l it never hits
    # one and every counter reports 0 ("in 0 sec.") even though the run
    # otherwise completed fine. Always deriving -i from -l here (instead of
    # letting them drift independently) makes that failure mode impossible.
    ./tpcc_start -h127.0.0.1 -P3306 -dtpcc1000 -u"$MYSQL_USER" \
        -w"$TPCC_WAREHOUSES" -c"$TPCC_CLIENTS" -r"$TPCC_RAMPUP" -l"$TPCC_DURATION" -i"$TPCC_DURATION" \
        -p"$MYSQL_PASSWD" > "$OUTDIR/result.txt" &
    wait $!
    cd "$SCRIPT_DIR"

    kill $PID_DF 2>/dev/null; wait $PID_DF 2>/dev/null

    echo "==== Processing results ===="
    dmesg > "$OUTDIR/dmesg"

    # Teardown
    sudo systemctl stop mysql
    "$GENERAL_RES/setup_fs.sh" teardown

    # dmesg parsing
    parse_dmesg "$OUTDIR"
    parse_dmesg_fs_specific "$OUTDIR"

    rm -f new; ln -sf "$OUTDIR" new; sudo chown -h "${SUDO_USER:-$USER}" new 2>/dev/null || true
    sudo chown -R "${SUDO_USER:-$USER}" "$OUTDIR" 2>/dev/null || true

    "$GAZE_DIR/experiment_script/graph/AE_results/update_graph.sh"

    echo "==== Done. Results in: $OUTDIR ===="
}

main
