#!/usr/bin/env python3
"""plot-v3-memory.py — chart v3's in-process memory-over-time signals.

Fills the gap called out in OBSERVABILITY_AUDIT_2026-06-03.md: the two
richest in-process time-series had no plotter.

Two sources, either or both:

  --live-periodic <csv>   the NIX_V3_LIVE_TRACE_PERIODIC=<K> CSV
                          (per-Tag arena LIVE bytes + L(t), sampled every
                          K MB of arena allocation).  This is the only
                          decomposed "where is v3's memory going over the
                          run" signal.  x-axis defaults to arena MB
                          allocated (the sampler's native axis); --x wall
                          switches to wall_ms (note: safepoint-biased, so
                          sparse on deep evals — see the audit).

  --heap-trace <logfile>  a capture of stderr from a run with
                          NIX_V3_HEAP_TRACE=1 (the unified sampler:
                          t_us / heap / free / total / rss / cpu_ms).
                          Plots resident RSS + Boehm heap + CPU%
                          (CPU% = Δcpu_ms/Δwall between samples).

Produces one SVG (matplotlib Agg).  Mirrors perf-trace.py conventions:
graceful skip if matplotlib is unavailable; psutil is NOT required (this
script reads files, it does not sample a live process).

Usage:
    # produce /tmp/v3-live-periodic-<pid>.csv first:
    NIX_V3_LIVE_TRACE_PERIODIC=32 v3-eval --expr '...' ...
    ./plot-v3-memory.py --live-periodic /tmp/v3-live-periodic-12345.csv \\
                        --out samples/hne-livemem

    # or capture the unified heap-trace stream:
    NIX_V3_HEAP_TRACE=1 v3-eval --expr '...' 2> hne.heaptrace.log
    ./plot-v3-memory.py --heap-trace hne.heaptrace.log --out samples/hne-rss

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group.  SPDX-License-Identifier: Apache-2.0
"""

import argparse
import csv
import re
import sys
from pathlib import Path


# Same prefix the C++ sampler (heap_trace.cc) emits.  rss/cpu_ms are
# optional so this also reads pre-2026-06-03 logs (4-field lines).
HEAP_TRACE_RE = re.compile(
    r"v3 heap-trace t_us=(\d+) heap=(\d+) free=(\d+) total=(\d+)"
    r"(?: rss=(\d+) cpu_ms=(-?\d+))?")

# Per-Tag live-byte columns in the LIVE_TRACE_PERIODIC CSV, stacked
# largest-typical-first (bindings dominate the arena ~84%).
TAG_COLS = [
    ("live_bindings_mb", "Bindings"),
    ("live_thunks_mb",   "Thunks"),
    ("live_closures_mb", "Closures"),
    ("live_lists_mb",    "Lists"),
    ("live_pairs_mb",    "Pairs"),
]


def parse_args():
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--live-periodic", metavar="CSV",
                   help="NIX_V3_LIVE_TRACE_PERIODIC CSV path")
    p.add_argument("--heap-trace", metavar="LOG",
                   help="captured stderr with NIX_V3_HEAP_TRACE lines")
    p.add_argument("--x", choices=["alloc", "wall"], default="alloc",
                   help="x-axis for the live-periodic panel "
                        "(alloc MB [default] or wall ms)")
    p.add_argument("--out", default="v3-memory",
                   help="output prefix; writes <prefix>.svg")
    p.add_argument("--title", default=None,
                   help="figure title (default: derived from --out)")
    return p.parse_args()


def read_live_periodic(path):
    """Return list of row dicts (floats) from the periodic CSV.

    Returns [] if the file is absent (a deep eval that never crossed the
    sampler's K-MB threshold writes no CSV) — the caller then renders the
    heap-trace panels alone rather than failing."""
    if not Path(path).is_file():
        print(f"plot-v3-memory.py: no live-periodic CSV at {path} "
              f"(0 samples crossed the threshold); skipping live panels",
              file=sys.stderr)
        return []
    rows = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            try:
                rows.append({k: float(v) for k, v in r.items()})
            except (ValueError, TypeError):
                # Skip malformed / partial rows (e.g. a truncated final
                # line from a killed process).
                continue
    return rows


def read_heap_trace(path):
    """Return list of dicts: t_ms, heap_mb, rss_mb, cpu_ms (cpu_ms/rss
    may be None for legacy 4-field logs)."""
    out = []
    with open(path, errors="replace") as f:
        for line in f:
            m = HEAP_TRACE_RE.search(line)
            if not m:
                continue
            t_us, heap, _free, _total, rss, cpu = m.groups()
            out.append({
                "t_ms": int(t_us) / 1000.0,
                "heap_mb": int(heap) / 1e6,
                "rss_mb": (int(rss) / 1e6) if rss is not None else None,
                "cpu_ms": (int(cpu)) if cpu is not None else None,
            })
    return out


def cpu_percent_series(events):
    """Differentiate cumulative cpu_ms over wall to a CPU% series.
    Returns (t_ms_list, pct_list); empty if no cpu data."""
    ts, pcts = [], []
    prev = None
    for e in events:
        if e["cpu_ms"] is None:
            continue
        if prev is not None:
            dt = e["t_ms"] - prev["t_ms"]
            dcpu = e["cpu_ms"] - prev["cpu_ms"]
            if dt > 0:
                ts.append(e["t_ms"])
                pcts.append(100.0 * dcpu / dt)
        prev = e
    return ts, pcts


def main():
    args = parse_args()
    if not args.live_periodic and not args.heap_trace:
        print("plot-v3-memory.py: need --live-periodic and/or --heap-trace",
              file=sys.stderr)
        sys.exit(2)

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("plot-v3-memory.py: matplotlib unavailable; install via "
              "nix-shell -p python3Packages.matplotlib", file=sys.stderr)
        sys.exit(2)

    # Read the live CSV up front so panel layout reflects actual content:
    # a deep eval that never crossed the sampler threshold yields 0 rows,
    # and we then omit the (empty) live panels rather than drawing blanks.
    live_rows = read_live_periodic(args.live_periodic) if args.live_periodic else []

    # Decide panel count: live-periodic = 3 panels (dead-band + per-Tag
    # stack + L); heap-trace = 2 panels (RSS/heap + CPU%).
    panels = []
    if live_rows:
        panels += ["live_deadband", "live_stack", "live_L"]
    if args.heap_trace:
        panels += ["ht_mem", "ht_cpu"]
    if not panels:
        print("plot-v3-memory.py: nothing to plot (no live rows, no "
              "heap-trace lines)", file=sys.stderr)
        sys.exit(2)

    fig, axes = plt.subplots(len(panels), 1,
                             figsize=(10, 3 * len(panels)), sharex=False)
    if len(panels) == 1:
        axes = [axes]
    ax_by_name = dict(zip(panels, axes))

    # ---- live-periodic panels ------------------------------------------
    if live_rows:
        rows = live_rows
        if True:
            xkey = "wall_ms" if args.x == "wall" else "alloc_offset_mb"
            xlabel = ("wall (ms, safepoint-biased)" if args.x == "wall"
                      else "arena allocated (MB)")
            xs = [r[xkey] for r in rows]

            # ---- dead-band panel: arena mapped vs live ----------------
            # The single most useful memory picture: the gap between what
            # the arena has MAPPED (resident_mb) and what is actually LIVE
            # (live_mb) is DEAD-but-resident bytes the non-moving collector
            # cannot reclaim mid-eval.  A widening band over the run is the
            # firefox "224 MB scattered dead" finding, made visible — and
            # the direct input to the generational-GC decision.
            ax = ax_by_name["live_deadband"]
            resident = [r["resident_mb"] for r in rows]
            live = [r["live_mb"] for r in rows]
            dead = [max(0.0, rr - ll) for rr, ll in zip(resident, live)]
            ax.plot(xs, resident, color="tab:red", lw=1.4,
                    label="arena mapped (resident)")
            ax.plot(xs, live, color="tab:green", lw=1.4, label="live")
            ax.fill_between(xs, live, resident, color="tab:red", alpha=0.18,
                            label="dead (stranded, unreclaimed)")
            peak_dead = max(dead) if dead else 0.0
            end_dead = dead[-1] if dead else 0.0
            ax.set_ylabel("arena (MB)")
            ax.set_xlabel(xlabel)
            ax.legend(loc="upper left", fontsize=8)
            ax.grid(True, alpha=0.3)
            ax.set_title(
                "dead-band: arena mapped − live = unreclaimed dead "
                f"(peak {peak_dead:.0f} MB, end {end_dead:.0f} MB)",
                fontsize=10)

            ax = ax_by_name["live_stack"]
            present = [(k, lab) for (k, lab) in TAG_COLS
                       if any(r.get(k, 0.0) for r in rows)]
            if present:
                ys = [[r.get(k, 0.0) for r in rows] for (k, _) in present]
                ax.stackplot(xs, *ys, labels=[lab for (_, lab) in present],
                             alpha=0.85)
            ax.plot(xs, [r["live_mb"] for r in rows], color="black",
                    lw=1.2, label="live total")
            ax.set_ylabel("live arena (MB)")
            ax.set_xlabel(xlabel)
            ax.legend(loc="upper left", fontsize=8, ncol=3)
            ax.grid(True, alpha=0.3)
            ax.set_title("v3 live arena bytes by Tag (precise-root walk)",
                         fontsize=10)

            ax = ax_by_name["live_L"]
            ax.plot(xs, [r["L_resident"] for r in rows],
                    label="L_resident (live/arena)")
            ax.plot(xs, [r["L_cumulative"] for r in rows],
                    label="L_cumulative (live/alloc'd)")
            ax.set_ylabel("live fraction L")
            ax.set_xlabel(xlabel)
            ax.set_ylim(0, 1)
            ax.legend(loc="upper right", fontsize=8)
            ax.grid(True, alpha=0.3)
            n = len(rows)
            ax.set_title(f"L(t) over the run ({n} samples"
                         + ("; sparse — see audit)" if n < 5 else ")"),
                         fontsize=10)

    # ---- heap-trace panels ---------------------------------------------
    if args.heap_trace:
        evts = read_heap_trace(args.heap_trace)
        if not evts:
            print(f"plot-v3-memory.py: no heap-trace lines in "
                  f"{args.heap_trace}", file=sys.stderr)
        else:
            t = [e["t_ms"] for e in evts]
            ax = ax_by_name["ht_mem"]
            if any(e["rss_mb"] is not None for e in evts):
                ax.plot(t, [e["rss_mb"] for e in evts],
                        label="resident RSS", color="tab:red")
            ax.plot(t, [e["heap_mb"] for e in evts],
                    label="Boehm heap", color="tab:blue", alpha=0.7)
            ax.set_ylabel("MB")
            ax.set_xlabel("wall (ms)")
            ax.legend(loc="upper left", fontsize=8)
            ax.grid(True, alpha=0.3)
            ax.set_title("process resident RSS vs Boehm heap "
                         "(RSS is the honest signal)", fontsize=10)

            ax = ax_by_name["ht_cpu"]
            cts, cpct = cpu_percent_series(evts)
            if cts:
                ax.plot(cts, cpct, color="tab:green", label="CPU%")
                ax.set_ylabel("CPU %")
            else:
                ax.text(0.5, 0.5, "no cpu_ms field (legacy heap-trace log)",
                        ha="center", va="center", transform=ax.transAxes,
                        fontsize=9, color="gray")
            ax.set_xlabel("wall (ms)")
            ax.legend(loc="upper right", fontsize=8)
            ax.grid(True, alpha=0.3)
            ax.set_title("CPU% (Δcpu_ms/Δwall)", fontsize=10)

    title = args.title or f"v3 memory-over-time — {Path(args.out).name}"
    fig.suptitle(title, fontsize=12)
    fig.tight_layout(rect=(0, 0, 1, 0.98))
    svg = Path(f"{args.out}.svg")
    svg.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(svg, format="svg")
    plt.close(fig)
    print(f"plot-v3-memory: wrote {svg}", file=sys.stderr)


if __name__ == "__main__":
    main()
