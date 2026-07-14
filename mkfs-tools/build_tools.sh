#!/bin/bash
# Build + install IPLFS's own mkfs/fsck (one-time on this branch).
# Compile as the invoking user; only the install step elevates.
set -e
cd "$(dirname "${BASH_SOURCE[0]}")"

make clean >/dev/null 2>&1 || true
./configure
# this vintage autotools tree races under -j; serial build is quick anyway
make
sudo make install
