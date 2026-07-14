#!/bin/bash
# make_graph.sh -- regenerate the paper's "GC latency breakdown of zoned
# F2FS" 3-panel figure (Fig 5):
#   gc_phase_latency_breakdown.eps                 (a: total, per GC phase)
#   read_phase_analysis_gc_latency_breakdown_OSDI.eps          (b: read phase)
#   write_phase_analysis_gc_latency_breakdown_OSDI_include_others_exclude_lock_index_block.eps  (c: write phase)
#
# Modes:
#   ./make_graph.sh            paper mode -- plot the archived, hand-assembled
#                              data files verbatim (figure == paper).
#   ./make_graph.sh RUN_DIR    fresh mode -- recompute all three data files
#     (or GC_PHASE_RUN env)    from RUN_DIR's parse outputs (a vanilla f2fs
#                              profile FIO run). The Conv. (conventional SSD)
#                              row is plotted as all zeros: the repo has no
#                              data source for it.
#
# Why fresh numbers won't match the archived data byte-for-byte: the
# committed graph_data_gc_lat_read_phase Zoned row came from a different
# (14-GC) vanilla run than the write row (the staged archive run, 31 GCs),
# and the total row was averaged over yet another sample selection -- the
# paper's data files were assembled by hand across runs (see
# total_gc_phase_analysis_data_jw_manually in the archive). Fresh mode is
# instead self-consistent: all three panels come from the same run.
#
# Data-file assembly rules (reverse-engineered from the archived files and
# the manual work log they came from):
#   read panel  <- gc_lat_read_phase_only, columns copied as-is.
#   write panel <- gc_lat_write_phase_only by header name:
#                    p4_mig (already the sum of its leaf components),
#                    preproc_wrt, check_cache2+pg_others2 (merged), wait_rd,
#                    fmap_wrt, alloc_blk, submit_wrt,
#                    others = p4_mig - (the six components above).
#   total panel <- cp / do_gc_lat / p0_1_2 averaged over the util-40 GC
#                  samples in preprocessed_osdi_gc_lat; read phase = the read
#                  panel's p3_prerd; write phase = the write panel's p4_mig
#                  (component sum, the "_jw_manually" convention). In msec.

BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ARCHIVE_LEAF="$BASE_DIR/f2fs_vanilla_profile_fio_exp_output_4k_20260105_1955/randwrite/f2fs_vanilla_profile/1"
cd "$BASE_DIR" || exit 1

RUN_DIR="${1:-${GC_PHASE_RUN:-}}"

READ_DATA="graph_data_gc_lat_read_phase"
WRITE_DATA="graph_data_gc_lat_write_phase"
TOTAL_DATA="total_gc_phase_analysis_data_jw_manually"

GPIS=(plot_phase_analysis.gpi
      plot_read_phase_analysis.gpi
      plot_write_phase_analysis_exclude_lock_index_block.gpi)
EPSES=(gc_phase_latency_breakdown.eps
       read_phase_analysis_gc_latency_breakdown_OSDI.eps
       write_phase_analysis_gc_latency_breakdown_OSDI_include_others_exclude_lock_index_block.eps)

# field <file> <name> -> value under header token "<name>:" (2-line files:
# header row of "name:" tokens, data row of values)
field() {
    awk -v want="$2:" 'NR==1{for(i=1;i<=NF;i++) if($i==want) col=i}
                       NR==2{if(col) print $col; exit}' "$1"
}

# emit_rows LABEL RUN_DIR -> writes the three per-run row strings into
# R_READ / R_WRITE / R_TOTAL (computed exactly like the paper's manual method:
# averages over the sec_util==40 GC samples of that run).
emit_rows() {
    local label="$1" run="$2"
    local RD="$run/gc_lat_read_phase_only"
    local WR="$run/gc_lat_write_phase_only"
    local PRE="$run/preprocessed_osdi_gc_lat"
    local f
    for f in "$RD" "$WR" "$PRE"; do
        [ -s "$f" ] || { echo "ERROR: missing $f -- not a vanilla-style profile run?" >&2; return 1; }
    done
    R_READ="$label $(awk 'NR==2' "$RD")"
    local P4 PPW CC2 PO2 WRD FMW ALB SBW
    P4=$(field "$WR" p4_mig);           PPW=$(field "$WR" preproc_wrt)
    CC2=$(field "$WR" check_cache2);    PO2=$(field "$WR" pg_others2)
    WRD=$(field "$WR" wait_rd);         FMW=$(field "$WR" fmap_wrt)
    ALB=$(field "$WR" alloc_blk);       SBW=$(field "$WR" submit_wrt)
    R_WRITE=$(awk -v l="$label" -v p4="$P4" -v ppw="$PPW" -v cc2="$CC2" -v po2="$PO2" \
        -v wrd="$WRD" -v fmw="$FMW" -v alb="$ALB" -v sbw="$SBW" 'BEGIN{
        cc = cc2 + po2
        others = p4 - (ppw + cc + wrd + fmw + alb + sbw)
        printf "%s %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f", \
               l, p4, ppw, cc, wrd, fmw, alb, sbw, others }')
    R_TOTAL=$(awk -v l="$label" -v p3us="$(awk 'NR==2{print $1}' "$RD")" -v p4us="$P4" '
        {for(i=1;i<=NF;i++){
            if($i=="cp:")        cp += $(i+1)
            if($i=="do_gc_lat:") gc += $(i+1)
            if($i=="p0_1_2:")    pp += $(i+1)
        }}
        END{ n = NR
             printf "%s %.4f %.2f %.4f 0 0 %.2f 0 0 0 0 %.3f 0 0 0 0 0 0", \
                    l, cp/n/1000, gc/n/1000, pp/n/1000, p3us/1000, p4us/1000 }' "$PRE")
}

CONV_SRC="none"
if [ -z "$RUN_DIR" ]; then
    echo "[gc_phase] paper mode: archived hand-assembled data (figure == paper)"
    echo "[gc_phase] Zoned row: archive (paper run) -> $ARCHIVE_LEAF"
    echo "[gc_phase] Conv. row: archive (paper run) -> $ARCHIVE_LEAF"
    cp "$ARCHIVE_LEAF/$READ_DATA" "$ARCHIVE_LEAF/$WRITE_DATA" \
       "$ARCHIVE_LEAF/$TOTAL_DATA" "$BASE_DIR/" || exit 1
    CONV_SRC="archive"
else
    RUN_DIR="$(cd "$RUN_DIR" && pwd)" || exit 1
    echo "[gc_phase] fresh mode: $RUN_DIR"
    emit_rows "Zoned" "$RUN_DIR" || exit 1
    Z_READ="$R_READ"; Z_TOTAL="$R_TOTAL"
    emit_rows "Zoned." "$RUN_DIR" >/dev/null; Z_WRITE="$R_WRITE"
    echo "[gc_phase] Zoned row: fresh -> $RUN_DIR"

    # Conv. (conventional SSD) row: a fresh conventional run (CONV_RUN env,
    # must contain the same parse outputs) beats the archived paper row;
    # zeros only when neither exists.
    if [ -n "${CONV_RUN:-}" ] && [ -s "${CONV_RUN}/gc_lat_read_phase_only" ]; then
        CONV_RUN="$(cd "$CONV_RUN" && pwd)" || exit 1
        emit_rows "Conv." "$CONV_RUN" || exit 1
        C_READ="$R_READ"; C_WRITE="$R_WRITE"; C_TOTAL="$R_TOTAL"
        echo "[gc_phase] Conv. row: fresh -> $CONV_RUN"
        CONV_SRC="fresh"
    elif grep -q "^Conv" "$ARCHIVE_LEAF/$READ_DATA" 2>/dev/null; then
        C_READ=$(grep "^Conv" "$ARCHIVE_LEAF/$READ_DATA")
        C_WRITE=$(grep "^Conv" "$ARCHIVE_LEAF/$WRITE_DATA")
        C_TOTAL=$(grep "^Conv" "$ARCHIVE_LEAF/$TOTAL_DATA")
        echo "[gc_phase] Conv. row: archive (paper run) -> $ARCHIVE_LEAF (no fresh conventional-SSD run; set CONV_RUN=... to override)"
        CONV_SRC="archive"
    else
        C_READ="Conv. 0 0 0 0 0 0 0 0"
        C_WRITE="Conv. 0 0 0 0 0 0 0 0"
        C_TOTAL="Conv. 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0"
        echo "[gc_phase] Conv. row: none -- plotted as zero"
    fi

    {
        echo "#NAME	p3_prerd: preproc_rd: check_cache1: get_free_pg: zerofill: add_cache: pg_others1: submit_rd:"
        echo "$C_READ"
        echo "$Z_READ"
    } > "$READ_DATA"
    {
        echo "#NAME p4_mig: preproc_wrt: check_cache2: wait_rd: fmap_wrt: alloc_blk: submit_wrt: others:"
        echo "$C_WRITE"
        echo "$Z_WRITE"
    } > "$WRITE_DATA"
    {
        echo "#NAME	#cp	dogc_lat	p0	p1	p2	p3	p3_preproc	p3_fmap	p3_pcache	p3_submit_rd	p4	p4_fmap_rd	p4_pcache	p4_wait_rd	p4_fmap_wrt	p4_wrt	p4_preproc"
        echo "$C_TOTAL"
        echo "$Z_TOTAL"
    } > "$TOTAL_DATA"
fi

for i in 0 1 2; do
    cp "$ARCHIVE_LEAF/${GPIS[$i]}" "$BASE_DIR/" || exit 1
    if [ "$CONV_SRC" != "archive" ]; then
        # The archived gpi's carry a hand-placed annotation of the paper's
        # Conv. bar height ("6.0ms" etc.); it only holds when the Conv. row
        # is the archived paper one -- drop it otherwise.
        sed -i '/^set label "[0-9.]\+ms"/d' "$BASE_DIR/${GPIS[$i]}"
    fi
    gnuplot "${GPIS[$i]}" || { echo "ERROR: gnuplot ${GPIS[$i]} failed" >&2; exit 1; }
    if command -v gs >/dev/null 2>&1; then
        PNG="${EPSES[$i]%.eps}.png"
        gs -dQUIET -dBATCH -dNOPAUSE -dEPSCrop -r150 -sDEVICE=png16m \
           -sOutputFile="$BASE_DIR/$PNG" "$BASE_DIR/${EPSES[$i]}" \
            && echo "png : $BASE_DIR/$PNG"
    fi
done

# Paper-numbered aliases so an artifact evaluator can match files to the
# paper at a glance. Paper Fig 5: "Latency Breakdown of Garbage Collection
# (GC) (1 GByte Zone with 40% Utilization)", subfigures (a) Overall GC,
# (b) GC Read Phase, (c) GC Write Phase.
ALIASES=(Fig-05a.GC_latency_breakdown.Overall_GC
         Fig-05b.GC_latency_breakdown.GC_Read_Phase
         Fig-05c.GC_latency_breakdown.GC_Write_Phase)
for i in 0 1 2; do
    cp -f "${EPSES[$i]}" "${ALIASES[$i]}.eps"
    [ -f "${EPSES[$i]%.eps}.png" ] && cp -f "${EPSES[$i]%.eps}.png" "${ALIASES[$i]}.png"
done

echo "---------------------------------------"
echo "done: $READ_DATA / $WRITE_DATA / $TOTAL_DATA -> ${EPSES[*]}"
echo "paper-figure aliases: ${ALIASES[0]}.png ${ALIASES[1]}.png ${ALIASES[2]}.png"
