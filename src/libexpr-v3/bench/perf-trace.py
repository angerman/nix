#!/usr/bin/env python3
"""perf-trace.py — time-series sampler + plotter for v3 vs TW.

See `src/libexpr-v3/lode/PERF_TRACE_TOOL_DESIGN_2026-05-20.md` for
the full design.  TL;DR: spawn the evaluator child, sample
CPU%/RSS/threads via psutil + read in-process Boehm heap from
stderr (`NIX_V3_HEAP_TRACE=1` emits "v3 heap-trace ..." lines),
write JSONL traces, render an SVG overlay.

Usage:
    perf-trace.py \\
        --workload "(builtins.getFlake \\"/path/to/cardano-node\\") ? outputs" \\
        --mode tw,v3-bridge,v3-native \\
        --runs 5 \\
        --interval-ms 50 \\
        --out samples/2026-05-20/cardano-node

Outputs:
    samples/2026-05-20/cardano-node-tw.jsonl
    samples/2026-05-20/cardano-node-v3-bridge.jsonl
    samples/2026-05-20/cardano-node-v3-native.jsonl
    samples/2026-05-20/cardano-node.svg
    samples/2026-05-20/cardano-node-summary.md

Modes:
    tw          — pure tree-walker (no v3 env vars)
    v3-bridge   — v3-direct with bridge callFlake (NIX_V3_NO_NATIVE_CALL_FLAKE=1)
    v3-native   — v3-direct with v3-native callFlake (default since #700)

Rule 0 (PERF_TRACE_TOOL_DESIGN §"Rule 0 falsification anchor"):
    The first measurement on hello.drvPath kills or confirms
    "v3-direct's force-rate gap on hello.drvPath is dominated by
    Boehm GC scan time".

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group.  SPDX-License-Identifier: Apache-2.0
"""

import argparse
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import threading
import time
from collections import deque
from pathlib import Path

try:
    import psutil
except ImportError:
    print("perf-trace.py: psutil not available.  Install via nix-shell -p python3Packages.psutil",
          file=sys.stderr)
    sys.exit(2)


HEAP_TRACE_RE = re.compile(
    r"v3 heap-trace t_us=(\d+) heap=(\d+) free=(\d+) total=(\d+)")


def parse_args():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--workload", required=True,
                   help="Nix expression to evaluate (--impure --expr ...)")
    p.add_argument("--mode", default="tw,v3-native",
                   help="comma-separated modes: tw, v3-bridge, v3-native (default: tw,v3-native)")
    p.add_argument("--runs", type=int, default=5,
                   help="runs per mode (default 5)")
    p.add_argument("--interval-ms", type=int, default=50,
                   help="sampler cadence in ms (default 50)")
    p.add_argument("--nix", default="./build/src/nix/nix",
                   help="path to nix binary (default ./build/src/nix/nix)")
    p.add_argument("--out", default="samples/perf-trace",
                   help="output prefix (e.g. samples/2026-05-20/cardano-node)")
    p.add_argument("--cold", action="store_true",
                   help="clear v3 disk cache between runs (default: warm)")
    p.add_argument("--no-plot", action="store_true",
                   help="skip SVG rendering (JSONL only)")
    p.add_argument("--wall-time", type=int, default=180,
                   help="NIX_V3_MAX_WALL_TIME seconds (default 180)")
    return p.parse_args()


def mode_env(mode):
    """Return (env-additions, label) for a mode."""
    if mode == "tw":
        return {}, "TW alone"
    if mode == "v3-bridge":
        return {
            "NIX_V3_DIRECT_EVAL": "1",
            "NIX_V3_SKIP_INSTALLABLE_PREEVAL": "1",
            "NIX_V3_NO_NATIVE_CALL_FLAKE": "1",
        }, "v3-direct + bridge"
    if mode == "v3-native":
        return {
            "NIX_V3_DIRECT_EVAL": "1",
            "NIX_V3_SKIP_INSTALLABLE_PREEVAL": "1",
        }, "v3-direct (v3-native default)"
    raise ValueError(f"unknown mode: {mode}")


def sampler_loop(proc_pid, interval_s, samples, stop_evt):
    """Sampler thread — psutil polling of the process tree.

    Per design doc:
      - process-tree aggregation (proc + recursive children).
      - First cpu_percent() is delta-based; we seed once at thread
        entry and discard sample 0.
      - Wall clock is monotonic_ns to share an axis with stderr line
        tags.
    """
    try:
        proc = psutil.Process(proc_pid)
    except psutil.NoSuchProcess:
        return

    # Warm-up: psutil cpu_percent is delta-based; first call returns 0.0.
    try:
        proc.cpu_percent(interval=None)
    except psutil.NoSuchProcess:
        return

    t0_ns = time.monotonic_ns()
    while not stop_evt.is_set():
        try:
            # Aggregate over process tree.  Per design §"Sampling semantics".
            procs = [proc] + proc.children(recursive=True)
            rss = 0
            vms = 0
            cpu_pct = 0.0
            threads = 0
            for p in procs:
                try:
                    mi = p.memory_info()
                    rss += mi.rss
                    vms += mi.vms
                    cpu_pct += p.cpu_percent(interval=None)
                    threads += p.num_threads()
                except (psutil.NoSuchProcess, psutil.AccessDenied):
                    pass
            samples.append({
                "t_ns": time.monotonic_ns() - t0_ns,
                "rss_b": rss,
                "vms_b": vms,
                "cpu_pct": cpu_pct,
                "threads": threads,
            })
        except psutil.NoSuchProcess:
            break
        stop_evt.wait(interval_s)


def stderr_reader(stream, t0_ns, heap_events, stderr_buf, stop_evt):
    """Stderr reader thread — tags each line with wall-clock, extracts heap-trace lines."""
    for raw in stream:
        if stop_evt.is_set():
            break
        line = raw.decode("utf-8", errors="replace").rstrip()
        t_ns = time.monotonic_ns() - t0_ns
        stderr_buf.append({"t_ns": t_ns, "line": line})
        m = HEAP_TRACE_RE.search(line)
        if m:
            heap_events.append({
                "t_ns": t_ns,
                "kind": "heap",
                "heap_b": int(m.group(2)),
                "free_b": int(m.group(3)),
                "total_b": int(m.group(4)),
            })


def run_once(nix_bin, mode, workload, interval_ms, wall_time_s, cold):
    """One evaluator invocation; returns dict with samples + heap_events + wall_ms + exit."""
    env = os.environ.copy()
    add, label = mode_env(mode)
    env.update(add)
    env["NIX_V3_MAX_WALL_TIME"] = f"{wall_time_s}s"
    env["NIX_V3_HEAP_TRACE"] = "1"
    env["NIX_V3_HEAP_TRACE_INTERVAL_MS"] = str(interval_ms)

    if cold:
        # Clear v3 disk cache between runs.  TW doesn't use this
        # cache; clearing is a no-op for TW.
        cache = Path.home() / ".cache" / "nix" / "v3-cache.sqlite"
        for path in [cache, cache.with_suffix(".sqlite-shm"),
                     cache.with_suffix(".sqlite-wal")]:
            if path.exists():
                path.unlink()

    samples = deque()
    heap_events = deque()
    stderr_buf = deque()
    stop_evt = threading.Event()
    t_start_ns = time.monotonic_ns()
    proc = subprocess.Popen(
        [nix_bin, "eval", "--impure", "--expr", workload],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )

    samp_thr = threading.Thread(
        target=sampler_loop,
        args=(proc.pid, interval_ms / 1000.0, samples, stop_evt),
        daemon=True)
    err_thr = threading.Thread(
        target=stderr_reader,
        args=(proc.stderr, t_start_ns, heap_events, stderr_buf, stop_evt),
        daemon=True)
    samp_thr.start()
    err_thr.start()

    stdout, _ = proc.communicate(timeout=wall_time_s + 30)
    wall_ns = time.monotonic_ns() - t_start_ns
    stop_evt.set()
    samp_thr.join(timeout=1.0)
    err_thr.join(timeout=1.0)

    return {
        "mode": mode,
        "label": label,
        "wall_ms": wall_ns / 1e6,
        "exit": proc.returncode,
        "stdout": stdout.decode("utf-8", errors="replace").strip(),
        "samples": list(samples),
        "heap_events": list(heap_events),
        "stderr_buf": list(stderr_buf),
    }


def write_jsonl(path, run):
    """One JSONL file per (mode, run) — first line is metadata."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w") as f:
        f.write(json.dumps({
            "kind": "meta",
            "mode": run["mode"],
            "label": run["label"],
            "wall_ms": run["wall_ms"],
            "exit": run["exit"],
            "stdout": run["stdout"],
        }) + "\n")
        for s in run["samples"]:
            s2 = dict(s)
            s2["kind"] = "sample"
            f.write(json.dumps(s2) + "\n")
        for h in run["heap_events"]:
            f.write(json.dumps(h) + "\n")


def render_svg(out_prefix, runs_by_mode, interval_ms):
    """Render an SVG overlay across modes.  Matplotlib SVG backend."""
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("perf-trace.py: matplotlib unavailable; skipping SVG", file=sys.stderr)
        return

    fig, axes = plt.subplots(3, 1, figsize=(10, 9), sharex=True)
    ax_cpu, ax_rss, ax_heap = axes

    for mode, runs in runs_by_mode.items():
        # Median + IQR ribbon over resampled grid.
        max_t_ms = max(s["t_ns"] / 1e6
                       for run in runs for s in run["samples"]) if any(
                           r["samples"] for r in runs) else 0
        if max_t_ms <= 0:
            continue
        grid_ms = list(range(0, int(max_t_ms) + interval_ms, interval_ms))

        def resample(run, key):
            if not run["samples"]:
                return [0.0] * len(grid_ms)
            ts = [s["t_ns"] / 1e6 for s in run["samples"]]
            vs = [s[key] for s in run["samples"]]
            out = []
            for g in grid_ms:
                # Linear interp; clamp to ends.
                if g <= ts[0]: out.append(vs[0])
                elif g >= ts[-1]: out.append(vs[-1])
                else:
                    i = 0
                    while i < len(ts) - 1 and ts[i + 1] < g:
                        i += 1
                    frac = (g - ts[i]) / max(1e-9, (ts[i + 1] - ts[i]))
                    out.append(vs[i] + frac * (vs[i + 1] - vs[i]))
            return out

        def heap_resample(run):
            evts = run["heap_events"]
            if not evts: return None
            ts = [e["t_ns"] / 1e6 for e in evts]
            vs = [e["heap_b"] / 1e6 for e in evts]
            out = []
            for g in grid_ms:
                if g <= ts[0]: out.append(vs[0])
                elif g >= ts[-1]: out.append(vs[-1])
                else:
                    i = 0
                    while i < len(ts) - 1 and ts[i + 1] < g:
                        i += 1
                    frac = (g - ts[i]) / max(1e-9, (ts[i + 1] - ts[i]))
                    out.append(vs[i] + frac * (vs[i + 1] - vs[i]))
            return out

        cpu_series = [resample(r, "cpu_pct") for r in runs]
        rss_series = [[v / 1e6 for v in resample(r, "rss_b")] for r in runs]
        heap_series = [s for s in (heap_resample(r) for r in runs) if s is not None]

        def median_iqr(series):
            n_pts = len(series[0])
            med = [statistics.median(s[i] for s in series) for i in range(n_pts)]
            lo = [statistics.quantiles([s[i] for s in series], n=4)[0]
                  if len(series) >= 4 else min(s[i] for s in series)
                  for i in range(n_pts)]
            hi = [statistics.quantiles([s[i] for s in series], n=4)[-1]
                  if len(series) >= 4 else max(s[i] for s in series)
                  for i in range(n_pts)]
            return med, lo, hi

        label = runs[0]["label"]
        cpu_m, cpu_lo, cpu_hi = median_iqr(cpu_series)
        rss_m, rss_lo, rss_hi = median_iqr(rss_series)
        ax_cpu.plot(grid_ms, cpu_m, label=label)
        ax_cpu.fill_between(grid_ms, cpu_lo, cpu_hi, alpha=0.2)
        ax_rss.plot(grid_ms, rss_m, label=label)
        ax_rss.fill_between(grid_ms, rss_lo, rss_hi, alpha=0.2)

        if heap_series:
            heap_m, heap_lo, heap_hi = median_iqr(heap_series)
            ax_heap.plot(grid_ms, heap_m, label=label)
            ax_heap.fill_between(grid_ms, heap_lo, heap_hi, alpha=0.2)

    ax_cpu.set_ylabel("CPU%")
    ax_cpu.legend(loc="upper right")
    ax_cpu.grid(True, alpha=0.3)
    ax_rss.set_ylabel("RSS (MB, process tree)")
    ax_rss.legend(loc="upper right")
    ax_rss.grid(True, alpha=0.3)
    ax_heap.set_ylabel("Boehm heap (MB, v3 only)")
    ax_heap.set_xlabel("time (ms)")
    ax_heap.legend(loc="upper right")
    ax_heap.grid(True, alpha=0.3)
    fig.suptitle(f"perf-trace — {Path(out_prefix).name}", fontsize=12)
    fig.tight_layout()
    svg_path = Path(f"{out_prefix}.svg")
    svg_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(svg_path, format="svg")
    plt.close(fig)
    print(f"perf-trace: wrote {svg_path}", file=sys.stderr)


def write_summary(out_prefix, runs_by_mode):
    """Markdown summary with wall-time p50/p95 per mode."""
    out = Path(f"{out_prefix}-summary.md")
    with out.open("w") as f:
        f.write(f"# perf-trace summary — {Path(out_prefix).name}\n\n")
        f.write(f"| Mode | n | min ms | p50 ms | p95 ms | max ms | exit-OK |\n")
        f.write(f"|---|---|---|---|---|---|---|\n")
        for mode, runs in runs_by_mode.items():
            walls = [r["wall_ms"] for r in runs]
            ok = sum(1 for r in runs if r["exit"] == 0)
            label = runs[0]["label"]
            n = len(walls)
            if n == 0:
                continue
            walls.sort()
            mn = walls[0]
            mx = walls[-1]
            p50 = statistics.median(walls)
            # p95 — index ceil(0.95 * n) - 1
            p95 = walls[min(n - 1, max(0, int(0.95 * n) - 1 if n >= 20 else n - 1))]
            f.write(f"| {label} | {n} | {mn:.0f} | {p50:.0f} | {p95:.0f} | {mx:.0f} | {ok}/{n} |\n")
        f.write(f"\nWorkload: see JSONL meta lines.\n")
    print(f"perf-trace: wrote {out}", file=sys.stderr)


def main():
    args = parse_args()
    modes = [m.strip() for m in args.mode.split(",")]
    out_prefix = args.out
    runs_by_mode = {}
    for mode in modes:
        runs_by_mode[mode] = []
        for i in range(args.runs):
            print(f"[run {i + 1}/{args.runs}] mode={mode}", file=sys.stderr, flush=True)
            r = run_once(args.nix, mode, args.workload, args.interval_ms,
                         args.wall_time, args.cold)
            runs_by_mode[mode].append(r)
            write_jsonl(Path(f"{out_prefix}-{mode}-run{i + 1}.jsonl"), r)
            print(f"    wall={r['wall_ms']:.0f}ms exit={r['exit']} "
                  f"samples={len(r['samples'])} heap_evts={len(r['heap_events'])}",
                  file=sys.stderr)

    write_summary(out_prefix, runs_by_mode)
    if not args.no_plot:
        render_svg(out_prefix, runs_by_mode, args.interval_ms)


if __name__ == "__main__":
    main()
