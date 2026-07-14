# GC Latency Breakdown (paper figure: `pipe_gc_lat_breakdown.eps`)

Serial GC (F2FS) vs Pipelined GC latency breakdown, FIO 4K randwrite,
victim zone utilization 40%. Regenerates the figure in the paper's
evaluation section from profile-mode FIO runs.

## TL;DR (fully automatic path)

Profiling changes the kernel image, so this needs ONE reboot into the
profile kernel; after that everything is automatic:

```
sudo kernel/switch_mode.sh gaze --profile
sudo kernel/build_kernel.sh                  # once; then reboot into
                                             # 6.1.0.sosp26_ae_profile
kernel/build_all_profile_modules.sh          # all *_profile.ko, no reboot

experiment_script/select_fs.sh               # pick f2fs, profiling: y
sudo experiment_script/FIO/run_fio.sh
experiment_script/select_fs.sh               # pick f2fs_pipe, profiling: y
sudo experiment_script/FIO/run_fio.sh        # <- figure auto-generates here
```

> NOTE: this figure is no longer part of the AE profile flow (README
> §3.5 now covers paper Fig 5/14/16 only), so `run_fio.sh` no longer calls
> this `make_graph.sh` automatically — run it by hand (below). Also note
> the profile kernel now logs only util-40 GCs (`GC_PROFILE_UTIL_ONLY`),
> which is exactly what this figure parses, so fresh runs still work.

Run `./make_graph.sh` after both runs exist; the figure appears here as
`pipe_gc_lat_breakdown_data` / `.eps` / `.png`.

## Manual / re-plot

```
./make_graph.sh                              # newest runs, util 40
./make_graph.sh SERIAL_RUN_DIR PIPE_RUN_DIR  # explicit runs
UTIL=70 OUT_DIR=/tmp/x ./make_graph.sh       # overrides
```

## How it works

- Profile kernels print per-GC phase latencies to dmesg
  (`print_cur_gc_profiler`: `OSDI26_analysis` lines for serial GC,
  `pipe_OSDI26` lines for pipelined GC); `run_fio.sh` archives dmesg into
  each run dir.
- `parse_gc_breakdown.sh RUN_DIR (serial|pipe) [UTIL]` averages the phase
  latencies over the GCs at the given victim utilization. The logic is the
  exact pipeline that produced the paper figure (archived, with the raw
  December runs, in `f2fs_vanilla_profile_*/` and `f2fs_4step_profile_*/`
  here) — regenerating from those archives reproduces the committed
  `pipe_gc_lat_breakdown_data` bit-for-bit modulo hand-rounding.
- `make_graph.sh` assembles the 6-bar data file (serial Total; pipe Total;
  the 4 pipeline stages) and runs `plot_compare_f2fs_pipe_lat_breakdown.gpi`.

Bar math: pipe "Total" = checkpoint + preprocess(p0-p2) + (do_gc − p0-p2);
serial Read = p3 − preread, serial Write = p4 − prewrite.
