#!/bin/bash
# GAZE machine setup wizard. (Re)configures:
#   - SSD size, DRAM size, CPU isolation, and the GRUB `memmap=` parameter
#     + ssd_emulator/insmod.sh values that back them
#   - the conventional-SSD partition used for F2FS metadata
#   - the experiment mount point
#   - the workload-intensity parameters (experiment_params.sh)
# and writes the results to ./.config, which config.sh (sourced by every
# experiment script) reads.
#
# Three configuration modes:
#
#   paper  (default) — reproduce the paper's exact machine configuration:
#            255G emulated SSD (SSD_GB=256), 64G host DRAM, and the paper's
#            workload parameters (general_resource/paper_params.sh, restored
#            verbatim). Refuses machines with too little RAM and points you
#            at auto instead. This is the recommended mode on any machine
#            with >= 320G of RAM.
#
#   auto   — size everything to THIS machine: detects total RAM and NUMA
#            topology, derives the largest emulated SSD that fits (leaving
#            the OS a sensible share), and scales the workload parameters
#            proportionally to the ratio between this SSD and the paper's
#            255G one (FIO filesize, fileserver file count, YCSB record/op
#            counts, TPC-C warehouses — derived from the paper runs' actual
#            disk footprints). Durations/threads/block sizes are unchanged.
#
#   manual — answer every sizing question yourself (the original behavior).
#            Workload parameters are left untouched (edit via
#            experiment_script/set_params.sh).
#
# --- SSD/DRAM/NUMA design ---
# The chosen SSD size is removed from the kernel's view via a GRUB
# `memmap=` cmdline parameter; ssd_emulator/NVMeVirt claims that region as
# the emulated NVMe device's backing store (memmap_start/memmap_size passed
# to insmod).
#
# On a NUMA machine, host DRAM is kept entirely on node0 and every other
# node's CPUs are isolated (isolcpus=), so the emulator's threads — pinned
# to two of those isolated CPUs — and the SSD's backing memory stay on the
# same node and never take a remote-memory hit. On a non-NUMA machine the
# SSD is placed at the end of RAM and ONLY the two CPUs given to the
# emulator are isolated, keeping them free of scheduler noise without
# starving the benchmark of cores.
#
# Editing GRUB only edits config; you still need `update-grub` (offered at
# the end) and a REBOOT for the new memmap/isolcpus to take effect.
#
# Usage: ./set_config.sh [paper|auto|manual]
#   (no argument: interactive menu, Enter picks paper)
# Testing hooks (no system changes): DRY_RUN=1 [TOTAL_GB_OVERRIDE=N]

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSMOD_SH="$SCRIPT_DIR/ssd_emulator/insmod.sh"
GRUB_FILE="/etc/default/grub"
DOTCONFIG="$SCRIPT_DIR/.config"
GENERAL_RES="$SCRIPT_DIR/experiment_script/general_resource"
PARAMS_LIVE="$GENERAL_RES/experiment_params.sh"
PARAMS_PAPER="$GENERAL_RES/paper_params.sh"
DRY_RUN="${DRY_RUN:-0}"

log()  { echo "[set_config] $*"; }
warn() { echo "[set_config] WARNING: $*" >&2; }
die()  { echo "[set_config] ERROR: $*" >&2; exit 1; }

# --- The paper machine's configuration (the provided server). Keep these
# in sync with README §1.2; paper_params.sh is the workload-side snapshot.
PAPER_SSD_GB=256     # 255G usable emulated SSD
PAPER_DRAM_GB=64

[ -f "$INSMOD_SH" ] || die "$INSMOD_SH not found (expected under ssd_emulator/)."
[ -f "$PARAMS_PAPER" ] || die "$PARAMS_PAPER not found."

# Load previous answers (if any) as this run's defaults.
[ -f "$DOTCONFIG" ] && source "$DOTCONFIG"
SSD_GB="${SSD_GB:-$PAPER_SSD_GB}"
DRAM_GB="${DRAM_GB:-$PAPER_DRAM_GB}"
CONV_PART="${CONV_PART:-/dev/nvme0n1p1}"
CONV_DEV="${CONV_DEV:-/dev/nvme0n1}"
ZNS_DEV="${ZNS_DEV:-/dev/nvme2n1}"
ZNS_DEV_NAME="${ZNS_DEV_NAME:-nvme2n1}"
MNT="${MNT:-/mnt}"
MYSQL_DATA_PATH="${MYSQL_DATA_PATH:-/exp_mysql_data}"
MYSQL_USER="${MYSQL_USER:-aec}"
MYSQL_PASSWD="${MYSQL_PASSWD:-aec}"

# ---------------------------------------------------------------------------
# 0. Mode selection.
# ---------------------------------------------------------------------------
MODE="${1:-}"
if [[ -z "$MODE" ]]; then
    echo "Configuration mode:"
    echo "  1) paper   — the paper's machine config + workload params (recommended, default)"
    echo "  2) auto    — size the SSD and workload to THIS machine automatically"
    echo "  3) manual  — answer every sizing question yourself"
    read -r -p "> [1] " PICK
    case "${PICK:-1}" in
        1|paper)  MODE=paper ;;
        2|auto)   MODE=auto ;;
        3|manual) MODE=manual ;;
        *) die "unknown choice '$PICK'" ;;
    esac
fi
case "$MODE" in paper|auto|manual) ;; *) die "usage: $0 [paper|auto|manual]" ;; esac
log "Mode: $MODE"

if [ "$DRY_RUN" != "1" ] && ! sudo -n true 2>/dev/null; then
    warn "this will prompt for your sudo password (needed for dmidecode / $GRUB_FILE / update-grub)."
fi

# ---------------------------------------------------------------------------
# 1. True total physical RAM, in GB -- independent of any memmap= already
#    in effect on this boot. /proc/meminfo and numactl both under-report if
#    a restrictive memmap= is already active, so prefer dmidecode (reads
#    installed DIMM capacity straight from SMBIOS, unaffected by any kernel
#    cmdline parameter).
# ---------------------------------------------------------------------------
detect_total_gb() {
    if [ -n "${TOTAL_GB_OVERRIDE:-}" ]; then
        echo "$TOTAL_GB_OVERRIDE"
        return
    fi
    if command -v dmidecode >/dev/null 2>&1; then
        local mb
        mb=$(sudo dmidecode -t memory 2>/dev/null | awk '
            /^[[:space:]]*Size:[[:space:]]*[0-9]+[[:space:]]*(MB|GB)[[:space:]]*$/ {
                val=$2; unit=$3
                if (unit == "GB") sum += val*1024
                else if (unit == "MB") sum += val
            }
            END { print sum+0 }')
        if [ -n "$mb" ] && [ "$mb" -gt 0 ] 2>/dev/null; then
            echo $(( mb / 1024 ))
            return
        fi
    fi
    warn "dmidecode unavailable or returned nothing -- falling back to /proc/meminfo, which" \
         "under-reports the true total if a memmap= is already active on this boot."
    awk '/MemTotal/{printf "%d", int($2/1024/1024 + 0.5)}' /proc/meminfo
}
TOTAL_GB=$(detect_total_gb)
[ -n "$TOTAL_GB" ] && [ "$TOTAL_GB" -gt 0 ] 2>/dev/null || die "could not determine total physical RAM."
log "Detected total physical RAM: ${TOTAL_GB}G"

# ---------------------------------------------------------------------------
# 2. NUMA topology. CPU-to-node mapping is static hardware topology and is
#    NOT affected by an existing memmap=, unlike per-node memory size.
# ---------------------------------------------------------------------------
expand_cpulist() {
    # "0-3,8,10-11" -> "0,1,2,3,8,10,11" (sorted, deduped)
    echo "$1" | tr ',' '\n' | while read -r part; do
        [ -z "$part" ] && continue
        if [[ "$part" == *-* ]]; then
            seq "${part%-*}" "${part#*-}"
        else
            echo "$part"
        fi
    done | sort -n -u | paste -sd, -
}

NODE_DIRS=(/sys/devices/system/node/node[0-9]*)
NUM_NODES=0
[ -e "${NODE_DIRS[0]}" ] && NUM_NODES=${#NODE_DIRS[@]}
IS_NUMA=0
[ "$NUM_NODES" -ge 2 ] && IS_NUMA=1

NODE0_GB_OVERRIDE="${NODE0_GB:-}"
NODE0_GB=0
ISOLCPUS=""
if [ "$IS_NUMA" -eq 1 ]; then
    NODE0_CPUS=$(expand_cpulist "$(cat /sys/devices/system/node/node0/cpulist)")
    OTHER_RAW=""
    for d in "${NODE_DIRS[@]}"; do
        [ "$(basename "$d")" = "node0" ] && continue
        c=$(cat "$d/cpulist" 2>/dev/null)
        [ -n "$c" ] && OTHER_RAW="${OTHER_RAW:+$OTHER_RAW,}$c"
    done
    ISOLCPUS=$(expand_cpulist "$OTHER_RAW")
    log "NUMA detected: $NUM_NODES nodes."
    log "  node0 CPUs (stay on general scheduler): $NODE0_CPUS"
    log "  other-node CPUs (to isolate):            $ISOLCPUS"

    # Symmetric-NUMA assumption: node0 gets an equal share of total RAM.
    # Per-node RAM can't be read reliably here -- if a memmap= is already
    # active it hides most of the non-node0 nodes from /sys and numactl.
    # True for balanced dual/quad-socket servers. If this machine's nodes
    # are NOT equally sized, re-run as: NODE0_GB=<real size> ./set_config.sh
    NODE0_GB=${NODE0_GB_OVERRIDE:-$(( TOTAL_GB / NUM_NODES ))}
    log "  assuming node0 holds ${NODE0_GB}G of the ${TOTAL_GB}G total (symmetric $NUM_NODES-way split;" \
        "override with NODE0_GB=... if this machine's nodes aren't equally sized)."
else
    log "Single NUMA node (or no NUMA) detected."
fi

# ---------------------------------------------------------------------------
# 3. Decide SSD / DRAM sizes (whole GB), per mode.
# ---------------------------------------------------------------------------
read_gb() {
    local prompt="$1" default="$2" val
    while true; do
        read -r -p "$prompt" val
        val="${val:-$default}"
        [[ "$val" =~ ^[0-9]+$ ]] && [ "$val" -gt 0 ] && { echo "$val"; return; }
        echo "  -> enter a whole number of GB (e.g. $default), no units." >&2
    done
}

case "$MODE" in
paper)
    SSD_GB=$PAPER_SSD_GB
    DRAM_GB=$PAPER_DRAM_GB
    if [ $(( SSD_GB + DRAM_GB )) -gt "$TOTAL_GB" ]; then
        die "the paper config needs ${SSD_GB}G (SSD) + ${DRAM_GB}G (DRAM) = $(( SSD_GB + DRAM_GB ))G of RAM, but this machine has ${TOTAL_GB}G.
[set_config]   Use auto mode instead:  ./set_config.sh auto   (sizes the SSD and the workloads to this machine)"
    fi
    log "Paper config: SSD=${SSD_GB}G DRAM=${DRAM_GB}G (workload params will be restored to the paper's)."
    ;;
auto)
    # Give the OS a sensible share: a quarter of RAM, clamped to [8G, 64G]
    # (64G = what the paper machine ran with; more buys the benchmarks
    # nothing since the pagecache is dwarfed by the workload anyway).
    DRAM_GB=$(( TOTAL_GB / 4 ))
    [ "$DRAM_GB" -lt 8 ]  && DRAM_GB=8
    [ "$DRAM_GB" -gt 64 ] && DRAM_GB=64
    if [ "$IS_NUMA" -eq 1 ]; then
        # DRAM must fit on node0; the SSD backing lives on the other nodes.
        [ "$DRAM_GB" -gt "$NODE0_GB" ] && DRAM_GB=$NODE0_GB
        REMOTE_GB=$(( TOTAL_GB - NODE0_GB ))
        SSD_GB=$REMOTE_GB
        # Symmetric nodes usually leave node0 half-idle; that's fine — the
        # paper machine did the same (node0: OS, node1: SSD backing).
    else
        SSD_GB=$(( TOTAL_GB - DRAM_GB ))
    fi
    [ "$SSD_GB" -ge 16 ] || die "auto-derived SSD of ${SSD_GB}G is too small to run the benchmarks meaningfully (need >= 16G; this machine has ${TOTAL_GB}G RAM total)."
    log "Auto config for this machine: SSD=${SSD_GB}G DRAM=${DRAM_GB}G (of ${TOTAL_GB}G total)."
    ;;
manual)
    SSD_GB=$(read_gb "SSD size in GB [default ${SSD_GB}, Enter to accept]: " "$SSD_GB")
    DRAM_GB=$(read_gb "DRAM / host memory size in GB [default ${DRAM_GB}, Enter to accept]: " "$DRAM_GB")
    ;;
esac

# ---------------------------------------------------------------------------
# 4. Validate.
# ---------------------------------------------------------------------------
[ $(( SSD_GB + DRAM_GB )) -le "$TOTAL_GB" ] \
    || die "SSD (${SSD_GB}G) + DRAM (${DRAM_GB}G) = $(( SSD_GB + DRAM_GB ))G exceeds total physical RAM (${TOTAL_GB}G)."

if [ "$IS_NUMA" -eq 1 ]; then
    [ "$DRAM_GB" -le "$NODE0_GB" ] \
        || die "DRAM (${DRAM_GB}G) exceeds node0's memory (${NODE0_GB}G) -- host RAM must fit on node0 to stay NUMA-local."
    REMOTE_GB=$(( TOTAL_GB - NODE0_GB ))
    [ "$SSD_GB" -le "$REMOTE_GB" ] \
        || die "SSD (${SSD_GB}G) exceeds the non-node0 memory available for it (${REMOTE_GB}G)."
fi

# ---------------------------------------------------------------------------
# 5. Derive memmap / isolcpus / insmod.sh parameters.
#    memmap_size = SSD_GB*1024 - 1023 (1MB-ish headroom for emulator metadata,
#    matching ssd_emulator/insmod.sh's existing convention).
# ---------------------------------------------------------------------------
RESERVE_GB=$(( TOTAL_GB - DRAM_GB ))
ESC_DOLLAR='\\\$'   # literal 3 backslashes + $ -- GRUB's own cmdline needs the
                    # $ escaped so grub.cfg's script interpreter doesn't touch it
GRUB_MEMMAP="memmap=${RESERVE_GB}G${ESC_DOLLAR}${DRAM_GB}G"

if [ "$IS_NUMA" -eq 1 ]; then
    # NUMA: isolate every non-node0 CPU; benchmarks run on node0, the
    # emulator's threads on two isolated CPUs of the SSD-backing node.
    MM_START_MB=$(( NODE0_GB * 1024 ))                       # node1's first address
    NVMEV_CPUS=$(echo "$ISOLCPUS" | tr ',' '\n' | tail -2 | paste -sd, -)  # last 2 isolated CPUs
else
    # Non-NUMA: isolate ONLY the two CPUs handed to the emulator, so its
    # polling threads run undisturbed while every other core stays
    # available to the benchmark.
    MM_START_MB=$(( (TOTAL_GB - SSD_GB) * 1024 ))            # SSD pinned to the end of RAM
    LAST_CPU=$(( $(nproc) - 1 ))
    NVMEV_CPUS="$(( LAST_CPU - 1 )),${LAST_CPU}"              # two highest-numbered CPUs
    ISOLCPUS="$NVMEV_CPUS"
    log "Non-NUMA machine: isolating only the emulator's two CPUs (isolcpus=$ISOLCPUS)."
fi
MM_SIZE_MB=$(( SSD_GB * 1024 - 1023 ))

log "Computed:"
log "  GRUB memmap:  ${RESERVE_GB}G\$${DRAM_GB}G"
[ -n "$ISOLCPUS" ] && log "  isolcpus:     $ISOLCPUS"
log "  insmod:       memmap_start=${MM_START_MB}M memmap_size=${MM_SIZE_MB}M cpus=${NVMEV_CPUS}"

# ---------------------------------------------------------------------------
# 6. Workload parameters (experiment_params.sh), per mode.
#    Scaling anchors come from the paper runs' measured footprints on the
#    255G SSD: FIO writes a 180G file (~71% of the SSD; peak 206G used incl.
#    GC slack), fileserver's 1.8M-file set and the 16M-record YCSB / 1700-
#    warehouse TPC-C datasets peak near 99G plus copies. All scale ~linearly
#    with dataset knobs, so auto mode scales those knobs by SSD_GB/256 and
#    leaves thread counts and I/O sizes untouched.
#
#    GC-onset measurement start (FIO_GC_START_SEC / FILESERVER_GC_START_SEC):
#    fio/filebench are patched to record stats only after GC starts. GC
#    onset ~= free-space fill time = (SSD - dataset) / write rate; SSD and
#    dataset both scale linearly, and the fill rate is device-bound (same
#    emulator), so auto mode scales these knobs by SSD_GB/256 too, and
#    extends FIO_RUNTIME / FILESERVER_RUNTIME to keep the paper's measured
#    window lengths (700 s / 1500 s). It is an ESTIMATE: on a machine whose
#    fill throughput differs a lot, verify against the run (fio prints
#    "jw_gcstart: true" at the moment it starts recording; compare with the
#    throughput drop in the iops log) and adjust via set_params.sh.
# ---------------------------------------------------------------------------
scale_int() { # PAPER_VALUE MIN  -> round(PAPER_VALUE * SSD_GB/PAPER_SSD_GB), floored at MIN
    local v="$1" min="$2"
    local s=$(( (v * SSD_GB + PAPER_SSD_GB / 2) / PAPER_SSD_GB ))
    [ "$s" -lt "$min" ] && s=$min
    echo "$s"
}

write_params() {
    case "$MODE" in
    paper)
        if [ "$DRY_RUN" = "1" ]; then log "(dry-run) would restore paper params"; return; fi
        cp "$PARAMS_PAPER" "$PARAMS_LIVE"
        log "Workload params: restored to the paper's (experiment_params.sh <- paper_params.sh)."
        ;;
    auto)
        local fio_gb nfiles reccnt opcnt wh gcs_fio gcs_fs fio_rt fs_rt
        fio_gb=$(scale_int 180 4)
        nfiles=$(scale_int 1800000 50000)
        reccnt=$(scale_int 16000000 500000)
        opcnt=$(scale_int 10000000 500000)
        wh=$(scale_int 1700 20)
        gcs_fio=$(scale_int 500 20)     # estimated GC onset (see section comment)
        gcs_fs=$(scale_int 100 10)
        fio_rt=$(( gcs_fio + 700 ))     # keep the paper's 700 s measured window
        fs_rt=$(( gcs_fs + 1500 ))      # keep the paper's 1500 s measured window
        if [ "$DRY_RUN" = "1" ]; then
            log "(dry-run) auto-scaled params for ${SSD_GB}G SSD: FIO=${fio_gb}GB nfiles=${nfiles} records=${reccnt} ops=${opcnt} warehouses=${wh}"
            log "(dry-run)   GC-onset stats start: FIO ${gcs_fio}s (runtime ${fio_rt}s), fileserver ${gcs_fs}s (runtime ${fs_rt}s)"
            return
        fi
        cp "$PARAMS_PAPER" "$PARAMS_LIVE"
        sed -i \
            -e "s|^FIO_FILESIZE=.*|FIO_FILESIZE=${fio_gb}GB|" \
            -e "s|^FIO_GC_START_SEC=.*|FIO_GC_START_SEC=${gcs_fio}|" \
            -e "s|^FIO_RUNTIME=.*|FIO_RUNTIME=${fio_rt}|" \
            -e "s|^FILESERVER_GC_START_SEC=.*|FILESERVER_GC_START_SEC=${gcs_fs}|" \
            -e "s|^FILESERVER_RUNTIME=.*|FILESERVER_RUNTIME=${fs_rt}|" \
            -e "s|^FILESERVER_NFILES=.*|FILESERVER_NFILES=${nfiles}|" \
            -e "s|^YCSBA_RECORDCOUNT=.*|YCSBA_RECORDCOUNT=${reccnt}|" \
            -e "s|^YCSBF_RECORDCOUNT=.*|YCSBF_RECORDCOUNT=${reccnt}|" \
            -e "s|^YCSBA_OPERATIONCOUNT=.*|YCSBA_OPERATIONCOUNT=${opcnt}|" \
            -e "s|^YCSBF_OPERATIONCOUNT=.*|YCSBF_OPERATIONCOUNT=${opcnt}|" \
            -e "s|^TPCC_WAREHOUSES=.*|TPCC_WAREHOUSES=${wh}|" \
            "$PARAMS_LIVE"
        sed -i "1a # AUTO-SCALED by set_config.sh for a ${SSD_GB}G SSD (paper: ${PAPER_SSD_GB}G).\n# Dataset + GC-onset knobs scaled by ${SSD_GB}/${PAPER_SSD_GB}; FIO/fileserver runtimes\n# extended to keep the paper's measured-window lengths; threads/sizes unchanged.\n# NOTE: TPC-C/YCSB preloaded cold data (MYSQL_DATA_PATH) must be regenerated\n# at this scale -- see SETUP.md 'If you don't have pre-built cold MySQL data'." "$PARAMS_LIVE"
        log "Workload params: auto-scaled to this SSD (FIO=${fio_gb}GB, nfiles=${nfiles}, records=${reccnt}, ops=${opcnt}, warehouses=${wh})."
        log "GC-onset stats start (estimated): FIO ${gcs_fio}s (runtime ${fio_rt}s), fileserver ${gcs_fs}s (runtime ${fs_rt}s)."
        warn "GC onset is an estimate scaled from the paper machine; after the first FIO run,"
        warn "  check that fio's 'jw_gcstart: true' line lands after the throughput drop in the"
        warn "  iops log, and adjust FIO_GC_START_SEC via experiment_script/set_params.sh if not."
        warn "TPC-C/YCSB use preloaded MySQL cold data (MYSQL_DATA_PATH=$MYSQL_DATA_PATH);"
        warn "  regenerate it at the new record/warehouse counts (SETUP.md shows how), or those two"
        warn "  benchmarks will load the paper-sized data and overflow a smaller SSD."
        ;;
    manual)
        log "Workload params: left unchanged (edit via experiment_script/set_params.sh)."
        ;;
    esac
}

if [ "$DRY_RUN" = "1" ]; then
    write_params
    log "(dry-run) stopping before any system changes (insmod.sh/GRUB/.config untouched)."
    exit 0
fi

# ---------------------------------------------------------------------------
# 7. Write ssd_emulator/insmod.sh: comment out the currently-active insmod
#    line(s) and append the newly computed one, same style as the file's
#    existing history of commented-out configs.
# ---------------------------------------------------------------------------
sed -i '/^[[:space:]]*sudo insmod \.\/nvmev\.ko/ s/^/#/' "$INSMOD_SH"
{
    echo "# set_config.sh[$MODE]: SSD=${SSD_GB}G DRAM=${DRAM_GB}G ($(date +%Y-%m-%d))"
    echo "sudo insmod ./nvmev.ko memmap_start=${MM_START_MB}M memmap_size=${MM_SIZE_MB}M cpus=${NVMEV_CPUS}"
} >> "$INSMOD_SH"
log "Updated $INSMOD_SH"

# ---------------------------------------------------------------------------
# 8. Write /etc/default/grub.
# ---------------------------------------------------------------------------
CMDLINE="$GRUB_MEMMAP intremap=off"
[ -n "$ISOLCPUS" ] && CMDLINE="$CMDLINE isolcpus=$ISOLCPUS"

sudo cp "$GRUB_FILE" "${GRUB_FILE}.bak_$(date +%Y%m%d_%H%M%S)"
sudo sed -i "/^GRUB_CMDLINE_LINUX=/d" "$GRUB_FILE"
echo "GRUB_CMDLINE_LINUX=\"$CMDLINE\"" | sudo tee -a "$GRUB_FILE" > /dev/null
log "Updated $GRUB_FILE (backup saved alongside it)."

read -r -p "[set_config] Run 'update-grub' now to apply this? [y/N] " REPLY
if [[ "$REPLY" =~ ^[Yy]$ ]]; then
    sudo update-grub || die "update-grub failed."
    log "update-grub done. REBOOT is required for the new memmap/isolcpus to take effect."
else
    log "Skipped update-grub -- run 'sudo update-grub' yourself, then reboot, when ready."
fi
log "After reboot, verify with:  grep -o 'memmap=[^ ]*' /proc/cmdline   and   numactl --hardware"

# ---------------------------------------------------------------------------
# 9. Conventional SSD partition (F2FS metadata) -- a real physical
#    partition, not auto-detectable like the ZNS emulator (that's handled
#    by ssd_emulator/run_nvmevirt.sh instead), so ask -- or keep the
#    previous value if it still exists on this machine.
# ---------------------------------------------------------------------------
CONV_RECO_GB=$(( SSD_GB * 6 / 100 )); [ "$CONV_RECO_GB" -lt 2 ] && CONV_RECO_GB=2
if [ -n "$CONV_PART" ] && lsblk "$CONV_PART" &>/dev/null; then
    read -r -p "[set_config] Conventional SSD partition for F2FS metadata (recommend ~${CONV_RECO_GB}G for your ${SSD_GB}G SSD) [default: $CONV_PART, Enter to keep]: " REPLY
    [ -n "$REPLY" ] && CONV_PART="$REPLY"
else
    warn "no valid conventional-SSD partition found on this machine (${CONV_PART:-<none>})."
    log "Create one first, e.g.: sudo fdisk /dev/nvme0n1  ->  n (new) -> +${CONV_RECO_GB}G -> w (write)"
    while true; do
        read -r -p "[set_config] Conventional SSD partition path (REQUIRED, e.g. /dev/nvme0n1p1): " REPLY
        [ -n "$REPLY" ] && lsblk "$REPLY" &>/dev/null && { CONV_PART="$REPLY"; break; }
        echo "  -> that path doesn't exist. Try again." >&2
    done
fi

# Derive the whole-disk device from the partition (nvme0n1p1 -> nvme0n1, sda1 -> sda)
if [[ "$CONV_PART" =~ ^(/dev/nvme[0-9]+n[0-9]+)p[0-9]+$ ]]; then
    CONV_DEV="${BASH_REMATCH[1]}"
elif [[ "$CONV_PART" =~ ^(/dev/[a-z]+[0-9]*[a-z]+)[0-9]+$ ]]; then
    CONV_DEV="${BASH_REMATCH[1]}"
else
    warn "couldn't derive the whole-disk device from $CONV_PART -- CONV_DEV left as $CONV_DEV, check it by hand."
fi
log "CONV_PART=$CONV_PART  CONV_DEV=$CONV_DEV"

read -r -p "[set_config] Experiment mount point [default: $MNT, Enter to accept]: " REPLY
[ -n "$REPLY" ] && MNT="$REPLY"
log "MNT=$MNT"

# ---------------------------------------------------------------------------
# 10. Persist everything to ./.config, then the workload params.
# ---------------------------------------------------------------------------
cat > "$DOTCONFIG" << EOF
# GAZE machine-local configuration.
# Written by ./set_config.sh (mode: $MODE) -- do not hand-edit unless you
# know what you're doing (set_config.sh will overwrite these on its next
# run). Read by config.sh (sourced by every experiment script).

SSD_GB=${SSD_GB}
DRAM_GB=${DRAM_GB}

# Conventional SSD partition used for F2FS metadata (~6% of the ZNS SSD
# size is a good rule of thumb, e.g. ~15G for a ~255G SSD).
CONV_PART=${CONV_PART}
CONV_DEV=${CONV_DEV}

# ZNS SSD device -- auto-detected & rewritten here by
# ssd_emulator/run_nvmevirt.sh every time the emulator is (re)loaded, since
# NVMeVirt's actual /dev/nvmeX number depends on what else is plugged in.
ZNS_DEV=${ZNS_DEV}
ZNS_DEV_NAME=${ZNS_DEV_NAME}

# Experiment mount point
MNT=${MNT}

# Database paths & credentials (TPC-C / YCSB)
MYSQL_DATA_PATH=${MYSQL_DATA_PATH}
MYSQL_USER=${MYSQL_USER}
MYSQL_PASSWD=${MYSQL_PASSWD}
EOF

# Preserve hand-pinned overrides from the previous .config (sourced above);
# config.sh derives defaults for these when the lines are absent.
if [ -n "${SCALELFS_TOOLS:-}" ]; then
    printf '\n# ScaleLFS traditional f2fs-tools checkout (pinned; see config.sh for the default)\nSCALELFS_TOOLS=%s\n' "$SCALELFS_TOOLS" >> "$DOTCONFIG"
fi
if [ -n "${SCALELFS_DEV:-}" ]; then
    printf '\n# ScaleLFS device pin (default: tracks ZNS_DEV)\nSCALELFS_DEV=%s\n' "$SCALELFS_DEV" >> "$DOTCONFIG"
fi
log "Saved $DOTCONFIG"

write_params
