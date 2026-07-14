#!/bin/bash
# Timing history for run_experiment.sh — records how long each benchmark
# actually took, so the next person to run the picker sees a real measured
# estimate instead of a guess. Interrupted runs are never recorded (a
# Ctrl-C'd run says nothing about how long a full run takes).
#
# Storage: experiment_script/AE_results/timing_history.tsv
#   columns: timestamp_iso  benchmark  fs_mode  duration_sec  status  params_hash
#   status is always "completed" — interrupted runs are simply never appended.
#   params_hash comes from params_fingerprint_for_bench() (params_lib.sh) —
#   a measured duration is only offered as an estimate when it was recorded
#   under the parameters still live now; rows from before this column existed
#   have no hash and so are never matched (correctly falls back to default).
#
# Requires config.sh AND params_lib.sh to have been sourced first (uses
# $GAZE_DIR and params_fingerprint_for_bench()).

TIMING_FILE="$GAZE_DIR/experiment_script/AE_results/timing_history.tsv"

# Paper-config anchor estimates (minutes): how long each benchmark takes at
# the *paper's* parameters (paper_params.sh). FIO/Fileserver are derived from
# the runtime= values (1200s / 1600s) plus mkfs+mount+teardown overhead;
# YCSB-A/F and the TPC-C figure are what the user reported from real runs.
# These are the numbers printed when the live params still match the paper's.
declare -A TIMING_DEFAULT_MIN=(
    [FIO]=23
    [Fileserver]=29
    [YCSB-A]=33
    [YCSB-F]=40
    [TPC-C]=90
)

# Fixed (non-scaling) overhead per benchmark, in minutes: mkfs + mount +
# cold-data copy-in + teardown — the part that does NOT change when you scale
# the workload's runtime/op-count knob. The remaining (anchor - overhead) is
# the "active" portion that scales linearly with the driver param below.
# TPC-C/YCSB overheads are dominated by their large cold-MySQL-data copy.
# Only the split's *slope* depends on these; at paper params the estimate is
# always exactly the anchor above regardless of the overhead value.
declare -A TIMING_OVERHEAD_MIN=(
    [FIO]=3
    [Fileserver]=3
    [YCSB-A]=8
    [YCSB-F]=8
    [TPC-C]=60
)

# _timing_driver BENCH VALFUNC -- the single workload knob that dominates each
# benchmark's active runtime, read via VALFUNC (params_live_val/params_paper_val).
# FIO/Fileserver: the runtime= seconds. TPC-C: measured window + rampup seconds.
# YCSB: operations per thread (throughput-bound), so op-count up / threads up
# both move the estimate the right way. Secondary knobs (filesize, nfiles,
# record count's effect on copy time) are intentionally not modelled — this is
# a heads-up estimate that a single real run at the new params then replaces.
_timing_driver() {
    local bench="$1" vf="$2"
    case "$bench" in
        FIO)        "$vf" FIO_RUNTIME ;;
        Fileserver) "$vf" FILESERVER_RUNTIME ;;
        YCSB-A)     awk -v o="$("$vf" YCSBA_OPERATIONCOUNT)" -v t="$("$vf" YCSBA_THREADS)" 'BEGIN{print (t>0)? o/t : o}' ;;
        YCSB-F)     awk -v o="$("$vf" YCSBF_OPERATIONCOUNT)" -v t="$("$vf" YCSBF_THREADS)" 'BEGIN{print (t>0)? o/t : o}' ;;
        TPC-C)      awk -v r="$("$vf" TPCC_RAMPUP)" -v d="$("$vf" TPCC_DURATION)" 'BEGIN{print r+d}' ;;
    esac
}

# timing_model_minutes BENCH -- rough estimate (integer minutes) for the
# *current live* params, scaling the anchor's active portion by how far the
# live driver has moved from the paper's. At live==paper it returns the anchor
# exactly. Used only until a real measured run at these params exists.
timing_model_minutes() {
    local bench="$1"
    local anchor="${TIMING_DEFAULT_MIN[$bench]:-}"
    [[ -z "$anchor" ]] && { echo "unknown"; return; }
    local ovh="${TIMING_OVERHEAD_MIN[$bench]:-0}"
    local dl dp
    dl=$(_timing_driver "$bench" params_live_val)
    dp=$(_timing_driver "$bench" params_paper_val)
    awk -v a="$anchor" -v o="$ovh" -v dl="$dl" -v dp="$dp" 'BEGIN{
        if (dp+0 <= 0) { printf "%d", a; exit }
        est = o + (a - o) * (dl / dp);
        if (est < 1) est = 1;
        printf "%d", int(est + 0.5);
    }'
}

# timing_estimate_minutes BENCH — prints an integer minute estimate.
# Prefers the most recent *completed* run of this benchmark ON THE CURRENTLY
# SELECTED FILESYSTEM ($FS_MODE) at the current params — filesystems differ a
# lot in wall-clock (throughput-bound macros especially: GC overhead changes
# how long a fixed op-count takes). Falls back to the parameter-scaled model
# when this filesystem hasn't been run yet at these params.
timing_estimate_minutes() {
    local bench="$1"
    if [[ -f "$TIMING_FILE" ]]; then
        local hash last_sec
        hash=$(params_fingerprint_for_bench "$bench")
        # 'copy' is the pre-rename fs_mode of zns_plus; match old rows too.
        last_sec=$(awk -F'\t' -v b="$bench" -v h="$hash" -v fs="${FS_MODE:-}" \
            '$2==b && ($3==fs || (fs=="zns_plus" && $3=="copy")) && $5=="completed" && $6==h {v=$4} END{print v}' "$TIMING_FILE")
        if [[ -n "$last_sec" ]]; then
            echo "$(( (last_sec + 30) / 60 ))"
            return
        fi
    fi
    # No measured run for this filesystem at the current params: fall back to
    # the parameter-scaled model (returns the paper anchor exactly when live
    # params == paper).
    timing_model_minutes "$bench"
}

# timing_estimate_label BENCH — human-readable estimate with source tag,
# for the picker menu. Scoped to the current filesystem ($FS_MODE).
timing_estimate_label() {
    local bench="$1"
    local hash measured="" other_fs=""
    if [[ -f "$TIMING_FILE" ]]; then
        hash=$(params_fingerprint_for_bench "$bench")
        measured=$(awk -F'\t' -v b="$bench" -v h="$hash" -v fs="${FS_MODE:-}" \
            '$2==b && ($3==fs || (fs=="zns_plus" && $3=="copy")) && $5=="completed" && $6==h {v=$4} END{print v}' "$TIMING_FILE")
        # If this filesystem has no measured run yet, note whether *another*
        # filesystem does (same params) — a hint that the model number is a
        # generic estimate, not this FS's own timing.
        if [[ -z "$measured" ]]; then
            other_fs=$(awk -F'\t' -v b="$bench" -v h="$hash" -v fs="${FS_MODE:-}" \
                '$2==b && $3!=fs && !(fs=="zns_plus" && $3=="copy") && $5=="completed" && $6==h {o=$3} END{print o}' "$TIMING_FILE")
        fi
    fi
    local mins
    mins=$(timing_estimate_minutes "$bench")
    if [[ -n "$measured" ]]; then
        echo "~${mins} min (measured, last ${FS_MODE:-?} run)"
    elif params_is_default_for_bench "$bench"; then
        [[ -n "$other_fs" ]] \
            && echo "~${mins} min (paper default; no ${FS_MODE:-?} run yet, but $other_fs was measured)" \
            || echo "~${mins} min (paper default; no ${FS_MODE:-?} run yet)"
    else
        echo "~${mins} min (estimated for your params; no ${FS_MODE:-?} run yet)"
    fi
}

# timing_record BENCH FS_MODE START_EPOCH END_EPOCH — append a completed run.
# Never call this for an interrupted run.
timing_record() {
    local bench="$1" fs_mode="$2" start_ts="$3" end_ts="$4"
    local dur=$(( end_ts - start_ts ))
    local hash
    hash=$(params_fingerprint_for_bench "$bench")
    mkdir -p "$(dirname "$TIMING_FILE")"
    if [[ ! -f "$TIMING_FILE" ]]; then
        printf 'timestamp\tbenchmark\tfs_mode\tduration_sec\tstatus\tparams_hash\n' > "$TIMING_FILE"
    fi
    printf '%s\t%s\t%s\t%d\tcompleted\t%s\n' \
        "$(date -Iseconds)" "$bench" "$fs_mode" "$dur" "$hash" >> "$TIMING_FILE"
}

# timing_progress_ticker BENCH START_EPOCH EST_MIN REST_MIN [INTERVAL_SEC] —
# background heartbeat for run_experiment.sh. Every benchmark redirects its
# workload output to its result file, so without this the console is silent
# for the entire run (up to ~90 min for TPC-C). Prints one line per interval:
# elapsed vs. estimate for the running benchmark, plus the whole session's
# remaining estimate (REST_MIN = sum of the estimates of the benchmarks still
# queued after this one). The estimate is timing_estimate_minutes() — measured
# from a past run when available — so a slow run can exceed it; it then says
# "over estimate" rather than showing a negative countdown. The caller kills
# this when the benchmark exits.
timing_progress_ticker() {
    local bench="$1" start_ts="$2" est_min="$3" rest_min="$4" interval="${5:-60}"
    local now elapsed_min left_min session_left line
    while true; do
        sleep "$interval"
        now=$(date +%s)
        elapsed_min=$(( (now - start_ts) / 60 ))
        if [[ "$est_min" =~ ^[0-9]+$ ]] && (( est_min > 0 )); then
            left_min=$(( est_min - elapsed_min ))
            if (( left_min >= 0 )); then
                line="$bench: ${elapsed_min}/${est_min} min elapsed, ~${left_min} min left"
            else
                line="$bench: ${elapsed_min} min elapsed, $(( -left_min )) min over the ~${est_min} min estimate (still running)"
            fi
            # Skip the session part when it would just echo an overrun 0 —
            # (current benchmark over its estimate and nothing queued after).
            if (( rest_min > 0 || left_min > 0 )); then
                session_left=$(( (left_min > 0 ? left_min : 0) + rest_min ))
                line="$line | session: ~${session_left} min left"
            fi
        else
            line="$bench: ${elapsed_min} min elapsed (no estimate for this config yet)"
        fi
        echo "[progress $(date '+%H:%M:%S')] $line"
    done
}
