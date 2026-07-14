#!/bin/bash
# Unloads the NVMeVirt (ZNS SSD) kernel module started by run_nvmevirt.sh.

if ! lsmod | grep -q '^nvmev '; then
    echo "[stop_nvmevirt] nvmev module is not loaded. Nothing to do."
    exit 0
fi

# Refuse to unload while any NVMeVirt namespace is still mounted, to avoid
# yanking a device out from under a live filesystem.
MODEL_FILE=$(grep -l "^CSL_Virt_MN" /sys/class/nvme/nvme*/model 2>/dev/null | head -1)
if [[ -n "$MODEL_FILE" ]]; then
    NVME_CTRL="$(basename "$(dirname "$MODEL_FILE")")"
    for ns in /sys/class/nvme/"$NVME_CTRL"/"$NVME_CTRL"n[0-9]*; do
        [[ -e "$ns" ]] || continue
        ns_name="$(basename "$ns")"
        if mount | grep -q "/dev/${ns_name}"; then
            echo "[stop_nvmevirt] ERROR: /dev/${ns_name} is still mounted. Unmount it first, then re-run this script." >&2
            mount | grep "/dev/${ns_name}"
            exit 1
        fi
    done
fi

sudo rmmod nvmev
echo "[stop_nvmevirt] nvmev module removed."
