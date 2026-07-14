#!/bin/bash
# Integrated experiment runner for artifact evaluation.
#
# Reminds you which filesystem is currently selected, asks which
# benchmark(s) to run (with time estimates — measured from past runs once
# any exist, otherwise a default), runs them back to back, then builds
# both the "this session only" and "merged with paper baseline" versions
# of the paper-Fig-17/18-style comparison figures and tells you exactly
# how to view them.
#
# Usage: ./run_experiment.sh
#
# To change filesystem: experiment_script/select_fs.sh (before running this).
# GC-latency-breakdown figures (paper Fig 5/14/16) need PROFILE_MODE=on;
# profile-mode FIO runs build them automatically (see README §3.5).

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../config.sh"
source "$GENERAL_RES/lib.sh"
source "$GENERAL_RES/params_lib.sh"
source "$GENERAL_RES/timing_lib.sh"
source "$GENERAL_RES/cold_data_lib.sh"

# Refuse a filesystem/branch mismatch up front (iplfs lives on branch
# 'iplfs' with its own kernel/emulator/mkfs sources; everything else runs
# from 'main'). setup_fs.sh re-checks per benchmark; failing here is nicer.
require_branch_for_fs_mode

BENCH_ORDER=(FIO Fileserver YCSB-A YCSB-F TPC-C)
declare -A BENCH_DIR=(
    [FIO]="$SCRIPT_DIR/FIO"
    [Fileserver]="$SCRIPT_DIR/Fileserver"
    [YCSB-A]="$SCRIPT_DIR/YCSB-A"
    [YCSB-F]="$SCRIPT_DIR/YCSB-F"
    [TPC-C]="$SCRIPT_DIR/TPC-C"
)
declare -A BENCH_SCRIPT=(
    [FIO]=run_fio.sh
    [Fileserver]=run_fileserver.sh
    [YCSB-A]=run_ycsba.sh
    [YCSB-F]=run_ycsbf.sh
    [TPC-C]=run_tpcc.sh
)

INTERRUPTED=0
TICKER_PID=""
on_interrupt() {
    [[ -n "$TICKER_PID" ]] && kill "$TICKER_PID" 2>/dev/null
    echo
    echo "[run_experiment] Interrupted — this benchmark's own cleanup trap"
    echo "[run_experiment]   should already have unmounted/torn things down;"
    echo "[run_experiment]   running one more safety-net cleanup pass..."
    INTERRUPTED=1
    "$GENERAL_RES/preflight_cleanup.sh"
    echo "[run_experiment] Stopped. Nothing after the interrupted benchmark was run,"
    echo "[run_experiment]   and its time was not recorded (interrupted runs don't"
    echo "[run_experiment]   count toward future time estimates)."
    exit 130
}
trap on_interrupt INT TERM

echo "================================================================"
echo " GAZE integrated experiment runner"
echo "================================================================"
echo "Current filesystem under test: $FS_NAME  (module: $FS_KO)"
if [[ "$PROFILE_MODE" == "on" ]]; then
    echo "Profiling: ON — the FIO run will also rebuild the GC-latency-"
    echo "  breakdown figures (paper Fig 5/14/16) into AE_results/profile/."
fi
echo "To change filesystem: $SCRIPT_DIR/select_fs.sh (run that first, then re-run this)."
PARAMS_DIFF_OUT="$(params_show_diff)"
if [[ "$PARAMS_DIFF_OUT" != "Matches paper config exactly." ]]; then
    echo "Workload params -- $PARAMS_DIFF_OUT" | sed '2,$s/^/  /'
    echo "  -> not the paper's config; durations/results may not match the paper."
    echo "  To review/reset: $SCRIPT_DIR/set_params.sh"
else
    echo "Workload params: match the paper's config exactly."
fi
echo

echo "[run_experiment] Pre-flight cleanup (in case a previous session was"
echo "  interrupted and left mysql running, $MNT mounted, or a stray process)..."
"$GENERAL_RES/preflight_cleanup.sh"
echo

# =============================================================================
# Picker
# =============================================================================
echo "Which benchmark(s) do you want to run?"
echo "(per-run time estimates below are for the current filesystem, $FS_NAME:"
echo "  'measured, last $FS_NAME run' = timed from a past completed $FS_NAME run at these params"
echo "  'paper default' / 'estimated for your params' = model estimate; $FS_NAME"
echo "     hasn't been run at these params yet, so this is not $FS_NAME's own timing)"
echo
i=1
declare -A IDX_TO_BENCH
for b in "${BENCH_ORDER[@]}"; do
    printf "  %d) %-11s %s\n" "$i" "$b" "$(timing_estimate_label "$b")"
    IDX_TO_BENCH[$i]="$b"
    i=$((i+1))
done
echo "  a) All of the above (sequential — total: sum of the above)"
echo "  b) Ablation sweep — FIO across the 8 technique combinations"
echo "     (paper Fig 15; ~21 min per mode, needs all ablation .ko's built)"
echo
echo "Enter numbers separated by spaces (e.g. '1 3 4'), 'a' for all, 'b' for ablation:"
read -r -p "> " PICK

if [[ "$PICK" =~ ^[Bb]$ ]]; then
    exec "$SCRIPT_DIR/run_ablation.sh"
fi

SELECTED=()
if [[ "$PICK" =~ ^[Aa]$ ]]; then
    SELECTED=("${BENCH_ORDER[@]}")
else
    for tok in $PICK; do
        if [[ -n "${IDX_TO_BENCH[$tok]:-}" ]]; then
            SELECTED+=("${IDX_TO_BENCH[$tok]}")
        else
            echo "[run_experiment] Ignoring unrecognized selection: $tok"
        fi
    done
fi

if [[ "${#SELECTED[@]}" -eq 0 ]]; then
    echo "[run_experiment] Nothing selected. Exiting."
    exit 0
fi

# Run in canonical order regardless of typed order, for predictability.
ORDERED_SELECTED=()
for b in "${BENCH_ORDER[@]}"; do
    for s in "${SELECTED[@]}"; do
        [[ "$s" == "$b" ]] && ORDERED_SELECTED+=("$b")
    done
done

# YCSB/TPC-C need their preloaded cold data (MySQL datadirs + fillers).
# Drop any selected benchmark whose data is missing, up front and loudly —
# generating it is a one-time step (experiment_script/make_cold_data.sh),
# after which they are selectable again.
COLD_OK_SELECTED=()
for b in "${ORDERED_SELECTED[@]}"; do
    case "$b" in
        YCSB-A|YCSB-F) ycsb_cold_data_ok && COLD_OK_SELECTED+=("$b")             || echo "[run_experiment] $b removed from this session (cold data missing — see above)." ;;
        TPC-C)         tpcc_cold_data_ok && COLD_OK_SELECTED+=("$b")             || echo "[run_experiment] $b removed from this session (cold data missing — see above)." ;;
        *)             COLD_OK_SELECTED+=("$b") ;;
    esac
done
ORDERED_SELECTED=("${COLD_OK_SELECTED[@]}")
if [[ "${#ORDERED_SELECTED[@]}" -eq 0 ]]; then
    echo "[run_experiment] Nothing left to run. Generate the cold data once with:"
    echo "[run_experiment]   $SCRIPT_DIR/make_cold_data.sh"
    exit 1
fi

TOTAL_MIN=0
for b in "${ORDERED_SELECTED[@]}"; do
    est=$(timing_estimate_minutes "$b")
    [[ "$est" =~ ^[0-9]+$ ]] && TOTAL_MIN=$((TOTAL_MIN + est))
done
echo
echo "Will run: ${ORDERED_SELECTED[*]}"
echo "Estimated total: ~${TOTAL_MIN} min"
read -r -p "Proceed? [Y/n]: " CONFIRM
[[ "$CONFIRM" =~ ^[Nn]$ ]] && { echo "Cancelled."; exit 0; }

# Fail fast if sudo can't run the selected benchmark entrypoints from THIS
# copy of the repo. On a full-sudo account this at worst asks for the
# password here (once, cached) instead of mid-run. On a restricted NOPASSWD
# account (SETUP.md "Known gotchas") whose whitelist pins a different
# clone's absolute path, sudo would otherwise fall back to a password
# prompt and then deny with a cryptic "not allowed to execute ./run_X.sh"
# only after setup already started — surface that as an actionable error
# before anything runs.
for b in "${ORDERED_SELECTED[@]}"; do
    entry="${BENCH_DIR[$b]}/${BENCH_SCRIPT[$b]}"
    sudo -n -l "$entry" >/dev/null 2>&1 && continue    # NOPASSWD covers it
    sudo -l "$entry" >/dev/null 2>&1 && continue       # full sudo; password now cached
    echo "[run_experiment] sudo refuses to run this repo copy's benchmark entrypoint:"
    echo "    $entry"
    echo "  If this account uses a restricted NOPASSWD sudoers list, that list is"
    echo "  probably pinned to a different clone's absolute path — extend it to"
    echo "  cover this location (check with: sudo -n -l $entry )."
    echo "  Otherwise run from an account with working sudo. See SETUP.md,"
    echo "  'Known gotchas'."
    exit 1
done

# =============================================================================
# Run
# =============================================================================
COMPLETED=()
FAILED=""
for b in "${ORDERED_SELECTED[@]}"; do
    dir="${BENCH_DIR[$b]}"
    script="${BENCH_SCRIPT[$b]}"
    echo
    echo "================================================================"
    echo " Running $b  ($(date '+%H:%M:%S'))"
    echo "================================================================"

    "$GENERAL_RES/preflight_cleanup.sh" > /dev/null

    # Progress heartbeat: the benchmark scripts redirect the workload's own
    # output to their result files, so the console would otherwise stay
    # silent for the whole run. rest_min = estimates of what is still queued
    # after this benchmark, for a session-wide "time left".
    est_min=$(timing_estimate_minutes "$b")
    rest_min=0
    past_current=0
    for later in "${ORDERED_SELECTED[@]}"; do
        if [[ $past_current -eq 0 ]]; then
            [[ "$later" == "$b" ]] && past_current=1
            continue
        fi
        e=$(timing_estimate_minutes "$later")
        [[ "$e" =~ ^[0-9]+$ ]] && rest_min=$((rest_min + e))
    done
    timing_progress_ticker "$b" "$(date +%s)" "$est_min" "$rest_min" &
    TICKER_PID=$!

    start_ts=$(date +%s)
    ( cd "$dir" && sudo ./"$script" )
    rc=$?
    end_ts=$(date +%s)

    kill "$TICKER_PID" 2>/dev/null
    wait "$TICKER_PID" 2>/dev/null
    TICKER_PID=""

    if [[ $rc -eq 0 ]]; then
        timing_record "$b" "$FS_MODE" "$start_ts" "$end_ts"
        dur=$(( (end_ts - start_ts + 30) / 60 ))
        echo "[run_experiment] $b completed in ~${dur} min."
        COMPLETED+=("$b")
    elif [[ $rc -eq 130 ]]; then
        # on_interrupt's trap already ran (or is about to via the deferred
        # signal); nothing more to do here — this branch mainly exists so
        # a 130 from the child itself (not a live Ctrl-C, e.g. a resumed
        # session) is also treated as "not recorded, stop here".
        echo "[run_experiment] $b was interrupted — not recorded, stopping."
        FAILED="$b"
        break
    else
        echo "[run_experiment] $b exited with code $rc (not interrupted — a real"
        echo "  failure). Not recorded. Stopping so you can investigate before"
        echo "  the next benchmark reuses a possibly-inconsistent \$MNT."
        "$GENERAL_RES/preflight_cleanup.sh"
        FAILED="$b"
        break
    fi
done

echo
if [[ -n "$FAILED" ]]; then
    echo "[run_experiment] Stopped after $FAILED. Completed before that: ${COMPLETED[*]:-none}."
else
    echo "[run_experiment] All selected benchmarks completed: ${COMPLETED[*]}"
fi

if [[ "${#COMPLETED[@]}" -eq 0 ]]; then
    echo "[run_experiment] Nothing completed — skipping graph generation."
    exit 1
fi

# =============================================================================
# Graphs
# =============================================================================
echo
echo "[run_experiment] Building comparison figures (this-run-only + merged-with-baseline)..."
"$SCRIPT_DIR/AE_results/build_graphs.sh"

# =============================================================================
# View
# =============================================================================
echo
echo "----------------------------------------------------------------"

show_paths() { # solo|merged
    local kind="$1" label="$2"
    echo
    echo "$label:"
    echo "  FIO:    $SCRIPT_DIR/AE_results/$kind/fio/   (Fig-17 FIO throughput)"
    echo "  Macro:  $SCRIPT_DIR/AE_results/$kind/macro/ (Fig-18 macrobenchmark throughput, keys.eps legend)"
}

VIEW_DIRS=()
if [[ "${PAPER_ENV:-0}" != "1" ]]; then
    # Non-paper env: build_graphs.sh only produces solo/ (paper baseline
    # lives on a different y-scale, cannot be merged). Skip the prompt
    # entirely and point straight at solo/ so the user isn't offered a
    # 'merged' option that would resolve to an empty directory.
    echo "Environment mismatch: SSD_GB=${SSD_GB:-?}/DRAM_GB=${DRAM_GB:-?} (paper: $PAPER_SSD_GB/$PAPER_DRAM_GB)."
    echo "  Paper-baseline series can't share a graph with this run's scale, so only"
    echo "  the 'this session only' (solo) figures were built — showing those."
    show_paths solo "This session only (local runs from this machine)"
    VIEW_DIRS=(solo)
else
    echo "Which version do you want to look at?"
    echo "  1) This session's results only (dummy 0 for anything not run)"
    echo "  2) Merged with the paper's baseline data"
    echo "  3) Both"
    read -r -p "> " VIEW_PICK

    case "$VIEW_PICK" in
        1) show_paths solo "This session only"; VIEW_DIRS=(solo) ;;
        2) show_paths merged "Merged with baseline"; VIEW_DIRS=(merged) ;;
        3|*) show_paths solo "This session only"; show_paths merged "Merged with baseline"; VIEW_DIRS=(solo merged) ;;
    esac
fi

echo
echo "Each .eps also has a rendered .png alongside it (same name), for viewing"
echo "without needing X / gv."
echo

# --- Display detection: can we just pop gv open right here? ---
CAN_DISPLAY=0
if [[ -n "${DISPLAY:-}" ]] && command -v xset > /dev/null 2>&1 && xset q > /dev/null 2>&1; then
    CAN_DISPLAY=1
fi

if [[ "$CAN_DISPLAY" -eq 1 ]]; then
    echo "[run_experiment] X display detected ($DISPLAY) — this looks like a local"
    echo "  console / iDRAC-attached desktop session, not a plain SSH login."
    echo "  Opening the figures in gv now (close the window(s) when done)."
    for kind in "${VIEW_DIRS[@]}"; do
        for sub in fio macro; do
            # open only the paper figures + legend; Supp-* latency figures
            # stay on disk but are not popped up
            eps_files=("$SCRIPT_DIR/AE_results/$kind/$sub/"Fig-*.eps
                       "$SCRIPT_DIR/AE_results/$kind/$sub/"keys.eps)
            existing=()
            for f in "${eps_files[@]}"; do [[ -e "$f" ]] && existing+=("$f"); done
            [[ ${#existing[@]} -gt 0 ]] && gv "${existing[@]}" > /dev/null 2>&1 &
        done
    done
else
    echo "[run_experiment] No usable X display from this session (this is the"
    echo "  normal case over plain SSH — gv can't pop up here). To view the"
    echo "  figures, pick one:"
    echo
    echo "  1) Use the server's iDRAC virtual console (its address is in the"
    echo "     server access doc): open it in your web browser,"
    echo "     log in, open the Virtual Console, log into the desktop session"
    echo "     there, and open the figure paths above (they are .png files;"
    echo "     any image viewer or 'gv' for the .eps versions works)."
    echo
    echo "  2) Copy the files to your LOCAL machine and open them there (no X needed):"
    echo "       scp -r $(whoami)@$(hostname -I 2>/dev/null | awk '{print $1}'):$SCRIPT_DIR/AE_results/{solo,merged} ."
    echo "     then open the .png files (or the .eps ones, with any EPS-capable viewer)."
    echo
    echo "  3) SSH with X11 forwarding, then run gv yourself:"
    echo "       ssh -X $(whoami)@$(hostname -I 2>/dev/null | awk '{print $1}')"
    echo "       gv <path-from-above>/*.eps"
    echo
    echo "  4) If you're already inside the iDRAC virtual console but still saw"
    echo "     this message, a desktop session may not be logged in / \$DISPLAY"
    echo "     isn't exported to this shell — log into the desktop session"
    echo "     first, or run 'export DISPLAY=:0' if one is already running,"
    echo "     then re-run the view step (or just: gv <path-from-above>/*.eps)."
fi
