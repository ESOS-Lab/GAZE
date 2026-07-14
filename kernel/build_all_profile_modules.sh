#!/bin/bash
# Build the *_profile.ko variant of every module-only filesystem mode, so
# that after ONE profile-kernel build + reboot you can flip between profiled
# modes with experiment_script/select_fs.sh (no rebuild, no reboot) -- the
# profile-mode counterpart of ./build_all_modules.sh.
#
# Profiling (GC_PROFILE_TIMESERIAL & friends in include/linux/jw_trace.h)
# changes code OUTSIDE fs/f2fs too, so profile modules only load on the
# PROFILE kernel image (6.1.0.sosp26_ae_profile), never on the base one.
# The tree must therefore be in profile state when running this:
#
#   ./switch_mode.sh gaze --profile     # sets jw_trace.h + EXTRAVERSION
#   ./build_kernel.sh                   # full build (also refreshes
#                                            #  Module.symvers) + grub entry
#   <reboot into 6.1.0.sosp26_ae_profile>
#   ./build_all_profile_modules.sh
#
# scalelfs has no profile variant (config.sh ignores .profile_mode for it).
# zns_plus --profile still needs its own kernel image (.sosp26_ae_copy_profile),
# same limitation as in build_all_modules.sh.
#
# The GC-latency-breakdown figure of the paper needs the f2fs_profile and
# f2fs_pipe_profile modules built here; see
# experiment_script/graph/OSDI26/pipe_gc_lat_breakdown_SOSP/make_graph.sh.

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE_DIR="$SCRIPT_DIR/../experiment_script/general_resource/filesystem_module"
cd "$SCRIPT_DIR"

BASE_VER=".sosp26_ae_profile"
KVER=$(grep "^EXTRAVERSION" Makefile | awk '{print $3}')

if [[ "$KVER" != "$BASE_VER" ]]; then
    echo "[build_all_profile_modules] ERROR: Makefile EXTRAVERSION is '$KVER', expected '$BASE_VER'." >&2
    echo "[build_all_profile_modules]   Run './switch_mode.sh gaze --profile' (+ ./build_kernel.sh once) first." >&2
    exit 1
fi
if ! grep -q '^#define GC_PROFILE_TIMESERIAL' include/linux/jw_trace.h; then
    echo "[build_all_profile_modules] ERROR: GC_PROFILE_TIMESERIAL is not enabled in include/linux/jw_trace.h." >&2
    echo "[build_all_profile_modules]   Run './switch_mode.sh gaze --profile' first." >&2
    exit 1
fi
if grep -q '^#define JW_COPY_COMMAND' include/linux/jw_trace.h; then
    echo "[build_all_profile_modules] ERROR: JW_COPY_COMMAND is enabled (zns_plus tree state)." >&2
    echo "[build_all_profile_modules]   Run './switch_mode.sh gaze --profile' first." >&2
    exit 1
fi
if [[ ! -f Module.symvers ]]; then
    echo "[build_all_profile_modules] ERROR: Module.symvers not found -- do a full kernel build first (./build_kernel.sh)." >&2
    exit 1
fi

declare -A MODE_CONST=(
    [gaze]=FS_MODE_GAZE
    [pgc]=FS_MODE_PGC
    [f2fs]=FS_MODE_F2FS
    [f2fs_pool]=FS_MODE_F2FS_POOL
    [f2fs_sb]=FS_MODE_F2FS_SB
    [f2fs_pipe]=FS_MODE_F2FS_PIPE
    [f2fs_pool_sb]=FS_MODE_F2FS_POOL_SB
    [f2fs_pool_pipe]=FS_MODE_F2FS_POOL_PIPE
    [f2fs_sb_pipe]=FS_MODE_F2FS_SB_PIPE
)
ORDER=(gaze pgc f2fs f2fs_pool f2fs_sb f2fs_pipe f2fs_pool_sb f2fs_pool_pipe f2fs_sb_pipe)

# Decide what to build from the arguments:
#   (no args)           every mode's profile variant (~1 h)
#   <mode> [<mode>...]  only those (each ~a few min)
BUILD_LIST=()
if [[ $# -eq 0 ]]; then
    BUILD_LIST=("${ORDER[@]}")
else
    for arg in "$@"; do
        if [[ -n "${MODE_CONST[$arg]:-}" ]]; then
            BUILD_LIST+=("$arg")
        else
            echo "[build_all_profile_modules] ERROR: unknown mode '$arg'." >&2
            echo "[build_all_profile_modules]   Valid: ${ORDER[*]} (no arg = all)." >&2
            exit 1
        fi
    done
fi

mkdir -p "$MODULE_DIR"

echo "[build_all_profile_modules] Building profile modules: ${BUILD_LIST[*]}"
echo "[build_all_profile_modules]   Each is a full module rebuild (a few minutes);"
echo "[build_all_profile_modules]   all of them together take roughly AN HOUR."

for mode in "${BUILD_LIST[@]}"; do
    echo "[build_all_profile_modules] === fs/f2fs as '${mode}_profile' (${MODE_CONST[$mode]}) ==="
    make M=fs/f2fs clean >/dev/null
    make -j"$(nproc)" M=fs/f2fs KCFLAGS="-DCURRENT_FS_MODE=${MODE_CONST[$mode]}" modules
    cp --remove-destination fs/f2fs/f2fs.ko "$MODULE_DIR/${mode}_profile.ko"
    echo "[build_all_profile_modules] -> $MODULE_DIR/${mode}_profile.ko"
done

make M=fs/f2fs clean >/dev/null

echo "[build_all_profile_modules] Done. Profile modules (load only on 6.1.0${BASE_VER}):"
ls -la "$MODULE_DIR"/*_profile.ko
