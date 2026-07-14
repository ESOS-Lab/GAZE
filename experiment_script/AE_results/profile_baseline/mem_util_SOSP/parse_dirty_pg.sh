#!/bin/bash
# parse_dirty_pg.sh LOG_FILE > parsed.dat
#
# Extracts the dirty-page samples that feed the paper's memory-utilization
# figures (Fig. "Memory Utilization and FIO Throughput in F2FS" and the
# selective-blocking counterpart) from a kernel log.
#
# The kernel emits one sample line per interval when APP_WRITE_PROFILE is
# enabled (switch_mode.sh --profile; mm/page-writeback.c,
# jw_balance_dirty_pages_ratelimited_flags):
#
#   old builds : [  119.563175] jw_balance_dirty_pages_ratelimited_flags: nr_jw_dirty_pgs: 47117 avail_pgs: 14794633 dirty_ratio: 0
#   new builds : [  119.563175] jw_balance_dirty_pages_ratelimited_flags: nr_jw_dirty: 47117
#
# Both formats are accepted. LOG_FILE may be a `dmesg` capture or a
# /var/log/kern.log copy (raw_kern_log) -- the syslog prefix is stripped.
#
# Output: two columns "timestamp_sec nr_dirty_pages" (timestamps left raw;
# the caller normalizes to the first sample, exactly like the original
# figure pipeline did).

set -u
LOG="${1:-}"
[[ -n "$LOG" && -f "$LOG" ]] || { echo "usage: $0 LOG_FILE" >&2; exit 1; }

sed -E 's/^.* kernel: //' "$LOG" |
sed -nE 's/^\[ *([0-9.]+)\] jw_balance_dirty_pages_ratelimited_flags: nr_jw_dirty(_pgs)?: ([0-9]+).*/\1 \3/p'
