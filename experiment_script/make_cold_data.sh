#!/bin/bash
# Generates the preloaded cold data the YCSB/TPC-C benchmarks copy in
# before every run: MySQL datadirs (YCSB records / TPC-C warehouses, loaded
# once here so runs never pay the hours-long load) and plain cold filler
# files. Sizes come from the CURRENT config: record/warehouse counts from
# experiment_script/general_resource/experiment_params.sh (already scaled
# if you used `set_config.sh auto`) and filler sizes from SSD_GB — so the
# generated set always matches what the run scripts will need.
#
# Safety rules (deliberate):
#   - never overwrites: if any target name already exists at the
#     destination, it lists them and asks YOU to delete them first.
#   - checks the destination's free space against the computed total
#     before doing anything.
#   - prints exactly what it will create, where, and how big, and asks for
#     confirmation before starting.
#
# The MySQL datadirs are built on the emulated SSD ($MNT, freshly
# formatted via setup_fs.sh with the currently selected filesystem) and
# then copied out to the destination, mirroring SETUP.md's bootstrap
# procedure. Filler files are written directly at the destination.
#
# Usage: ./make_cold_data.sh          (interactive)

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../config.sh"
source "$GENERAL_RES/lib.sh"
source "$GENERAL_RES/cold_data_lib.sh"

log()  { echo "[make_cold_data] $*"; }
die()  { echo "[make_cold_data] ERROR: $*" >&2; exit 1; }

require_branch_for_fs_mode
[[ "$FS_MODE" == "scalelfs" ]] && die "select a ZNS-family filesystem first (scalelfs has no MySQL benchmarks; the datadir would be built on the wrong filesystem). Run experiment_script/select_fs.sh."

# Paper anchors: dataset sizes measured on the provided server.
PAPER_SSD_GB=256
PAPER_YCSB_GB=174   ; PAPER_YCSB_RECORDS=16000000
PAPER_TPCC_GB=155   ; PAPER_TPCC_WAREHOUSES=1700

scale_gb() { # PAPER_GB NUMER DENOM -> max(1, round(PAPER_GB * NUMER/DENOM))
    local g=$(( ($1 * $2 + $3 / 2) / $3 ))
    [ "$g" -lt 1 ] && g=1
    echo "$g"
}

YCSB_GB=$(scale_gb "$PAPER_YCSB_GB" "$YCSBA_RECORDCOUNT" "$PAPER_YCSB_RECORDS")
TPCC_GB=$(scale_gb "$PAPER_TPCC_GB" "$TPCC_WAREHOUSES" "$PAPER_TPCC_WAREHOUSES")
C10_GB=$(scale_gb 10 "$SSD_GB" "$PAPER_SSD_GB")
C20_GB=$(scale_gb 20 "$SSD_GB" "$PAPER_SSD_GB")
C40_GB=$(scale_gb 40 "$SSD_GB" "$PAPER_SSD_GB")

# Names: the paper's exact names when the scale is the paper's, otherwise
# size-truthful scaled names (recorded into ./.config at the end so the run
# scripts pick them up via cold_data_lib.sh).
if [[ "$YCSBA_RECORDCOUNT" -eq "$PAPER_YCSB_RECORDS" ]]; then N_YCSB="mysql_174G"; else N_YCSB="mysql_ycsb_${YCSB_GB}G"; fi
if [[ "$TPCC_WAREHOUSES" -eq "$PAPER_TPCC_WAREHOUSES" ]]; then N_TPCC="mysql-tpcc-155G"; else N_TPCC="mysql_tpcc_${TPCC_GB}G"; fi
if [[ "$SSD_GB" -eq "$PAPER_SSD_GB" ]]; then
    N_C10="cold_10G"; N_C20="cold_20G"; N_C40="cold_40G"
else
    N_C10="cold_A_${C10_GB}G"; N_C20="cold_B_${C20_GB}G"; N_C40="cold_C_${C40_GB}G"
fi

# ---------------------------------------------------------------------------
# 1. What to build?
# ---------------------------------------------------------------------------
echo "Which benchmarks' cold data do you want to generate?"
echo "  1) YCSB   (MySQL datadir ~${YCSB_GB}G + fillers ${C20_GB}G/${C10_GB}G)"
echo "  2) TPC-C  (MySQL datadir ~${TPCC_GB}G + fillers ${C40_GB}G/${C20_GB}G)"
echo "  3) Both"
read -r -p "> [3] " PICK
case "${PICK:-3}" in
    1) DO_YCSB=1; DO_TPCC=0 ;;
    2) DO_YCSB=0; DO_TPCC=1 ;;
    3) DO_YCSB=1; DO_TPCC=1 ;;
    *) die "unknown choice '$PICK'" ;;
esac

# ---------------------------------------------------------------------------
# 2. Where? (user-chosen; recorded as MYSQL_DATA_PATH afterwards)
# ---------------------------------------------------------------------------
read -r -p "Destination directory [default: $MYSQL_DATA_PATH]: " DEST
DEST="${DEST:-$MYSQL_DATA_PATH}"
sudo mkdir -p "$DEST" || die "could not create $DEST"

# ---------------------------------------------------------------------------
# 3. Build the item list (name, GB, kind). cold_20 is shared by both.
# ---------------------------------------------------------------------------
ITEMS_NAME=(); ITEMS_GB=(); ITEMS_KIND=()
add_item() { ITEMS_NAME+=("$1"); ITEMS_GB+=("$2"); ITEMS_KIND+=("$3"); }
[[ "$DO_YCSB" -eq 1 ]] && add_item "$N_YCSB" "$YCSB_GB" ycsb_db
[[ "$DO_TPCC" -eq 1 ]] && add_item "$N_TPCC" "$TPCC_GB" tpcc_db
add_item "$N_C20" "$C20_GB" filler
[[ "$DO_YCSB" -eq 1 ]] && add_item "$N_C10" "$C10_GB" filler
[[ "$DO_TPCC" -eq 1 ]] && add_item "$N_C40" "$C40_GB" filler

# ---------------------------------------------------------------------------
# 4. Refuse to overwrite anything that already exists.
# ---------------------------------------------------------------------------
CLASH=()
for i in "${!ITEMS_NAME[@]}"; do
    [[ -e "$DEST/${ITEMS_NAME[$i]}" ]] && CLASH+=("$DEST/${ITEMS_NAME[$i]}")
done
if [[ ${#CLASH[@]} -gt 0 ]]; then
    echo "[make_cold_data] The following already exist and will NOT be overwritten:" >&2
    printf '[make_cold_data]   %s\n' "${CLASH[@]}" >&2
    echo "[make_cold_data] Delete them yourself first (so nothing of yours is removed by" >&2
    echo "[make_cold_data]   this script), then re-run. Nothing was created." >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# 5. Space check at the destination (and sanity vs the emulated SSD, which
#    is where the MySQL datadirs get built before being copied out).
# ---------------------------------------------------------------------------
TOTAL_GB_NEEDED=0
for g in "${ITEMS_GB[@]}"; do TOTAL_GB_NEEDED=$(( TOTAL_GB_NEEDED + g )); done
NEED_KB=$(( (TOTAL_GB_NEEDED * 105 / 100 + 1) * 1024 * 1024 ))
AVAIL_KB=$(df --output=avail -k "$DEST" | tail -1 | tr -d ' ')
if [[ "$AVAIL_KB" -lt "$NEED_KB" ]]; then
    die "not enough space at $DEST: need ~${TOTAL_GB_NEEDED}G (+5% slack), available $(( AVAIL_KB / 1024 / 1024 ))G.
[make_cold_data]   Pick a bigger destination, or re-run ./set_config.sh (auto mode on a smaller
[make_cold_data]   machine shrinks the datasets) and try again."
fi
BIGGEST_DB=0
[[ "$DO_YCSB" -eq 1 && "$YCSB_GB" -gt "$BIGGEST_DB" ]] && BIGGEST_DB=$YCSB_GB
[[ "$DO_TPCC" -eq 1 && "$TPCC_GB" -gt "$BIGGEST_DB" ]] && BIGGEST_DB=$TPCC_GB
[[ "$BIGGEST_DB" -gt 0 && $(( BIGGEST_DB * 12 / 10 )) -ge "$SSD_GB" ]] && \
    die "the MySQL datadir (~${BIGGEST_DB}G) would not fit on the ${SSD_GB}G emulated SSD it must be built on. Re-run ./set_config.sh and check your sizing."

# ---------------------------------------------------------------------------
# 6. Final confirmation.
# ---------------------------------------------------------------------------
echo
echo "================================================================"
echo " About to create in: $DEST"
echo "================================================================"
for i in "${!ITEMS_NAME[@]}"; do
    printf "  %-22s ~%4sG  (%s)\n" "${ITEMS_NAME[$i]}" "${ITEMS_GB[$i]}" \
        "$([ "${ITEMS_KIND[$i]}" == filler ] && echo "cold filler file" || echo "MySQL datadir, loaded via ${ITEMS_KIND[$i]%%_db}")"
done
echo "  ------------------------------------------"
echo "  Total: ~${TOTAL_GB_NEEDED}G   (destination free: $(( AVAIL_KB / 1024 / 1024 ))G)"
echo
echo "MySQL datadir loading runs on the emulated SSD first and can take a"
echo "long time (the paper-sized YCSB load is multiple hours)."
read -r -p "Proceed? [y/N]: " OK
[[ "$OK" =~ ^[Yy]$ ]] || { echo "[make_cold_data] Cancelled. Nothing was created."; exit 0; }

# ---------------------------------------------------------------------------
# 7. Generate.
# ---------------------------------------------------------------------------
make_filler() { # NAME GB
    log "Creating filler $DEST/$1 (${2}G)..."
    sudo dd if=/dev/zero of="$DEST/$1" bs=1M count=$(( $2 * 1024 )) status=progress \
        || die "filler creation failed for $1"
    sync
}

fresh_mysql_on_mnt() {
    "$GENERAL_RES/preflight_cleanup.sh" > /dev/null
    "$GENERAL_RES/setup_fs.sh" setup || die "filesystem setup failed"
    sudo mkdir -p "$MNT/mysql" && sudo chown -R mysql:mysql "$MNT/mysql"
    # Fresh datadir: initialize without a root password so we can create
    # the benchmark account below (matches SETUP.md's bootstrap).
    sudo -u mysql mysqld --initialize-insecure --datadir="$MNT/mysql" \
        || die "mysqld --initialize failed (check /var/log/mysql/error.log)"
    sudo systemctl start mysql || die "mysql failed to start on $MNT/mysql"
    sudo mysql -u root <<SQL || die "creating the benchmark MySQL account failed"
CREATE USER IF NOT EXISTS '$MYSQL_USER'@'%' IDENTIFIED BY '$MYSQL_PASSWD';
CREATE USER IF NOT EXISTS '$MYSQL_USER'@'localhost' IDENTIFIED BY '$MYSQL_PASSWD';
GRANT ALL PRIVILEGES ON *.* TO '$MYSQL_USER'@'%';
GRANT ALL PRIVILEGES ON *.* TO '$MYSQL_USER'@'localhost';
FLUSH PRIVILEGES;
SQL
}

copy_out_datadir() { # NAME
    sudo systemctl stop mysql
    log "Copying datadir out to $DEST/$1 (this can take a while)..."
    sudo cp -rp "$MNT/mysql" "$DEST/$1" || die "copy-out failed"
    sync
    "$GENERAL_RES/setup_fs.sh" teardown
}

make_ycsb_db() {
    log "Building the YCSB MySQL datadir (${YCSBA_RECORDCOUNT} records, ~${YCSB_GB}G)..."
    fresh_mysql_on_mnt
    sudo mysql -u root -e "CREATE DATABASE IF NOT EXISTS ycsb;"
    ( cd "$GAZE_DIR/benchmarks/YCSB" && \
      sudo bin/ycsb.sh load jdbc -P workloads/workloada -P db.properties \
          -p recordcount="$YCSBA_RECORDCOUNT" -p fieldlength=800 \
          -threads "$(nproc)" ) || die "YCSB load failed"
    copy_out_datadir "$N_YCSB"
}

make_tpcc_db() {
    log "Building the TPC-C MySQL datadir (${TPCC_WAREHOUSES} warehouses, ~${TPCC_GB}G)..."
    fresh_mysql_on_mnt
    sudo mysql -u root -e "CREATE DATABASE IF NOT EXISTS tpcc1000;"
    sudo mysql -u root tpcc1000 < "$GAZE_DIR/benchmarks/tpcc/create_table.sql" \
        || die "TPC-C create_table.sql failed"
    ( cd "$GAZE_DIR/benchmarks/tpcc" && \
      ./tpcc_load -h 127.0.0.1 -P 3306 -d tpcc1000 -u "$MYSQL_USER" -p "$MYSQL_PASSWD" \
          -w "$TPCC_WAREHOUSES" ) || die "tpcc_load failed"
    sudo mysql -u root tpcc1000 < "$GAZE_DIR/benchmarks/tpcc/add_fkey_idx.sql" \
        || log "WARNING: add_fkey_idx.sql failed (indexes/foreign keys) — continuing; TPC-C may run slower."
    copy_out_datadir "$N_TPCC"
}

for i in "${!ITEMS_NAME[@]}"; do
    [[ "${ITEMS_KIND[$i]}" == filler ]] && make_filler "${ITEMS_NAME[$i]}" "${ITEMS_GB[$i]}"
done
[[ "$DO_YCSB" -eq 1 ]] && make_ycsb_db
[[ "$DO_TPCC" -eq 1 ]] && make_tpcc_db

# ---------------------------------------------------------------------------
# 8. Record names/location into ./.config so the run scripts find them.
# ---------------------------------------------------------------------------
DOTCONFIG="$GAZE_DIR/.config"
sudo sed -i "/^MYSQL_DATA_PATH=/d; /^YCSB_COLD_DB=/d; /^TPCC_COLD_DB=/d; /^COLD_10G_NAME=/d; /^COLD_20G_NAME=/d; /^COLD_40G_NAME=/d" "$DOTCONFIG"
{
    echo "MYSQL_DATA_PATH=$DEST"
    echo "# cold-data names generated by make_cold_data.sh ($(date +%Y-%m-%d))"
    [[ "$DO_YCSB" -eq 1 ]] && echo "YCSB_COLD_DB=$N_YCSB"
    [[ "$DO_TPCC" -eq 1 ]] && echo "TPCC_COLD_DB=$N_TPCC"
    echo "COLD_20G_NAME=$N_C20"
    [[ "$DO_YCSB" -eq 1 ]] && echo "COLD_10G_NAME=$N_C10"
    [[ "$DO_TPCC" -eq 1 ]] && echo "COLD_40G_NAME=$N_C40"
} | sudo tee -a "$DOTCONFIG" > /dev/null

# db.properties (YCSB's JDBC credentials) is a tracked file with the paper
# machine's account; warn if this machine's differs.
if [[ "$DO_YCSB" -eq 1 ]] && ! grep -q "db.user=$MYSQL_USER" "$GAZE_DIR/benchmarks/YCSB/db.properties"; then
    log "WARNING: benchmarks/YCSB/db.properties uses a different db.user than MYSQL_USER=$MYSQL_USER;"
    log "  edit db.properties (db.user/db.passwd) to match, or YCSB runs will fail to connect."
fi

log "Done. Created in $DEST:"
for i in "${!ITEMS_NAME[@]}"; do log "  $DEST/${ITEMS_NAME[$i]}"; done
log "Recorded in $DOTCONFIG — the YCSB/TPC-C benchmarks are now runnable."
