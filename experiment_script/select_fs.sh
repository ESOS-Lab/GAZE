#!/bin/bash
# Interactive filesystem-mode picker for GAZE experiments.
#
# This only switches between .ko modules that are already built for the
# kernel you're currently running (writes kernel/.build_mode and
# kernel/.profile_mode, which config.sh reads). It never touches kernel
# source or recompiles anything -- that's kernel/switch_mode.sh's job, and
# it's only needed once, before a `kernel/build_kernel.sh` + reboot.
# Keeping the two separate is what lets you pre-build all the modules you
# want once, then just re-run this script (no sudo, no reboot) to flip
# between them for experiments.
#
# Usage: ./select_fs.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GAZE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
MODULE_DIR="$GAZE_DIR/experiment_script/general_resource/filesystem_module"
BUILD_MODE_FILE="$GAZE_DIR/kernel/.build_mode"
PROFILE_MODE_FILE="$GAZE_DIR/kernel/.profile_mode"

ORDER=(gaze pgc f2fs zns_plus f2fs_pool f2fs_sb f2fs_pipe f2fs_pool_sb f2fs_pool_pipe f2fs_sb_pipe scalelfs iplfs)
declare -A DESCR=(
    [gaze]="GAZE (GC Page Pool + Pipelined GC + Selective Blocking)"
    [pgc]="P-GC (Parallel GC baseline)"
    [f2fs]="Vanilla zoned F2FS"
    [zns_plus]="ZNS+ (copy-offload) -- different kernel build, needs reboot"
    [f2fs_pool]="F2FS + GC Page Pool"
    [f2fs_sb]="F2FS + Selective Blocking"
    [f2fs_pipe]="F2FS + Pipelined GC"
    [f2fs_pool_sb]="F2FS + GC Page Pool + Selective Blocking"
    [f2fs_pool_pipe]="F2FS + GC Page Pool + Pipelined GC"
    [f2fs_sb_pipe]="F2FS + Selective Blocking + Pipelined GC"
    [scalelfs]="ScaleLFS baseline -- conventional (non-zoned) emulated SSD + traditional f2fs-tools"
    [iplfs]="IPLFS baseline -- git branch 'iplfs' (own 5.11 kernel + emulator + mkfs), needs reboot"
)

echo "Available filesystem modes:"
i=1
for m in "${ORDER[@]}"; do
    printf "  %2d) %-16s %s\n" "$i" "$m" "${DESCR[$m]}"
    i=$((i+1))
done
echo

MODE=""
while true; do
    read -r -p "Select a mode (number or name): " PICK
    if [[ "$PICK" =~ ^[0-9]+$ ]] && [ "$PICK" -ge 1 ] && [ "$PICK" -le "${#ORDER[@]}" ]; then
        MODE="${ORDER[$((PICK-1))]}"
        break
    elif [[ -n "${DESCR[$PICK]:-}" ]]; then
        MODE="$PICK"
        break
    fi
    echo "  -> not a valid choice, try again."
done

PROFILE_STATE=off
if [[ "$MODE" == "scalelfs" || "$MODE" == "iplfs" ]]; then
    echo "(GC profiling is not available for $MODE.)"
else
    read -r -p "Enable GC profiling? [y/N]: " PROF
    [[ "$PROF" =~ ^[Yy]$ ]] && PROFILE_STATE=on
fi

# IPLFS lives on its own git branch (kernel/emulator/mkfs-tools differ;
# every script is identical across branches) and its own kernel image.
CUR_BRANCH=$(git -C "$GAZE_DIR" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)
if [[ "$MODE" == "iplfs" && "$CUR_BRANCH" != "iplfs" ]]; then
    echo
    echo "[select_fs] iplfs needs git branch 'iplfs' (current: $CUR_BRANCH)."
    echo "[select_fs]   1. git -C $GAZE_DIR checkout iplfs    (run data survives checkouts)"
    echo "[select_fs]   2. re-run this script, then build/boot its 5.11 kernel if prompted."
    echo "[select_fs] (.build_mode left unchanged.)"
    exit 1
fi
if [[ "$MODE" != "iplfs" && "$CUR_BRANCH" == "iplfs" ]]; then
    echo
    echo "[select_fs] '$MODE' runs from branch 'main'; this branch (iplfs) carries the"
    echo "[select_fs]   IPLFS kernel/emulator sources. git -C $GAZE_DIR checkout main first."
    echo "[select_fs] (.build_mode left unchanged.)"
    exit 1
fi

# Same BASE_VER mapping switch_mode.sh uses when it sets EXTRAVERSION, kept
# in sync manually since we don't invoke that script here.
BASE_VER=".sosp26_ae"
[[ "$MODE" == "zns_plus" ]] && BASE_VER=".sosp26_ae_copy"
TARGET_KVER="6.1.0${BASE_VER}"
[[ "$MODE" == "iplfs" ]] && TARGET_KVER="5.11.0.iplfs"
[[ "$PROFILE_STATE" == "on" ]] && TARGET_KVER="${TARGET_KVER}_profile"
CURRENT_KVER=$(uname -r)

DEST="${MODE}.ko"
[[ "$PROFILE_STATE" == "on" ]] && DEST="${MODE}_profile.ko"

echo
if [[ "$CURRENT_KVER" == "$TARGET_KVER" ]] && [[ -f "$MODULE_DIR/$DEST" ]]; then
    echo "$MODE" > "$BUILD_MODE_FILE"
    echo "$PROFILE_STATE" > "$PROFILE_MODE_FILE"
    echo "[select_fs] Switched to $MODE ($DEST) -- already built for the running kernel ($CURRENT_KVER)."
    echo "[select_fs] No rebuild or reboot needed -- experiment scripts will pick it up now."
    if [[ "$MODE" == "scalelfs" ]]; then
        echo "[select_fs] NOTE: scalelfs runs on a CONVENTIONAL emulated SSD (970PRO profile, own"
        echo "[select_fs]   traditional f2fs-tools mkfs, no separate metadata SSD). The benchmark"
        echo "[select_fs]   scripts reload the emulator into the right mode automatically."
    elif [[ "$MODE" == "iplfs" ]]; then
        echo "[select_fs] NOTE: iplfs uses this branch's own emulator and mkfs-tools; the"
        echo "[select_fs]   benchmark scripts load them automatically."
    else
        echo "[select_fs] NOTE: $MODE needs the emulated SSD in ZNS mode -- the benchmark scripts"
        echo "[select_fs]   reload the emulator automatically (even right after a scalelfs run)."
    fi
else
    echo "[select_fs] Can't switch to $MODE with profiling=$PROFILE_STATE from the running kernel ($CURRENT_KVER)."
    if [[ "$CURRENT_KVER" != "$TARGET_KVER" ]]; then
        echo "[select_fs]   That combination needs a different kernel build ($TARGET_KVER)."
        if [[ "$MODE" == "iplfs" ]]; then
            echo "[select_fs]   Build this branch's IPLFS kernel: cd $GAZE_DIR/kernel && ./build_kernel.sh"
            echo "[select_fs]   then copy the module: cp fs/f2fs/f2fs.ko $MODULE_DIR/iplfs.ko, and reboot into $TARGET_KVER."
        else
            echo "[select_fs]   Run: $GAZE_DIR/kernel/switch_mode.sh $MODE $([[ "$PROFILE_STATE" == "on" ]] && echo --profile)"
            echo "[select_fs]   then: $GAZE_DIR/kernel/build_kernel.sh, then reboot into it."
        fi
    else
        echo "[select_fs]   $DEST hasn't been built yet. Run: $GAZE_DIR/kernel/build_kernel.sh"
    fi
    echo "[select_fs] (.build_mode/.profile_mode left unchanged -- still on $(cat "$BUILD_MODE_FILE" 2>/dev/null || echo unset).)"
fi
