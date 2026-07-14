#!/bin/bash

FILE="kern.log"
KEYWORD="f2fs_put_super"

START_LINE=$(grep -n "$KEYWORD" "$FILE" | tail -n 1 | cut -d: -f1)

if [ -z "$START_LINE" ]; then
  echo "Keyword '$KEYWORD' not found in the file."
  exit 1
fi

tail -n +"$START_LINE" "$FILE" | sed -E 's/^.*kernel: \[([0-9.]+)\] /\1 /; s/\[|\]//g'

