#!/bin/bash
# Build EVERY module-only filesystem mode from the single base kernel image,
# with no reboot between modes.
#
# Background: the 9 non-copy FS modes (gaze, pgc, f2fs, f2fs_pool, f2fs_sb,
# f2fs_pipe, f2fs_pool_sb, f2fs_pool_pipe, f2fs_sb_pipe) differ ONLY in the
# compile-time CURRENT_FS_MODE of the fs/f2fs module -- the kernel image
# (vmlinuz 6.1.0.sosp26_ae) is identical for all of them, and fs/f2fs is
# built as a module (CONFIG_F2FS_FS=m). So after ONE full kernel build +
# reboot into 6.1.0.sosp26_ae, this script produces all nine .ko variants by
# rebuilding just fs/f2fs with -DCURRENT_FS_MODE=<mode> (via the #ifndef
# guard in fs/f2fs/f2fs.h), plus scalelfs.ko (mode-independent). Switching
# between them afterwards is experiment_script/select_fs.sh -- module swap
# only, no rebuild, no reboot.
#
# NOT covered (these change code outside the fs module, i.e. a different
# kernel image, so they still need switch_mode.sh + build_kernel.sh + reboot):
#   - zns_plus  (JW_COPY_COMMAND: block layer / NVMe driver -> .sosp26_ae_copy)
#   - --profile (GC_*_TIMESERIAL in jw_trace.h -> .sosp26_ae*_profile)
#
# Usage: ./build_all_modules.sh   (run after a successful base kernel build)

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE_DIR="$SCRIPT_DIR/../experiment_script/general_resource/filesystem_module"
cd "$SCRIPT_DIR"

BASE_VER=".sosp26_ae"
KVER=$(grep "^EXTRAVERSION" Makefile | awk '{print $3}')

# The tree must be in plain base state: modules built here must match the
# base image's vermagic and must not carry zns_plus/profile kernel-side state.
if [[ "$KVER" != "$BASE_VER" ]]; then
    echo "[build_all_modules] ERROR: Makefile EXTRAVERSION is '$KVER', expected '$BASE_VER'." >&2
    echo "[build_all_modules]   Run './switch_mode.sh gaze' (+ ./build_kernel.sh once) first." >&2
    exit 1
fi
if grep -q '^#define JW_COPY_COMMAND' include/linux/jw_trace.h; then
    echo "[build_all_modules] ERROR: JW_COPY_COMMAND is enabled (zns_plus tree state)." >&2
    echo "[build_all_modules]   Run './switch_mode.sh gaze' first." >&2
    exit 1
fi
if [[ ! -f Module.symvers ]]; then
    echo "[build_all_modules] ERROR: Module.symvers not found -- do a full kernel build first (./build_kernel.sh)." >&2
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
# The 8 modes the ablation sweep (paper Fig 15) needs: every f2fs-family mode
# except pgc.
ABLATION=(gaze f2fs f2fs_pool f2fs_sb f2fs_pipe f2fs_pool_sb f2fs_pool_pipe f2fs_sb_pipe)

# Decide what to build from the arguments:
#   (no args)           every base module-only mode + scalelfs (~1 h)
#   ablation            just the 8 ablation-sweep modes
#   <mode> [<mode>...]  only those (each f2fs mode ~a few min; 'scalelfs' allowed)
F2FS_BUILD_LIST=()
BUILD_SCALELFS=0
if [[ $# -eq 0 ]]; then
    F2FS_BUILD_LIST=("${ORDER[@]}")
    BUILD_SCALELFS=1
elif [[ $# -eq 1 && "$1" == "ablation" ]]; then
    F2FS_BUILD_LIST=("${ABLATION[@]}")
else
    for arg in "$@"; do
        if [[ "$arg" == "scalelfs" ]]; then
            BUILD_SCALELFS=1
        elif [[ -n "${MODE_CONST[$arg]:-}" ]]; then
            F2FS_BUILD_LIST+=("$arg")
        else
            echo "[build_all_modules] ERROR: unknown mode '$arg'." >&2
            echo "[build_all_modules]   Valid: ${ORDER[*]} scalelfs ablation (no arg = all)." >&2
            exit 1
        fi
    done
fi

mkdir -p "$MODULE_DIR"

echo "[build_all_modules] Building: ${F2FS_BUILD_LIST[*]}$([[ $BUILD_SCALELFS -eq 1 ]] && echo ' scalelfs')"
echo "[build_all_modules]   Each f2fs-family mode is a full module rebuild (a few minutes);"
echo "[build_all_modules]   all base modes together take roughly AN HOUR on the paper machine."

for mode in "${F2FS_BUILD_LIST[@]}"; do
    echo "[build_all_modules] === fs/f2fs as '$mode' (${MODE_CONST[$mode]}) ==="
    # Full rebuild of the module each time: the mode flag changes the
    # command line of every object, so kbuild recompiles everything anyway.
    make M=fs/f2fs clean >/dev/null
    make -j"$(nproc)" M=fs/f2fs KCFLAGS="-DCURRENT_FS_MODE=${MODE_CONST[$mode]}" modules
    cp --remove-destination fs/f2fs/f2fs.ko "$MODULE_DIR/${mode}.ko"
    echo "[build_all_modules] -> $MODULE_DIR/${mode}.ko"
done

# Leave fs/f2fs rebuilt in the tree's own (f2fs.h) mode so a later plain
# kernel build isn't left with stale KCFLAGS objects.
if [[ ${#F2FS_BUILD_LIST[@]} -gt 0 ]]; then
    make M=fs/f2fs clean >/dev/null
fi

if [[ $BUILD_SCALELFS -eq 1 ]]; then
    echo "[build_all_modules] === fs/scalelfs (mode-independent) ==="
    # fs/scalelfs is gated by CONFIG_F3FS_FS — a GAZE-local symbol that a
    # .config seeded from a stock /boot config leaves off. With it off, the
    # M= build below silently produces nothing and the cp would stage a stale
    # artifact from an older (possibly profile/zns_plus) build. Force it on.
    if ! grep -q '^CONFIG_F3FS_FS=m' .config; then
        echo "[build_all_modules] Enabling CONFIG_F3FS_FS=m (scalelfs) in .config"
        ./scripts/config --module F3FS_FS
        make olddefconfig >/dev/null
        make modules_prepare >/dev/null
    fi
    make -j"$(nproc)" M=fs/scalelfs modules
    SCALELFS_VM=$(modinfo -F vermagic fs/scalelfs/scalelfs.ko 2>/dev/null | awk '{print $1}')
    if [[ "$SCALELFS_VM" != "6.1.0${BASE_VER}" ]]; then
        echo "[build_all_modules] ERROR: scalelfs.ko vermagic '$SCALELFS_VM' != 6.1.0${BASE_VER} — not staging it." >&2
        exit 1
    fi
    cp --remove-destination fs/scalelfs/scalelfs.ko "$MODULE_DIR/scalelfs.ko"
    echo "[build_all_modules] -> $MODULE_DIR/scalelfs.ko"
fi

echo
echo "[build_all_modules] Modules now staged for 6.1.0${BASE_VER} in $MODULE_DIR:"
ls -1 "$MODULE_DIR"
echo "[build_all_modules] Switch between them with experiment_script/select_fs.sh (no rebuild/reboot)."
echo "[build_all_modules] zns_plus and --profile variants still need switch_mode.sh + build_kernel.sh + reboot."
