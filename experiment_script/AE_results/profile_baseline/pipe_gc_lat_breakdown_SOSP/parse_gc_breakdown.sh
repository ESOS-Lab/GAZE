#!/bin/bash
# parse_gc_breakdown.sh RUN_DIR (serial|pipe) [UTIL]
#
# Extracts the per-phase GC latency averages that feed the paper's
# "GC Latency Breakdown" figure (pipe_gc_lat_breakdown.eps) from one
# profile-mode FIO run directory (must contain a `dmesg` file written by a
# *_profile.ko run, i.e. kernel prints from print_cur_gc_profiler).
#
#   serial : a plain f2fs_profile run  -> parses the OSDI26_analysis lines
#   pipe   : a f2fs_pipe_profile run   -> parses the pipe_OSDI26 lines
#
# UTIL (default 40) picks GCs whose victim-section utilization matches the
# paper's setting ("victim zone utilization: 40%").
#
# Output (single line on stdout, all values are per-GC averages in us):
#   CP P0_1_2 PREREAD READ PREWRITE WRITE DO_GC
#
# The extraction logic is byte-for-byte the pipeline that produced the
# figure in the paper (see the archived run dirs next to this script:
# f2fs_vanilla_*/.../parse_lat.sh + get_gc_lat_breakdown_data.sh and
# f2fs_4step_*/.../parse_pipe.sh + get_gc_lat_breakdown_data_pipe.sh),
# only refactored to be callable on any run dir. Quirks like the
# substring-y `grep "cnt: 1"` filter are intentionally preserved so
# regenerated numbers stay comparable with the paper's.

set -u
RUN_DIR="${1:-}"
FLAVOR="${2:-}"
UTIL="${3:-40}"

die() { echo "[parse_gc_breakdown] ERROR: $*" >&2; exit 1; }

[[ -n "$RUN_DIR" && -n "$FLAVOR" ]] || die "usage: $0 RUN_DIR (serial|pipe) [UTIL]"
DMESG="$RUN_DIR/dmesg"
[[ -f "$DMESG" ]] || die "$DMESG not found (is this a benchmark run dir?)"

case "$FLAVOR" in
    serial) GREP_PAT="OSDI";      FIRST_PREPROC="preproc_rd:"  ;;
    pipe)   GREP_PAT="pipe_OSDI"; FIRST_PREPROC="preproc_wrt:" ;;
    *) die "flavor must be 'serial' or 'pipe' (got '$FLAVOR')" ;;
esac

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# 1. Filter the profiler lines for this flavor & victim utilization, and
#    normalize tokens ($ -> cache, split brackets) exactly as the paper
#    pipeline did.
grep "cnt: 1" "$DMESG" | grep "util: $UTIL" | grep "$GREP_PAT" |
sed -e 's/\$/cache/g' \
    -e 's/(/( /g' \
    -e 's/\[/[ /g' > "$TMP/filtered"

if [[ ! -s "$TMP/filtered" ]]; then
    echo "[parse_gc_breakdown] ERROR: no '$GREP_PAT' profiler lines with util: $UTIL in $DMESG" >&2
    echo "[parse_gc_breakdown]   Utilizations present in this run:" >&2
    grep -o 'sec_util: [0-9]*' "$DMESG" | sort -u | head -20 | sed 's/^/[parse_gc_breakdown]     /' >&2
    echo "[parse_gc_breakdown]   (was this run made with a *_profile.ko on the profile kernel?)" >&2
    exit 1
fi

# 2. Disambiguate the two identically-named 'preproc:' fields (read-side
#    vs write-side). serial renames them preproc_rd/preproc_wrt; the pipe
#    lines only carry write-side ones, so both become preproc_wrt.
awk -v first="$FIRST_PREPROC" '{
    c = 0
    for (i = 1; i <= NF; i++) {
        if ($i ~ /preproc:/) {
            c++
            if (c == 1)      sub(/preproc:/, first, $i)
            else if (c == 2) sub(/preproc:/, "preproc_wrt:", $i)
        }
    }
    print
}' "$TMP/filtered" > "$TMP/pre"

# avg_sum KEY... -> sum over KEYs of (per-GC average of KEY's value)
avg_sum() {
    awk -v keys="$*" '
    BEGIN { n = split(keys, K, " "); for (i = 1; i <= n; i++) sum[K[i]] = 0 }
    {
        for (i = 1; i <= n; i++) val[K[i]] = 0
        for (i = 1; i <= NF; i++)
            for (k = 1; k <= n; k++)
                if ($i == K[k]) val[K[k]] = $(i+1)
        for (k = 1; k <= n; k++) sum[K[k]] += val[K[k]]
        cnt++
    }
    END {
        total = 0
        for (k = 1; k <= n; k++) total += sum[K[k]] / cnt
        printf "%.6f", total
    }' "$TMP/pre"
}

CP=$(avg_sum cp:)
P012=$(avg_sum p0_1_2:)
DOGC=$(avg_sum do_gc_lat:)

if [[ "$FLAVOR" == "serial" ]]; then
    # Serial GC: phases carved out of the single do_gc pass.
    PREREAD=$(avg_sum preproc_rd:)
    P3=$(avg_sum p3_prerd:)
    READ=$(echo "scale=6; $P3 - $PREREAD" | bc -l)
    PREWRITE=$(avg_sum preproc_wrt: check_cache2: wait_rd: pg_others2: bef_write: fmap_wrt: pg_wb:)
    P4=$(avg_sum p4_mig:)
    WRITE=$(echo "scale=6; $P4 - $PREWRITE" | bc -l)
else
    # Pipelined GC: the four pipeline stages are reported directly.
    PREREAD=$(avg_sum p3_s1:)
    READ=$(avg_sum p3_s2:)
    PREWRITE=$(avg_sum p4_s1:)
    WRITE=$(avg_sum p4_s2:)
fi

echo "$CP $P012 $PREREAD $READ $PREWRITE $WRITE $DOGC"
