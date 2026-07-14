# AE_results — every evaluator-facing result lives here

All figures an artifact evaluator looks at are generated into (and managed
from) this directory. Figure files are named after the **paper's** figure
numbers (`Fig-17.FIO_throughput.png` = paper Fig. 17). `Supp-*` figures
back latency claims made in the paper's text but have no numbered figure
in the paper.

Both figure families always resolve each series/bar to the **newest
successfully-completed run on disk**, falling back to the paper's recorded
baseline for anything you haven't (re)run — so with zero fresh runs they
reproduce the paper's figures, and they refresh series-by-series as you
rerun things.

## Layout

| Path | What it is |
|---|---|
| `merged/fio`, `merged/macro` | **Generated** benchmark figures: your freshest valid run per filesystem, overlaid on the paper baselines for everything else (paper Fig 17, 18 + Supp latency) |
| `solo/fio`, `solo/macro` | Same, but **only** from what you ran — nothing paper-provided is mixed in |
| `view/<fs>/` | Scratch output of `view_results.sh` single-filesystem views |
| `profile/` | **Generated** profile figures (paper Fig 5a/b/c, 14, 16 + legend); `SOURCES.txt` records which run each bar came from |
| `profile/paper_baseline/` | The same profile figures forced to the paper's archived runs only (regenerate: `./collect_profile_figures.sh --paper`) |
| `fio_baseline/`, `macro_baseline/`, `macro_keys/` | **Paper reference data** (SOSP26 submission runs) + the gnuplot templates for the benchmark figures |
| `ablation_baseline/` | **Paper reference data + pipeline** for the ablation figure (paper Fig 15): per-mode KIOPS samples (`parsed/`), gnuplot templates, `make_graph.sh` (newest FIO run per mode, paper fallback), generated `Fig-15.*` + `SOURCES.txt`. Produce fresh bars with `../run_ablation.sh` |
| `profile_baseline/` | **Paper reference data + pipelines** for the profile figures: `ablation_gc_lat_SOSP` (Fig 14), `sota_gc_lat_SOSP` (Fig 16), `vanilla_gc_latency_breakdown_accurate` (Fig 5), `pipe_gc_lat_breakdown_SOSP` (Fig 11), `mem_util_SOSP` (Fig 6/13) — each holds its archived paper run(s), parser, plot template, and `make_graph.sh` |
| `build_graphs.sh` | Builds `merged/` + `solo/` from whatever runs are on disk (called automatically at the end of `run_experiment.sh`) |
| `../run_ablation.sh` | Runs the Fig 15 ablation sweep (FIO across the 8 technique modes; ~21 min each) and rebuilds `ablation_baseline/Fig-15.*` |
| `collect_profile_figures.sh` | Rebuilds Fig 5/14/16 (newest run per mode, paper archive fallback) and collects them into `profile/` (called automatically at the end of profile-mode `FIO/run_fio.sh`) |
| `timing_history.tsv` | Wall-clock log of past benchmark runs (feeds the ETA heartbeat) |

## How "old data" vs "new data" is kept apart

- Paper (SOSP26 submission) data is **read-only input**, staged under
  `*_baseline/` directories. Nothing ever writes into them during a run.
- Fresh runs land under each benchmark's own directory
  (`experiment_script/FIO/<mode>_data/...` etc.); the figure builders read
  them from there and write **only** into `merged/`, `solo/`, `view/`,
  and `profile/`.
- Every generated figure directory records its provenance in a
  `SOURCES.txt` (one line per series/bar: `[NEW]` = generated here,
  `[OLD]` = SOSP26 paper data), and `view_results.sh` prints it under
  each figure.

## Paper-figure name map (what this artifact can generate)

| File name | Paper figure |
|---|---|
| `Fig-05a.GC_latency_breakdown.Overall_GC` | Fig. 5(a) |
| `Fig-05b.GC_latency_breakdown.GC_Read_Phase` | Fig. 5(b) |
| `Fig-05c.GC_latency_breakdown.GC_Write_Phase` | Fig. 5(c) |
| `Fig-06.Memory_util_and_FIO_throughput.F2FS` | Fig. 6 (in `profile_baseline/mem_util_SOSP/`) |
| `Fig-11.Serial_vs_pipelined_GC_latency_breakdown` | Fig. 11 (in `profile_baseline/pipe_gc_lat_breakdown_SOSP/`) |
| `Fig-13.Memory_util_and_FIO_throughput.GAZE` | Fig. 13 (in `profile_baseline/mem_util_SOSP/`) |
| `Fig-14.GC_latency_under_individual_techniques` | Fig. 14 |
| `Fig-15.FIO_throughput_under_individual_techniques.{legend,avg,stddev}` | Fig. 15 (in `ablation_baseline/`) |
| `Fig-16.GC_latency_breakdown_across_filesystems` (+ `Fig-16-legend.*`) | Fig. 16 |
| `Fig-17.FIO_throughput` | Fig. 17 |
| `Fig-18.Macrobenchmark_throughput` | Fig. 18 |
| `Supp-a.FIO_avg_latency`, `Supp-b.FIO_99.9999_latency` | latency claims in §8 text (no numbered figure) |
| `Supp-c.Macrobenchmark_avg_latency`, `Supp-d.Macrobenchmark_tail_latency` | latency claims in §8 text (no numbered figure) |

Figs 6/11/13 are not part of the standard evaluation flow (README §3.5
covers Fig 5/14/16); their pipelines are kept working under
`profile_baseline/` for completeness.

## Viewing

`../view_results.sh` (or the prompt at the end of `run_experiment.sh`)
rebuilds and lists everything. The default benchmark view is the
**merged** figure — filesystems you haven't (re)run are filled in from the
paper data, so the figure is complete and always reflects your most recent
successful runs. Every view also prints (and saves to `SOURCES.txt`) the
per-series provenance: `[NEW]` = generated on this machine, `[OLD]` =
SOSP26 paper data. Fig 5's `Conv.` bar likewise falls back to the paper's
archived row (no conventional-SSD profile flow exists in this tree;
`CONV_RUN=<run dir>` overrides). `Supp-*` latency figures are generated
but not auto-opened.

No X display? Either open the iDRAC virtual console (its address is in the
server access doc; open the Virtual Console, log into the desktop) or
`scp` the `.png` files to your machine.
