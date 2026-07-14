#!/bin/bash
# Build + install the IPLFS storage stack's kernel (5.11.0.iplfs) and stage
# its filesystem module as iplfs.ko. Same conventions as main's
# build_kernel.sh: compile as the invoking user (no root-owned files in the
# tree), self-elevate only for the install steps, and make the new kernel
# the persistent default boot entry (GRUB_DEFAULT=saved).
#
# Usage: ./build_kernel.sh      (no sudo needed in front)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE_DIR="$SCRIPT_DIR/../experiment_script/general_resource/filesystem_module"
TARGET_KVER="5.11.0.iplfs"
cd "$SCRIPT_DIR" || exit 1

echo "[build_kernel] Building the IPLFS branch kernel (${TARGET_KVER})..."

# Settle any config symbols newer toolchains introduce without prompting
# (the vendored .config predates the host's gcc/make).
make olddefconfig >/dev/null

make -j"$(nproc)" && \
sudo make modules_install -j"$(nproc)" && \
sudo find "/lib/modules/${TARGET_KVER}" -name "*.ko" -exec strip --strip-unneeded {} + && \
sudo make install -j"$(nproc)" || exit 1

# --remove-destination: the existing .ko may be root-owned from an old
# sudo build; unlinking first only needs directory write permission.
cp --remove-destination fs/f2fs/f2fs.ko "$MODULE_DIR/iplfs.ko" \
    && echo "[build_kernel] Copied fs/f2fs/f2fs.ko -> iplfs.ko"

CURRENT_KVER=$(uname -r)

# Make the freshly installed kernel the *persistent* default (requires
# GRUB_DEFAULT=saved in /etc/default/grub — setup_env.sh configures this).
# Same GRUB handling as main's build_kernel.sh: prompt before editing (a
# shared-machine setting), force a 7s menu window, and select the target
# by *menuentry ID*, not by menu title — titles are locale-translated by
# gettext (e.g. Korean "Ubuntu용 고급 설정"), so a title-based lookup
# silently fails on non-English systems and GRUB_SAVEDEFAULT=true then
# persists whatever the user actually booted. IDs stay in English.
GRUB_FILE="/etc/default/grub"
GRUB_CFG="/boot/grub/grub.cfg"
GRUB_OK=0
if [ -f "/boot/vmlinuz-${TARGET_KVER}" ]; then
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
        # Field 4 with -F"'" is the id in both cases.
        # grub.cfg is root-only readable, so read it once via `sudo cat`
        # (that exact command is in the evaluator account's sudo whitelist;
        # `sudo awk` is not) and run awk unprivileged on the captured text.
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
    echo "[build_kernel] WARNING: /boot/vmlinuz-${TARGET_KVER} missing — install failed? Pick the kernel manually at boot." >&2
fi

if [[ "$CURRENT_KVER" != "$TARGET_KVER" ]]; then
    echo ""
    echo "[build_kernel] ============================================================"
    echo "[build_kernel]  REBOOT REQUIRED"
    echo "[build_kernel]  Currently running : $CURRENT_KVER"
    echo "[build_kernel]  Newly installed   : $TARGET_KVER"
    if [ "$GRUB_OK" = 1 ]; then
        echo "[build_kernel]  The next reboot already boots $TARGET_KVER — just: sudo reboot"
    else
        echo "[build_kernel]  Default boot entry NOT set (see warning above) — pick"
        echo "[build_kernel]  $TARGET_KVER at the boot menu after: sudo reboot"
    fi
    echo "[build_kernel] ============================================================"
fi
