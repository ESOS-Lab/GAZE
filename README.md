# GAZE: SOSP '26 Artifact

Source code and artifact of **GAZE: Rearchitecting Log-Structured Filesystem
for Garbage Collection on Zoned Namespace Flash Storage** (SOSP '26).

GAZE is developed by [Juwon Kim](https://juwonkim97.github.io/) @ [OSLAB-KAIST](https://oslab.kaist.ac.kr/) (joo97508 at gmail.com).

GAZE has received all SOSP Artifact Evaluation badges.  
The version evaluated during the SOSP Artifact Evaluation is archived on Zenodo:

[![DOI](https://zenodo.org/badge/DOI/10.5281/zenodo.22119686.svg)](https://doi.org/10.5281/zenodo.22119686)


Table of contents:

1. **Overview**: what the artifact contains, which claims it backs, and how to set up
2. **Getting Started**: kick the tires with a ~1 min GAZE smoke run
3. **GAZE**: the real GAZE runs (FIO, optionally the macro benchmarks)
4. **Baselines without a reboot**: zoned F2FS, P-GC, ScaleLFS, ablation
5. **Profile figures (optional)**: Fig 5, 14, 16 on the profile kernel
6. **Baselines with a reboot (optional)**: ZNS+, IPLFS
7. **Viewing results**: solo vs merged figures and data provenance
8. **Source Code Overview**: paper §4 → kernel functions
9. **Troubleshooting**
10. **Reference**: modes, benchmarks, params, repo layout

---

## 1. Overview

### 1.1 What is being evaluated

GAZE is a filesystem-level garbage collection scheme, implemented in
F2FS, that rearchitects garbage collection to efficiently reclaim the
gigabyte-scale zones of ZNS flash storage. This artifact reproduces the
paper's main performance claims:

| Claim | Paper figures | How to reproduce |
|---|---|---|
| GAZE outperforms zoned F2FS, P-GC, ScaleLFS, ZNS+ and IPLFS in throughput and latency on FIO | Fig. 17 | [§3](#3-gaze-no-reboot) (GAZE); baselines in [§4](#4-baselines-without-a-reboot-f2fs-p-gc-scalelfs-ablation) and [§6](#6-baselines-with-a-reboot-optional-zns-iplfs) |
| Same versus zoned F2FS, P-GC, ZNS+ and IPLFS on macro workloads (Fileserver, YCSB-A, YCSB-F, TPC-C) | Fig. 18 | [§3](#3-gaze-no-reboot) (GAZE); baselines in [§4](#4-baselines-without-a-reboot-f2fs-p-gc-scalelfs-ablation) and [§6](#6-baselines-with-a-reboot-optional-zns-iplfs) |
| GAZE's techniques shorten effective GC latency (breakdown & ablation) | Fig. 5, 14, 16 | [§5](#5-profile-figures-optional-fig-5-14-16-on-the-profile-kernel) (profile kernel) |
| Contribution of each GAZE component (ablation) | Fig. 15 | [§4.3](#43-ablation-optional-paper-fig-15) (`run_ablation.sh`) |

Generated figure files are named after the paper's figure numbers
(`Fig-17.FIO_throughput.png` = paper Fig. 17, etc.).

The artifact has two branches. Branch `main` carries GAZE and every
baseline except IPLFS: **zoned F2FS** (`f2fs`), **P-GC** (`pgc`),
**ScaleLFS** (`scalelfs`), **ZNS+** (`zns_plus`), plus the six ablation
combinations of GAZE's three techniques. **IPLFS** differs so much from
the rest that it does not fit cleanly in the same branch, so it lives on
its own branch `iplfs`.

### 1.2 Evaluation guide

Experiments run on an **emulated** ZNS (or conventional) SSD provided by
NVMeVirt, a kernel module backed by reserved host DRAM, so no real ZNS
device is needed. That makes **memory** the resource that matters most:
the paper's 255 GB emulated SSD is carved out of physical RAM, so
reproducing the paper's setup takes roughly **512 GB of DRAM**, plus a
small **conventional SSD** partition for the ZNS modes' metadata device.
The paper machine's configuration:

| Component | Paper machine |
|---|---|
| CPU | 2 × Intel Xeon Gold 6230 (40 logical cores, 2 NUMA nodes) |
| DRAM | 512 GB physical; a large GRUB `memmap` reservation backs the emulated SSD (~64 GB left to the OS) |
| Emulated SSD | 255 GB NVMeVirt, ZNS (1 GB zones) or conventional per mode |
| Conventional SSD | one ~15 GB partition (`CONV_PART`) for the ZNS modes' metadata device |

**The provided server.** We provide the exact machine above, the one the
paper's numbers were produced on. If you do not have a machine of the
paper's configuration and want numbers directly comparable to the paper's,
use it (see [§1.3](#13-using-the-provided-server) to connect); you install and build on it from scratch
with [§1.4](#14-installation-and-build), exactly as on your own machine. On a machine with less memory
than the paper's (recall the emulated SSD is carved out of DRAM),
throughput and latency absolutes will drift from the paper's accordingly.

### 1.3 Using the provided server

If you are not using the provided server, skip to **[§1.4](#14-installation-and-build)**.

We give AEC reviewers access to the exact machine the paper's numbers were
produced on. For security, its address and login credentials are **not**
kept in this public repository (a commit would preserve them in the git
history for good, even if later removed). Instead, the server access
details are given in the **Additional Remarks** of our HotCRP submission.

Those details cover the Remote Access Controller (console access, and
rebooting the server if it ever gets stuck) and the SSH login to the
experimental server. Once connected, continue with **[§1.4](#14-installation-and-build)** to install and
build on the server.

**Reserving server time (required):** please add a comment on HotCRP at
least 24 hours in advance with your desired slot (date, time, and time
zone), and we will reserve the server for you then. As this is a shared
lab server, we kindly ask that each reservation be at most 24 hours; you
are of course welcome to book another slot if you need more time. Thank
you for understanding.

### 1.4 Installation and build

The commands below download the artifact and get your machine ready:
`setup_env.sh` installs the required packages, builds the benchmarks, and
configures MySQL; `set_config.sh` sizes the emulated SSD and the workloads
to your machine's memory.

```bash
cd ~
git clone -b main https://github.com/ESOS-Lab/GAZE.git GAZE   # any name works; the commands below assume "GAZE"
cd ~/GAZE
./setup_env.sh          # packages, benchmark builds, MySQL config (details: SETUP.md)
./set_config.sh         # machine sizing. Pick "paper" on a paper-config
                        # machine; pick "auto" otherwise, it sizes the
                        # emulated SSD and workloads to your RAM.
```

`set_config.sh` is interactive and asks four questions, in order:
1. Configuration mode  [1=paper, 2=auto, 3=manual]
2. Run 'update-grub' now?  [y/N]
3. Conventional SSD partition: the real disk partition (not the emulated SSD)
   used as the F2FS metadata device for the ZNS modes
4. Experiment mount point  [/mnt]

**On the provided server, accept the defaults, in order:**

    Configuration mode           -> Enter (paper)
    Run 'update-grub'?           -> y
    Conventional SSD partition   -> Enter (keeps /dev/nvme0n1p1)
    Experiment mount point       -> Enter (keeps /mnt)

**On a different machine, check each before answering:**

    Configuration mode           -> use `auto` unless this machine has ~320 GB+
                                    RAM (paper needs a 255 GB emulated SSD);
                                    `auto` sizes the SSD and workloads to fit
    Run 'update-grub'?           -> `y` is safe (only adds memmap/isolcpus,
                                    keeps your default boot entry)
    Conventional SSD partition   -> confirm the offered path is the disk you
                                    mean to use as the metadata device; if none
                                    is offered, create one first (~6% of the
                                    SSD, e.g. ~15 GB) and enter its path
    Experiment mount point       -> `/mnt` is fine if nothing important is
                                    mounted there

Once `set_config.sh` finishes, build and install the GAZE kernel:
`switch_mode.sh gaze` puts the kernel source tree in GAZE mode, and
`build_kernel.sh` compiles and installs that kernel and marks it as the
boot entry. One reboot then lands you in it.

```bash
cd ~/GAZE/kernel
./switch_mode.sh gaze
./build_kernel.sh       # ~15-20 min; installs the kernel, sets the boot entry
sudo reboot             # one reboot: applies the GRUB memmap/isolcpus
                        # parameters and boots into the artifact kernel
```

Rebooting normally lands in the artifact kernel by itself
(`build_kernel.sh` marks it as the default entry behind a 7-second GRUB
menu). Occasionally the GRUB menu stops and waits for input instead; in
that case select the `6.1.0.sosp26_ae` entry yourself from the console.
Verify with `uname -r` after boot, then continue with **[§2](#2-getting-started-kick-the-tires)**.

---

## 2. Getting Started (kick the tires)

If you would rather jump straight to the real experiments, skip to [§3](#3-gaze-no-reboot).

Otherwise, kick the tires first: a ~1 minute GAZE FIO run that exercises
the whole path (emulator, module, mkfs, benchmark, figure). GC does not
kick in over so short a run, so the throughput number is not meaningful;
this only confirms the pipeline works end to end.

The commands below save your current workload parameters, shorten the FIO
runtime to ~60 s, run GAZE on FIO, and open the resulting figure:

```bash
cd ~/GAZE/experiment_script
cp general_resource/experiment_params.sh general_resource/experiment_params.sh.bak   # save current params
sed -i 's/^FIO_RUNTIME=.*/FIO_RUNTIME=60/' general_resource/experiment_params.sh     # ~60 s smoke
printf 'gaze\nn\n' | ./select_fs.sh       # mode gaze, profiling n
printf '1\ny\n'    | ./run_experiment.sh  # FIO, then confirm
./view_results.sh gaze fio                # opens the figure (FIO throughput, paper Fig 17)
```

The emulator and the GAZE module load automatically; nothing else to set
up. **Restore your parameters before any real run** ([§3](#3-gaze-no-reboot) onward assume
them). The command below puts back exactly what you had, whether the
paper's config or your machine's auto-scaled one:

```bash
cp general_resource/experiment_params.sh.bak general_resource/experiment_params.sh   # restore the params saved above
```

The sections that follow are ordered to save your time (fewest reboots and
least kernel compile time) and by importance, with the most important
results first. We recommend following them in order.

---

## 3. GAZE (no reboot)

### 3.1 FIO (required)

GAZE runs on the base kernel you built in [§1.4](#14-installation-and-build). The commands below select
GAZE, run it on FIO (the fastest core result), and open the figure:

```bash
cd ~/GAZE/experiment_script
printf 'gaze\nn\n' | ./select_fs.sh       # gaze, profiling n (skip if already selected)
printf '1\ny\n'    | ./run_experiment.sh  # FIO (~23 min)
./view_results.sh gaze fio                # FIO throughput (paper Fig 17): your GAZE curve
```
### 3.2 Macrobenchmark (optional)

You can also run the macro benchmarks if you want; they feed the
macrobenchmark figure (paper Fig 18). Run all five back to back with
`printf 'a\ny\n' | ./run_experiment.sh`, or pick any subset when
`run_experiment.sh` prompts. Estimated times at paper params (the runner
shows a refined estimate, and your own completed runs replace these):

| Benchmark | Time | Feeds |
|---|---|---|
| FIO | ~23 min | Fig 17 |
| Fileserver | ~29 min | Fig 18 |
| YCSB-A | ~33 min | Fig 18 |
| YCSB-F | ~40 min | Fig 18 |
| TPC-C | ~90 min | Fig 18 |

All five is about 3.5 h. **YCSB / TPC-C need cold datasets** (FIO and
Fileserver need none). On the provided server they are already in place,
so you can skip this; if you use your own server or PC, generate them once
before the first YCSB-A / YCSB-F / TPC-C run with
`experiment_script/make_cold_data.sh` (about 220 GB, hours).

### 3.3 Viewing Results (required)

`./view_results.sh` opens the figures with your fresh GAZE runs drawn on
top of the paper's recorded data for every filesystem you have not run
yourself. These overlay plots are what the rest of this guide calls the
**merged figures**; [§7](#7-viewing-results) explains them and the
alternative view that shows only your own runs.

---

## 4. Baselines without a reboot (F2FS, P-GC, ScaleLFS, ablation)

Every baseline in this chapter uses the same kernel you already built in
[§1.4](#14-installation-and-build). You never rebuild the kernel or reboot to move between them: you
just build a small module for the mode you want, then select it. (This
covers only the base-kernel modes: `gaze`, `f2fs`, `pgc`, the six ablation
modes, and `scalelfs`. ZNS+ and the profile figures run on their own
kernels, [§6](#6-baselines-with-a-reboot-optional-zns-iplfs) and [§5](#5-profile-figures-optional-fig-5-14-16-on-the-profile-kernel).)

The command below builds one mode's module (here `f2fs`, a few minutes);
with no argument it builds every base mode at once (~1 h):

```bash
cd ~/GAZE/kernel
./build_all_modules.sh f2fs     # just this mode's module
# ./build_all_modules.sh        # no argument: every base mode, ~1 h
```

### 4.1 Zoned F2FS (required)

With the `f2fs` module built (above), the commands below select `f2fs` and
run it on FIO:

```bash
cd ~/GAZE/experiment_script
printf 'f2fs\nn\n' | ./select_fs.sh       # f2fs (no reboot)
printf '1\ny\n'    | ./run_experiment.sh  # FIO (~23 min)
./view_results.sh f2fs fio                # Fig 17 now shows GAZE vs F2FS
```

You can also run the other benchmarks, exactly as in [§3](#3-gaze-no-reboot). Their times match
[§3](#3-gaze-no-reboot) for FIO, Fileserver, and TPC-C, but YCSB-A and YCSB-F take roughly twice
as long for F2FS. Your fresh F2FS series replaces the paper's in the merged
figures.

### 4.2 P-GC and ScaleLFS (optional)

Same flow for more baselines. First make sure the mode's module is built
(`./build_all_modules.sh pgc`, or `./build_all_modules.sh scalelfs`), then
select, run, and view:

- `pgc` (P-GC): `printf 'pgc\nn\n' | ./select_fs.sh` then run; feeds Fig 17
  and Fig 18.
- `scalelfs` (ScaleLFS): `printf 'scalelfs\n' | ./select_fs.sh` then run;
  feeds **Fig 17 only** (the paper's macro figure never included it).
  `run_experiment.sh` handles the setup automatically: although ScaleLFS
  needs a different, conventional emulated SSD, the script switches the
  emulator to it for you.

### 4.3 Ablation (optional, paper Fig 15)

The sweep covers the six combinations of GAZE's three techniques plus
`f2fs` and `gaze` (eight modes). The commands below build all eight
modules once, then run the sweep:

```bash
cd ~/GAZE/kernel && ./build_all_modules.sh ablation   # 8 ablation modes, ~50 min
cd ~/GAZE/experiment_script
printf 'm\ny\n' | ./run_ablation.sh       # modes not run yet, ~23 min each
./view_results.sh ablation
```

`run_ablation.sh` lists the newest run per mode, then asks what to run:
**`m`** (used above) runs only the modes with no run yet and reuses your
existing `f2fs`/`gaze` runs, `a` runs all eight, or give a list of numbers.
You get paper **Fig 15** (FIO throughput under individual techniques). Each
mode is a ~23 min FIO run, so `m` after [§4.1](#41-zoned-f2fs) (six modes left) is about
2.5 h and `a` (all eight) about 3 h, on top of the ~50 min module build.

---

## 5. Profile figures (optional): Fig 5, 14, 16 on the profile kernel

These figures need code that measures the GC overhead inside the kernel
modules, so they run on a separate kernel image
(`6.1.0.sosp26_ae_profile`).

### 5.1 Build the profile kernel

The commands below put the tree in profile state, build that kernel (once,
~20 min), and reboot into it:

```bash
cd ~/GAZE/kernel
./switch_mode.sh f2fs --profile     # --profile is what matters; any mode works
./build_kernel.sh                   # ~15-20 min (confirm the GRUB prompt)
sudo reboot                         # ~2 min → 6.1.0.sosp26_ae_profile
```

### 5.2 Build a profile module, run it, view

After the [§5.1](#51-build-the-profile-kernel) reboot, confirm `uname -r` shows
`6.1.0.sosp26_ae_profile`. Then, as in
[§4](#4-baselines-without-a-reboot-f2fs-p-gc-scalelfs-ablation), build only
the profile module you need. The commands below build the `f2fs` profile
module, run it, and show Fig 5:

```bash
cd ~/GAZE/kernel && ./build_all_profile_modules.sh f2fs   # one module, a few min
cd ~/GAZE/experiment_script
printf 'f2fs\ny\n' | ./select_fs.sh   # f2fs, profiling y
printf '1\ny\n'    | ./run_experiment.sh  # FIO only (~25 min); figures rebuild at end
./view_results.sh profile             # Fig 5
```

For the other figures, build and run the modes they need the same way
(`./build_all_profile_modules.sh <mode>`, then select + run), or build
every profile module at once with `./build_all_profile_modules.sh` (~1 h).
Skipped bars fall back to the paper's data.

**Which modes for which figure:**

| Figure | Modes needed |
|---|---|
| **Fig 5**: 3 panels (overall / read / write phase) | vanilla `f2fs` |
| **Fig 14**: Vanilla / Page / Pipe / Pipe+Page | `f2fs`, `f2fs_pool`, `f2fs_pipe`, `gaze` |
| **Fig 16**: zF2FS / P-GC / ZNS+ / GAZE | `f2fs`, `pgc`, `gaze`, and a ZNS+ bar (see below) |

The Fig 16 ZNS+ bar needs a profiling build of the ZNS+ kernel,
`6.1.0.sosp26_ae_copy_profile`: run `switch_mode.sh zns_plus --profile`,
then build and reboot as in [§6.1](#61-zns), and do one profiling FIO run. If you skip
it, that bar falls back to the paper's recorded data.

Return to base afterwards: `switch_mode.sh gaze && build_kernel.sh` +
reboot (you built the base kernel in [§1.4](#14-installation-and-build), so this recompiles quickly).

---

## 6. Baselines with a reboot (optional): ZNS+, IPLFS

Unlike the baselines in
[§4](#4-baselines-without-a-reboot-f2fs-p-gc-scalelfs-ablation), ZNS+ and
IPLFS each run on their own kernel, so running either one means building a
whole new kernel image and rebooting.

### 6.1 ZNS+

ZNS+ modifies the block layer and NVMe driver, so it needs a separate
kernel image (`6.1.0.sosp26_ae_copy`). Budget ~25 min for the build and
reboot, then up to ~3.5 h for all five benchmarks (FIO alone ~23 min).

The commands below build the ZNS+ kernel and reboot into it:

```bash
cd ~/GAZE/kernel
./switch_mode.sh zns_plus
./build_kernel.sh                          # ~15-20 min (confirm the GRUB prompt)
sudo reboot                                # ~2 min
```

After the reboot, confirm `uname -r` shows `6.1.0.sosp26_ae_copy`, then run
ZNS+ on FIO with the commands below:

```bash
cd ~/GAZE/experiment_script
printf 'zns_plus\nn\n' | ./select_fs.sh    # zns_plus
printf '1\ny\n' | ./run_experiment.sh      # FIO ~23 min (or 'a' for all five, ~3.5 h)
./view_results.sh zns_plus fio             # Fig 17 / 18: your ZNS+ bar
```

When you are done and want to go back to the base GAZE kernel, rebuild it
and reboot:

```bash
cd ~/GAZE/kernel
./switch_mode.sh gaze && ./build_kernel.sh   # recompiles quickly (built in §1.4)
sudo reboot
```

### 6.2 IPLFS (branch `iplfs`)

IPLFS runs its authors' original storage stack. It is not just a
different filesystem: the block-I/O layer
and other kernel internals, the SSD emulator, and the mkfs tooling all
differ substantially from GAZE's, so it lives in exactly three
directories (`kernel/`, `ssd_emulator/`, `mkfs-tools/`) on branch
`iplfs`. Everything else (scripts, config, figures) is identical to
`main`. The scripts refuse mismatches in both directions (running
`iplfs` from `main`, or any other mode from the `iplfs` branch), so a
wrong-branch run stops with instructions instead of touching the wrong
kernel or emulator.

Budget ~20 min for setup, then up to ~3.5 h for the experiments.

Note that switching between `main` and `iplfs` via `git checkout` will invalidate the kernel build tree and emulator configuration (e.g., `auto.conf` and NVMeVirt module state). If you have uncommitted changes or active build artifacts, please commit/stash them before switching, and ensure you rebuild the kernel/tools as instructed below.

The commands below switch to the `iplfs` branch, build its kernel and
mkfs, and reboot into it:

```bash
git checkout iplfs                    # run data survives checkouts
cd kernel && ./build_kernel.sh        # one-time: IPLFS's kernel, ~15 min;
                                      #   stages iplfs.ko, sets the boot entry
cd ../mkfs-tools && ./build_tools.sh  # one-time: IPLFS's mkfs
sudo reboot                           # boots the IPLFS kernel
```

After the reboot, confirm `uname -r` shows the IPLFS kernel, then run IPLFS
on FIO with the commands below:

```bash
printf 'iplfs\n' | ./experiment_script/select_fs.sh      # pick iplfs
printf '1\ny\n'  | ./experiment_script/run_experiment.sh # FIO (or 'a' for all five)
./experiment_script/view_results.sh iplfs fio            # Fig 17 / 18: your IPLFS bar
```

To return to GAZE, switch back to `main` and rebuild the base kernel:

```bash
git checkout main
cd kernel && ./switch_mode.sh gaze && ./build_kernel.sh
sudo reboot
```

Fresh IPLFS runs land in the same merged figures automatically.

---

## 7. Viewing results

`./view_results.sh` (in `experiment_script/`) rebuilds the figures from
the latest runs on disk and opens them. Run it any time; earlier chapters
call it right after each experiment.

Run it with no arguments and answer the prompts, or skip them with a
shortcut:

```bash
./view_results.sh all          # comparison figure (all filesystems), FIO + macro
./view_results.sh gaze fio     # one filesystem, one category
./view_results.sh ablation     # Fig 15
./view_results.sh profile      # Fig 5 / 14 / 16
```

**Solo vs merged.** `build_graphs.sh` (which `view_results.sh` calls)
writes two trees:

| Directory | Contents |
|---|---|
| `AE_results/solo/{fio,macro}` | Fig 17 / 18 from **only what you ran**; skipped series are zero |
| `AE_results/merged/{fio,macro}` | The same figures with your fresh runs overlaid on the paper's data |

Series you re-run replace the paper's in `merged/`; everything else stays
paper data (including IPLFS, unless you re-measure it in [§6.2](#62-iplfs-branch-iplfs)). ScaleLFS
appears in the FIO figures only.

**Data provenance.** For every series the viewer prints where the data
came from, and saves the same list as `SOURCES.txt` next to each figure:

```
Data sources per series ([NEW] = generated here, [OLD] = SOSP26 paper data):
  fio/gaze: [NEW] .../FIO/gaze_data/fio_20260711_2246
  fio/zns:  [OLD] .../AE_results/fio_baseline/zns_data
  ...
```

For Fig 5 the `Conv.` bar falls back to paper data (override with
`CONV_RUN=<dir>`).

**Env-aware merging.** `merged/` only makes sense on the paper machine's
config. When `set_config.sh` ran in `auto`/`manual` mode (SSD/DRAM differ
from the paper, or workload params were auto-scaled), `build_graphs.sh`
**skips `merged/`** and builds only `solo/` with autoscaled axes;
`view_results.sh` then falls back to `solo/` with an "ENVIRONMENT
MISMATCH" banner. Each run dir carries a `run_env.txt` fingerprint, so
runs from a previous config are excluded automatically. For figures
reproducible verbatim against the paper, use `paper` mode ([§1.2](#12-evaluation-guide)).

**No local display?** `view_results.sh` always prints the figure paths.
View them on the Remote Access Controller's console ([§1.3](#13-using-the-provided-server)), or `scp` the
printed paths to your own machine.

---

## 8. Source Code Overview: GAZE Design (paper §4)

This section maps the three design elements of the paper (§4: Pipelined
Garbage Collection, Garbage Collection Page Pool, Selective Blocking) to the
kernel functions that implement them, so that evaluators can inspect the
implementation directly. All paths are relative to `kernel/fs/f2fs/`.

Each technique is compiled in or out by `CURRENT_FS_MODE` (`f2fs.h`, set by
`kernel/switch_mode.sh`); the mode-to-technique matrix (`__FS_HAS_PIPE`,
`__FS_HAS_POOL`, `__FS_HAS_SB` near the top of `f2fs.h`) enables the feature
gates `GC_PIPELINE`, `GC_PAGE_POOL`, and `NONBLOCKING_GC` respectively. This
is what the ablation modes of [§10.1](#101-filesystem-modes) toggle; grepping for these three macros
delimits exactly the code each technique adds.

### 8-1. Pipelined Garbage Collection (§4.1)

The four pipeline stages run as four concurrent actors (all in `gc.c`).
Naming note: the code calls the read phase's two stages "reading first/second
step" and the write phase's two stages "writing first/second step"; they
correspond one-to-one to the paper's preread/read/prewrite/write.

| Paper stage (thread) | Kernel actor | Per-stage work |
|---|---|---|
| preread (prereader) | GC thread: `do_garbage_collect_batched_deep_pipelined()` | `preread_data_segment_first_step()`, `preread_node_segment_first_step()`: scans the block bitmap (`check_valid_map()`), enqueues valid-block entries |
| read (reader) | `batched_deep_pipelined_gc_read_second_step_thread_func()` → `do_garbage_collect_batched_deep_pipelined_reading_second_step()` | `preread_data_segment_second_step()`: page-cache probe, pool page allocation, read I/O submission |
| prewrite (prewriter) | `batched_pipelined_gc_write_thread_func()` → `do_garbage_collect_batched_deep_pipelined_writing_first_step()` | `clean_data_segment_first_step()` → `move_data_page_first_step()`: re-validates the block, locates its page (page cache vs. pool), waits for read completion |
| write (writer) | `batched_pipelined_gc_write_second_step_thread_func()` → `do_garbage_collect_batched_deep_pipelined_writing_second_step()` | `clean_data_segment_second_step()` → `move_data_page_second_step()`: allocates the new disk block, updates filemap/bitmap, issues write I/O |

- **Control path** (paper: per-stage completion variables): the atomic
  sequence counters in `struct pipelined_gc_info` (`f2fs.h`):
  `preread_blk_seq` (prereader→reader), `read_step2_done_blk_seq`
  (reader→prewriter/writer), `prewrite_blk_seq` (prewriter→writer), plus the
  backpressure credits `read_step2_credit_seq` / `write_step1_credit_seq`.
  Each stage polls its predecessor's counter for a bounded interval, then
  sleeps on the corresponding wait queue (`prereader_wq`,
  `prereader_step2_wq`, `prewriter_step1_wq`).
- **Data path** (paper: two ring buffers): `reader_pass_buffer[]`
  (prereader→reader: inode/offset/blkaddr) and `writer_pass_buffer[]`
  (prewriter→writer: blkaddr/page/index pointer), both in
  `struct pipelined_gc_info` with head/tail indices.
- **Partitioning** (paper: 32 MB partitions): `N_BATCHED_SEGS = 16`
  (`f2fs.h`): one pipeline round reclaims 16 segments × 2 MB = 32 MB, and
  partitions of a victim section are processed serially.

### 8-2. Garbage Collection Page Pool (§4.2)

All four components live in `struct f2fs_sb_info` (`f2fs.h`, under
`#ifdef GC_PAGE_POOL`):

- **Free page pool**: created at module init by
  `f2fs_create_garbage_collection_page_pool()` (`gc.c`); pages are drawn with
  `f2fs_gc_pool_alloc_page()` / returned with `f2fs_gc_pool_free_page()`,
  bypassing the buddy allocator and zero-filling. The pool
  (`sbi->gc_pages_holded[]`, `N_GC_PAGES = 512 × N_BATCHED_SEGS`) is
  pre-populated at mount in `super.c`.
- **Valid page map**: `sbi->gc_pages[]`, indexed by
  `blkaddr − victim_sec_start_addr` for O(1) lookup.
  `f2fs_gc_pool_set_victim()` (`gc.c`) re-bases and clears it at the start of
  each reclamation round.
- **Pending page map**: `sbi->gc_pending_pages` (xarray keyed by the *new*
  blkaddr). The writer publishes an entry when it issues the migration write
  (`segment.c`, `xa_store` in the GC write path) and the write-completion
  handler removes it (`f2fs_write_end_io()`, `data.c`), unpinning the page.
- **Cache bitmap**: `sbi->gc_page_pool_bitmap`, a per-block bit that tells the
  prewriter whether a victim block is served from the page cache or from the
  pool.
- **Consistency with `read()`/`write()` (§4.2.2, shadow copy)**:
  `f2fs_gc_shadow_read()` (`data.c`): on a page-cache miss for a block under
  migration, it consults the valid/pending page maps under `gc_pool_lock`,
  waits for in-flight I/O, and copies the block into the page cache. Call
  sites sit in the buffered read / partial-write paths (e.g.
  `f2fs_get_read_data_page()`, the mpage read path, and write-begin).

### 8-3. Selective Blocking (§4.3)

- **Buffered write path (never blocks on GC)**: the buffered-write
  preallocation in `f2fs_preallocate_blocks()` (`file.c`) calls
  `f2fs_map_blocks_nonblocking()` (`data.c`): it performs the space
  reservation (counting reclaimable-invalid space, so a successful return
  still guarantees no later `ENOSPC`) and then calls
  `f2fs_balance_fs_nonblocking()` (`segment.c`), which unlike the legacy path
  returns immediately instead of running/waiting on foreground GC.
- **Synchronous / direct writes (still block)**: they keep the legacy
  `f2fs_balance_fs()` (`segment.c`), which blocks in `f2fs_gc()` until enough
  free sections exist; this is the "selective" part.
- Free-space accounting used by both paths: `has_not_enough_free_secs()` and
  `has_not_enough_free_secs_gc_trigger()` (`segment.h`).
- The known benign race this concurrency opens (a validity check that can
  misfire against an in-flight GC move) and why it is safe is documented at
  the `SKIP_BLKADDR_RACE_CHECK_ON_BUFFERED_WRITE` definition in `f2fs.h`.

**Crash consistency**: none of the three techniques adds recovery logic. All
filemap/bitmap updates commit through the unmodified F2FS checkpoint, the
page pool is volatile (lost on crash while the victim zone still holds the
original data), and the victim zone is reset only after the checkpoint that
persists the migrated metadata.

---

## 9. Troubleshooting

- **`select_fs.sh` refuses a mode**: the running kernel is not the one that
  mode needs (it prints the exact build + reboot commands). Each mode maps
  to one kernel image:
  - base `6.1.0.sosp26_ae`: `gaze`, `f2fs`, `pgc`, `scalelfs`, and the six ablation modes.
  - `6.1.0.sosp26_ae_copy`: `zns_plus` ([§6.1](#61-zns)).
  - `6.1.0.sosp26_ae_profile`: any mode run with profiling on ([§5](#5-profile-figures-optional-fig-5-14-16-on-the-profile-kernel)).
  - IPLFS kernel (branch `iplfs`): `iplfs` ([§6.2](#62-iplfs-branch-iplfs)).
- **A benchmark fails or is interrupted**: if you stop a run (Ctrl-C),
  `run_experiment.sh` halts the sequence, does not record that run's time,
  and runs a cleanup pass; the interrupted run is left out of the figures
  (each series uses its newest *completed* run). You can also clear stale
  state by hand with
  `experiment_script/general_resource/preflight_cleanup.sh`.
- **Results differ from the paper**: check `./set_params.sh`; the runner
  warns when the current workload parameters deviate from the paper's, and
  "reset to paper's original config" restores them exactly.

---

## 10. Reference

### 10.1 Filesystem modes

| Mode | What it is | Emulated SSD | mkfs | Kernel image |
|---|---|---|---|---|
| `gaze` | GAZE (all three techniques) | ZNS | zoned (`kernel/f2fs-tools-1.15.0`) | base |
| `f2fs` | zoned F2FS baseline | ZNS | zoned | base |
| `pgc` | P-GC baseline | ZNS | zoned | base |
| `f2fs_pool`, `f2fs_sb`, `f2fs_pipe`, plus the three pairwise combinations | ablation modes | ZNS | zoned | base |
| `scalelfs` | ScaleLFS baseline | **conventional** | **traditional f2fs-tools 1.16.0** (`scalelfs-mkfs-tools/`) | base |
| `zns_plus` | ZNS+ (copy-offload) | ZNS | zoned | `..._copy` ([§6.1](#61-zns)) |
| `iplfs` | IPLFS baseline | branch-specific | branch-specific | own image (branch `iplfs`, [§6.2](#62-iplfs-branch-iplfs)) |

`select_fs.sh` verifies the running kernel matches the requested mode and
refuses with a fix-up hint on mismatch. Modes that share the base kernel
image (everything except `zns_plus` and `iplfs`) switch with no reboot;
own-machine users build them once with `kernel/build_all_modules.sh`.

**ScaleLFS emulator swap** (auto-detected by the benchmark scripts; do
manually only if crossing modes outside them):

```bash
cd ~/GAZE/ssd_emulator
./stop_nvmevirt.sh && ./run_nvmevirt.sh conv    # for scalelfs
./stop_nvmevirt.sh && ./run_nvmevirt.sh zns     # for everything else
```

### 10.2 Running benchmarks (details)

`run_experiment.sh` shows the current FS, verifies workload params
against the paper's, cleans up stale state, then lets you pick any
subset of {FIO, Fileserver, YCSB-A, YCSB-F, TPC-C} with per-benchmark
time estimates. Selected benchmarks run back-to-back; figures rebuild at
the end. The emulated SSD is (re)loaded automatically into the variant
each mode needs.

Console prints a `[progress]` heartbeat every minute (elapsed vs.
estimated, current benchmark + remaining session). YCSB additionally
reports live op count / throughput. A silent console for several minutes
means something is wrong.

Raw results per run: `experiment_script/<Benchmark>/<fs>_data/<timestamped-run>/`.

### 10.3 Adjusting workload parameters

`./set_params.sh` (in `experiment_script/`): shorten runtimes for smoke
tests, tune dataset sizes for smaller SSDs, or restore the paper's config
with "reset to paper's original config". Auto-scaled defaults come from
`set_config.sh` (SETUP.md).

### 10.4 Repository structure

| Path | Contents |
|---|---|
| `kernel/` | Linux 6.1 tree. GAZE and all f2fs-family modes in `fs/f2fs`; ScaleLFS in `fs/scalelfs`; mode/profile switching in `switch_mode.sh`; builds via `build_kernel.sh`, `build_all_modules.sh`, `build_all_profile_modules.sh` |
| `kernel/f2fs-tools-1.15.0/` | zoned mkfs for the ZNS modes |
| `scalelfs-mkfs-tools/` | traditional f2fs-tools 1.16.0 (ScaleLFS's mkfs; built by `setup_env.sh`) |
| `ssd_emulator/` | NVMeVirt with GAZE's device-side GC + migration upcalls |
| `benchmarks/` | fio, filebench, YCSB, tpcc-mysql |
| `experiment_script/` | `select_fs.sh`, `set_params.sh`, `run_experiment.sh`, per-benchmark `run_*.sh`, shared config |
| `experiment_script/AE_results/` | figure builder + `solo/`, `merged/`, `profile/`, `ablation_baseline/` |
| `SETUP.md` | environment setup for a new machine |
