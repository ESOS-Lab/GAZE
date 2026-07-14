#!/bin/bash

# Usage: record_df.sh <output_file> [partition]
PART="${2:-nvme}"
touch "$1"
for (( ; ; )); do
    df -h | grep "$PART" >> "$1"
    sleep 1
done



