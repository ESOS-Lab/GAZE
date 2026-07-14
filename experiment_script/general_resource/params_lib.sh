#!/bin/bash
# Shared helpers for reading/comparing experiment_params.sh vs
# paper_params.sh. Used by set_params.sh (editing) and run_experiment.sh
# (a heads-up banner if the live config has drifted from the paper's).
#
# Requires GENERAL_RES to already be set (config.sh sets it).

PARAMS_LIVE="$GENERAL_RES/experiment_params.sh"
PARAMS_PAPER="$GENERAL_RES/paper_params.sh"

PARAMS_GROUP_ORDER=(FIO Fileserver YCSB-A YCSB-F TPC-C)
declare -A PARAMS_GROUP_VARS=(
    [FIO]="FIO_BLOCK_SIZE FIO_IO_TYPE FIO_NUM_JOBS FIO_RUNTIME FIO_FILESIZE FIO_GC_START_SEC"
    [Fileserver]="FILESERVER_NFILES FILESERVER_MEANDIRWIDTH FILESERVER_NTHREADS FILESERVER_IOSIZE FILESERVER_MEANAPPENDSIZE FILESERVER_RUNTIME FILESERVER_GC_START_SEC"
    [YCSB-A]="YCSBA_RECORDSIZE YCSBA_RECORDCOUNT YCSBA_OPERATIONCOUNT YCSBA_THREADS"
    [YCSB-F]="YCSBF_RECORDSIZE YCSBF_RECORDCOUNT YCSBF_OPERATIONCOUNT YCSBF_THREADS"
    [TPC-C]="TPCC_WAREHOUSES TPCC_CLIENTS TPCC_RAMPUP TPCC_DURATION"
)

# params_live_val/params_paper_val VAR -- sourcing each file in a subshell
# (rather than grep-parsing it) is what makes this robust to
# comment/formatting differences between the two files; both just need to
# define the same shell variable names, which they do by construction.
params_live_val()  { ( source "$PARAMS_LIVE";  eval "echo \"\${$1:-}\"" ); }
params_paper_val() { ( source "$PARAMS_PAPER"; eval "echo \"\${$1:-}\"" ); }

# params_show_diff -- prints every variable whose live value differs from
# the paper's; "Matches paper config exactly." if none do.
params_show_diff() {
    local any=0 g v live paper
    for g in "${PARAMS_GROUP_ORDER[@]}"; do
        for v in ${PARAMS_GROUP_VARS[$g]}; do
            live=$(params_live_val "$v"); paper=$(params_paper_val "$v")
            if [[ "$live" != "$paper" ]]; then
                [[ "$any" -eq 0 ]] && echo "Differs from paper config:"
                printf "  %-24s paper=%-12s live=%s\n" "$v" "$paper" "$live"
                any=1
            fi
        done
    done
    [[ "$any" -eq 0 ]] && echo "Matches paper config exactly."
    return "$any"
}

# _params_fingerprint GROUP VALFUNC -- short hash of that group's values as
# read by VALFUNC (params_live_val or params_paper_val).
_params_fingerprint() {
    local bench="$1" valfunc="$2" v vals=""
    for v in ${PARAMS_GROUP_VARS[$bench]}; do
        vals+="$v=$($valfunc "$v");"
    done
    printf '%s' "$vals" | md5sum | cut -c1-8
}

# params_fingerprint_for_bench GROUP -- short hash of that group's current
# live values. timing_lib.sh tags each recorded run with this so a
# "measured" duration is only reused when it was measured under the params
# still in effect now (e.g. a shortened smoke-test run must not be quoted
# as the estimate after set_params.sh resets things back to the paper config).
params_fingerprint_for_bench() { _params_fingerprint "$1" params_live_val; }

# params_fingerprint_paper_for_bench GROUP -- same, for the paper's config.
# Lets callers tell "live == paper default" from "user changed these params".
params_fingerprint_paper_for_bench() { _params_fingerprint "$1" params_paper_val; }

# params_is_default_for_bench GROUP -- succeeds (0) iff this group's live
# values are identical to the paper's config.
params_is_default_for_bench() {
    [[ "$(params_fingerprint_for_bench "$1")" == "$(params_fingerprint_paper_for_bench "$1")" ]]
}
