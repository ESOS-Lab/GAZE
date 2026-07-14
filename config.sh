#!/bin/bash
# GAZE experiment configuration -- sourced by every benchmark script.
# Pure variable loading, no side effects, safe to source any number of
# times.
#
# Machine-local values (device paths, credentials, mount point) live in
# ./.config, written by ./set_config.sh -- run that first on a new
# machine, or to change SSD/DRAM sizing, the conventional-SSD partition,
# or the mount point. Never hand-edit .config; it gets overwritten.
#
# DO NOT manually set FS_MODE here -- use experiment_script/select_fs.sh
# (or kernel/switch_mode.sh directly):
#   experiment_script/select_fs.sh
#   sudo kernel/switch_mode.sh [gaze|f2fs|pgc|zns_plus|...]

# Compute GAZE_DIR without leaking SCRIPT_DIR: callers set their own
# SCRIPT_DIR (their benchmark dir) before sourcing this file, and rely on
# it surviving (for OUTDIR, workload files, etc.).
GAZE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# --- Machine-local variables (CONV_PART, CONV_DEV, ZNS_DEV, ZNS_DEV_NAME,
#     MNT, MYSQL_*) ---
DOTCONFIG="$GAZE_DIR/.config"
if [[ -f "$DOTCONFIG" ]]; then
    source "$DOTCONFIG"
else
    echo "[config] Warning: $DOTCONFIG not found. Run ./set_config.sh first." >&2
    echo "[config]   Using placeholder defaults -- experiments will likely fail until you do." >&2
    CONV_PART=/dev/nvme0n1p1
    CONV_DEV=/dev/nvme0n1
    ZNS_DEV=/dev/nvme2n1
    ZNS_DEV_NAME=nvme2n1
    MNT=/mnt
    MYSQL_DATA_PATH=/exp_mysql_data
    MYSQL_USER=aec
    MYSQL_PASSWD=aec
fi

# --- Derived / tool paths -- do not edit ---
MKFS_F2FS="$GAZE_DIR/kernel/f2fs-tools-1.15.0/mkfs/mkfs.f2fs"

# ScaleLFS must be formatted with its own traditional f2fs-tools (1.16.0,
# non-zoned), NEVER with the GAZE zoned mkfs above. Vendored in the repo at
# scalelfs-mkfs-tools/ and built in place by setup_env.sh (or its own
# build_tools.sh). Overridable via .config (SCALELFS_TOOLS=...) to use an
# external checkout instead.
SCALELFS_TOOLS="${SCALELFS_TOOLS:-$GAZE_DIR/scalelfs-mkfs-tools}"
MKFS_SCALELFS="$SCALELFS_TOOLS/mkfs/mkfs.f2fs"
MKFS_SCALELFS_LIB="$SCALELFS_TOOLS/lib/.libs"
# ScaleLFS runs on the emulated device as a CONVENTIONAL (non-zoned) SSD --
# same NVMeVirt device path as ZNS_DEV, but nvmev must be loaded in conv
# mode (ssd_emulator/run_nvmevirt.sh picks the variant from the FS mode).
SCALELFS_DEV="${SCALELFS_DEV:-$ZNS_DEV}"
MODULE_DIR="$GAZE_DIR/experiment_script/general_resource/filesystem_module"
GENERAL_RES="$GAZE_DIR/experiment_script/general_resource"
FIO_PATH="$GAZE_DIR/benchmarks/fio_src"
FILEBENCH_PATH="$GAZE_DIR/benchmarks/filebench"
TPCC_PATH="$GAZE_DIR/benchmarks/tpcc"
YCSB_PATH="$GAZE_DIR/benchmarks/YCSB"

# --- Workload-intensity parameters (record counts, thread counts, runtime,
#     ...) -- separate from the machine/FS config above. Edit via
#     experiment_script/set_params.sh, not by hand. ---
source "$GENERAL_RES/experiment_params.sh"

# --- Filesystem mode (FS_MODE / FS_KO / FS_NAME / PROFILE_MODE) ---
# Resolved from kernel/.build_mode, written by switch_mode.sh (called by
# experiment_script/select_fs.sh).
FS_MODE_DEFAULT=gaze
_BUILD_MODE_FILE="$GAZE_DIR/kernel/.build_mode"
if [[ -f "$_BUILD_MODE_FILE" ]]; then
    FS_MODE=$(cat "$_BUILD_MODE_FILE")
    # Legacy alias: this mode used to be called 'copy' (old .build_mode files)
    [[ "$FS_MODE" == "copy" ]] && FS_MODE=zns_plus
else
    FS_MODE="$FS_MODE_DEFAULT"
    echo "[config] Warning: kernel/.build_mode not found. Using default FS_MODE=$FS_MODE" >&2
    echo "[config]   Run: experiment_script/select_fs.sh   (or: sudo kernel/switch_mode.sh [gaze|f2fs|pgc|zns_plus])" >&2
fi

case "$FS_MODE" in
    gaze)           FS_KO="gaze.ko";           FS_NAME="gaze"           ;;
    f2fs)           FS_KO="f2fs.ko";           FS_NAME="f2fs"           ;;
    pgc)            FS_KO="pgc.ko";            FS_NAME="pgc"            ;;
    zns_plus)       FS_KO="zns_plus.ko";       FS_NAME="zns_plus"       ;;
    f2fs_pool)      FS_KO="f2fs_pool.ko";      FS_NAME="f2fs_pool"      ;;
    f2fs_sb)        FS_KO="f2fs_sb.ko";        FS_NAME="f2fs_sb"        ;;
    f2fs_pipe)      FS_KO="f2fs_pipe.ko";      FS_NAME="f2fs_pipe"      ;;
    f2fs_pool_sb)   FS_KO="f2fs_pool_sb.ko";   FS_NAME="f2fs_pool_sb"   ;;
    f2fs_pool_pipe) FS_KO="f2fs_pool_pipe.ko"; FS_NAME="f2fs_pool_pipe" ;;
    f2fs_sb_pipe)   FS_KO="f2fs_sb_pipe.ko";   FS_NAME="f2fs_sb_pipe"   ;;
    scalelfs)       FS_KO="scalelfs.ko";       FS_NAME="scalelfs"       ;;
    iplfs)          FS_KO="iplfs.ko";          FS_NAME="iplfs"          ;;
    *)    echo "[config] Unknown FS_MODE: '$FS_MODE'. Valid: gaze | f2fs | pgc | zns_plus | f2fs_pool | f2fs_sb | f2fs_pipe | f2fs_pool_sb | f2fs_pool_pipe | f2fs_sb_pipe | scalelfs | iplfs" >&2; exit 1 ;;
esac

_PROFILE_MODE_FILE="$GAZE_DIR/kernel/.profile_mode"
PROFILE_MODE=off
[[ -f "$_PROFILE_MODE_FILE" ]] && PROFILE_MODE=$(cat "$_PROFILE_MODE_FILE")

if [[ "$PROFILE_MODE" == "on" ]]; then
    if [[ "$FS_MODE" == "scalelfs" || "$FS_MODE" == "iplfs" ]]; then
        echo "[config] Warning: GC profiling is not supported for $FS_MODE; ignoring .profile_mode." >&2
    else
        FS_KO="${FS_KO%.ko}_profile.ko"
        FS_NAME="${FS_NAME}_profile"
    fi
fi

# --- Branch coupling (IPLFS baseline) ---
# The iplfs mode runs a different kernel tree, SSD emulator, and mkfs-tools;
# those three directories (and only those) differ on git branch 'iplfs' —
# every script, config, and figure path is identical on both branches.
# GAZE_GIT_BRANCH lets scripts refuse a mode/branch mismatch instead of
# silently building or running against the wrong kernel/emulator sources.
GAZE_GIT_BRANCH=$(git -C "$GAZE_DIR" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)
# require_branch_for_fs_mode — call before building/loading/mounting
# anything mode-specific. Prints the exact fix and exits on mismatch.
require_branch_for_fs_mode() {
    if [[ "$FS_MODE" == "iplfs" && "$GAZE_GIT_BRANCH" != "iplfs" ]]; then
        echo "[config] ERROR: FS mode 'iplfs' needs the IPLFS kernel/emulator/mkfs sources," >&2
        echo "[config]   which live on git branch 'iplfs' (current branch: $GAZE_GIT_BRANCH)." >&2
        echo "[config]   Fix: git -C $GAZE_DIR checkout iplfs   (run data survives checkouts)" >&2
        exit 1
    fi
    if [[ "$FS_MODE" != "iplfs" && "$GAZE_GIT_BRANCH" == "iplfs" ]]; then
        echo "[config] ERROR: branch 'iplfs' carries the IPLFS kernel/emulator sources;" >&2
        echo "[config]   FS mode '$FS_MODE' must run from branch 'main'." >&2
        echo "[config]   Fix: git -C $GAZE_DIR checkout main   (run data survives checkouts)" >&2
        exit 1
    fi
}

# IPLFS formats a partition of the emulated device with its own modified
# f2fs-tools (mkfs-tools/, present on the iplfs branch only).
MKFS_IPLFS="$GAZE_DIR/mkfs-tools/mkfs/mkfs.f2fs"
IPLFS_PART="${IPLFS_PART:-${ZNS_DEV}p1}"

# --- Paper-environment fingerprint ------------------------------------------
# The paper baseline data under experiment_script/AE_results/*_baseline was
# taken with the paper machine's SSD/DRAM sizing and the paper's workload
# parameters. Mixing local runs from a differently sized machine (auto mode)
# into those figures produces a graph whose series live on incompatible
# y-scales -- so build_graphs.sh needs to know whether the current config is
# paper-compatible, and each run needs to record its own snapshot so the
# builder can filter old runs from a previous config.
#
# PAPER_SSD_GB / PAPER_DRAM_GB stay in sync with set_config.sh's constants.
PAPER_SSD_GB=256
PAPER_DRAM_GB=64

# is_paper_env — 0 (paper) or 1 (other). "Paper" means SSD_GB/DRAM_GB match
# the paper machine AND experiment_params.sh hasn't been auto-scaled
# (set_config.sh auto/manual leaves the file with an "# AUTO-SCALED" or
# equivalent header; paper mode restores paper_params.sh verbatim).
is_paper_env() {
    [[ "${SSD_GB:-}" == "$PAPER_SSD_GB" && "${DRAM_GB:-}" == "$PAPER_DRAM_GB" ]] || return 1
    ! grep -q '^# AUTO-SCALED' "$GENERAL_RES/experiment_params.sh" 2>/dev/null || return 1
    return 0
}
PAPER_ENV=0
is_paper_env && PAPER_ENV=1

# write_run_env <outdir> -- snapshot the env into <outdir>/run_env.txt so
# build_graphs.sh can later tell which runs belong to the current config.
# Called by every benchmark runner right after it makes its OUTDIR.
write_run_env() {
    local outdir="$1"
    [[ -d "$outdir" ]] || return 0
    cat > "$outdir/run_env.txt" <<EOF
# Environment snapshot at run start (used by build_graphs.sh to keep runs
# from different configs out of the same figure). Do not edit.
PAPER_ENV=$PAPER_ENV
SSD_GB=${SSD_GB:-}
DRAM_GB=${DRAM_GB:-}
FS_MODE=${FS_MODE:-}
FIO_FILESIZE=${FIO_FILESIZE:-}
FILESERVER_NFILES=${FILESERVER_NFILES:-}
YCSBA_RECORDCOUNT=${YCSBA_RECORDCOUNT:-}
YCSBF_RECORDCOUNT=${YCSBF_RECORDCOUNT:-}
TPCC_WAREHOUSES=${TPCC_WAREHOUSES:-}
EOF
}
