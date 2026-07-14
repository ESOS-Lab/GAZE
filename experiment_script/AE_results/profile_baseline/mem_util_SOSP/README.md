# Memory-Utilization Figures (paper: "Memory Utilization and FIO Throughput in F2FS" / "... in Selective Blocking")

Regenerates the paper's two dual-axis memory-utilization figures
(`mem_utilization_f2fs_rev.eps` = F2FS, `mem_utilization_gaze_rev.eps` =
GAZE/Selective Blocking in the paper LaTeX; the `_rev` files are renamed
copies of this pipeline's `mem_utilization_{f2fs,gaze}.eps`):

- red / y1: dirty-page ratio, `nr_dirty_pages * 4KB / RAM_GB * 100` (%)
- black / y2: FIO throughput (KIOPS, 20 sec moving average)
- blue dashed: dirty soft (10%) / hard (20%) limit watermarks
- green: GC-onset marker ("GC starts.")

## Data flow

```
kernel (APP_WRITE_PROFILE, mm/page-writeback.c):
  [ts] jw_balance_dirty_pages_ratelimited_flags: nr_jw_dirty(_pgs): N ...
        |                                    fio: 4k_iops.1.log -> kiops_sum
        v                                             |
  parse_dirty_pg.sh  ->  <fs>_parsed.dat              v
        \________________ make_graph.sh + plot_mem_util.gpi
                                   |
                    mem_utilization_{f2fs,gaze}.eps
```

## Reproduce from the committed reference data (no experiments needed)

```
./make_graph.sh
```

Inputs: dirty-page kernel logs `reference/dirty_page (paper logs; formerly graph/seungwon_support/gnuplot/dirty_page/
{f2fs,gaze}_log` + `reference/{f2fs,gaze}_kiops_sum` (both from the
2025-09-04 dirty_pg_monitor FIO runs). `parse_dirty_pg.sh` regenerates the
committed `{f2fs,gaze}_parsed.dat` in that directory byte-identically.

Note on fidelity: the F2FS output matches the paper figure closely. For the
selective-blocking (gaze/GAZE) figure the curve shapes and the story match,
but the paper eps (created 2025-12-08) was plotted from a later GAZE run whose
dirty-page log did not survive; the surviving 2025-09-04 reference run settles
at a slightly lower dirty ratio (~15% vs ~17.5%) and throughput. A fresh run
(below) reflects the current code.

## Reproduce from fresh runs

Both figures need runs on a kernel with `APP_WRITE_PROFILE`, which emits
the dirty-page samples (one line / 50 ms; the reference runs sampled every
10 ms — the curves are insensitive to this).

> NOTE: these figures are no longer part of the AE profile flow (README
> §3.5 covers paper Fig 5/14/16 only), and `switch_mode.sh --profile` no
> longer enables `APP_WRITE_PROFILE` (its samples were ~97% of profile-run
> dmesg). To reproduce them, first uncomment `#define APP_WRITE_PROFILE`
> in `kernel/include/linux/jw_trace.h` and `kernel/fs/f2fs/f2fs.h`, then:

```
kernel/switch_mode.sh gaze --profile && kernel/build_kernel.sh   # + reboot
kernel/build_all_profile_modules.sh
experiment_script/select_fs.sh        # 3 = f2fs (profile)
experiment_script/FIO/run_fio.sh      # -> FIO/f2fs_profile_data/fio_<ts>/
experiment_script/select_fs.sh        # 1 = gaze (profile)
experiment_script/FIO/run_fio.sh      # -> FIO/gaze_profile_data/fio_<ts>/

./make_graph.sh --f2fs ../../../FIO/f2fs_profile_data/fio_<ts> \
                --gaze ../../../FIO/gaze_profile_data/fio_<ts>
```

Each run dir must contain `kiops_sum` and a kernel log (`raw_kern_log`
preferred, else `dmesg`). Caveat: a 1200 s profile run emits ~24k dirty-page
lines (~2.5 MB); the dmesg ring buffer (4 MB on this kernel) can wrap when
other profiler output is heavy, truncating the left edge of the figure. If
that happens, save `/var/log/kern.log` as `raw_kern_log` in the run dir.

Knobs: `RAM_GB` (default 64, the paper machine's memory — the y1 percent
denominator), `SOFT_PCT`/`HARD_PCT` (default 10/20).
