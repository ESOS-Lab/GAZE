#!/bin/bash
# Shared helper functions sourced by all benchmark run scripts.
# Requires config.sh to have been sourced first.

# root_write FILE VALUE — write VALUE to a root-owned sysfs/procfs/kmsg file.
root_write() {
    echo "$2" | sudo tee "$1" > /dev/null
}

# parse_dmesg OUTDIR
# Common dmesg post-processing (GC profiler, write detail, submit_bio).
# Called after 'dmesg > OUTDIR/dmesg'. No-op when PROFILE_MODE != on.
parse_dmesg() {
    local OUTDIR="$1"
    [[ "${PROFILE_MODE:-off}" != "on" ]] && return
    local number

    # Profile runs can emit more printk than the dmesg ring buffer holds
    # (e.g. APP_WRITE_PROFILE's dirty-page samples for the memory-utilization
    # figures) -- keep the full kernel log alongside the ring-buffer capture.
    [[ -r /var/log/kern.log ]] && cp /var/log/kern.log "$OUTDIR/raw_kern_log" 2>/dev/null

    number=$(grep STARTT "$OUTDIR/dmesg" | sed 's/\[//g;s/\]//g' | awk '{print $1}')

    grep print_cur_gc_profiler "$OUTDIR/dmesg" | grep 'lat summary' \
        > "$OUTDIR/gc_per_sec_only"

    echo -e "#time\tutil\tdo_gc_cnt\tfsgc_lat\tdogc_lat\tcalc_dogc_lat\tcp\tmeta\tfilemap\tpcache\tread\twrite\tothers" \
        > "$OUTDIR/gc_lat_per_sec"
    sed 's/\]//g;s/\[//g;s/(//g;s/)//g' "$OUTDIR/gc_per_sec_only" | \
        awk -v n="$number" '{$1=$1-n; print $1,$6,$8,$10,$12,$14,$16,$18,$20,$22,$24,$30,$26+$28-$30}' \
        >> "$OUTDIR/gc_lat_per_sec"

    echo -e "#time\tutil\tdo_gc_cnt\tfsgc_lat\tdogc_lat\tcalc_dogc_lat\tcp\tmeta\tfilemap\tpcache\tread\twrite\tothers" \
        > "$OUTDIR/gc_lat_per_sec_one_gc"
    sed 's/\]//g;s/\[//g;s/(//g;s/)//g' "$OUTDIR/gc_per_sec_only" | \
        awk -v n="$number" '{$1=$1-n; print $1,$6,$8,$10,$12,$14,$16,$18,$20,$22,$24,$30,$26+$28-$30}' | \
        awk '{if($3==1) print}' >> "$OUTDIR/gc_lat_per_sec_one_gc"

    echo -e "#time\tdo_gc_cnt\tdo_write_page_lat\t_order_lock\t_aloc_block\t_inv_cache\t_submit_pg\t__io_rwsem\t__zone_wait\t__other1\t__submit_previo\t__bio_alloc\t__bio_add\t__submit_write_by_bio_add\t___finish_plug\t___submit_bio\t__cur_zone_write\t__end_write\tupdate_map" \
        > "$OUTDIR/write_detail_lat_per_sec"
    grep outplace_detail_gc_profiler "$OUTDIR/dmesg" | sed 's/\]//g;s/\[//g;s/(//g;s/)//g' | \
        awk -v n="$number" '{$1=$1-n; print $1,$4,$6,$8,$10,$12,$14,$16,$18,$20,$22,$24,$26,$28,$30,$32,$34,$36,$38}' \
        >> "$OUTDIR/write_detail_lat_per_sec"

    echo -e "#time\tdo_gc_cnt\tdo_write_page_lat\t_order_lock\t_aloc_block\t_inv_cache\t_submit_pg\t__io_rwsem\t__zone_wait\t__other1\t__submit_previo\t__bio_alloc\t__bio_add\t__submit_write_by_bio_add\t___finish_plug\t___submit_bio\t__cur_zone_write\t__end_write\tupdate_map" \
        > "$OUTDIR/write_detail_lat_per_sec_one_gc"
    grep outplace_detail_gc_profiler "$OUTDIR/dmesg" | sed 's/\]//g;s/\[//g;s/(//g;s/)//g' | \
        awk -v n="$number" '{$1=$1-n; print $1,$4,$6,$8,$10,$12,$14,$16,$18,$20,$22,$24,$26,$28,$30,$32,$34,$36,$38}' | \
        awk '{if($2==1) print}' >> "$OUTDIR/write_detail_lat_per_sec_one_gc"

    echo -e "#time\tdo_gc_cnt\tsubmit_bio\t_split\t_get_new_reqs\t__merge\t__throttle\t__alloc_requests\t___get_tag\t____io_schedule\t_mq_sched_insert" \
        > "$OUTDIR/submit_bio_detail_lat_per_sec"
    grep io_profiler "$OUTDIR/dmesg" | sed 's/\]//g;s/\[//g;s/(//g;s/)//g' | \
        awk -v n="$number" '{$1=$1-n; print $1,$4,$6,$8,$10,$12,$14,$16,$18,$20,$22}' \
        >> "$OUTDIR/submit_bio_detail_lat_per_sec"

    echo -e "#time\tdo_gc_cnt\tsubmit_bio\t_split\t_get_new_reqs\t__merge\t__throttle\t__alloc_requests\t___get_tag\t____io_schedule\t_mq_sched_insert" \
        > "$OUTDIR/submit_bio_detail_lat_per_sec_one_gc"
    grep io_profiler "$OUTDIR/dmesg" | sed 's/\]//g;s/\[//g;s/(//g;s/)//g' | \
        awk -v n="$number" '{$1=$1-n; print $1,$4,$6,$8,$10,$12,$14,$16,$18,$20,$22}' | \
        awk '{if($2==1) print}' >> "$OUTDIR/submit_bio_detail_lat_per_sec_one_gc"
}

# parse_dmesg_fs_specific OUTDIR
# FS-mode-specific post-processing: kern.log parsing (gaze only) + plot script copy.
# Must be called after teardown so kern.log contains post-umount messages.
parse_dmesg_fs_specific() {
    local OUTDIR="$1"

    # gaze only: kern.log → intra_seg / intra_zone
    if [[ "$FS_MODE" == "gaze" ]]; then
        "$GENERAL_RES/cp_kern_log.sh"
        "$GENERAL_RES/parse_kern_log.sh" > "$OUTDIR/dmesg_aft_umount"
        grep print_intra_seg "$OUTDIR/dmesg_aft_umount" | sed 's/\[//g;s/\]//g' \
            > "$OUTDIR/intra_seg_read_lat"
        grep print_intra_zone "$OUTDIR/dmesg_aft_umount" | sed 's/\[//g;s/\]//g' \
            > "$OUTDIR/intra_zone_info"
    fi

    # ZNS+ uses copy-specific plot scripts; all other modes use the standard ones
    if [[ "$FS_MODE" == "zns_plus" ]]; then
        cp "$GENERAL_RES/plot/parse_copy_lat.sh" "$OUTDIR/" 2>/dev/null || true
        cp "$GENERAL_RES/plot/get_copy_gc_lat_breakdown_data.sh" "$OUTDIR/" 2>/dev/null || true
    else
        cp "$GENERAL_RES/plot/parse_lat.sh" "$OUTDIR/" 2>/dev/null || true
        cp "$GENERAL_RES/plot/get_gc_lat_breakdown_data.sh" "$OUTDIR/" 2>/dev/null || true
        # Modes that include Pipelined GC also need pipeline-specific plot scripts
        case "$FS_MODE" in
            gaze|f2fs_pipe|f2fs_pool_pipe|f2fs_sb_pipe)
                cp "$GENERAL_RES/plot/parse_pipe_lat.sh" "$OUTDIR/" 2>/dev/null || true
                cp "$GENERAL_RES/plot/get_pipe_gc_lat_breakdown_data.sh" "$OUTDIR/" 2>/dev/null || true
                ;;
        esac
    fi

    cp "$GENERAL_RES/plot/"*.gpi "$OUTDIR/" 2>/dev/null || true
}
