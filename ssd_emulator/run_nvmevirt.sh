#!/bin/bash
# Usage: ./run_nvmevirt.sh [zns|conv]
#   zns  - emulate a ZNS SSD (WD_ZN540 profile). Needed by every GAZE/f2fs
#          family mode (gaze, pgc, f2fs, zns_plus, f2fs_*).
#   conv - emulate a CONVENTIONAL SSD (SAMSUNG_970PRO profile). Needed by
#          the scalelfs baseline, which is formatted with the traditional
#          f2fs-tools and cannot use a zoned device.
# With no argument, the variant is picked from kernel/.build_mode
# (scalelfs -> conv, everything else -> zns).
#
# The variant is chosen at nvmev.ko compile time (Kbuild CONFIG_NVMEVIRT_*
# overridden on the make command line), so switching always rebuilds. If an
# nvmev is already loaded, stop it first: ./stop_nvmevirt.sh

GAZE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG_FILE="$GAZE_DIR/.config"
BUILD_MODE_FILE="$GAZE_DIR/kernel/.build_mode"

# This is the GAZE-side emulator (branch 'main'). The IPLFS baseline uses
# its own emulator sources: branch 'iplfs' replaces this whole directory,
# including this script. Refuse the mismatched combination outright.
MODE=$(cat "$BUILD_MODE_FILE" 2>/dev/null || echo gaze)
if [[ "$MODE" == "iplfs" ]]; then
    echo "[run_nvmevirt] ERROR: FS mode is 'iplfs', but this is the GAZE emulator tree." >&2
    echo "[run_nvmevirt]   The IPLFS emulator lives on git branch 'iplfs':" >&2
    echo "[run_nvmevirt]     git -C $GAZE_DIR checkout iplfs && cd ssd_emulator && ./run_nvmevirt.sh" >&2
    exit 1
fi

VARIANT="$1"
if [[ -z "$VARIANT" ]]; then
    if [[ "$MODE" == "scalelfs" ]]; then VARIANT=conv; else VARIANT=zns; fi
    echo "[run_nvmevirt] No variant given -- FS mode '$MODE' -> $VARIANT"
fi

case "$VARIANT" in
    zns)  MAKE_ARGS=() ;;
    conv) MAKE_ARGS=(CONFIG_NVMEVIRT_ZNS= CONFIG_NVMEVIRT_SSD=y) ;;
    *) echo "Usage: $0 [zns|conv]" >&2; exit 1 ;;
esac

if lsmod | grep -q '^nvmev '; then
    echo "[run_nvmevirt] ERROR: nvmev is already loaded. Run ./stop_nvmevirt.sh first." >&2
    exit 1
fi

sudo dmesg -C
make clean
make "${MAKE_ARGS[@]}" || exit 1
./insmod.sh

# --- Auto-detect the NVMeVirt SSD and write it into .config ---
# (ZNS_DEV is the historical variable name; in conv mode the same device
# path is consumed as SCALELFS_DEV, which defaults to ZNS_DEV in config.sh.)
ZNS_NS=""
for i in $(seq 1 20); do
    MODEL_FILE=$(grep -l "^CSL_Virt_MN" /sys/class/nvme/nvme*/model 2>/dev/null | head -1)
    if [[ -n "$MODEL_FILE" ]]; then
        NVME_CTRL="$(basename "$(dirname "$MODEL_FILE")")"
        ZNS_NS=$(basename "$(ls -d /sys/class/nvme/"$NVME_CTRL"/"$NVME_CTRL"n[0-9]* 2>/dev/null | head -1)")
        [[ -n "$ZNS_NS" ]] && break
    fi
    sleep 0.5
done

if [[ -n "$ZNS_NS" ]] && [[ -f "$CONFIG_FILE" ]]; then
    sed -i -E "s|^ZNS_DEV=.*|ZNS_DEV=/dev/${ZNS_NS}|" "$CONFIG_FILE"
    sed -i -E "s|^ZNS_DEV_NAME=[^ ]*|ZNS_DEV_NAME=${ZNS_NS}|" "$CONFIG_FILE"
    ZONED=$(cat "/sys/block/${ZNS_NS}/queue/zoned" 2>/dev/null || echo unknown)
    echo "[run_nvmevirt] NVMeVirt SSD ($VARIANT, zoned=$ZONED) detected at /dev/${ZNS_NS} -> updated $CONFIG_FILE"
else
    echo "[run_nvmevirt] WARNING: could not auto-detect the NVMeVirt device (model CSL_Virt_MN not found under /sys/class/nvme). .config NOT updated." >&2
fi
