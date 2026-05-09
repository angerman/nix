#!/usr/bin/env python3
"""v3 vs TW benchmark driver.

Runs each workload (defined in workloads.toml) under multiple
evaluator modes (TW, v3-direct, v3-hook, v3-hook-fhook).  Captures
wall time, max RSS, V3_TIMING phase split, and NIX_VM_STATS dispatch
counters.

Output formats:
  table    — human-readable summary (default)
  json     — machine-readable, suitable for storing as a baseline
  markdown — pasteable into a report
  csv      — one row per individual run, for downstream analysis

Comparison:
  --baseline FILE  — diff against a prior JSON snapshot; flag rows
                     where v3.run regresses by more than --threshold
                     (default 5%) and where TW changes by more than
                     --threshold (which usually indicates noise).

Workload selection:
  --only NAMES     — comma-separated workload names
  --tag TAG        — include workloads with the matching tag
  --skip-tag TAG   — exclude workloads matching the tag

Modes:
  --modes MODES    — comma-separated modes; default depends on whether
                     nixpkgs is available (skips real-world workloads
                     under v3-direct since v3-direct doesn't yet wire
                     all primops cleanly).

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
  Input Output Group.
SPDX-License-Identifier: Apache-2.0
"""

import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass, asdict, field
from pathlib import Path
from typing import Optional

try:
    import tomllib  # Python 3.11+
except ImportError:
    print("ERROR: bench.py requires Python 3.11+ (for tomllib).", file=sys.stderr)
    sys.exit(2)


# ---------------------------------------------------------------------------
# Configuration / constants.
# ---------------------------------------------------------------------------

ROOT = Path(__file__).resolve().parent.parent.parent.parent  # repo root
DEFAULT_NIX = ROOT / "build" / "src" / "nix" / "nix"
DEFAULT_WORKLOADS = Path(__file__).parent / "workloads.toml"

# Mode definitions: env vars to set when invoking nix.
MODES = {
    "tw": {},  # baseline tree-walker
    "v3-direct": {"NIX_V3_DIRECT_EVAL": "1"},
    "v3-hook": {"NIX_USE_V3": "1"},
    "v3-hook-fhook": {"NIX_USE_V3": "1", "NIX_USE_V3_FORCE": "1"},
}


# ---------------------------------------------------------------------------
# Result types.
# ---------------------------------------------------------------------------

@dataclass
class Run:
    """One execution of a workload+mode."""
    wall: float        # wall time in seconds
    user: float        # user CPU in seconds
    sys: float         # sys CPU in seconds
    rss_max_b: int     # max resident set size in bytes (macOS-style)
    rc: int            # process return code
    v3_lower_ms: float = 0.0
    v3_compile_ms: float = 0.0
    v3_run_ms: float = 0.0
    v3_bridge_ms: float = 0.0
    v3_eval_entries: int = 0
    v3_force_entries: int = 0
    v3_dispatch_insns: int = 0


@dataclass
class Cell:
    """All runs for a single (workload, mode) cell."""
    workload: str
    mode: str
    n: int
    runs: list[Run] = field(default_factory=list)

    def fail_count(self) -> int:
        return sum(1 for r in self.runs if r.rc != 0)

    def wall_stats(self) -> dict:
        """min / p50 / mean / p95 / stddev over wall times.

        Returns 0s if all runs failed.  Excludes failed runs when at
        least one succeeded (so partial-success cells are still
        reportable, but the failure is flagged in `fail_count`)."""
        good = [r.wall for r in self.runs if r.rc == 0]
        if not good:
            return {"min": 0, "p50": 0, "mean": 0, "p95": 0, "stddev": 0}
        good_sorted = sorted(good)
        n = len(good_sorted)
        p50 = good_sorted[n // 2]
        # p95 with safe rounding for small N.
        p95idx = max(0, min(n - 1, int(round(n * 0.95)) - 1))
        return {
            "min": good_sorted[0],
            "p50": p50,
            "mean": statistics.mean(good),
            "p95": good_sorted[p95idx],
            "stddev": statistics.stdev(good) if n > 1 else 0.0,
        }

    def rss_max_b(self) -> int:
        """Peak RSS across runs (single max, not averaged)."""
        good = [r.rss_max_b for r in self.runs if r.rc == 0]
        return max(good) if good else 0

    def v3_run_min_ms(self) -> float:
        """Minimum v3.run dispatch time across runs (the apples-to-
        apples comparand vs TW total wall, since lower+compile is
        amortizable via the disk cache)."""
        good = [r.v3_run_ms for r in self.runs if r.rc == 0 and r.v3_run_ms > 0]
        return min(good) if good else 0.0


# ---------------------------------------------------------------------------
# Workload loading.
# ---------------------------------------------------------------------------

def load_workloads(path: Path) -> dict:
    with path.open("rb") as f:
        return tomllib.load(f)


def resolve_nixpkgs() -> Optional[str]:
    """Resolve a nixpkgs path.  Order:
      1. $NIXPKGS env var.
      2. `nix flake archive --json` from the repo root, looking for
         the nixpkgs input.
      3. None if neither works.
    """
    if (p := os.environ.get("NIXPKGS")) and Path(p).is_dir():
        return p
    try:
        out = subprocess.run(
            ["nix", "--extra-experimental-features", "nix-command flakes",
             "flake", "archive", "--json"],
            cwd=ROOT, capture_output=True, text=True, timeout=120, check=True
        ).stdout
        d = json.loads(out)
        return d.get("inputs", {}).get("nixpkgs", {}).get("path") or None
    except Exception:
        return None


def filter_workloads(
    all_workloads: dict,
    only: Optional[list[str]],
    tags: Optional[list[str]],
    skip_tags: Optional[list[str]],
    nixpkgs_path: Optional[str],
) -> dict:
    """Apply --only / --tag / --skip-tag filters and skip nixpkgs
    workloads if no nixpkgs path is available."""
    out = {}
    for name, w in all_workloads.items():
        wtags = set(w.get("tags", []))
        if only:
            if name not in only:
                continue
        else:
            if "skip-by-default" in wtags:
                continue
            if tags and not (set(tags) & wtags):
                continue
        if skip_tags and (set(skip_tags) & wtags):
            continue
        if w["kind"] in ("nixpkgs", "lib") and not nixpkgs_path:
            print(
                f"WARN: skipping {name} — needs nixpkgs path "
                f"(set $NIXPKGS or have a flake.nix with a nixpkgs input)",
                file=sys.stderr)
            continue
        out[name] = w
    return out


def render_expr(expr: str, nixpkgs_path: Optional[str], system: str) -> str:
    return (expr
            .replace("@NIXPKGS@", nixpkgs_path or "<<<MISSING>>>")
            .replace("@SYSTEM@", system))


# ---------------------------------------------------------------------------
# Single-run execution.
# ---------------------------------------------------------------------------

# Regex bank for parsing V3_TIMING + NIX_VM_STATS output.
_TIME_RE_RSS = re.compile(r"^\s*(\d+)\s+maximum resident set size", re.M)
_TIME_RE_REAL = re.compile(r"^\s*([\d.]+)\s+real\s+([\d.]+)\s+user\s+([\d.]+)\s+sys", re.M)
_V3_LOWER = re.compile(r"lower=([\d.]+)")
_V3_COMPILE = re.compile(r"compile=([\d.]+)")
_V3_RUN = re.compile(r"run=([\d.]+)")
_V3_BRIDGE = re.compile(r"bridge=([\d.]+)")
_V3_EVALENT = re.compile(r"evalEntries=(\d+)")
_V3_FORCEENT = re.compile(r"forceEntries=(\d+)")
_V3_INSNS = re.compile(r"v3 dispatch: bytecode instructions=(\d+)")


def run_one(nix: Path, mode: str, expr: str,
            with_v3_timing: bool = False,
            with_vm_stats: bool = False,
            timeout: float = 300.0) -> Run:
    """Execute one (mode, expr) and capture metrics.

    Wraps the real `nix eval` with `/usr/bin/time -l` so we get RSS.
    Routes stderr through to also capture V3_TIMING / NIX_VM_STATS
    dumps when those env vars are set.

    Returns a Run with wall=0 and rc=non-zero if the eval failed.
    """
    env = os.environ.copy()
    env.update(MODES[mode])
    if with_v3_timing:
        env["V3_TIMING"] = "1"
    if with_vm_stats:
        env["NIX_VM_STATS"] = "1"
    cmd = [
        "/usr/bin/time", "-l",
        str(nix), "--extra-experimental-features", "nix-command",
        "eval", "--impure", "--expr", expr,
    ]
    t0 = time.monotonic()
    try:
        proc = subprocess.run(
            cmd, env=env, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return Run(wall=timeout, user=0, sys=0, rss_max_b=0, rc=124)
    t1 = time.monotonic()
    # Prefer Python monotonic clock for wall-time: macOS's
    # `/usr/bin/time -l` reports `real` to 0.01s precision (10ms),
    # which crushes sub-100ms workloads to a single bucket.  The
    # Python clock has microsecond granularity.  Keep parsing
    # time -l for `user`/`sys`/`rss` though — the rusage values
    # themselves are accurate, only the `real` seconds are
    # truncated.
    wall = t1 - t0
    rss = 0
    user = sys_t = 0.0
    if m := _TIME_RE_RSS.search(proc.stderr):
        rss = int(m.group(1))
    if m := _TIME_RE_REAL.search(proc.stderr):
        user = float(m.group(2))
        sys_t = float(m.group(3))
    r = Run(wall=wall, user=user, sys=sys_t, rss_max_b=rss, rc=proc.returncode)
    # Parse v3-emitted lines from stderr.
    if m := _V3_LOWER.search(proc.stderr):
        r.v3_lower_ms = float(m.group(1))
    if m := _V3_COMPILE.search(proc.stderr):
        r.v3_compile_ms = float(m.group(1))
    if m := _V3_RUN.search(proc.stderr):
        r.v3_run_ms = float(m.group(1))
    if m := _V3_BRIDGE.search(proc.stderr):
        r.v3_bridge_ms = float(m.group(1))
    if m := _V3_EVALENT.search(proc.stderr):
        r.v3_eval_entries = int(m.group(1))
    if m := _V3_FORCEENT.search(proc.stderr):
        r.v3_force_entries = int(m.group(1))
    if m := _V3_INSNS.search(proc.stderr):
        r.v3_dispatch_insns = int(m.group(1))
    return r


# ---------------------------------------------------------------------------
# Cell driver: run N times per (workload, mode), collect Runs.
# ---------------------------------------------------------------------------

def run_cell(nix: Path, workload_name: str, mode: str, expr: str,
             n: int, deep: bool, verbose: bool) -> Cell:
    """Run a workload+mode N times.  When `deep`, the LAST run also
    collects V3_TIMING + NIX_VM_STATS data; that cell-level data lands
    on the corresponding Run."""
    cell = Cell(workload=workload_name, mode=mode, n=n)
    for i in range(n):
        is_last = (i == n - 1)
        r = run_one(
            nix=nix, mode=mode, expr=expr,
            with_v3_timing=deep and is_last and mode != "tw",
            with_vm_stats=deep and is_last and mode != "tw")
        cell.runs.append(r)
        if verbose:
            tag = "OK" if r.rc == 0 else f"FAIL(rc={r.rc})"
            print(f"  [{workload_name} / {mode}] run {i+1}/{n}: "
                  f"wall={r.wall:.4f}s rss={r.rss_max_b/1e6:.1f}MB {tag}",
                  file=sys.stderr)
    return cell


# ---------------------------------------------------------------------------
# Output formatters.
# ---------------------------------------------------------------------------

def format_table(cells: list[Cell], modes: list[str]) -> str:
    """Wide table: one row per workload, one column block per mode."""
    out = []
    cw = 13  # workload-name column width
    sub = ["min(s)", "p50(s)", "p95(s)", "rss(MB)"]

    # Header
    h0 = f"{'workload':<{cw}}"
    for m in modes:
        h0 += f" | {m:^{len(' '.join(sub))}}"
    out.append(h0)
    h1 = f"{'':<{cw}}"
    for m in modes:
        h1 += " | " + " ".join(f"{s:>7}" for s in sub)
    out.append(h1)
    out.append("-" * len(h1))

    # By workload, gather cells.
    by_wl = {}
    for c in cells:
        by_wl.setdefault(c.workload, {})[c.mode] = c
    for wl, mdict in by_wl.items():
        line = f"{wl:<{cw}}"
        for m in modes:
            c = mdict.get(m)
            if not c:
                line += " | " + " ".join(f"{'-':>7}" for _ in sub)
                continue
            if c.fail_count() == c.n:
                line += " | " + " ".join(f"{'FAIL':>7}" for _ in sub)
                continue
            s = c.wall_stats()
            line += (" | "
                     f"{s['min']:>7.4f} "
                     f"{s['p50']:>7.4f} "
                     f"{s['p95']:>7.4f} "
                     f"{c.rss_max_b()/1e6:>7.1f}")
        out.append(line)
    return "\n".join(out)


def format_ratio_table(cells: list[Cell], modes: list[str]) -> str:
    """For each mode, the ratio of its min to TW's min.  TW's column
    shows absolute ms.  Ratios <1.0 = mode is faster."""
    by_wl = {}
    for c in cells:
        by_wl.setdefault(c.workload, {})[c.mode] = c
    out = []
    cw = 13
    out.append(f"{'workload':<{cw}}  {'TW min':>9}" +
               "".join(f"  {m+' ratio':>14}" for m in modes if m != "tw"))
    out.append("-" * len(out[0]))
    for wl, mdict in by_wl.items():
        tw = mdict.get("tw")
        if not tw or tw.fail_count() == tw.n:
            continue
        twmin = tw.wall_stats()["min"]
        cells_str = f"{wl:<{cw}}  {twmin*1000:>7.1f}ms"
        for m in modes:
            if m == "tw":
                continue
            c = mdict.get(m)
            if not c or c.fail_count() == c.n:
                cells_str += f"  {'-':>14}"
                continue
            r = c.wall_stats()["min"] / twmin if twmin > 0 else 0
            cells_str += f"  {r:>10.2f}x ({c.wall_stats()['min']*1000:.0f}ms)".rjust(14)
        out.append(cells_str)
    return "\n".join(out)


def format_v3_phases(cells: list[Cell]) -> str:
    """V3_TIMING phase breakdown for v3-* modes.  Only shows cells
    that ran with --deep (so v3 phase data was captured)."""
    out = []
    out.append(f"{'workload':<13}  {'mode':<14}  "
               f"{'lower':>7}  {'compile':>7}  {'run':>9}  "
               f"{'bridge':>7}  {'evHk':>5}  {'fcHk':>5}  {'insns':>10}")
    out.append("-" * len(out[0]))
    for c in cells:
        if c.mode == "tw":
            continue
        # Find the deepest run.
        with_data = [r for r in c.runs
                     if r.rc == 0 and r.v3_run_ms > 0]
        if not with_data:
            continue
        r = with_data[-1]
        out.append(
            f"{c.workload:<13}  {c.mode:<14}  "
            f"{r.v3_lower_ms:>7.3f}  "
            f"{r.v3_compile_ms:>7.3f}  "
            f"{r.v3_run_ms:>9.3f}  "
            f"{r.v3_bridge_ms:>7.3f}  "
            f"{r.v3_eval_entries:>5}  "
            f"{r.v3_force_entries:>5}  "
            f"{r.v3_dispatch_insns:>10}"
        )
    return "\n".join(out)


def to_json(cells: list[Cell], metadata: dict) -> str:
    return json.dumps({
        "metadata": metadata,
        "results": [
            {
                "workload": c.workload,
                "mode": c.mode,
                "n": c.n,
                "fail_count": c.fail_count(),
                "wall": c.wall_stats(),
                "rss_max_b": c.rss_max_b(),
                "v3_run_ms_min": c.v3_run_min_ms(),
                "runs": [asdict(r) for r in c.runs],
            }
            for c in cells
        ],
    }, indent=2, sort_keys=True)


def to_csv(cells: list[Cell]) -> str:
    """One row per individual run.  Format chosen to be easy to
    pipe into awk / pandas without parsing surprises."""
    out = ["workload,mode,run_idx,wall,user,sys,rss_b,rc,"
           "v3_lower_ms,v3_compile_ms,v3_run_ms,v3_bridge_ms,"
           "v3_eval_entries,v3_force_entries,v3_dispatch_insns"]
    for c in cells:
        for i, r in enumerate(c.runs):
            out.append(
                f"{c.workload},{c.mode},{i},"
                f"{r.wall:.4f},{r.user:.4f},{r.sys:.4f},"
                f"{r.rss_max_b},{r.rc},"
                f"{r.v3_lower_ms:.3f},{r.v3_compile_ms:.3f},"
                f"{r.v3_run_ms:.3f},{r.v3_bridge_ms:.3f},"
                f"{r.v3_eval_entries},{r.v3_force_entries},"
                f"{r.v3_dispatch_insns}"
            )
    return "\n".join(out)


def to_markdown(cells: list[Cell], modes: list[str]) -> str:
    """GitHub-flavoured Markdown table.  Same shape as ratio table
    but with pipes."""
    by_wl = {}
    for c in cells:
        by_wl.setdefault(c.workload, {})[c.mode] = c
    out = []
    header_modes = [m for m in modes if m != "tw"]
    out.append("| workload | TW min |" +
               "".join(f" {m} | ratio |" for m in header_modes))
    out.append("|---|---|" + "".join("---|---|" for _ in header_modes))
    for wl, mdict in by_wl.items():
        tw = mdict.get("tw")
        if not tw or tw.fail_count() == tw.n:
            continue
        twmin = tw.wall_stats()["min"]
        line = f"| {wl} | {twmin*1000:.1f}ms |"
        for m in header_modes:
            c = mdict.get(m)
            if not c or c.fail_count() == c.n:
                line += " - | - |"
                continue
            cmin = c.wall_stats()["min"]
            r = cmin / twmin if twmin > 0 else 0
            line += f" {cmin*1000:.1f}ms | **{r:.2f}x** |"
        out.append(line)
    return "\n".join(out)


# ---------------------------------------------------------------------------
# Baseline diff.
# ---------------------------------------------------------------------------

def diff_against_baseline(cells: list[Cell], baseline_path: Path,
                          threshold_pct: float) -> str:
    with baseline_path.open() as f:
        base = json.load(f)
    base_idx = {(r["workload"], r["mode"]): r for r in base["results"]}
    out = [
        f"diff vs {baseline_path.name} (threshold ±{threshold_pct}%):",
        f"  baseline date: {base.get('metadata', {}).get('date', '?')}",
        f"  baseline rev:  {base.get('metadata', {}).get('git_rev', '?')[:12]}",
    ]
    out.append(f"  {'workload':<13} {'mode':<14} "
               f"{'baseline':>10} {'current':>10} {'delta%':>8}  flag")
    out.append("  " + "-" * (len(out[-1]) - 2))
    flagged = 0
    for c in cells:
        b = base_idx.get((c.workload, c.mode))
        if not b:
            continue
        b_min = b["wall"]["min"]
        c_min = c.wall_stats()["min"]
        if b_min == 0:
            continue
        delta = (c_min - b_min) / b_min * 100
        flag = ""
        if abs(delta) > threshold_pct:
            flag = "REGRESSION" if delta > 0 else "IMPROVEMENT"
            flagged += 1
        out.append(f"  {c.workload:<13} {c.mode:<14} "
                   f"{b_min*1000:>8.1f}ms {c_min*1000:>8.1f}ms "
                   f"{delta:>+7.1f}%  {flag}")
    out.append(f"  {flagged} cells flagged.")
    return "\n".join(out)


# ---------------------------------------------------------------------------
# CLI.
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description="v3 vs TW benchmark harness.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    ap.add_argument("--nix", default=str(DEFAULT_NIX),
                    help=f"path to `nix` binary (default {DEFAULT_NIX})")
    ap.add_argument("--workloads", default=str(DEFAULT_WORKLOADS),
                    help="path to workloads.toml")
    ap.add_argument("-n", type=int, default=3,
                    help="number of runs per cell (default 3)")
    ap.add_argument("--modes", default="tw,v3-direct",
                    help="comma-separated modes (tw, v3-direct, v3-hook, v3-hook-fhook)")
    ap.add_argument("--only", default="",
                    help="comma-separated workload names to run")
    ap.add_argument("--tag", default="",
                    help="comma-separated tag(s); workloads matching ANY tag are kept")
    ap.add_argument("--skip-tag", default="",
                    help="comma-separated tag(s) to exclude")
    ap.add_argument("--system", default=os.uname().machine + "-darwin",
                    help="@SYSTEM@ substitution (default: current arch + -darwin)")
    ap.add_argument("--format", default="table",
                    choices=["table", "json", "csv", "markdown", "ratio", "phases", "all"],
                    help="output format")
    ap.add_argument("--save", metavar="FILE", default=None,
                    help="save JSON snapshot to FILE")
    ap.add_argument("--baseline", metavar="FILE", default=None,
                    help="diff against a prior JSON snapshot")
    ap.add_argument("--threshold", type=float, default=5.0,
                    help="diff threshold percentage (default 5)")
    ap.add_argument("--deep", action="store_true", default=True,
                    help="capture V3_TIMING + NIX_VM_STATS on last run of each cell")
    ap.add_argument("--no-deep", dest="deep", action="store_false",
                    help="skip V3_TIMING / NIX_VM_STATS capture")
    ap.add_argument("--verbose", "-v", action="store_true",
                    help="log each individual run to stderr")
    ap.add_argument("--timeout", type=float, default=300.0,
                    help="per-run timeout in seconds (default 300)")
    args = ap.parse_args()

    # Resolve inputs.
    nix = Path(args.nix)
    if not nix.exists():
        print(f"ERROR: nix binary not found at {nix}", file=sys.stderr)
        sys.exit(2)
    wl_path = Path(args.workloads)
    if not wl_path.exists():
        print(f"ERROR: workloads file not found at {wl_path}", file=sys.stderr)
        sys.exit(2)
    only = [s.strip() for s in args.only.split(",") if s.strip()]
    tags = [s.strip() for s in args.tag.split(",") if s.strip()]
    skip_tags = [s.strip() for s in args.skip_tag.split(",") if s.strip()]
    modes = [s.strip() for s in args.modes.split(",") if s.strip()]
    for m in modes:
        if m not in MODES:
            print(f"ERROR: unknown mode '{m}'; valid: {','.join(MODES)}", file=sys.stderr)
            sys.exit(2)

    nixpkgs = resolve_nixpkgs()
    if not nixpkgs and (any(only) or any(tags)):
        # User explicitly asked for workloads — require nixpkgs if any
        # are nixpkgs/lib kind.
        pass
    elif nixpkgs:
        print(f"# nixpkgs: {nixpkgs}", file=sys.stderr)
    else:
        print("# nixpkgs not available — skipping real-world workloads",
              file=sys.stderr)

    workloads = load_workloads(wl_path)
    workloads = filter_workloads(workloads, only or None,
                                 tags or None, skip_tags or None,
                                 nixpkgs)
    if not workloads:
        print("ERROR: no workloads selected after filtering.", file=sys.stderr)
        sys.exit(2)

    print(f"# {len(workloads)} workload(s) × {len(modes)} mode(s) × "
          f"n={args.n} = {len(workloads)*len(modes)*args.n} runs",
          file=sys.stderr)

    # Run.
    cells = []
    t0 = time.monotonic()
    for wl_name, w in workloads.items():
        expr = render_expr(w["expr"], nixpkgs, args.system)
        for mode in modes:
            c = run_cell(
                nix=nix, workload_name=wl_name, mode=mode, expr=expr,
                n=args.n, deep=args.deep, verbose=args.verbose)
            cells.append(c)
            if c.fail_count() > 0:
                print(f"# {wl_name}/{mode}: {c.fail_count()}/{c.n} failed",
                      file=sys.stderr)
    elapsed = time.monotonic() - t0
    print(f"# total bench time: {elapsed:.1f}s", file=sys.stderr)

    # Metadata for JSON output.
    git_rev = ""
    try:
        git_rev = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=ROOT, capture_output=True, text=True, check=True
        ).stdout.strip()
    except Exception:
        pass
    metadata = {
        "date": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "git_rev": git_rev,
        "system": args.system,
        "nix_path": str(nix.resolve()),
        "nixpkgs_path": nixpkgs or "",
        "n": args.n,
        "modes": modes,
    }

    # Save snapshot if requested.
    if args.save:
        Path(args.save).parent.mkdir(parents=True, exist_ok=True)
        Path(args.save).write_text(to_json(cells, metadata))
        print(f"# saved snapshot to {args.save}", file=sys.stderr)

    # Format output.
    fmt = args.format
    if fmt == "json":
        print(to_json(cells, metadata))
    elif fmt == "csv":
        print(to_csv(cells))
    elif fmt == "markdown":
        print(to_markdown(cells, modes))
    elif fmt == "ratio":
        print(format_ratio_table(cells, modes))
    elif fmt == "phases":
        print(format_v3_phases(cells))
    elif fmt == "all":
        print("== TIMING TABLE ==\n")
        print(format_table(cells, modes))
        print("\n== RATIO TABLE ==\n")
        print(format_ratio_table(cells, modes))
        print("\n== V3 PHASE BREAKDOWN ==\n")
        print(format_v3_phases(cells))
    else:  # table
        print(format_table(cells, modes))

    # Baseline diff.
    if args.baseline:
        bp = Path(args.baseline)
        if not bp.exists():
            print(f"ERROR: baseline {bp} not found.", file=sys.stderr)
            sys.exit(2)
        print()
        print(diff_against_baseline(cells, bp, args.threshold))


if __name__ == "__main__":
    main()
