#!/bin/bash
# Usage: setup_fs.sh [setup|teardown]
#   setup    — check partition, remount fresh filesystem with the configured .ko
#   teardown — unmount and clean the mount point

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../../config.sh"

# ensure_emulator zns|conv — make sure NVMeVirt is loaded as the wanted
# variant, reloading it automatically when it's absent or in the other one.
# This is what lets an evaluator switch freely between scalelfs (conventional
# emulated SSD, 970PRO profile) and every ZNS-based mode (gaze/f2fs/pgc/...)
# without touching the emulator by hand: each benchmark's setup converges the
# device to what its filesystem needs. A reload resets the emulated device to
# factory state, and both setup paths mkfs fresh right after, so no stale
# on-device state ever leaks between filesystems.
ensure_emulator() {
    local want="$1" dev_name zoned="missing"
    dev_name="${ZNS_DEV##*/}"
    [[ -b "$ZNS_DEV" ]] && zoned=$(cat "/sys/block/$dev_name/queue/zoned" 2>/dev/null || echo missing)

    case "$want" in
        zns)  [[ "$zoned" != "none" && "$zoned" != "missing" ]] && return 0 ;;
        conv) [[ "$zoned" == "none" ]] && return 0 ;;
        *) echo "[setup_fs] BUG: ensure_emulator '$want'" >&2; exit 1 ;;
    esac

    if [[ "$zoned" == "missing" ]]; then
        echo "[setup_fs] NVMeVirt not loaded -- loading it in $want mode..."
    else
        echo "[setup_fs] NVMeVirt is in the wrong mode for $FS_MODE (zoned=$zoned) -- reloading as $want..."
    fi
    "$GAZE_DIR/ssd_emulator/stop_nvmevirt.sh" \
        || { echo "[setup_fs] ERROR: could not unload the running NVMeVirt (see above)." >&2; exit 1; }
    ( cd "$GAZE_DIR/ssd_emulator" && ./run_nvmevirt.sh "$want" ) \
        || { echo "[setup_fs] ERROR: NVMeVirt $want build/load failed." >&2; exit 1; }

    # run_nvmevirt.sh re-detects the device and rewrites .config -- refresh
    # the vars this script already sourced (device name can change across
    # reloads). SCALELFS_DEV tracks the new ZNS_DEV unless .config pins it.
    source "$GAZE_DIR/.config"
    grep -q '^SCALELFS_DEV=' "$GAZE_DIR/.config" 2>/dev/null || SCALELFS_DEV="$ZNS_DEV"

    [[ -b "$ZNS_DEV" ]] || { echo "[setup_fs] ERROR: emulated device $ZNS_DEV did not appear after reload." >&2; exit 1; }
}

setup_scalelfs() {
    # ScaleLFS path: single conventional device (no separate metadata SSD --
    # unlike the ZNS modes there is no CONV_PART pairing), traditional
    # f2fs-tools (1.16.0, non-zoned) for mkfs, fs type "scalelfs".
    # That toolchain is vendored at scalelfs-mkfs-tools/ (source only; the
    # binary is a gitignored build product) -- gate with the build command
    # instead of a bare exec failure mid-setup.
    if [[ ! -x "$MKFS_SCALELFS" ]]; then
        echo "[setup_fs] ERROR: ScaleLFS's own mkfs not found at $MKFS_SCALELFS." >&2
        echo "[setup_fs]   ScaleLFS must be formatted with traditional f2fs-tools 1.16.0" >&2
        echo "[setup_fs]   (NOT the repo's zoned 1.15.0). Build the vendored copy:" >&2
        echo "[setup_fs]     $GAZE_DIR/scalelfs-mkfs-tools/build_tools.sh" >&2
        echo "[setup_fs]   (./setup_env.sh also builds it; to use an external checkout" >&2
        echo "[setup_fs]   instead, set SCALELFS_TOOLS=<dir> in .config.)" >&2
        echo "[setup_fs]   Every other filesystem mode runs fine without this." >&2
        exit 1
    fi
    # Converge the emulator to the conventional 970PRO profile automatically.
    ensure_emulator conv

    echo "[setup_fs] Setting up scalelfs ($FS_KO) on $SCALELFS_DEV (conventional) → $MNT"

    sudo rm -rf "$MNT"/* 2>/dev/null || true
    sudo umount "$SCALELFS_DEV" 2>/dev/null || true
    sudo rmmod scalelfs 2>/dev/null || true
    sudo insmod "$MODULE_DIR/$FS_KO" || { echo "[setup_fs] ERROR: insmod $FS_KO failed." >&2; exit 1; }

    # Traditional ScaleLFS mkfs: plain non-zoned format of the whole device.
    # Its libtool build needs its own libf2fs on LD_LIBRARY_PATH.
    echo "[setup_fs] mkfs: $MKFS_SCALELFS"
    sudo env LD_LIBRARY_PATH="$MKFS_SCALELFS_LIB" "$MKFS_SCALELFS" -f "$SCALELFS_DEV" \
        || { echo "[setup_fs] ERROR: scalelfs mkfs.f2fs failed." >&2; exit 1; }
    sudo mount -t scalelfs -o nobarrier -o mode=lfs "$SCALELFS_DEV" "$MNT" \
        || { echo "[setup_fs] ERROR: mount -t scalelfs failed." >&2; exit 1; }
    # Keep the benchmark mount out of every namespace snapshot (see the
    # make-unbindable comment in setup() below for the full story).
    sudo mount --make-unbindable "$MNT"

    echo "[setup_fs] Mounted $SCALELFS_DEV on $MNT (scalelfs)"
}

# IPLFS path (branch 'iplfs' only): the IPLFS artifact's own kernel,
# emulator, and mkfs-tools — everything else (this script included) is the
# same on both branches. Flow mirrors the original artifact's iplfs.sh:
# partition the emulated device, insmod the 5.11-built iplfs module, format
# the partition with IPLFS's modified f2fs-tools, mount as f2fs mode=lfs.
setup_iplfs() {
    if [[ "$(uname -r)" != 5.11.0.iplfs* ]]; then
        echo "[setup_fs] ERROR: iplfs needs kernel 5.11.0.iplfs (running: $(uname -r))." >&2
        echo "[setup_fs]   Build it from this branch's kernel/ and reboot into it first." >&2
        exit 1
    fi
    [[ -x "$MKFS_IPLFS" ]] || {
        echo "[setup_fs] ERROR: $MKFS_IPLFS not found/executable." >&2
        echo "[setup_fs]   Build it: cd $GAZE_DIR/mkfs-tools && ./build_tools.sh" >&2
        exit 1
    }
    [[ -f "$MODULE_DIR/iplfs.ko" ]] || {
        echo "[setup_fs] ERROR: $MODULE_DIR/iplfs.ko not found." >&2
        echo "[setup_fs]   After building this branch's kernel:" >&2
        echo "[setup_fs]     cp $GAZE_DIR/kernel/fs/f2fs/f2fs.ko $MODULE_DIR/iplfs.ko" >&2
        exit 1
    }

    # IPLFS's own NVMeVirt (this branch's ssd_emulator/, conventional
    # target). Load it if the emulated device isn't up; the branch's
    # run_nvmevirt.sh rebuilds, insmods, and rewrites ZNS_DEV in .config.
    if [[ ! -b "$ZNS_DEV" ]]; then
        echo "[setup_fs] NVMeVirt not loaded -- loading the IPLFS emulator..."
        ( cd "$GAZE_DIR/ssd_emulator" && ./run_nvmevirt.sh ) \
            || { echo "[setup_fs] ERROR: IPLFS NVMeVirt build/load failed." >&2; exit 1; }
        source "$GAZE_DIR/.config"
        IPLFS_PART="${ZNS_DEV}p1"
        [[ -b "$ZNS_DEV" ]] || { echo "[setup_fs] ERROR: emulated device $ZNS_DEV did not appear." >&2; exit 1; }
    fi

    echo "[setup_fs] Setting up iplfs (iplfs.ko) on $IPLFS_PART → $MNT"

    sudo rm -rf "$MNT"/* 2>/dev/null || true
    sudo umount "$IPLFS_PART" 2>/dev/null || true
    sudo rmmod f2fs 2>/dev/null || true
    sudo insmod "$MODULE_DIR/iplfs.ko" || { echo "[setup_fs] ERROR: insmod iplfs.ko failed." >&2; exit 1; }

    # single partition spanning the emulated device (fresh after each
    # emulator reload; recreating it is idempotent)
    if ! lsblk "$IPLFS_PART" &>/dev/null; then
        echo -e "n\n\n\n\n\nw" | sudo fdisk "$ZNS_DEV" >/dev/null
        sudo partprobe "$ZNS_DEV" 2>/dev/null || true
        sleep 1
        lsblk "$IPLFS_PART" &>/dev/null || { echo "[setup_fs] ERROR: could not create $IPLFS_PART." >&2; exit 1; }
    fi

    sudo "$MKFS_IPLFS" -f "$IPLFS_PART" || { echo "[setup_fs] ERROR: IPLFS mkfs.f2fs failed." >&2; exit 1; }
    sudo mount -t f2fs -o mode=lfs -o discard "$IPLFS_PART" "$MNT" \
        || { echo "[setup_fs] ERROR: mount failed." >&2; exit 1; }
    sudo mount --make-unbindable "$MNT"

    echo "[setup_fs] Mounted $IPLFS_PART on $MNT (iplfs)"
}

setup() {
    # Wrong-branch runs are refused up front: iplfs needs this repo's
    # 'iplfs' branch (its kernel/emulator/mkfs sources); every other mode
    # needs 'main'. See require_branch_for_fs_mode in config.sh.
    require_branch_for_fs_mode

    if [[ "$FS_MODE" == "iplfs" ]]; then
        setup_iplfs
        return
    fi
    if [[ "$FS_MODE" == "scalelfs" ]]; then
        setup_scalelfs
        return
    fi

    # Safety check: refuse to run if partition doesn't exist.
    # Never auto-create partitions to avoid destroying evaluator's disk.
    if ! lsblk "$CONV_PART" &>/dev/null; then
        echo "[setup_fs] ERROR: Partition $CONV_PART not found." >&2
        echo "[setup_fs]   Please create a ~15GB partition on $CONV_DEV manually:" >&2
        echo "[setup_fs]     sudo fdisk $CONV_DEV  →  n (new) → +15G → w (write)" >&2
        exit 1
    fi

    # The GAZE/f2fs family needs the emulated device in ZNS mode (e.g. right
    # after a scalelfs run left it conventional). Converge automatically.
    ensure_emulator zns

    echo "[setup_fs] Setting up $FS_NAME ($FS_KO) on $CONV_PART (ZNS: $ZNS_DEV) → $MNT"

    sudo rm -rf "$MNT"/* 2>/dev/null || true
    sudo umount "$CONV_PART" 2>/dev/null || true

    # A stale in-memory f2fs (and the lz4/lz4hc compress modules it pulled in)
    # left over from a previous boot/build can have different modversions CRCs
    # than the just-rebuilt $FS_KO, causing "Unknown symbol LZ4_compress_*" on
    # insmod. Drop them and reload the on-disk copies (built alongside $FS_KO)
    # so symbol versions match. A leftover scalelfs from a previous scalelfs
    # run is dropped for the same reason.
    sudo rmmod scalelfs 2>/dev/null || true
    sudo rmmod f2fs 2>/dev/null || true
    sudo rmmod lz4hc_compress 2>/dev/null || true
    sudo rmmod lz4_compress 2>/dev/null || true
    sudo insmod "/lib/modules/$(uname -r)/kernel/lib/lz4/lz4_compress.ko" 2>/dev/null || true
    sudo insmod "/lib/modules/$(uname -r)/kernel/lib/lz4/lz4hc_compress.ko" 2>/dev/null || true
    sudo insmod "$MODULE_DIR/$FS_KO" || { echo "[setup_fs] ERROR: insmod $FS_KO failed." >&2; exit 1; }

    sudo "$MKFS_F2FS" -f -m -s 512 -c "$ZNS_DEV" "$CONV_PART" || { echo "[setup_fs] ERROR: mkfs.f2fs failed." >&2; exit 1; }
    sudo mount -t f2fs -o discard -o nobarrier -o mode=lfs "$CONV_PART" "$MNT" || { echo "[setup_fs] ERROR: mount failed." >&2; exit 1; }
    # make-unbindable: exclude this mount from the recursive mount-tree copy
    # every new mount namespace takes. Without it, anything that snapshots a
    # namespace mid-run (snap-confine most of all -- a snap timer firing
    # during a benchmark preserves its namespace in /run/snapd/ns/*.mnt with
    # no owning process) keeps a clone of this mount alive after our umount.
    # The superblock then survives with its GC kthreads, the module refcount
    # stays pinned, and the next mode's insmod fails with 'File exists'
    # (observed 2026-07-12: firmware-updater timer fired at midnight during a
    # YCSB-A run; recovery is snap-discard-ns <snap>).
    sudo mount --make-unbindable "$MNT"

    echo "[setup_fs] Mounted $CONV_PART on $MNT"
}

teardown() {
    echo "[setup_fs] Tearing down $MNT"
    sudo rm -rf "$MNT"/* 2>/dev/null || true
    sudo umount "$MNT" 2>/dev/null || true
    # Verify instead of assuming: the old unconditional "Unmounted" message
    # hid a real umount failure and let the next run start on a stale fs.
    if mountpoint -q "$MNT"; then
        echo "[setup_fs] ERROR: $MNT is still mounted after umount:" >&2
        sudo umount "$MNT" || true   # retry once, loudly this time
        mountpoint -q "$MNT" && { echo "[setup_fs] ERROR: umount failed -- check 'fuser -vm $MNT'." >&2; exit 1; }
    fi
    # The umount can also succeed while the superblock lives on in a leaked
    # namespace clone (see the make-unbindable comment in setup()); that
    # state breaks the NEXT run's insmod, so flag it now while the cause is
    # fresh. Only f2fs-family modes expose this via /sys/fs/f2fs/<dev>.
    local dev_base="${CONV_PART##*/}"
    if [[ -d "/sys/fs/f2fs/$dev_base" ]]; then
        echo "[setup_fs] WARNING: superblock for $dev_base is still alive after umount" >&2
        echo "[setup_fs]   (likely a leaked mount-namespace clone; check /run/snapd/ns/*.mnt:" >&2
        echo "[setup_fs]    for ns in /run/snapd/ns/*.mnt; do sudo nsenter --mount=\$ns grep $dev_base /proc/self/mountinfo && echo \$ns; done" >&2
        echo "[setup_fs]    then: sudo /usr/lib/snapd/snap-discard-ns <snap>)" >&2
    fi
    echo "[setup_fs] Unmounted $MNT"
}

case "$1" in
    setup)    setup ;;
    teardown) teardown ;;
    *) echo "Usage: $0 [setup|teardown]" >&2; exit 1 ;;
esac
