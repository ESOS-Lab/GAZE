#!/usr/bin/env python3
# Scans each benchmark's <FS_NAME>_data/*/result.txt (the layout written by
# Fileserver/run_fileserver.sh, YCSB-A/run_ycsba.sh, YCSB-F/run_ycsbf.sh and
# TPC-C/run_tpcc.sh) and turns the latest run per mode into TSVs gnuplot can
# read directly. Run this (or update_graph.sh) any time after an experiment
# to refresh the comparison graphs with whatever modes have been run so far.
import glob
import os
import re

EXP_DIR = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT_DIR = os.path.dirname(os.path.abspath(__file__))

BENCHMARKS = ["Fileserver", "YCSB-A", "YCSB-F", "TPC-C"]

# Run-directory name each config.sh-based run_*.sh script produces:
# OUTDIR="$SCRIPT_DIR/${FS_NAME}_data/<prefix>_$(date +%Y%m%d_%H%M)"
# Older pre-refactor scripts use different naming (e.g. ycsba_exp_output_fastgc_<ts>,
# filebench_OSDI_exp_output_...); this regex excludes those legacy result trees.
RUN_DIR_PREFIX = {
    "Fileserver": "fileserver",
    "YCSB-A": "ycsba",
    "YCSB-F": "ycsbf",
    "TPC-C": "tpcc",
}


def fnum(s):
    try:
        return float(s)
    except (TypeError, ValueError):
        return None


def discover_modes(bench_dir, prefix):
    run_re = re.compile(rf"^{re.escape(prefix)}_\d{{8}}_\d{{4}}$")
    modes = {}
    for data_dir in sorted(glob.glob(os.path.join(bench_dir, "*_data"))):
        mode = os.path.basename(data_dir)[: -len("_data")]
        results = [
            r
            for r in glob.glob(os.path.join(data_dir, "*", "result.txt"))
            if run_re.match(os.path.basename(os.path.dirname(r)))
        ]
        if not results:
            continue
        # newest first; main() falls back to older runs when the newest
        # result.txt is still being written by an in-progress experiment
        modes[mode] = sorted(results, key=os.path.getmtime, reverse=True)
    return modes


def parse_fileserver(text):
    throughput = avg_lat = tail_lat = None
    for line in text.splitlines():
        if "GC IO Summary" in line:
            f = line.split()
            v = fnum(f[6]) if len(f) > 6 else None
            throughput = v / 1000 if v is not None else None  # ops/s -> KOPS
        elif line.startswith("GC append"):
            f = line.replace("ms", " ").split()
            avg_lat = fnum(f[5]) if len(f) > 5 else None
            # histogram tokens carry spaces around commas, so positional
            # indexing is fragile; pull p99.99 (microseconds) by name
            m = re.search(r"p99\.99:\s*([\d.]+)", line)
            tail_lat = float(m.group(1)) / 1000 / 1000 if m else None
    return {"throughput": {"": throughput}, "avg_lat": {"": avg_lat}, "tail_lat": {"": tail_lat}}


def parse_ycsb(text):
    throughput = None
    avg = {"update": None, "read": None}
    tail = {"update": None, "read": None}
    for line in text.splitlines():
        f = [x.strip() for x in line.split(",")]
        if len(f) < 3:
            continue
        op, metric, value = f[0], f[1], f[2]
        v = fnum(value)
        if v is None:
            continue
        if metric == "Throughput(ops/sec)":
            throughput = v / 1000
        elif metric == "AverageLatency(us)":
            if op == "[UPDATE]":
                avg["update"] = v / 1000
            elif op == "[READ]":
                avg["read"] = v / 1000
        elif metric == "99.99PercentileLatency(us)":
            if op == "[UPDATE]":
                tail["update"] = v / 1000 / 1000
            elif op == "[READ]":
                tail["read"] = v / 1000 / 1000
    return {"throughput": {"": throughput}, "avg_lat": avg, "tail_lat": tail}


def parse_tpcc(text):
    throughput = avg_lat = tail_lat = None
    lines = text.splitlines()
    for line in lines:
        if "trx:" in line and "99.99%:" in line:
            f = line.replace(",", "").split()
            elapsed, trx = fnum(f[0]), fnum(f[2])
            if elapsed and trx:
                throughput = trx / elapsed
            tail_lat = fnum(f[12]) / 1000 if len(f) > 12 else None
            break
    for line in lines:
        if "Raw Results2" in line:
            break
        s = line.strip()
        if s.startswith("[1]"):
            f = s.split()
            avg_lat = fnum(f[6]) if len(f) > 6 else None
            break
    return {"throughput": {"": throughput}, "avg_lat": {"": avg_lat}, "tail_lat": {"": tail_lat}}


PARSERS = {
    "Fileserver": parse_fileserver,
    "YCSB-A": parse_ycsb,
    "YCSB-F": parse_ycsb,
    "TPC-C": parse_tpcc,
}


def main():
    per_bench_modes = {}
    all_modes = set()
    parsed = {}  # bench -> mode -> parsed dict

    for bench in BENCHMARKS:
        bench_dir = os.path.join(EXP_DIR, bench)
        modes = discover_modes(bench_dir, RUN_DIR_PREFIX[bench])
        per_bench_modes[bench] = modes
        all_modes.update(modes.keys())
        parsed[bench] = {}
        for mode, result_paths in modes.items():
            for result_path in result_paths:
                with open(result_path) as f:
                    text = f.read()
                d = PARSERS[bench](text)
                if d["throughput"].get("") is not None:
                    parsed[bench][mode] = d
                    break
            if mode not in parsed[bench]:
                print(f"[parse_results] {bench}/{mode}: no completed run yet, skipping")

    modes_sorted = sorted(all_modes)
    if not modes_sorted:
        print("[parse_results] No results found yet under */<mode>_data/*/result.txt.")
        print("[parse_results] Run one of the run_*.sh benchmarks first, then re-run this.")
        return

    def write_tsv(fname, metric, subkeys):
        path = os.path.join(OUT_DIR, fname)
        with open(path, "w") as out:
            out.write("Row\t" + "\t".join(modes_sorted) + "\n")
            for bench in BENCHMARKS:
                for sub in subkeys.get(bench, [""]):
                    label = bench if sub == "" else f"{bench} ({sub})"
                    row = [label]
                    for mode in modes_sorted:
                        d = parsed[bench].get(mode)
                        val = d[metric].get(sub) if d else None
                        row.append("" if val is None else f"{val:.4f}")
                    out.write("\t".join(row) + "\n")
        print(f"[parse_results] wrote {path}")

    subkeys = {
        "Fileserver": [""],
        "YCSB-A": ["update", "read"],
        "YCSB-F": ["update", "read"],
        "TPC-C": [""],
    }

    write_tsv("throughput.tsv", "throughput", {b: [""] for b in BENCHMARKS})
    write_tsv("avg_lat.tsv", "avg_lat", subkeys)
    write_tsv("tail_lat.tsv", "tail_lat", subkeys)

    print(f"[parse_results] modes found: {', '.join(modes_sorted)}")


if __name__ == "__main__":
    main()
