#!/bin/bash
# IPLFS-branch NVMeVirt loader. Same interface as the main branch's
# run_nvmevirt.sh (called by setup_fs.sh), but this emulator tree is the
# IPLFS artifact's own (older) NVMeVirt: conventional-SSD target selected
# in its Makefile, no zns/conv variants — any variant argument is ignored.
#
# Only meaningful on kernel 5.11.0.iplfs (the module builds against the
# running kernel's headers). setup_fs.sh checks that before calling us.

GAZE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG_FILE="$GAZE_DIR/.config"

if lsmod | grep -q '^nvmev '; then
    echo "[run_nvmevirt] ERROR: nvmev is already loaded. Run ./stop_nvmevirt.sh first." >&2
    exit 1
fi

sudo dmesg -C
make clean
make || exit 1

# This machine reserves physical memory at 256GiB (GRUB memmap=261121M\$262144M,
# set by set_config.sh). This older NVMeVirt takes memmap_start in GiB and
# memmap_size in MiB; stay within the reserved 261121MiB.
BEFORE=$(ls /sys/class/nvme 2>/dev/null)
sudo insmod ./nvmev.ko memmap_start=256 memmap_size=261121 cpus=37,39 || exit 1

# --- Detect the new NVMe controller (this NVMeVirt predates the
# CSL_Virt_MN model string, so diff the controller list instead) ---
NEW_NS=""
for i in $(seq 1 20); do
    AFTER=$(ls /sys/class/nvme 2>/dev/null)
    NEW_CTRL=$(comm -13 <(echo "$BEFORE" | sort) <(echo "$AFTER" | sort) | head -1)
    if [[ -n "$NEW_CTRL" ]]; then
        NEW_NS=$(basename "$(ls -d /sys/class/nvme/"$NEW_CTRL"/"$NEW_CTRL"n[0-9]* 2>/dev/null | head -1)")
        [[ -n "$NEW_NS" ]] && break
    fi
    sleep 0.5
done

if [[ -n "$NEW_NS" ]] && [[ -f "$CONFIG_FILE" ]]; then
    sed -i -E "s|^ZNS_DEV=.*|ZNS_DEV=/dev/${NEW_NS}|" "$CONFIG_FILE"
    sed -i -E "s|^ZNS_DEV_NAME=[^ ]*|ZNS_DEV_NAME=${NEW_NS}|" "$CONFIG_FILE"
    echo "[run_nvmevirt] IPLFS NVMeVirt detected at /dev/${NEW_NS} -> updated $CONFIG_FILE"
else
    echo "[run_nvmevirt] WARNING: could not auto-detect the NVMeVirt device. .config NOT updated." >&2
fi
