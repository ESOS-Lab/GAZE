#!/bin/bash
# Idempotent cleanup — safe to run any number of times, including on a
# machine that's already clean. Meant to be called (a) once before the
# first benchmark in a run_experiment.sh session, in case a previous
# session was interrupted (Ctrl-C, SSH drop, crash) and left mysqld
# running against a now-unmounted datadir, a stray fio/filebench/ycsb/
# tpcc_start process, or $MNT mounted; and (b) again after any benchmark
# that gets interrupted mid-run, before either continuing to the next
# benchmark or exiting, so the next invocation of this script (or of a
# run_*.sh directly) starts from known-clean state.
#
# Does NOT touch kernel modules — setup_fs.sh's own setup() already does
# rmmod/insmod idempotently right before every mkfs+mount, so there's
# nothing extra to do here for that layer.
#
# Usage: preflight_cleanup.sh   (requires config.sh already sourced by caller)

echo "[preflight_cleanup] Cleaning up any leftover state from a previous run..."

# Kill any stray benchmark processes from an interrupted run whose own
# cleanup trap didn't get to run (e.g. SSH connection dropped, SIGKILL,
# a crash in the orchestrator itself rather than a clean Ctrl-C).
for pat in "$FIO_PATH/fio" "$FILEBENCH_PATH/filebench" "$TPCC_PATH/tpcc_start" "bin/ycsb.sh"; do
    if pgrep -f "$pat" > /dev/null 2>&1; then
        echo "[preflight_cleanup]   killing leftover: $pat"
        sudo pkill -TERM -f "$pat" 2>/dev/null
    fi
done
sleep 1
for pat in "$FIO_PATH/fio" "$FILEBENCH_PATH/filebench" "$TPCC_PATH/tpcc_start" "bin/ycsb.sh"; do
    pgrep -f "$pat" > /dev/null 2>&1 && sudo pkill -KILL -f "$pat" 2>/dev/null
done

# MySQL may still be up against a datadir on $MNT that's about to be
# unmounted/reformatted underneath it (YCSB-A/F, TPC-C all use it).
# Stopping it is always safe here — every run_*.sh that needs it starts
# it fresh after mounting.
if systemctl is-active --quiet mysql 2>/dev/null; then
    echo "[preflight_cleanup]   stopping leftover mysql service"
    sudo systemctl stop mysql
fi

# Unmount + clear $MNT if a previous run left it mounted.
if mountpoint -q "$MNT" 2>/dev/null; then
    echo "[preflight_cleanup]   unmounting leftover mount at $MNT"
    "$GENERAL_RES/setup_fs.sh" teardown
fi

echo "[preflight_cleanup] Done."
