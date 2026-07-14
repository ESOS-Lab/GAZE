#!/bin/bash
# Interactive editor for experiment_script/general_resource/experiment_params.sh
# -- the workload-intensity knobs (record counts, thread counts, runtime,
# ...) used by every run_*.sh. No sudo, no reboot; takes effect on the
# next run of any benchmark.
#
# Usage: ./set_params.sh
#
# Option 7 in the menu below ("reset to paper's original config") copies
# general_resource/paper_params.sh -- a read-only, never-edited snapshot
# of the values the paper's Fig-15/16/17/18 were produced with -- back
# over the live experiment_params.sh. That's the safety net this script
# exists to make easy: however far you stray while tuning things (e.g.
# shortening runtimes for a smoke test), picking "paper config" always
# gets you back to exactly what the paper used.

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GENERAL_RES="$SCRIPT_DIR/general_resource"
source "$GENERAL_RES/params_lib.sh"

set_live() { # VAR NEWVAL
    sed -i "s|^$1=.*|$1=$2|" "$PARAMS_LIVE"
}

show_all() {
    local g v
    for g in "${PARAMS_GROUP_ORDER[@]}"; do
        echo "-- $g --"
        for v in ${PARAMS_GROUP_VARS[$g]}; do
            printf "  %-24s %s\n" "$v" "$(params_live_val "$v")"
        done
    done
}

edit_group() { # GROUP
    local g="$1" v cur nv
    echo "-- $g -- (Enter keeps current value)"
    for v in ${PARAMS_GROUP_VARS[$g]}; do
        cur=$(params_live_val "$v")
        read -r -p "  $v [$cur]: " nv
        [[ -n "$nv" ]] && set_live "$v" "$nv"
    done
}

while true; do
    echo
    echo "================================================================"
    echo " GAZE experiment parameters"
    echo "================================================================"
    params_show_diff
    echo
    echo "  1) FIO          2) Fileserver   3) YCSB-A   4) YCSB-F   5) TPC-C"
    echo "  6) Show all current values"
    echo "  7) Reset ALL to paper's original config"
    echo "  0) Done"
    read -r -p "> " CH
    case "$CH" in
        1) edit_group FIO ;;
        2) edit_group Fileserver ;;
        3) edit_group "YCSB-A" ;;
        4) edit_group "YCSB-F" ;;
        5) edit_group "TPC-C" ;;
        6) show_all ;;
        7) cp "$PARAMS_PAPER" "$PARAMS_LIVE"; echo "[set_params] Reset -- experiment_params.sh now matches the paper's original config." ;;
        0) break ;;
        *) echo "  -> not a valid choice." ;;
    esac
done

echo
echo "[set_params] Done. New values take effect on the next run_*.sh invocation"
echo "  (or via run_experiment.sh)."
