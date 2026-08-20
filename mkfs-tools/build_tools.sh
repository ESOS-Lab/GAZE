#!/bin/bash
# Build + install IPLFS's own mkfs/fsck (one-time on this branch).
# Compile as the invoking user; only the install step elevates.
set -e
cd "$(dirname "${BASH_SOURCE[0]}")"

me="$(id -un)"

# --- Self-heal root-owned build leftovers ------------------------------------
# libtool writes objects into .libs/ owned by whoever ran `make`. If the build
# ever ran as root (e.g. the whole script was launched under sudo), those
# .libs/ dirs stay root-owned; a later unprivileged rebuild then dies with
#   Fatal error: can't create .libs/....o: Permission denied
# and `make clean` can't remove them either. Reclaim ownership before building.
# aec is whitelisted for `sudo find *` (NOPASSWD), so no sudoers change is
# needed -- and nothing here elevates unless a leftover is actually present.
if [ -n "$(find . ! -user "$me" -print -quit 2>/dev/null)" ]; then
    echo "build_tools.sh: reclaiming root-owned build leftovers -> $me"
    sudo find . ! -user "$me" -exec chown "$me":"$(id -gn)" {} +
fi

# --- Regenerate autotools glue against the local toolchain -------------------
# The committed aclocal.m4/configure/libtool were generated with a specific
# libtool/autoconf version. On a host with a different libtool -- or after
# hopping between branches carrying different generated trees -- ./configure
# emits a libtool wrapper whose version no longer matches the LT_INIT macros in
# aclocal.m4, and the build dies with:
#   libtool: Version mismatch error. This is libtool 2.4.7 ...,
#   but the definition of this LT_INIT comes from libtool 2.4.6.
# Regenerating first makes the build reproducible regardless of prior branch/tools.
if command -v autoreconf >/dev/null 2>&1; then
    autoreconf -fi
else
    echo "build_tools.sh: 'autoreconf' not found; install autoconf/automake/libtool" >&2
    echo "  e.g.  sudo apt-get install -y autoconf automake libtool" >&2
    exit 1
fi

make clean >/dev/null 2>&1 || true
./configure
# this vintage autotools tree races under -j; serial build is quick anyway
make
sudo make install
