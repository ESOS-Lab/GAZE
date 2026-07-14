#!/bin/bash
# Cold-data (preloaded MySQL datadirs + cold filler files) helpers, shared
# by the YCSB/TPC-C run scripts, run_experiment.sh, and make_cold_data.sh.
#
# The paper machine's cold-data set under $MYSQL_DATA_PATH:
#   mysql_174G       YCSB MySQL datadir  (16M records loaded, ~174G)
#   mysql-tpcc-155G  TPC-C MySQL datadir (1700 warehouses,   ~155G)
#   cold_10G/20G/40G plain filler files  (exact 10/20/40 GiB)
# Those names are the defaults below; a machine that generated a scaled set
# with make_cold_data.sh gets its names recorded in ./.config, which
# config.sh sources before this file — hence the := fallbacks.
#
# Requires config.sh to have been sourced first (MYSQL_DATA_PATH).

: "${YCSB_COLD_DB:=mysql_174G}"
: "${TPCC_COLD_DB:=mysql-tpcc-155G}"
: "${COLD_10G_NAME:=cold_10G}"
: "${COLD_20G_NAME:=cold_20G}"
: "${COLD_40G_NAME:=cold_40G}"

# _cold_report_missing LABEL ITEM...
# Prints what's missing under $MYSQL_DATA_PATH and how to fix it.
_cold_report_missing() {
    local label="$1"; shift
    echo "[cold_data] $label needs preloaded cold data that is missing under $MYSQL_DATA_PATH:" >&2
    local it
    for it in "$@"; do echo "[cold_data]   MISSING: $MYSQL_DATA_PATH/$it" >&2; done
    echo "[cold_data] Generate it once with:  experiment_script/make_cold_data.sh" >&2
    echo "[cold_data]   (sizes itself to your current config; after that this benchmark runs normally)" >&2
}

# ycsb_cold_data_ok / tpcc_cold_data_ok — return 0 when every item the
# benchmark's setup copy needs exists; otherwise print what's missing and
# return 1. Callers decide whether to skip or abort.
ycsb_cold_data_ok() {
    local missing=()
    [[ -d "$MYSQL_DATA_PATH/$YCSB_COLD_DB" ]] || missing+=("$YCSB_COLD_DB/ (YCSB MySQL datadir)")
    [[ -e "$MYSQL_DATA_PATH/$COLD_20G_NAME" ]] || missing+=("$COLD_20G_NAME (cold filler)")
    [[ -e "$MYSQL_DATA_PATH/$COLD_10G_NAME" ]] || missing+=("$COLD_10G_NAME (cold filler)")
    [[ ${#missing[@]} -eq 0 ]] && return 0
    _cold_report_missing "YCSB" "${missing[@]}"
    return 1
}

tpcc_cold_data_ok() {
    local missing=()
    [[ -d "$MYSQL_DATA_PATH/$TPCC_COLD_DB" ]] || missing+=("$TPCC_COLD_DB/ (TPC-C MySQL datadir)")
    [[ -e "$MYSQL_DATA_PATH/$COLD_20G_NAME" ]] || missing+=("$COLD_20G_NAME (cold filler)")
    [[ -e "$MYSQL_DATA_PATH/$COLD_40G_NAME" ]] || missing+=("$COLD_40G_NAME (cold filler)")
    [[ ${#missing[@]} -eq 0 ]] && return 0
    _cold_report_missing "TPC-C" "${missing[@]}"
    return 1
}
