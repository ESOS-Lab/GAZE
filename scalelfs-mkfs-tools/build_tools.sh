#!/bin/bash
# Build ScaleLFS's traditional f2fs-tools (1.16.0) in place -- no install.
# config.sh's MKFS_SCALELFS points straight at mkfs/mkfs.f2fs and
# MKFS_SCALELFS_LIB at lib/.libs, so building is all that's needed.
# Run as the repo owner (no sudo). Called by ./setup_env.sh; safe to rerun.
set -e
cd "$(dirname "${BASH_SOURCE[0]}")"

if [ ! -x mkfs/mkfs.f2fs ] || [ ! -f Makefile ]; then
    ./autogen.sh
fi
./configure
make
echo "[scalelfs-mkfs-tools] OK: $(./mkfs/mkfs.f2fs -V 2>&1 | head -1)"
