#!/bin/bash

# Usage: ./switch_mode.sh <mode> [--profile]   (no sudo needed — only edits
#        files in this tree; restricted-sudoers AE accounts can run it as-is)
# Switches between filesystem modes and optionally enables GC profiling.
#
# Available modes:
#   gaze           - GAZE (GC Page Pool + Pipelined GC + Selective Blocking)
#   pgc            - P-GC (Parallel GC baseline)
#   f2fs           - Vanilla zoned F2FS
#   zns_plus       - ZNS+ (copy-offload)
#   f2fs_pool      - F2FS + GC Page Pool
#   f2fs_sb        - F2FS + Selective Blocking
#   f2fs_pipe      - F2FS + Pipelined GC
#   f2fs_pool_sb   - F2FS + GC Page Pool + Selective Blocking
#   f2fs_pool_pipe - F2FS + GC Page Pool + Pipelined GC
#   f2fs_sb_pipe   - F2FS + Selective Blocking + Pipelined GC
#   scalelfs       - ScaleLFS baseline (own module fs/scalelfs, always built;
#                    conventional SSD + traditional f2fs-tools; no --profile)
#
# Options:
#   --profile      Enable GC profiling (GC_PROFILE_TIMESERIAL, GC_IO_PROFILE_TIMESERIAL).
#                  Adds '_profile' suffix to kernel version and .ko name.
#                  Without this flag, profiling is always disabled.
#                  Prints are emitted only for GCs at victim utilization 40
#                  (GC_PROFILE_UTIL_ONLY, jw_trace.h) -- the setting all the
#                  paper's breakdown figures (Fig 5/14/16) are taken at.
#                  NOTE: GC_LIGHT must stay OFF here -- it compiles out the
#                  OSDI26_analysis / OSDI26_analysis_copy / pipe_OSDI26 printks
#                  (super.c, inside '#ifndef GC_LIGHT') that the GC-latency-
#                  breakdown figures are parsed from.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODE_FILE="$SCRIPT_DIR/.build_mode"
PROFILE_FILE="$SCRIPT_DIR/.profile_mode"
F2FS_H="$SCRIPT_DIR/fs/f2fs/f2fs.h"
JW_TRACE_H="$SCRIPT_DIR/include/linux/jw_trace.h"
MAKEFILE="$SCRIPT_DIR/Makefile"

usage() {
    sed -n '3,26p' "$0"
    exit 1
}

# Parse arguments
MODE=""
PROFILE=off
for arg in "$@"; do
    case "$arg" in
        --profile)    PROFILE=on ;;
        --no-profile) PROFILE=off ;;
        -*)           echo "[switch_mode] Unknown option: $arg"; usage ;;
        *)
            if [[ -z "$MODE" ]]; then
                MODE="$arg"
            else
                echo "[switch_mode] Unexpected argument: $arg"; usage
            fi
            ;;
    esac
done

[[ -z "$MODE" ]] && usage

# Sets CURRENT_FS_MODE in fs/f2fs/f2fs.h
set_fs_mode() {
    local mode_const="$1"
    sed -i "s/^#define CURRENT_FS_MODE .*/#define CURRENT_FS_MODE ${mode_const}/" "$F2FS_H"
    echo "[switch_mode] fs/f2fs/f2fs.h: CURRENT_FS_MODE = ${mode_const}"
}

# Enables #define JW_COPY_COMMAND in jw_trace.h
enable_copy_command() {
    sed -i 's|^//#define JW_COPY_COMMAND|#define JW_COPY_COMMAND|' "$JW_TRACE_H"
    echo "[switch_mode] include/linux/jw_trace.h: JW_COPY_COMMAND enabled"
}

# Disables #define JW_COPY_COMMAND in jw_trace.h
disable_copy_command() {
    sed -i 's|^#define JW_COPY_COMMAND|//#define JW_COPY_COMMAND|' "$JW_TRACE_H"
    echo "[switch_mode] include/linux/jw_trace.h: JW_COPY_COMMAND disabled"
}

# Enables GC profiling defines in jw_trace.h and f2fs.h.
# GC_LIGHT is force-disabled: it strips the very printks (OSDI26_analysis,
# OSDI26_analysis_copy, pipe_OSDI26) that the breakdown figures parse.
# APP_WRITE_PROFILE (dirty-page samples for the dropped memory-utilization
# figures) is intentionally NOT enabled: its 50ms samples were ~97% of a
# profile run's dmesg. Prints are further gated to victim utilization 40
# (GC_PROFILE_UTIL_ONLY in jw_trace.h), the only utilization the figures use.
enable_profile() {
    sed -i 's|^//#define GC_IO_PROFILE_TIMESERIAL|#define GC_IO_PROFILE_TIMESERIAL|' "$JW_TRACE_H"
    sed -i 's|^//#define GC_PROFILE_TIMESERIAL|#define GC_PROFILE_TIMESERIAL|' "$JW_TRACE_H"
    sed -i 's|^//#define GC_PROFILE_TIMESERIAL|#define GC_PROFILE_TIMESERIAL|' "$F2FS_H"
    sed -i 's|^#define APP_WRITE_PROFILE|//#define APP_WRITE_PROFILE|' "$JW_TRACE_H"
    sed -i 's|^#define APP_WRITE_PROFILE|//#define APP_WRITE_PROFILE|' "$F2FS_H"
    sed -i 's|^#define GC_LIGHT|//#define GC_LIGHT|' "$F2FS_H"
    echo "[switch_mode] Profiling ENABLED  (GC_PROFILE_TIMESERIAL, GC_IO_PROFILE_TIMESERIAL, util-40-only prints; GC_LIGHT off)"
}

# Disables GC profiling defines in jw_trace.h and f2fs.h
disable_profile() {
    sed -i 's|^#define GC_IO_PROFILE_TIMESERIAL|//#define GC_IO_PROFILE_TIMESERIAL|' "$JW_TRACE_H"
    sed -i 's|^#define GC_PROFILE_TIMESERIAL|//#define GC_PROFILE_TIMESERIAL|' "$JW_TRACE_H"
    sed -i 's|^#define APP_WRITE_PROFILE|//#define APP_WRITE_PROFILE|' "$JW_TRACE_H"
    sed -i 's|^#define GC_PROFILE_TIMESERIAL|//#define GC_PROFILE_TIMESERIAL|' "$F2FS_H"
    sed -i 's|^#define APP_WRITE_PROFILE|//#define APP_WRITE_PROFILE|' "$F2FS_H"
    sed -i 's|^#define GC_LIGHT|//#define GC_LIGHT|' "$F2FS_H"
    echo "[switch_mode] Profiling DISABLED"
}

# Sets EXTRAVERSION in Makefile
set_extraversion() {
    local ver="$1"
    sed -i "s|^EXTRAVERSION = .*|EXTRAVERSION = ${ver}|" "$MAKEFILE"
    echo "[switch_mode] Makefile: EXTRAVERSION = ${ver}"
}

# Apply FS mode
case "$MODE" in
    gaze)           set_fs_mode FS_MODE_GAZE;           disable_copy_command; BASE_VER=".sosp26_ae" ;;
    pgc)            set_fs_mode FS_MODE_PGC;            disable_copy_command; BASE_VER=".sosp26_ae" ;;
    f2fs)           set_fs_mode FS_MODE_F2FS;           disable_copy_command; BASE_VER=".sosp26_ae" ;;
    zns_plus)       set_fs_mode FS_MODE_ZNS_PLUS;       enable_copy_command;  BASE_VER=".sosp26_ae_copy" ;;
    f2fs_pool)      set_fs_mode FS_MODE_F2FS_POOL;      disable_copy_command; BASE_VER=".sosp26_ae" ;;
    f2fs_sb)        set_fs_mode FS_MODE_F2FS_SB;        disable_copy_command; BASE_VER=".sosp26_ae" ;;
    f2fs_pipe)      set_fs_mode FS_MODE_F2FS_PIPE;      disable_copy_command; BASE_VER=".sosp26_ae" ;;
    f2fs_pool_sb)   set_fs_mode FS_MODE_F2FS_POOL_SB;   disable_copy_command; BASE_VER=".sosp26_ae" ;;
    f2fs_pool_pipe) set_fs_mode FS_MODE_F2FS_POOL_PIPE; disable_copy_command; BASE_VER=".sosp26_ae" ;;
    f2fs_sb_pipe)   set_fs_mode FS_MODE_F2FS_SB_PIPE;   disable_copy_command; BASE_VER=".sosp26_ae" ;;
    scalelfs)
        # ScaleLFS lives in fs/scalelfs and is built with every kernel build;
        # CURRENT_FS_MODE (fs/f2fs only) is irrelevant to it and left as-is.
        # It runs on the same base kernel image as the non-copy modes.
        disable_copy_command; BASE_VER=".sosp26_ae"
        if [[ "$PROFILE" == "on" ]]; then
            echo "[switch_mode] ERROR: --profile is not supported for scalelfs." >&2
            exit 1
        fi
        ;;
    *)              usage ;;
esac

# Apply profile mode
if [[ "$PROFILE" == "on" ]]; then
    enable_profile
    set_extraversion "${BASE_VER}_profile"
else
    disable_profile
    set_extraversion "${BASE_VER}"
fi

# Save state
echo "$MODE" > "$MODE_FILE"
echo "$PROFILE" > "$PROFILE_FILE"

echo "[switch_mode] FS mode  : $MODE"
echo "[switch_mode] Profiling: $PROFILE"
echo "[switch_mode] Now run  : ./build_kernel.sh"
