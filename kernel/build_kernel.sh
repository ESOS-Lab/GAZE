#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODE_FILE="$SCRIPT_DIR/.build_mode"
PROFILE_FILE="$SCRIPT_DIR/.profile_mode"
MODULE_DIR="$SCRIPT_DIR/../experiment_script/general_resource/filesystem_module"

# Everything below (make, fs/* paths) assumes the kernel tree as cwd; don't
# depend on where the user invoked us from.
cd "$SCRIPT_DIR" || exit 1

if [ ! -f "$MODE_FILE" ]; then
    echo "[build_kernel] No mode set. Run switch_mode.sh first, or defaulting to gaze."
    MODE="gaze"
else
    MODE=$(cat "$MODE_FILE")
fi

PROFILE=off
[[ -f "$PROFILE_FILE" ]] && PROFILE=$(cat "$PROFILE_FILE")

echo "[build_kernel] Building in mode: $MODE  (profiling: $PROFILE)"

# Kernel module version string follows EXTRAVERSION in Makefile
KVER=$(grep "^EXTRAVERSION" "$SCRIPT_DIR/Makefile" | awk '{print $3}')
MODULE_PATH="/lib/modules/6.1.0${KVER}"

# Seed .config non-interactively if the tree is fresh. Otherwise `make`
# drops the user into Kconfig/menuconfig prompts and the AE flow stalls.
# Prefer, in order: an exact-match previous install, any 6.1.0 variant,
# the running kernel, then defconfig as last resort. `olddefconfig`
# resolves any new symbols with their default answer.
if [ ! -f "$SCRIPT_DIR/.config" ]; then
    echo "[build_kernel] No .config found — bootstrapping non-interactively."
    SEED=""
    # Try, in order: exact-match previous install → any 6.1.0 variant
    # left over from earlier builds → the running kernel's config.
    for CAND in \
        "/boot/config-6.1.0${KVER}" \
        /boot/config-6.1.0 \
        /boot/config-6.1.0.sosp26_ae \
        /boot/config-6.1.0.profile \
        /boot/config-6.1.0.sad \
        "/boot/config-$(uname -r)"; do
        if [ -f "$CAND" ]; then SEED="$CAND"; break; fi
    done
    if [ -n "$SEED" ]; then
        echo "[build_kernel] Seeding .config from $SEED"
        cp "$SEED" "$SCRIPT_DIR/.config"
        ( cd "$SCRIPT_DIR" && make olddefconfig >/dev/null )
    else
        echo "[build_kernel] No /boot/config-* baseline found — falling back to 'make defconfig' (may miss GAZE-specific options)." >&2
        ( cd "$SCRIPT_DIR" && make defconfig && make olddefconfig >/dev/null )
    fi
fi

# fs/scalelfs (and fs/pgc) are gated by CONFIG_F3FS_FS — a GAZE-local
# symbol, so a .config seeded from a stock /boot config leaves it off
# (olddefconfig defaults it to n) and fs/scalelfs silently never builds.
# Then the staging below would copy a stale scalelfs.ko from an older
# build with the wrong vermagic (this happened on 2026-07-13: a base build
# staged a _profile-vermagic scalelfs.ko). Force it on for every build;
# the vermagic check at staging keeps wrong-kernel copies out regardless.
if ! grep -q '^CONFIG_F3FS_FS=m' "$SCRIPT_DIR/.config"; then
    echo "[build_kernel] Enabling CONFIG_F3FS_FS=m (scalelfs) in .config"
    ./scripts/config --module F3FS_FS
    make olddefconfig >/dev/null
fi

make -j40 && \
sudo make modules_install -j40 && \
sudo find "${MODULE_PATH}" -name "*.ko" -exec strip --strip-unneeded {} + && \
sudo make install -j40 || exit 1

case "$MODE" in
    gaze)           DEST="gaze.ko" ;;
    pgc)            DEST="pgc.ko" ;;
    f2fs)           DEST="f2fs.ko" ;;
    zns_plus)       DEST="zns_plus.ko" ;;
    f2fs_pool)      DEST="f2fs_pool.ko" ;;
    f2fs_sb)        DEST="f2fs_sb.ko" ;;
    f2fs_pipe)      DEST="f2fs_pipe.ko" ;;
    f2fs_pool_sb)   DEST="f2fs_pool_sb.ko" ;;
    f2fs_pool_pipe) DEST="f2fs_pool_pipe.ko" ;;
    f2fs_sb_pipe)   DEST="f2fs_sb_pipe.ko" ;;
    scalelfs)       DEST="" ;;   # scalelfs.ko is copied unconditionally below
    *)
        echo "[build_kernel] Unknown mode '$MODE', skipping .ko copy."
        exit 1
        ;;
esac

if [[ -n "$DEST" ]]; then
    [[ "$PROFILE" == "on" ]] && DEST="${DEST%.ko}_profile.ko"
    # --remove-destination: the existing .ko may be owned by root (a
    # previous sudo build); plain cp then fails with EACCES even though the
    # dir is ours. Unlinking first only needs dir write permission.
    cp --remove-destination fs/f2fs/f2fs.ko "$MODULE_DIR/$DEST"
    echo "[build_kernel] Copied f2fs.ko -> $DEST"
fi

# scalelfs (fs/scalelfs) is mode-independent and built with every kernel
# build (CONFIG_F3FS_FS=m, forced above), so refresh its copy in the module
# dir — but only when building the base kernel: scalelfs only ever runs on
# 6.1.0.sosp26_ae (select_fs.sh enforces this), so a zns_plus/profile build
# would overwrite the good copy with one whose vermagic no longer loads
# there. Never trust the on-disk artifact blindly: verify its vermagic
# matches this build, and rebuild fs/scalelfs if it doesn't (a stale .ko
# from an earlier profile/zns_plus build otherwise gets staged and then
# fails to insmod on the base kernel).
if [[ "$KVER" == ".sosp26_ae" ]]; then
    SCALELFS_KO="fs/scalelfs/scalelfs.ko"
    ARTIFACT_VM=""
    [[ -f "$SCALELFS_KO" ]] && ARTIFACT_VM=$(modinfo -F vermagic "$SCALELFS_KO" 2>/dev/null | awk '{print $1}')
    if [[ "$ARTIFACT_VM" != "6.1.0${KVER}" ]]; then
        echo "[build_kernel] fs/scalelfs/scalelfs.ko missing or stale (vermagic '${ARTIFACT_VM:-none}', want 6.1.0${KVER}) — rebuilding fs/scalelfs..."
        make -j"$(nproc)" M=fs/scalelfs modules
        ARTIFACT_VM=$(modinfo -F vermagic "$SCALELFS_KO" 2>/dev/null | awk '{print $1}')
    fi
    if [[ "$ARTIFACT_VM" == "6.1.0${KVER}" ]]; then
        cp --remove-destination "$SCALELFS_KO" "$MODULE_DIR/scalelfs.ko"
        echo "[build_kernel] Copied fs/scalelfs/scalelfs.ko -> scalelfs.ko"
    else
        echo "[build_kernel] WARNING: no scalelfs.ko with vermagic 6.1.0${KVER} could be produced — NOT staging it (the scalelfs mode would fail to insmod). Check CONFIG_F3FS_FS." >&2
    fi
fi

TARGET_KVER="6.1.0${KVER}"
CURRENT_KVER=$(uname -r)
GRUB_FILE="/etc/default/grub"
GRUB_CFG="/boot/grub/grub.cfg"

# Make the kernel we just built the *persistent* default (requires
# /etc/default/grub: GRUB_DEFAULT=saved, GRUB_SAVEDEFAULT=true — setup_env.sh
# sets these). Every reboot boots this kernel from now on, not just the
# next one, until either this script runs again with a different mode, or
# someone manually picks a different entry at the boot menu (which
# GRUB_SAVEDEFAULT=true then saves as the new default in their turn — so
# other users on a shared machine can still override it for themselves).
# `make install` above already ran update-grub via /etc/kernel/postinst.d,
# so the menu entry exists iff the image landed in /boot. Select the target
# by *menuentry ID*, not by menu title: `grub-set-default` stores whatever
# string is passed and grub then matches it against menu titles OR ids.
# Titles are locale-translated by gettext (e.g. Korean "Ubuntu용 고급 설정"
# instead of English "Advanced options for Ubuntu"), so a title-based
# lookup silently fails on non-English systems and GRUB_SAVEDEFAULT=true
# then persists whatever the user actually booted. IDs stay in English.
GRUB_OK=0
if [ -f "/boot/vmlinuz-${TARGET_KVER}" ]; then
    # Ask before touching $GRUB_FILE: this is a shared-machine setting that
    # affects every user's next boot. On yes, force GRUB_TIMEOUT=7 (and
    # GRUB_RECORDFAIL_TIMEOUT=7 so the same window applies after an unclean
    # shutdown — otherwise Ubuntu falls back to a -1 wait) so the menu is
    # visible long enough for someone else to override.
    echo ""
    echo "[build_kernel] ============================================================"
    echo "[build_kernel]  GRUB modification prompt"
    echo "[build_kernel]  About to edit $GRUB_FILE and update grub:"
    echo "[build_kernel]    (1) set GRUB_TIMEOUT=7 and GRUB_RECORDFAIL_TIMEOUT=7"
    echo "[build_kernel]        (menu visible 7s at boot, incl. after recordfail)"
    echo "[build_kernel]    (2) set default boot entry to ${TARGET_KVER}"
    echo "[build_kernel]  This affects every user of this machine on the next"
    echo "[build_kernel]  reboot. A backup of $GRUB_FILE is saved alongside it."
    echo "[build_kernel]  Others still get a 7-second window at boot to pick a"
    echo "[build_kernel]  different kernel."
    echo "[build_kernel] ============================================================"
    read -r -p "[build_kernel] Proceed with GRUB changes? [y/N] " REPLY
    if [[ "$REPLY" =~ ^[Yy]$ ]]; then
        sudo cp "$GRUB_FILE" "${GRUB_FILE}.bak_$(date +%Y%m%d_%H%M%S)" \
            || echo "[build_kernel] WARNING: could not back up $GRUB_FILE" >&2
        if sudo grep -q '^[[:space:]]*GRUB_TIMEOUT=' "$GRUB_FILE"; then
            sudo sed -i 's/^[[:space:]]*GRUB_TIMEOUT=.*/GRUB_TIMEOUT=7/' "$GRUB_FILE"
        else
            echo "GRUB_TIMEOUT=7" | sudo tee -a "$GRUB_FILE" > /dev/null
        fi
        if sudo grep -q '^[[:space:]]*GRUB_RECORDFAIL_TIMEOUT=' "$GRUB_FILE"; then
            sudo sed -i 's/^[[:space:]]*GRUB_RECORDFAIL_TIMEOUT=.*/GRUB_RECORDFAIL_TIMEOUT=7/' "$GRUB_FILE"
        else
            echo "GRUB_RECORDFAIL_TIMEOUT=7" | sudo tee -a "$GRUB_FILE" > /dev/null
        fi
        sudo update-grub || echo "[build_kernel] WARNING: update-grub failed" >&2
        # Parse menuentry IDs from grub.cfg (locale-independent). Layout:
        #   submenu '<translated title>' $menuentry_id_option '<submenu-id>' {
        #     menuentry '<translated title>' ... $menuentry_id_option '<entry-id>' {
        # Field 4 with -F"'" is the id in both cases. grub.cfg is root-only
        # readable, so read it once via `sudo cat` (that exact command is in
        # the evaluator account's sudo whitelist; `sudo awk` is not) and run
        # awk unprivileged on the captured text.
        GRUB_CFG_TEXT=$(sudo cat "$GRUB_CFG" 2>/dev/null)
        SUBMENU_ID=$(awk -F"'" '/^submenu / {print $4; exit}' <<<"$GRUB_CFG_TEXT")
        TARGET_ID=$(awk -F"'" -v k="${TARGET_KVER}" '$4 ~ ("^gnulinux-" k "-advanced-") {print $4; exit}' <<<"$GRUB_CFG_TEXT")
        if [ -n "$SUBMENU_ID" ] && [ -n "$TARGET_ID" ]; then
            if sudo grub-set-default "${SUBMENU_ID}>${TARGET_ID}"; then
                echo "[build_kernel] ${TARGET_KVER} is now the persistent default boot entry (7s menu timeout)."
                echo "[build_kernel]   saved_entry=${SUBMENU_ID}>${TARGET_ID}"
                GRUB_OK=1
            else
                echo "[build_kernel] WARNING: grub-set-default failed — pick ${TARGET_KVER} manually at boot." >&2
            fi
        else
            echo "[build_kernel] WARNING: could not find menuentry id for ${TARGET_KVER} in ${GRUB_CFG}." >&2
            echo "[build_kernel]   submenu_id='${SUBMENU_ID}' target_id='${TARGET_ID}'" >&2
            echo "[build_kernel]   Falling back to title-based selection (works only on English locale)." >&2
            if sudo grub-set-default "Advanced options for Ubuntu>Ubuntu, with Linux ${TARGET_KVER}"; then
                echo "[build_kernel] Set default by title (verify at boot menu)."
                GRUB_OK=1
            else
                echo "[build_kernel] WARNING: grub-set-default failed — pick ${TARGET_KVER} manually at boot." >&2
            fi
        fi
    else
        echo "[build_kernel] Skipped GRUB changes. Pick ${TARGET_KVER} manually at the boot menu." >&2
    fi
else
    echo "[build_kernel] WARNING: /boot/vmlinuz-${TARGET_KVER} missing — the install step above failed? Pick the kernel manually at boot." >&2
fi

# Warn if the running kernel differs from the newly installed one
if [[ "$CURRENT_KVER" != "$TARGET_KVER" ]]; then
    echo ""
    echo "[build_kernel] ============================================================"
    echo "[build_kernel]  REBOOT REQUIRED"
    echo "[build_kernel]  Currently running : $CURRENT_KVER"
    echo "[build_kernel]  Newly installed   : $TARGET_KVER"
    if [ "$GRUB_OK" = 1 ]; then
        echo "[build_kernel]  The next reboot is already set to boot $TARGET_KVER — just:"
        echo "[build_kernel]    sudo reboot"
    else
        echo "[build_kernel]  Default boot entry NOT set (see warning above) — pick"
        echo "[build_kernel]  $TARGET_KVER at the boot menu after: sudo reboot"
    fi
    echo "[build_kernel] ============================================================"
fi
