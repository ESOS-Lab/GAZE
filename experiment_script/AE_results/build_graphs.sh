#!/bin/bash
# Builds the paper-style comparison figures (paper Fig 17 FIO throughput +
# paper Fig 18 macro throughput, plus the Supp-* latency figures) in two flavors:
#
#   solo/   — only filesystems that have actually been run appear as real
#             data; every other series is a dummy zero (per request: don't
#             fabricate numbers for filesystems that weren't run).
#   merged/ — each filesystem's freshest local run replaces the corresponding
#             paper baseline series; everything not run locally keeps the
#             original paper baseline data.
#
# ACCUMULATION (the important part): this script always rebuilds *every*
# filesystem's series from whatever run data is on disk under
# FIO/<mode>_data/ and <macro>/<mode>_data/ — NOT just the mode that happens
# to be selected right now. So running gaze, then switching to f2fs and
# running that, leaves BOTH series in the figures (each showing its own
# latest run). Switching filesystems never discards a previously-run one.
#
# The .gpi files themselves are never modified — they're the exact ones
# used to produce the SOSP paper's benchmark figures, copied verbatim into
# fio_baseline/ and macro_baseline/. This script only stages which *_data
# directories are visible to make_graph.sh (also copied verbatim) before
# running it; make_graph.sh's own `[ -s FILE ]` / awk fallbacks already
# turn a missing/empty result file into a 0, so an empty dummy file is a
# real, script-native "no data" — not a hack layered on top.
#
# GC-latency-breakdown / phase-breakdown figures (Fig-19+) are out of
# scope here — those need PROFILE_MODE=on kernel builds and are handled
# separately once profiling is in scope.
#
# Usage: ./build_graphs.sh   (run after run_experiment.sh finishes, or
#                              standalone any time to refresh figures from
#                              whatever has been run so far)
#
# Optional env knobs (used by view_results.sh to render a filtered view;
# defaults reproduce the normal "accumulate everything" behaviour):
#   INCLUDE_FS_MODES  space-separated FS modes to stage as real data
#                     (default: gaze f2fs pgc zns_plus scalelfs = all of them).
#                     A mode left out falls back to dummy (solo) / paper
#                     baseline (merged), exactly as if it had never been run.
#   GRAPH_KINDS       which flavors to build (default: "solo merged").
#   OUT_ROOT          where solo/ and merged/ are written (default: this dir).

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../../config.sh"

FIO_DIR="$GAZE_DIR/experiment_script/FIO"
MACRO_BENCHES=(Fileserver YCSB-A YCSB-F TPC-C)

INCLUDE_FS_MODES="${INCLUDE_FS_MODES:-gaze f2fs pgc zns_plus scalelfs iplfs}"
# Paper-baseline data lives on the paper machine's y-scale (255G SSD, paper
# workload params). When the current config is not paper (e.g. auto mode on
# a smaller machine), those series can't share a graph with local runs, so
# we drop 'merged' silently in that case -- see PAPER_ENV handling below.
DEFAULT_GRAPH_KINDS="solo merged"
[[ "${PAPER_ENV:-0}" == "1" ]] || DEFAULT_GRAPH_KINDS="solo"
GRAPH_KINDS="${GRAPH_KINDS:-$DEFAULT_GRAPH_KINDS}"
# Non-paper env: refuse a merged request instead of silently producing a
# wrong-scale figure. Caller (view_results.sh) falls back to solo.
if [[ "${PAPER_ENV:-0}" != "1" ]] && [[ " $GRAPH_KINDS " == *" merged "* ]]; then
    echo "[build_graphs] Non-paper env (SSD_GB=${SSD_GB:-?}, DRAM_GB=${DRAM_GB:-?}) — skipping 'merged' (paper baseline lives on a different y-scale). Producing solo only." >&2
    GRAPH_KINDS=$(echo "$GRAPH_KINDS" | tr ' ' '\n' | grep -v '^merged$' | paste -sd' ' -)
    [[ -z "$GRAPH_KINDS" ]] && GRAPH_KINDS=solo
fi
OUT_ROOT="${OUT_ROOT:-$SCRIPT_DIR}"

# Which graph-series each locally-runnable FS mode feeds. The FIO and macro
# baseline trees use different prefixes for the same filesystem (e.g. zoned
# F2FS is 'zns' in the FIO figures but 'f2fs' in the macro figures) — that's
# how the original paper scripts named their directories; preserved here.
# Run data always lives under <bench>/<FS_MODE>_data/ regardless.
#   FS_MODE : FIO series : macro series (empty = not in the macro figures)
GRAPH_FS_TABLE=(
    "gaze:gaze:gaze"
    "f2fs:zns:f2fs"
    "pgc:pgc:pgc"
    "zns_plus:copy:copy"
    "scalelfs:scalelfs:"
    "iplfs:iplfs:iplfs"
)

# Reverse maps: series name -> the FS mode that produces it. A series with no
# entry (e.g. 'iplfs') is baseline-only: never run locally, so always dummy
# (solo) or paper baseline (merged).
declare -A FIO_SERIES_FS MACRO_SERIES_FS
for _row in "${GRAPH_FS_TABLE[@]}"; do
    IFS=: read -r _mode _fio _macro <<<"$_row"
    [[ -n "$_fio"   ]] && FIO_SERIES_FS[$_fio]="$_mode"
    [[ -n "$_macro" ]] && MACRO_SERIES_FS[$_macro]="$_mode"
done

ALL_FIO_SERIES=(gaze zns iplfs pgc copy scalelfs)
ALL_MACRO_SERIES=(gaze f2fs iplfs pgc copy)

# fs_mode_included MODE — true if MODE is in INCLUDE_FS_MODES.
fs_mode_included() {
    local m
    for m in $INCLUDE_FS_MODES; do [[ "$m" == "$1" ]] && return 0; done
    return 1
}

# find_valid_run_dir ROOT_GLOB VALIDATOR_FN — newest-first scan, skipping
# any run directory that doesn't pass VALIDATOR_FN (e.g. interrupted or
# crashed runs, which produce a result file that exists but has no
# parseable summary line — a case the reused make_graph.sh's own
# `[ -s FILE ]` guard doesn't catch, since the file isn't empty, just
# incomplete). Falls back to an older completed run, or "" if none exist.
find_valid_run_dir() { # ROOT_GLOB VALIDATOR_FN
    local root_glob="$1" validator="$2" d
    # shellcheck disable=SC2086
    for d in $(ls -td $root_glob/*/ 2>/dev/null); do
        d="${d%/}"
        run_env_matches_current "$d" || continue
        if "$validator" "$d"; then
            echo "$d"
            return
        fi
    done
}

# run_env_matches_current DIR — true if <DIR>/run_env.txt was written under
# the same env we're building for now. A run with NO run_env.txt predates
# this fingerprinting feature: in the paper env accept it (pre-fingerprint
# runs on the paper machine were paper-config by construction), but in a
# non-paper env REJECT it — those runs are either the committed paper
# baselines or a different local config, and mixing them into solo/ puts
# series on incompatible y-scales. The one-shot summary at the top of the
# build says how to force-include a trusted old run (write its run_env.txt
# by hand).
run_env_matches_current() {
    local envf="$1/run_env.txt"
    if [[ ! -f "$envf" ]]; then     # pre-fingerprint run — see note above
        [[ "${PAPER_ENV:-0}" == "1" ]] && return 0 || return 1
    fi
    local run_paper
    run_paper=$(awk -F= '$1=="PAPER_ENV"{print $2; exit}' "$envf")
    [[ "${run_paper:-0}" == "${PAPER_ENV:-0}" ]] || return 1
    # Non-paper env: also require SSD_GB match — two different auto configs
    # on the same machine (e.g. 96G vs 48G) still shouldn't mix.
    if [[ "${PAPER_ENV:-0}" != "1" ]]; then
        local run_ssd
        run_ssd=$(awk -F= '$1=="SSD_GB"{print $2; exit}' "$envf")
        [[ "$run_ssd" == "${SSD_GB:-}" ]] || return 1
    fi
    return 0
}

fio_run_is_valid()        { [[ -s "$1/kiops_sum" ]]; }
fileserver_run_is_valid() { grep -q "GC IO Summary" "$1/result.txt" 2>/dev/null; }
ycsb_run_is_valid()       { grep -q '\[OVERALL\], Throughput' "$1/result.txt" 2>/dev/null; }
tpcc_run_is_valid()       { grep -q 'trx:' "$1/result.txt" 2>/dev/null; }

eps_to_png() { # DIR
    local f
    for f in "$1"/*.eps; do
        [[ -f "$f" ]] || continue
        gs -q -dSAFER -dBATCH -dNOPAUSE -dEPSCrop -sDEVICE=png16m -r150 \
            -sOutputFile="${f%.eps}.png" "$f" 2>/dev/null
    done
}

declare -A MACRO_VALIDATOR=(
    [Fileserver]=fileserver_run_is_valid
    [YCSB-A]=ycsb_run_is_valid
    [YCSB-F]=ycsb_run_is_valid
    [TPC-C]=tpcc_run_is_valid
)

echo "[build_graphs] Staging figures from on-disk runs (include: $INCLUDE_FS_MODES)"

# One-shot summary of runs missing the env fingerprint — in non-paper env
# these are SKIPPED (they're the committed paper baselines or another local
# config; see run_env_matches_current). Cheap: only counts, no heavy scan.
if [[ "${PAPER_ENV:-0}" != "1" ]]; then
    _missing_env=$(find "$GAZE_DIR/experiment_script"/{FIO,Fileserver,YCSB-A,YCSB-F,TPC-C}/*_data -maxdepth 2 -mindepth 2 -type d 2>/dev/null | \
                   while read -r d; do [[ -f "$d/run_env.txt" ]] || echo "$d"; done | wc -l)
    if [[ "${_missing_env:-0}" -gt 0 ]]; then
        echo "[build_graphs] Note: $_missing_env run dir(s) have no run_env.txt (predate env-fingerprinting) — SKIPPED:" >&2
        echo "[build_graphs]   this is a non-paper config, and unfingerprinted runs are paper-machine data (or another" >&2
        echo "[build_graphs]   config), so mixing them into solo/ would put series on incompatible y-scales." >&2
        echo "[build_graphs]   To include one you trust as this config's data, write <run_dir>/run_env.txt by hand" >&2
        echo "[build_graphs]   (PAPER_ENV=0 and SSD_GB=<this machine's value>, same format as any recent run's)." >&2
    fi
fi

# =============================================================================
# FIO figures (paper Fig 17 throughput-over-time; Supp-a avg lat, Supp-b tail lat)
# =============================================================================
build_fio() { # OUTDIR_NAME(solo|merged)
    local kind="$1"
    local out="$OUT_ROOT/$kind/fio"
    rm -rf "$out"
    mkdir -p "$out"
    cp "$SCRIPT_DIR/fio_baseline/"*.gpi "$SCRIPT_DIR/fio_baseline/moving_avg.py" \
        "$SCRIPT_DIR/fio_baseline/make_graph.sh" "$out/"

    local SRC="$out/SOURCES.txt"
    {
        echo "# Data source per series ($kind figure) — $(date '+%Y-%m-%d %H:%M')"
        echo "# [NEW] = generated on this machine by the AE flow"
        echo "# [OLD] = SOSP26 paper baseline data (recorded for the submission)"
    } > "$SRC"

    local series fs fresh
    for series in "${ALL_FIO_SERIES[@]}"; do
        # merged starts every series from its paper baseline...
        if [[ "$kind" == "merged" && -d "$SCRIPT_DIR/fio_baseline/${series}_data" ]]; then
            cp -r "$SCRIPT_DIR/fio_baseline/${series}_data" "$out/"
        fi

        # ...then, if a local run exists for this series' FS mode (and it's
        # in the include set), overlay that FS's own freshest valid run.
        fs="${FIO_SERIES_FS[$series]:-}"
        fresh=""
        if [[ -n "$fs" ]] && fs_mode_included "$fs"; then
            fresh=$(find_valid_run_dir "$FIO_DIR/${fs}_data" fio_run_is_valid)
            # zns_plus runs recorded before the mode was renamed (it used to
            # be called 'copy') live in copy_data/ — fall back to those.
            [[ -z "$fresh" && "$fs" == "zns_plus" ]] && \
                fresh=$(find_valid_run_dir "$FIO_DIR/copy_data" fio_run_is_valid)
        fi

        if [[ -n "$fresh" ]]; then
            rm -rf "$out/${series}_data"
            mkdir -p "$out/${series}_data"
            cp -r "$fresh" "$out/${series}_data/"
            echo "[build_graphs]   fio/${series}: $(basename "$fresh") (${fs})"
            echo "fio/${series}: [NEW] $fresh" >> "$SRC"
        elif [[ "$kind" == "merged" && -d "$SCRIPT_DIR/fio_baseline/${series}_data" ]]; then
            echo "fio/${series}: [OLD] $SCRIPT_DIR/fio_baseline/${series}_data" >> "$SRC"
        elif [[ "$kind" == "solo" ]]; then
            mkdir -p "$out/${series}_data/dummy"
            printf '0.0\t0.0\n' > "$out/${series}_data/dummy/kiops_sum"
            : > "$out/${series}_data/dummy/result_4k.txt"
            echo "fio/${series}: (no fresh run — plotted as zero)" >> "$SRC"
        else
            echo "fio/${series}: (no data)" >> "$SRC"
        fi
    done

    (cd "$out" && PAPER_ENV="${PAPER_ENV:-0}" bash make_graph.sh > build.log 2>&1)
    eps_to_png "$out"
}

# =============================================================================
# Macro figures (paper Fig 18 throughput; Supp-c avg lat, Supp-d tail lat)
# =============================================================================
build_macro() { # OUTDIR_NAME(solo|merged)
    local kind="$1"
    local out="$OUT_ROOT/$kind/macro"
    rm -rf "$out"
    mkdir -p "$out"
    cp "$SCRIPT_DIR/macro_baseline/"*.gpi "$SCRIPT_DIR/macro_baseline/make_graph.sh" "$out/"

    local SRC="$out/SOURCES.txt"
    {
        echo "# Data source per series ($kind figure) — $(date '+%Y-%m-%d %H:%M')"
        echo "# [NEW] = generated on this machine by the AE flow"
        echo "# [OLD] = SOSP26 paper baseline data (recorded for the submission)"
    } > "$SRC"

    local series bench dname fs fresh
    for bench in "${MACRO_BENCHES[@]}"; do
        for series in "${ALL_MACRO_SERIES[@]}"; do
            dname="${series}_${bench}"
            if [[ "$kind" == "merged" && -d "$SCRIPT_DIR/macro_baseline/$dname" ]]; then
                cp -r "$SCRIPT_DIR/macro_baseline/$dname" "$out/"
            fi

            fs="${MACRO_SERIES_FS[$series]:-}"
            fresh=""
            if [[ -n "$fs" ]] && fs_mode_included "$fs"; then
                fresh=$(find_valid_run_dir "$GAZE_DIR/experiment_script/$bench/${fs}_data" \
                            "${MACRO_VALIDATOR[$bench]}")
                # legacy dir name from before the zns_plus mode rename
                [[ -z "$fresh" && "$fs" == "zns_plus" ]] && \
                    fresh=$(find_valid_run_dir "$GAZE_DIR/experiment_script/$bench/copy_data" \
                                "${MACRO_VALIDATOR[$bench]}")
            fi

            if [[ -n "$fresh" ]]; then
                rm -rf "$out/$dname"
                mkdir -p "$out/$dname"
                cp -r "$fresh" "$out/$dname/"
                echo "[build_graphs]   macro/${dname}: $(basename "$fresh") (${fs})"
                echo "macro/${dname}: [NEW] $fresh" >> "$SRC"
            elif [[ "$kind" == "merged" && -d "$SCRIPT_DIR/macro_baseline/$dname" ]]; then
                echo "macro/${dname}: [OLD] $SCRIPT_DIR/macro_baseline/$dname" >> "$SRC"
            elif [[ "$kind" == "solo" ]]; then
                mkdir -p "$out/$dname/dummy"
                : > "$out/$dname/dummy/result.txt"
                echo "macro/${dname}: (no fresh run — plotted as zero)" >> "$SRC"
            else
                echo "macro/${dname}: (no data)" >> "$SRC"
            fi
        done
    done

    (cd "$out" && PAPER_ENV="${PAPER_ENV:-0}" bash make_graph.sh > build.log 2>&1)
    # Static legend (not data-dependent) — build once, drop into both.
    if [[ ! -f "$SCRIPT_DIR/macro_keys/keys.eps" ]]; then
        (cd "$SCRIPT_DIR/macro_keys" && gnuplot plot_key.gpi 2>/dev/null)
    fi
    [[ -f "$SCRIPT_DIR/macro_keys/keys.eps" ]] && cp "$SCRIPT_DIR/macro_keys/keys.eps" "$out/"
    eps_to_png "$out"
}

for kind in $GRAPH_KINDS; do
    case "$kind" in
        solo|merged) build_fio "$kind"; build_macro "$kind" ;;
        *) echo "[build_graphs] unknown graph kind '$kind' (want solo|merged)" >&2 ;;
    esac
done

echo "[build_graphs] Done."
for kind in $GRAPH_KINDS; do
    echo "[build_graphs]   FIO   ($kind): $OUT_ROOT/$kind/fio/"
    echo "[build_graphs]   Macro ($kind): $OUT_ROOT/$kind/macro/"
done
