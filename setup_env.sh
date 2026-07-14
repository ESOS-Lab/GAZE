#!/bin/bash
# GAZE artifact environment setup.
#
# Installs every OS-level dependency and applies every one-time system
# configuration change (MySQL datadir/apparmor, filebench build, YCSB
# classpath fixup) needed to run the experiment scripts in
# experiment_script/. Safe to re-run — every step checks current state
# before changing anything.
#
# Usage: ./setup_env.sh
#
# Requires sudo. Two supported cases:
#   1. Normal account with full sudo access (the common case for a fresh
#      evaluator machine) — this script just works, prompting for a
#      password as needed.
#   2. An account with a *restricted* NOPASSWD sudoers file (as used for
#      this artifact's shared AE evaluation host) — the restricted list
#      must additionally allow the apt-get lines below, since installing
#      packages inherently requires broad privilege. If that hasn't been
#      granted, this script prints exactly what's missing and stops
#      instead of hanging on a password prompt.

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GENERAL_RES="$SCRIPT_DIR/experiment_script/general_resource"
FAIL=0
GRUB_JUST_CHANGED=0
GRUB_STILL_NEEDED=0

# Per-run log directory. Using a fixed path like /tmp/setup_env_*.log breaks
# when a different user (or a previous run with different ownership) left files
# behind — the redirection fails with "permission denied" and the step is
# reported as failed even though the underlying tool was never invoked. mktemp
# gives every run its own writable directory so ownership never collides.
LOGDIR="$(mktemp -d -t gaze-setup-XXXXXX 2>/dev/null || mktemp -d "${TMPDIR:-/tmp}/gaze-setup-XXXXXX")"
echo "[setup_env] Per-step logs: $LOGDIR"

log()  { echo "[setup_env] $*"; }
warn() { echo "[setup_env] WARNING: $*" >&2; }
fail() { echo "[setup_env] FAILED: $*" >&2; FAIL=1; }

# --- Preflight: make sure sudo works. Passwordless is NOT required — on a
#     normal fresh machine sudo prompts once here, gets cached, and a
#     background keep-alive refreshes the timestamp so long package installs
#     and builds never stall on a mid-run prompt. ---
if ! sudo -n true 2>/dev/null; then
    if [ -t 0 ]; then
        log "sudo needs your password once (cached and kept alive for the rest of this run):"
        sudo -v || {
            echo "[setup_env] This account cannot sudo. Run from an account with sudo access" >&2
            echo "[setup_env]   (needed for: apt-get, systemctl, mysql/apparmor config, usermod)." >&2
            exit 1
        }
    else
        echo "[setup_env] Non-interactive shell and no passwordless sudo — cannot prompt." >&2
        echo "[setup_env] Run interactively, or from an account with passwordless sudo." >&2
        exit 1
    fi
fi
# keep the sudo timestamp fresh for the whole run
( while true; do sudo -n true 2>/dev/null; sleep 60; done ) &
SUDO_KEEPALIVE_PID=$!
trap 'kill "$SUDO_KEEPALIVE_PID" 2>/dev/null' EXIT

# --- Cross-user hygiene: if the tree was previously touched by another user
#     (a common AE hand-off pattern: shared repo, whoever runs setup varies),
#     stale build artifacts left root/otheruser-owned break the make/mvn/
#     write steps below. Reclaim ownership of the *build directories* only
#     (not benchmark data, kernel/, etc.) so this run can rewrite them. ---
CURRENT_UID=$(id -u)
for reclaim in \
    "$SCRIPT_DIR/benchmarks/fio_src" \
    "$SCRIPT_DIR/benchmarks/filebench" \
    "$SCRIPT_DIR/benchmarks/YCSB/bin" \
    "$SCRIPT_DIR/benchmarks/YCSB/core/target" \
    "$SCRIPT_DIR/kernel/f2fs-tools-1.15.0" \
    "$SCRIPT_DIR/scalelfs-mkfs-tools"; do
    [ -e "$reclaim" ] || continue
    if find "$reclaim" -maxdepth 4 -not -uid "$CURRENT_UID" -print -quit 2>/dev/null | grep -q .; then
        log "Reclaiming ownership of $reclaim (previous run was a different user)."
        sudo chown -R "$(id -un):$(id -gn)" "$reclaim" 2>/dev/null || \
            warn "chown $reclaim failed — later build steps may hit permission errors."
    fi
done

# =============================================================================
# 1. OS packages
# =============================================================================
log "Installing OS packages (idempotent; apt skips anything already installed)..."

# Kernel / ssd_emulator build toolchain (cpio: kheaders packing; rsync:
# headers_install; zstd: module/initramfs compression on 24.04 kernels)
KERNEL_PKGS="build-essential bison flex libssl-dev libelf-dev bc libncurses-dev dwarves pkg-config cpio rsync zstd"
# filebench + f2fs-tools build toolchain (autotools)
AUTOTOOLS_PKGS="autoconf automake libtool uuid-dev libselinux1-dev"
# MySQL + YCSB + TPC-C toolchain
# libmysqlclient21 isn't pulled in transitively by mysql-server on this distro,
# but benchmarks/tpcc/tpcc_start is a prebuilt binary dynamically linked against
# it (mysql_config --libs_r at build time) — without it tpcc_start fails at
# runtime with "libmysqlclient.so.21: cannot open shared object file".
APP_PKGS="mysql-server libmysqlclient21 openjdk-11-jdk maven python-is-python3"
# Figures: gnuplot renders the .gpi plots, ghostscript (gs) converts their
# .eps output to .png (AE_results/build_graphs.sh), gv views .eps directly.
PLOT_PKGS="gnuplot gv ghostscript"
# System utilities the setup/run scripts lean on: dmidecode (set_config.sh
# RAM detection), parted (partprobe after partitioning), numactl (topology
# checks), git (branch workflow, e.g. the iplfs baseline).
UTIL_PKGS="dmidecode parted numactl git"

if sudo apt-get install -y $KERNEL_PKGS $AUTOTOOLS_PKGS $APP_PKGS $PLOT_PKGS $UTIL_PKGS > "$LOGDIR/apt.log" 2>&1; then
    log "Packages OK."
else
    fail "apt-get install failed — see $LOGDIR/apt.log"
fi

# =============================================================================
# 2. filebench: fix the generated autotools files, then build
#    (aclocal.m4/configure as checked into some snapshots may have been
#    generated by a different automake version than what's installed here,
#    which makes plain 'make' fail with a version-mismatch error).
# =============================================================================
FILEBENCH_DIR="$SCRIPT_DIR/benchmarks/filebench"
log "Building filebench..."
if [ -d "$FILEBENCH_DIR" ]; then
    (
        cd "$FILEBENCH_DIR" || exit 1
        if [ ! -x ./filebench ] || [ ! -f Makefile ]; then
            libtoolize -f && aclocal && autoheader && automake --add-missing && autoconf
        fi
        ./configure > "$LOGDIR/filebench_configure.log" 2>&1 && make > "$LOGDIR/filebench_make.log" 2>&1
    )
    if [ -x "$FILEBENCH_DIR/filebench" ]; then
        log "filebench OK ($("$FILEBENCH_DIR/filebench" -h 2>&1 | grep -o 'version [0-9.a-z-]*'))."
    else
        fail "filebench build did not produce a binary — see $LOGDIR/filebench_*.log"
    fi
else
    warn "benchmarks/filebench not found, skipping."
fi

# =============================================================================
# 2b. fio: rebuild the patched fio (GC-window measurement, benchmarks/
#     fio_src/jw_gc.h) so the binary matches the checked-in sources. A
#     prebuilt binary is tracked in git, so a failed build only warns —
#     but then the binary may predate local source changes.
#
#     Always re-run ./configure. The generated config-host.mak/config-host.h
#     encode host-specific feature detection (kernel-header struct layouts
#     like blk_zone_report::flags, glibc version, etc.); reusing a stale
#     copy from a different build machine bakes in the *wrong* CONFIG_*
#     macros and the compile fails with "no member named 'flags'" or
#     "NVME_URING_CMD_IO undeclared". `make` then rebuilds only the affected
#     objects so this is cheap.
# =============================================================================
FIO_SRC_DIR="$SCRIPT_DIR/benchmarks/fio_src"
log "Building fio..."
if (
    cd "$FIO_SRC_DIR" || exit 1
    rm -f config-host.mak config-host.h config.log
    ./configure > "$LOGDIR/fio_configure.log" 2>&1 \
        && make -j"$(nproc)" > "$LOGDIR/fio_make.log" 2>&1
); then
    log "fio built ($("$FIO_SRC_DIR/fio" --version 2>/dev/null))."
else
    warn "fio rebuild failed (see $LOGDIR/fio_*.log); using the checked-in binary."
fi

# =============================================================================
# 3. f2fs-tools: same story as filebench — configure/Makefile are gitignored
#    build output, config.sh's MKFS_F2FS points straight at the binary this
#    produces (no 'make install' — used in place).
# =============================================================================
F2FS_TOOLS_DIR="$SCRIPT_DIR/kernel/f2fs-tools-1.15.0"
log "Building f2fs-tools..."
if [ -d "$F2FS_TOOLS_DIR" ]; then
    (
        cd "$F2FS_TOOLS_DIR" || exit 1
        if [ ! -x mkfs/mkfs.f2fs ] || [ ! -f Makefile ]; then
            ./autogen.sh
        fi
        ./configure > "$LOGDIR/f2fstools_configure.log" 2>&1 && make > "$LOGDIR/f2fstools_make.log" 2>&1
    )
    if [ -x "$F2FS_TOOLS_DIR/mkfs/mkfs.f2fs" ]; then
        log "f2fs-tools OK (mkfs.f2fs $("$F2FS_TOOLS_DIR/mkfs/mkfs.f2fs" -V 2>&1 | head -1))."
    else
        fail "f2fs-tools build did not produce mkfs/mkfs.f2fs — see $LOGDIR/f2fstools_*.log"
    fi
else
    warn "kernel/f2fs-tools-1.15.0 not found, skipping."
fi

# =============================================================================
# 3a. ScaleLFS's traditional f2fs-tools (1.16.0, non-zoned): vendored at
#     scalelfs-mkfs-tools/, same in-place build (config.sh's MKFS_SCALELFS /
#     MKFS_SCALELFS_LIB point into it). Only the scalelfs baseline needs it,
#     but it builds in seconds, so build it unconditionally.
# =============================================================================
SCALELFS_TOOLS_DIR="$SCRIPT_DIR/scalelfs-mkfs-tools"
log "Building scalelfs-mkfs-tools (ScaleLFS's traditional f2fs-tools)..."
if [ -d "$SCALELFS_TOOLS_DIR" ]; then
    "$SCALELFS_TOOLS_DIR/build_tools.sh" > "$LOGDIR/scalelfs_tools_build.log" 2>&1 || true
    if [ -x "$SCALELFS_TOOLS_DIR/mkfs/mkfs.f2fs" ]; then
        log "scalelfs-mkfs-tools OK (mkfs.f2fs $("$SCALELFS_TOOLS_DIR/mkfs/mkfs.f2fs" -V 2>&1 | head -1))."
    else
        fail "scalelfs-mkfs-tools build did not produce mkfs/mkfs.f2fs — see $LOGDIR/scalelfs_tools_build.log"
    fi
else
    warn "scalelfs-mkfs-tools/ not found, skipping (only the scalelfs mode needs it)."
fi

# =============================================================================
# 3b. ssd_emulator memory reservation (GRUB).
#     NVMeVirt (ssd_emulator/insmod.sh) memory-maps a chunk of physical RAM
#     to back its emulated SSD, at a fixed physical address
#     (memmap_start) and size (memmap_size). The kernel must be told to
#     reserve that exact range at boot via a `memmap=` cmdline parameter,
#     or insmod will fail / behave incorrectly. On a NUMA machine, this
#     range should also line up with one NUMA node's memory (and that
#     node's CPUs isolated via `isolcpus`) so the emulator's threads never
#     take a remote-memory-access hit — see `numactl --hardware` to find
#     node boundaries on a different machine; insmod.sh's memmap_start/size
#     and this isolcpus list are single-machine-specific tuning, not portable
#     as-is.
#
#     GRUB_CMDLINE_LINUX in /etc/default/grub applies to *every* kernel
#     entry on this machine, not just this one — on a shared machine with
#     other kernels in use, this reserves memory/CPUs away from all of
#     them too. This step is interactive and skipped entirely when not
#     needed or not confirmed.
# =============================================================================
INSMOD_SH="$SCRIPT_DIR/ssd_emulator/insmod.sh"
if [ -f "$INSMOD_SH" ]; then
    # Exclude commented-out lines (e.g. an example invocation left in as a
    # comment) — otherwise a line that repeats the same insmod invocation
    # verbatim makes grep -o match twice, joining the two matches with a
    # newline and corrupting MM_START/MM_SIZE/ISOLCPUS below (which then
    # corrupts the GRUB_CMDLINE_LINUX value this script may write).
    MM_START=$(grep -v '^\s*#' "$INSMOD_SH" | grep -o 'memmap_start=[0-9]*M' | grep -o '[0-9]*' | head -1)
    MM_SIZE=$(grep -v '^\s*#' "$INSMOD_SH" | grep -o 'memmap_size=[0-9]*M' | grep -o '[0-9]*' | head -1)
    ISOLCPUS=$(grep -v '^\s*#' "$INSMOD_SH" | grep -o 'cpus=[0-9,]*' | cut -d= -f2 | head -1)
    if [ -n "$MM_START" ] && [ -n "$MM_SIZE" ]; then
        if sudo grep -q '^GRUB_CMDLINE_LINUX=.*memmap=' /etc/default/grub 2>/dev/null; then
            log "GRUB already carries a memmap reservation for the SSD emulator."
            log "  (Re-sizing this machine? Re-run ./set_config.sh — it recomputes memmap/"
            log "   isolcpus for this machine and rewrites GRUB itself.)"
        else
            # Do NOT write GRUB here: the committed insmod.sh values are the
            # paper server's, and the right memmap/isolcpus depend on THIS
            # machine's RAM and NUMA topology. ./set_config.sh owns that
            # (paper/auto/manual modes): it detects both, computes the
            # reservation, backs up and rewrites GRUB_CMDLINE_LINUX, and
            # offers update-grub. Just point there.
            echo
            warn "GRUB has no memmap reservation for the SSD emulator yet."
            warn "  Run ./set_config.sh next — default 'paper' mode reproduces the paper's"
            warn "  sizing, 'auto' sizes to this machine. It writes GRUB_CMDLINE_LINUX"
            warn "  (memmap + isolcpus) and offers update-grub; reboot afterwards."
            GRUB_STILL_NEEDED=1
        fi
    else
        warn "Could not parse memmap_start/memmap_size out of $INSMOD_SH — run ./set_config.sh to (re)generate it."
        GRUB_STILL_NEEDED=1
    fi
else
    warn "ssd_emulator/insmod.sh not found, skipping GRUB check."
fi

# GRUB_DEFAULT=saved + GRUB_SAVEDEFAULT=true: lets kernel/build_kernel.sh make
# each newly-built kernel the *persistent* default (via grub-set-default) —
# every reboot uses it, not just the next one — while still letting anyone
# who manually picks a different entry at the boot menu have *their* choice
# persist too. Low-risk/no functional effect until something calls
# grub-set-default, so this is applied directly rather than prompted.
if ! sudo grep -qE '^GRUB_DEFAULT=saved$' /etc/default/grub 2>/dev/null; then
    sudo cp /etc/default/grub "/etc/default/grub.bak_$(date +%Y%m%d_%H%M)"
    sudo sed -i '/^GRUB_DEFAULT=/d; /^GRUB_SAVEDEFAULT=/d' /etc/default/grub
    sudo sed -i '1i GRUB_SAVEDEFAULT=true' /etc/default/grub
    sudo sed -i '1i GRUB_DEFAULT=saved' /etc/default/grub
    sudo update-grub > /dev/null 2>&1 && log "GRUB_DEFAULT=saved set (build_kernel.sh can now persist its kernel as the default)." \
        || fail "update-grub failed after setting GRUB_DEFAULT=saved"
else
    log "GRUB_DEFAULT=saved already set."
fi

# =============================================================================
# 4. MySQL: datadir -> $MNT/mysql, apparmor allow-list, adm group for
#    kern.log reads used by the GC-profiling path.
# =============================================================================
log "Configuring MySQL..."

MNT=$(grep -m1 '^MNT=' "$SCRIPT_DIR/.config" 2>/dev/null | cut -d= -f2)
MNT="${MNT:-/mnt}"

if [ -f "$GENERAL_RES/mysqld.cnf.template" ]; then
    sudo cp "$GENERAL_RES/mysqld.cnf.template" /etc/mysql/mysql.conf.d/mysqld.cnf
    # The template hardcodes /mnt/mysql; rewrite it if this machine's MNT differs.
    sudo sed -i "s#^datadir\s*=.*#datadir\t= ${MNT}/mysql#" /etc/mysql/mysql.conf.d/mysqld.cnf
    log "mysqld.cnf installed (datadir=${MNT}/mysql)."
else
    fail "mysqld.cnf.template missing at $GENERAL_RES"
fi

APPARMOR_PROFILE=/etc/apparmor.d/usr.sbin.mysqld
if [ -f "$APPARMOR_PROFILE" ]; then
    if sudo grep -q "^\s*/var/lib/mysql/\*\* rwk," "$APPARMOR_PROFILE" 2>/dev/null; then
        sudo sed -i "s#/var/lib/mysql/ r,#${MNT}/mysql/ r,#; s#/var/lib/mysql/\*\* rwk,#${MNT}/mysql/** rwk,#" "$APPARMOR_PROFILE"
        log "apparmor mysqld profile updated for ${MNT}/mysql."
    else
        log "apparmor mysqld profile already points elsewhere than /var/lib/mysql (left as-is)."
    fi
    sudo /etc/init.d/apparmor reload > /dev/null 2>&1 || sudo systemctl reload apparmor
else
    warn "apparmor profile not found at $APPARMOR_PROFILE — mysqld may be unconfined or this is not an apparmor system."
fi

if ! id -nG "$USER" | grep -qw adm; then
    sudo usermod -aG adm "$USER"
    log "Added $USER to the 'adm' group (read access to /var/log/kern.log for GC-profiling mode). Log out/in for it to take effect."
else
    log "$USER already in 'adm' group."
fi

sudo systemctl disable mysql > /dev/null 2>&1
log "mysql.service set to NOT auto-start at boot (experiment scripts start/stop it per-run)."

# =============================================================================
# 5. YCSB: use the stock bin/ycsb.sh launcher (bin/ycsb is a Python-2-only
#    script and will not run under Python 3), and make sure the MySQL JDBC
#    connector jar is on its classpath.
# =============================================================================
YCSB_DIR="$SCRIPT_DIR/benchmarks/YCSB"
log "Configuring YCSB..."
if [ -d "$YCSB_DIR" ]; then
    CONNECTOR=$(find "$YCSB_DIR" -maxdepth 1 -iname "mysql-connector-j-*.jar" | head -1)
    if [ -n "$CONNECTOR" ]; then
        cat > "$YCSB_DIR/bin/setenv.sh" << EOF
#!/bin/sh
# Adds the MySQL JDBC connector jar to the classpath for all ycsb.sh invocations.
CLASSPATH="\$CLASSPATH:\$YCSB_HOME/$(basename "$CONNECTOR")"
EOF
        chmod 755 "$YCSB_DIR/bin/setenv.sh"
        log "bin/setenv.sh written (connector: $(basename "$CONNECTOR"))."
    else
        warn "No mysql-connector-j-*.jar found under $YCSB_DIR — YCSB jdbc runs will fail until one is added."
    fi

    if [ ! -d "$YCSB_DIR/core/target/dependency" ]; then
        log "core module is missing its dependency jars (htrace, etc.) — fetching (needs network)..."
        if (cd "$YCSB_DIR" && mvn -q dependency:copy-dependencies -pl core -DoutputDirectory=target/dependency); then
            log "core dependencies OK."
        else
            fail "mvn dependency:copy-dependencies failed for YCSB core"
        fi
    else
        log "YCSB core dependencies already present."
    fi
else
    warn "benchmarks/YCSB not found, skipping."
fi

# =============================================================================
# 6. Sanity checks
# =============================================================================
log "Verifying toolchain..."
"$SCRIPT_DIR/benchmarks/fio_src/fio" --version > /dev/null 2>&1 && log "fio OK" || fail "fio_src/fio did not run"
"$FILEBENCH_DIR/filebench" -h > /dev/null 2>&1 && log "filebench OK" || fail "filebench did not run"
if [ -x "$SCRIPT_DIR/benchmarks/tpcc/tpcc_start" ]; then
    # -x only checks the executable bit; tpcc_start is a prebuilt binary and can
    # still fail at runtime if a shared lib it was linked against (libmysqlclient)
    # isn't installed. ldd catches that before an experiment gets 30+ min in.
    if ldd "$SCRIPT_DIR/benchmarks/tpcc/tpcc_start" 2>&1 | grep -q "not found"; then
        fail "benchmarks/tpcc/tpcc_start is missing shared libraries: $(ldd "$SCRIPT_DIR/benchmarks/tpcc/tpcc_start" | grep 'not found')"
    else
        log "tpcc_start OK"
    fi
else
    fail "benchmarks/tpcc/tpcc_start missing or not executable"
fi
[ -x "$YCSB_DIR/bin/ycsb.sh" ] && log "ycsb.sh OK" || fail "YCSB/bin/ycsb.sh missing or not executable"
[ -x "$F2FS_TOOLS_DIR/mkfs/mkfs.f2fs" ] && log "mkfs.f2fs OK" || fail "kernel/f2fs-tools-1.15.0/mkfs/mkfs.f2fs missing or not executable"
mysql --version > /dev/null 2>&1 && log "mysql client OK" || fail "mysql client not found"
java -version > /dev/null 2>&1 && log "java OK" || fail "java not found"
gnuplot --version > /dev/null 2>&1 && log "gnuplot OK" || fail "gnuplot not found"

echo
if [ "$FAIL" -eq 0 ]; then
    log "All checks passed. Next steps, in order:"
    log ""
    log "  1. ./set_config.sh"
    log "       Sizes SSD/DRAM, sets mount point, writes GRUB memmap+isolcpus."
    log "       Answer 'y' when it prompts to run update-grub."
    log ""
    log "  2. sudo ./kernel/switch_mode.sh gaze && sudo ./kernel/build_kernel.sh"
    log "       Builds and installs the GAZE kernel."
    log ""
    if [ "$GRUB_JUST_CHANGED" -eq 1 ]; then
        log "  3. sudo reboot     # REQUIRED — this run just changed GRUB memory reservation."
    elif [ "$GRUB_STILL_NEEDED" -eq 1 ]; then
        log "  3. sudo reboot     # REQUIRED after step 1 runs update-grub."
    else
        log "  3. sudo reboot     # only if build_kernel.sh printed 'REBOOT REQUIRED'"
        log "                     # (GRUB memmap reservation is already in place)."
    fi
    log ""
    log "  4. grep -o 'memmap=[^ ]*' /proc/cmdline"
    log "       After reboot, confirms the memmap reservation is live. Must match"
    log "       insmod.sh's memmap=SIZE\$START. If empty, wrong boot entry was chosen —"
    log "       check /boot/grub/grub.cfg."
    log ""
    log "  5. ./ssd_emulator/run_nvmevirt.sh"
    log "       Creates the emulated ZNS device. Diff lsblk before/after; set"
    log "       ZNS_DEV / ZNS_DEV_NAME in config.sh."
    log ""
    log "  6. Run experiments — see README §2 (Getting Started) and §3."
    log "       Heads-up: YCSB / TPC-C need preloaded MySQL cold data at"
    log "       MYSQL_DATA_PATH (config.sh). If you don't have it, prep it"
    log "       first — SETUP.md 'MySQL cold data' section. FIO / Fileserver"
    log "       don't need it."
else
    log "Some steps failed — see messages above before proceeding."
    exit 1
fi
