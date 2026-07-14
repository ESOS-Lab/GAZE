# GC Latency Breakdown across Different Filesystems (paper Fig. "sota_gc_lat")

Regenerates `sota_gc_lat_graph.eps` (paper: stacked per-phase GC latency of
zF2FS / P-GC / ZNS+ / GAZE under FIO, victim zone utilization 40%). The
legend `keys/sota_gc_lat_breakdown_keys.eps` is static.

## Layout

Each of the four subdirectories holds exactly **one** benchmark run dir:

```
f2fs/<run>   zF2FS   (f2fs mode,  profile)   -> get_gc_lat_breakdown_data.sh
pgc/<run>    P-GC    (pgc mode,   profile)   -> get_gc_lat_breakdown_data.sh
copy/<run>   ZNS+    (zns_plus,   profile)   -> get_copy_gc_lat_breakdown_data.sh
gaze/<run>   GAZE    (gaze mode,  profile)   -> get_pipe_gc_lat_breakdown_data.sh
```

For each filesystem `./make_graph.sh` resolves the run dir as: env
override (`F2FS_RUN`/`PGC_RUN`/`ZNS_PLUS_RUN`/`GAZE_RUN`) > newest profile
FIO run (`experiment_script/FIO/<mode>_profile_data/fio_*/`) > the staged
archive dir above — and prints which one each bar came from. It then runs
the appropriate `get_*_gc_lat_breakdown_data.sh` inside that run dir
(lib.sh copies it + its `parse_*.sh` helper there at benchmark teardown),
collects the four `four_phase_lat_data` rows into `sota_gc_lat_data`, and
plots with `plot_sota_gc_lat.gpi`. With no fresh runs on disk, the staged
archive run dirs reproduce the committed `sota_gc_lat_data` and the paper
figure exactly.

## Reproducing with fresh runs

All four runs need profiling enabled (`--profile`), which keeps
`GC_PROFILE_TIMESERIAL` on and `GC_LIGHT` off so the parsed
`OSDI26_analysis` / `OSDI26_analysis_copy` / `pipe_OSDI26` printks exist.

1. f2fs / pgc / gaze — module-only modes, one profile kernel:
   `kernel/switch_mode.sh gaze --profile && kernel/build_kernel.sh`, reboot
   into `6.1.0.sosp26_ae_profile`, `kernel/build_all_profile_modules.sh`,
   then for each mode: `experiment_script/select_fs.sh` + FIO run.
2. ZNS+ — needs its own kernel image:
   `kernel/switch_mode.sh zns_plus --profile && kernel/build_kernel.sh`,
   reboot into `6.1.0.sosp26_ae_copy_profile`, then the FIO run.
3. Replace the run dir under the matching subdirectory here (keep exactly
   one per filesystem) and `./make_graph.sh`.

Notes
- The parse helpers average GCs at the paper's victim utilization
  (`grep "util: 40"` in `parse_*_lat.sh`); runs whose GC utilization
  differs produce empty data.
- The bar label (`"zF2FS"` etc.) is auto-derived from the run path;
  override with `GC_FS_NAME=... ` when invoking the get script by hand.
