#!/bin/bash
# Ablation sweep for the paper's Fig 15 ("FIO Throughput under Individual
# Techniques"): runs FIO 4k randwrite across the technique combinations,
# one filesystem mode per bar, then rebuilds the figure.
#
#   Vanilla=f2fs  Pg=f2fs_pool  Pipe=f2fs_pipe  NB=f2fs_sb
#   Pipe+Pg=f2fs_pool_pipe  NB+Pg=f2fs_pool_sb  NB+Pipe=f2fs_sb_pipe
#   NB+Pipe+Pg=gaze     (NB = selective blocking, Pg = GC page pool)
#
# No select_fs.sh needed — this script switches modes itself. Each mode is
# one full FIO run (~21 min at paper params), so all 8 take ~3 hours.
# Bars you skip fall back to the newest run on disk, or to the paper's
# recorded data ([OLD]) — see AE_results/ablation_baseline/SOURCES.txt.
#
# Prerequisites (checked below):
#   - the BASE kernel (6.1.0.sosp26_ae) is running — not the profile/zns_plus
#     images (kernel/switch_mode.sh gaze + kernel/build_kernel.sh + reboot)
#   - every mode's .ko exists: kernel/build_all_modules.sh builds all of
#     them in one go (WARNING: that build takes roughly AN HOUR)
#
# Usage: ./run_ablation.sh

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE_DIR="$SCRIPT_DIR/general_resource/filesystem_module"
ABLATION="$SCRIPT_DIR/AE_results/ablation_baseline"

MODES=(f2fs f2fs_pool f2fs_pipe f2fs_sb f2fs_pool_pipe f2fs_pool_sb f2fs_sb_pipe gaze)
declare -A LABEL=(
    [f2fs]="Vanilla" [f2fs_pool]="Pg" [f2fs_pipe]="Pipe" [f2fs_sb]="NB"
    [f2fs_pool_pipe]="Pipe+Pg" [f2fs_pool_sb]="NB+Pg" [f2fs_sb_pipe]="NB+Pipe"
    [gaze]="NB+Pipe+Pg"
)

echo "================================================================"
echo " GAZE ablation sweep (paper Fig 15: FIO under individual techniques)"
echo "================================================================"

# --- prerequisite: base kernel ------------------------------------------------
KVER=$(uname -r)
if [[ "$KVER" != "6.1.0.sosp26_ae" ]]; then
    echo "[run_ablation] ERROR: running kernel is '$KVER', but the ablation"
    echo "  modules are built for the BASE image '6.1.0.sosp26_ae'."
    echo "  Get back to it with:"
    echo "    cd ~/GAZE/kernel && ./switch_mode.sh gaze && ./build_kernel.sh"
    echo "    sudo reboot        # boots 6.1.0.sosp26_ae"
    exit 1
fi

# --- prerequisite: all ablation modules built ---------------------------------
MISSING=()
for m in "${MODES[@]}"; do
    [[ -f "$MODULE_DIR/$m.ko" ]] || MISSING+=("$m")
done
if [[ ${#MISSING[@]} -gt 0 ]]; then
    echo "[run_ablation] ERROR: missing module(s): ${MISSING[*]}"
    echo "  Build every mode's .ko in one go with:"
    echo "    cd ~/GAZE/kernel && ./build_all_modules.sh"
    echo "  WARNING: that builds the fs module 9 times and takes roughly"
    echo "  AN HOUR on the paper machine. It only needs to be done once."
    exit 1
fi

# --- pick modes ---------------------------------------------------------------
newest_run() {
    local d
    for d in $(ls -1dt "$SCRIPT_DIR/FIO/$1_data"/fio_*/ 2>/dev/null); do
        [[ -s "$d/kiops_sum" ]] && { basename "$d"; return; }
    done
    echo "-"
}
echo
echo "Technique combinations (each = one ~21-min FIO run at paper params):"
i=1; declare -A IDX
for m in "${MODES[@]}"; do
    printf "  %d) %-16s %-11s newest run on disk: %s\n" \
        "$i" "$m" "(${LABEL[$m]})" "$(newest_run "$m")"
    IDX[$i]="$m"
    i=$((i+1))
done
echo "  a) all 8               (~3 h)"
echo "  m) only modes with no run on disk yet"
echo
echo "Bars you don't run now fall back to the newest run on disk, or to the"
echo "paper's recorded data."
read -r -p "> " PICK

SELECTED=()
if [[ "$PICK" =~ ^[Aa]$ ]]; then
    SELECTED=("${MODES[@]}")
elif [[ "$PICK" =~ ^[Mm]$ ]]; then
    for m in "${MODES[@]}"; do
        [[ "$(newest_run "$m")" == "-" ]] && SELECTED+=("$m")
    done
else
    for tok in $PICK; do
        [[ -n "${IDX[$tok]:-}" ]] && SELECTED+=("${IDX[$tok]}") \
            || echo "[run_ablation] ignoring unrecognized selection: $tok"
    done
fi
if [[ ${#SELECTED[@]} -eq 0 ]]; then
    echo "[run_ablation] Nothing to run — rebuilding the figure from what's on disk."
else
    echo
    echo "Will run FIO for: ${SELECTED[*]}  (~$(( ${#SELECTED[@]} * 21 )) min total)"
    read -r -p "Proceed? [Y/n]: " C
    [[ "$C" =~ ^[Nn]$ ]] && { echo "Cancelled."; exit 0; }
fi

# --- run ----------------------------------------------------------------------
FAILED=()
n=0
for m in "${SELECTED[@]}"; do
    n=$((n+1))
    echo
    echo "================================================================"
    echo " [$n/${#SELECTED[@]}] $m (${LABEL[$m]})  ($(date '+%H:%M:%S'))"
    echo "================================================================"
    "$SCRIPT_DIR/general_resource/preflight_cleanup.sh" > /dev/null
    if ! printf '%s\nn\n' "$m" | "$SCRIPT_DIR/select_fs.sh" > /dev/null; then
        echo "[run_ablation] select_fs failed for $m — stopping." >&2
        exit 1
    fi
    if ( cd "$SCRIPT_DIR/FIO" && sudo ./run_fio.sh ); then
        echo "[run_ablation] $m done."
    else
        echo "[run_ablation] $m FAILED — its bar will use older/paper data." >&2
        FAILED+=("$m")
    fi
done

# --- figure -------------------------------------------------------------------
echo
echo "==== Rebuilding paper Fig 15 ===="
"$ABLATION/make_graph.sh" || exit 1
echo
echo "[run_ablation] Figures (paper Fig 15 = legend + avg + stddev):"
ls -1 "$ABLATION"/Fig-15.*.png 2>/dev/null | sed 's/^/    /'
echo "  Data sources per bar: $ABLATION/SOURCES.txt"
grep -v '^#' "$ABLATION/SOURCES.txt" | sed 's/^/    /'
echo
echo "  (no X display? open the iDRAC virtual console (address in the"
echo "   server access doc), or scp the .png files to your machine)"
[[ ${#FAILED[@]} -gt 0 ]] && { echo "[run_ablation] FAILED modes: ${FAILED[*]}"; exit 1; }
exit 0
