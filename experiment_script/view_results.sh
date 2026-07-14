#!/bin/bash
# Interactive viewer for the paper-style figures.
#
# Two figure families:
#   benchmark  FIO (paper Fig 17) and macrobenchmark (paper Fig 18)
#              throughput comparisons, built by AE_results/build_graphs.sh
#              (file names carry the paper figure numbers: Fig-17 FIO,
#               Fig-18 macro)
#   profile    GC-latency-breakdown figures from profile-kernel runs
#              (paper Fig 5 phase + Fig 14 ablation + Fig 16 sota; README §3.5)
#
# It first asks which family (default = profile when the tree is currently
# in profile mode, benchmark otherwise). For benchmark it then asks:
#   1) all filesystems (the comparison figure with every FS you've run
#      overlaid), or just one filesystem on its own?
#   2) if one: which filesystem? (default = the one you ran most recently)
#   3) which result set: FIO, macro, or both?
#
# Figures are always (re)built fresh from whatever run data is on disk first,
# so this reflects your latest runs without having to remember to rebuild.
# Per-filesystem results accumulate — running a new FS never drops an old
# one from the "all" figure (see AE_results/build_graphs.sh).
#
# Usage:
#   ./view_results.sh                 # fully interactive
#   ./view_results.sh profile         # profile figures (paper Fig 5 + 14 + 16)
#   ./view_results.sh ablation        # ablation throughput figure (paper Fig 15)
#   ./view_results.sh all fio         # non-interactive benchmark shortcuts
#   ./view_results.sh gaze both
#   ./view_results.sh f2fs macro

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../config.sh"

AE="$SCRIPT_DIR/AE_results"
BUILD="$AE/build_graphs.sh"
TIMING="$AE/timing_history.tsv"

# Filesystems that feed these figures (same set build_graphs.sh knows about).
KNOWN_FS=(gaze f2fs pgc zns_plus scalelfs iplfs)

# --- which filesystems actually have run data on disk -----------------------
fs_has_data() { # MODE
    local b m="$1"
    for b in FIO Fileserver YCSB-A YCSB-F TPC-C; do
        compgen -G "$SCRIPT_DIR/$b/${m}_data/*/" >/dev/null 2>&1 && return 0
        # zns_plus used to be called 'copy'; old runs live in copy_data/
        [[ "$m" == "zns_plus" ]] && \
            compgen -G "$SCRIPT_DIR/$b/copy_data/*/" >/dev/null 2>&1 && return 0
    done
    return 1
}
RUN_FS=()
for m in "${KNOWN_FS[@]}"; do fs_has_data "$m" && RUN_FS+=("$m"); done

require_benchmark_runs() {
    if [[ "${#RUN_FS[@]}" -eq 0 ]]; then
        echo "[view_results] No benchmark runs found yet under */<fs>_data/." >&2
        echo "[view_results]   Run a benchmark first (e.g. ./run_experiment.sh)." >&2
        exit 1
    fi
}

# --- most recently run filesystem (menu default) ----------------------------
recent_fs() {
    # Prefer the timing log's last completed row; fall back to the newest
    # run directory's mtime across all benchmarks.
    if [[ -f "$TIMING" ]]; then
        local last
        last=$(awk -F'\t' '$5=="completed"{v=$3} END{print v}' "$TIMING")
        if [[ -n "$last" ]]; then
            local f
            for f in "${RUN_FS[@]}"; do [[ "$f" == "$last" ]] && { echo "$last"; return; }; done
        fi
    fi
    local newest="" mode
    for mode in "${RUN_FS[@]}"; do
        local d
        d=$(ls -td "$SCRIPT_DIR"/*/"${mode}_data"/*/ 2>/dev/null | head -1)
        [[ -n "$d" ]] || continue
        if [[ -z "$newest" || "$d" -nt "$newest" ]]; then newest="$d"; RECENT="$mode"; fi
    done
    echo "${RECENT:-${RUN_FS[0]}}"
}
DEFAULT_FS=""
[[ "${#RUN_FS[@]}" -gt 0 ]] && DEFAULT_FS=$(recent_fs)

# --- open images (best effort; always prints the paths) ---------------------
open_images() {
    local any=0 f
    for f in "$@"; do
        [[ -f "$f" ]] || continue
        echo "    $f"
        if [[ -n "${DISPLAY:-}" ]]; then
            (xdg-open "$f" >/dev/null 2>&1 &) 2>/dev/null
        fi
        any=1
    done
    if [[ "$any" -eq 0 ]]; then
        echo "    (no figure produced — that benchmark/category may not have been run)"
    elif [[ -z "${DISPLAY:-}" ]]; then
        echo "    (no \$DISPLAY here — to view the figures at the paths above, either:"
        echo "     - open the server's iDRAC virtual console (its address is in the"
        echo "       access doc), open the Virtual Console, log into the desktop; or"
        echo "     - copy them to your local machine, e.g.:"
        echo "       scp $(whoami)@$(hostname -I 2>/dev/null | cut -d' ' -f1):'<path-above>' . )"
    fi
}

# --- ablation throughput figure (paper Fig 15; README §3.4) ------------------
show_ablation_figures() {
    local A="$AE/ablation_baseline" ok=0
    echo
    echo "[view_results] Rebuilding the ablation figure (newest FIO run per"
    echo "[view_results] technique combination, paper data for the rest):"
    "$A/make_graph.sh" || { echo "[view_results] ablation figure failed." >&2; ok=1; }
    echo
    echo "[view_results] paper Fig 15 — FIO Throughput under Individual Techniques"
    echo "[view_results] (legend + avg + stddev):"
    open_images "$A/Fig-15.FIO_throughput_under_individual_techniques.legend.png" \
                "$A/Fig-15.FIO_throughput_under_individual_techniques.avg.png" \
                "$A/Fig-15.FIO_throughput_under_individual_techniques.stddev.png"
    if [[ -f "$A/SOURCES.txt" ]]; then
        echo "    Data sources per bar ([NEW] = generated here, [OLD] = SOSP26 paper data):"
        grep -v '^#' "$A/SOURCES.txt" | sed 's/^/      /'
    fi
    echo "  (to produce fresh bars: ./run_ablation.sh — see README §3.4)"
    exit "$ok"
}

# --- profile figures (paper Fig 5 + 14 + 16; README §3.5) --------------------
show_profile_figures() {
    local P="$AE/profile" ok=0
    echo
    echo "[view_results] Rebuilding the profile figures (newest profile FIO run per"
    echo "[view_results] mode, archived paper run for the rest — see SOURCES.txt):"
    "$AE/collect_profile_figures.sh" || { echo "[view_results] profile figure rebuild failed." >&2; ok=1; }
    echo
    echo "[view_results] Profile figures (in AE_results/profile/, file names carry"
    echo "[view_results] the paper figure numbers):"
    if [[ -f "$P/SOURCES.txt" ]]; then
        echo "  Data source per bar ('fresh ->' = [NEW] generated here,"
        echo "  'archive (paper run)'/'paper mode' = [OLD] SOSP26 paper data):"
        grep -v '^#' "$P/SOURCES.txt" | sed 's/^/    /'
    fi
    echo "  paper Fig 5 — Latency Breakdown of GC (a: Overall, b: Read Phase, c: Write Phase):"
    open_images "$P/Fig-05a.GC_latency_breakdown.Overall_GC.png" \
                "$P/Fig-05b.GC_latency_breakdown.GC_Read_Phase.png" \
                "$P/Fig-05c.GC_latency_breakdown.GC_Write_Phase.png"
    echo "  paper Fig 14 — GC Latency under Individual Techniques:"
    open_images "$P/Fig-14.GC_latency_under_individual_techniques.png"
    echo "  paper Fig 16 — GC Latency Breakdown across Different Filesystems (+legend):"
    open_images "$P/Fig-16.GC_latency_breakdown_across_filesystems.png" \
                "$P/Fig-16-legend.sota_gc_lat_breakdown_keys.png"
    echo "  (paper-baseline versions: $P/paper_baseline/)"
    exit "$ok"
}

# =============================================================================
# Resolve selection (args if given, else interactive)
# =============================================================================
SCOPE=""   # "all" or a specific FS mode
CATEGORY="" # fio | macro | both

if [[ $# -ge 1 ]]; then
    case "$1" in
        profile) show_profile_figures ;;   # exits inside
        ablation) show_ablation_figures ;; # exits inside
        all) SCOPE=all ;;
        gaze|f2fs|pgc|zns_plus|scalelfs|iplfs) SCOPE="$1" ;;
        *) echo "[view_results] unknown scope '$1' (want: profile | ablation | all | ${RUN_FS[*]})" >&2; exit 1 ;;
    esac
    require_benchmark_runs
    CATEGORY="${2:-both}"
else
    echo
    echo "================================================================"
    echo " GAZE result viewer"
    echo "================================================================"
    echo
    echo "Which figures?"
    echo "  1) Benchmark results   (FIO: paper Fig 17, macro: paper Fig 18,"
    echo "                          ablation: paper Fig 15)"
    echo "  2) Profile figures     (GC latency breakdown: paper Fig 5, 14, 16)"
    # Default to the family matching the tree's current profiling state.
    FAM_DEFAULT=1; [[ "${PROFILE_MODE:-off}" == "on" ]] && FAM_DEFAULT=2
    read -r -p "> [$FAM_DEFAULT] " FAM; FAM="${FAM:-$FAM_DEFAULT}"
    [[ "$FAM" == "2" ]] && show_profile_figures   # exits inside
    require_benchmark_runs
    echo
    echo "Filesystems with results on disk: ${RUN_FS[*]}"
    echo "Most recently run: $DEFAULT_FS"
    echo
    echo "  1) All filesystems together (comparison figure)"
    echo "  2) Just one filesystem"
    echo "  3) Ablation study (paper Fig 15 — FIO under individual techniques)"
    read -r -p "> [1] " S; S="${S:-1}"
    [[ "$S" == "3" ]] && show_ablation_figures   # exits inside
    if [[ "$S" == "2" ]]; then
        echo
        echo "Which filesystem? (Enter = $DEFAULT_FS)"
        i=1; declare -A IDX
        for m in "${RUN_FS[@]}"; do printf "  %d) %s\n" "$i" "$m"; IDX[$i]="$m"; i=$((i+1)); done
        read -r -p "> [$DEFAULT_FS] " P
        if [[ -z "$P" ]]; then SCOPE="$DEFAULT_FS"
        elif [[ "$P" =~ ^[0-9]+$ && -n "${IDX[$P]:-}" ]]; then SCOPE="${IDX[$P]}"
        else
            SCOPE=""
            for m in "${RUN_FS[@]}"; do [[ "$m" == "$P" ]] && SCOPE="$P"; done
            [[ -z "$SCOPE" ]] && { echo "[view_results] '$P' has no results."; exit 1; }
        fi
    else
        SCOPE=all
    fi
    echo
    echo "Which results?"
    echo "  1) FIO       (paper Fig 17)"
    echo "  2) Macro     (paper Fig 18)"
    echo "  3) Both"
    read -r -p "> [3] " C; C="${C:-3}"
    case "$C" in 1) CATEGORY=fio ;; 2) CATEGORY=macro ;; *) CATEGORY=both ;; esac
fi
case "$CATEGORY" in fio|macro|both) ;; *) CATEGORY=both ;; esac

# =============================================================================
# Build the figures fresh, then open them
# =============================================================================
if [[ "$SCOPE" == "all" ]]; then
    echo
    if [[ "${PAPER_ENV:-0}" == "1" ]]; then
        echo "[view_results] Building the figures from your latest runs, merged over the"
        echo "[view_results] paper baselines (filesystems you haven't run are filled in"
        echo "[view_results] from the SOSP26 paper data)..."
        "$BUILD" >/dev/null 2>&1
        OUTFIO="$AE/merged/fio"; OUTMACRO="$AE/merged/macro"
        echo "[view_results] Fresh runs on disk: ${RUN_FS[*]}"
        echo "[view_results]   (your-runs-only version, nothing paper-provided: $AE/solo/)"
    else
        echo "[view_results] ============================================================"
        echo "[view_results]  ENVIRONMENT MISMATCH — not the paper machine's config"
        echo "[view_results]  (SSD_GB=${SSD_GB:-?} vs paper $PAPER_SSD_GB, DRAM_GB=${DRAM_GB:-?} vs paper $PAPER_DRAM_GB)"
        echo "[view_results]  Paper baseline data lives on a DIFFERENT y-scale, so it is"
        echo "[view_results]  NOT merged. Showing solo/ (your runs only), y-axis autoscaled."
        echo "[view_results] ============================================================"
        "$BUILD" >/dev/null 2>&1
        OUTFIO="$AE/solo/fio"; OUTMACRO="$AE/solo/macro"
        echo "[view_results] Fresh runs on disk: ${RUN_FS[*]}"
    fi
else
    echo
    echo "[view_results] Building the $SCOPE-only figure..."
    VIEWROOT="$AE/view/$SCOPE"
    rm -rf "$VIEWROOT"; mkdir -p "$VIEWROOT"
    INCLUDE_FS_MODES="$SCOPE" GRAPH_KINDS=solo OUT_ROOT="$VIEWROOT" "$BUILD" >/dev/null 2>&1
    OUTFIO="$VIEWROOT/solo/fio"; OUTMACRO="$VIEWROOT/solo/macro"
    echo "[view_results] Showing $SCOPE only."
fi

# Show only the paper figures (Fig-*) and the legend.
show_sources() {  # DIR — per-series data provenance written by build_graphs.sh
    [[ -f "$1/SOURCES.txt" ]] || return 0
    echo "    Data sources per series ([NEW] = generated here, [OLD] = SOSP26 paper data):"
    grep -v '^#' "$1/SOURCES.txt" | sed 's/^/      /'
}
if [[ "$CATEGORY" == "fio" || "$CATEGORY" == "both" ]]; then
    echo "[view_results] FIO figures (paper Fig 17):"
    open_images "$OUTFIO"/Fig-*.png
    show_sources "$OUTFIO"
fi
if [[ "$CATEGORY" == "macro" || "$CATEGORY" == "both" ]]; then
    echo "[view_results] Macro figures (paper Fig 18 + legend):"
    open_images "$OUTMACRO"/Fig-*.png "$OUTMACRO"/keys.png
    show_sources "$OUTMACRO"
fi
