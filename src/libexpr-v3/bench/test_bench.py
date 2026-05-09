#!/usr/bin/env python3
"""Self-tests for the v3 benchmark harness.

These exercise bench.py's pure logic: workload loading, filtering,
stats math, JSON round-trip, baseline diff, and the failure-mode
contract that a workload that crashes is reported as `fail_count > 0`
rather than silently swallowed.

Run with: nix develop -c python3 src/libexpr-v3/bench/test_bench.py
"""

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

# Make bench.py importable.
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import bench  # noqa: E402


# ---------------------------------------------------------------------------
# Helpers.
# ---------------------------------------------------------------------------

PASS = 0
FAIL = 0


def check(name, ok, detail=""):
    global PASS, FAIL
    if ok:
        PASS += 1
        print(f"  ok   {name}")
    else:
        FAIL += 1
        print(f"  FAIL {name}: {detail}", file=sys.stderr)


def make_cell(workload, mode, walls, rcs=None):
    """Construct a Cell from a list of wall times (rc=0 unless specified)."""
    if rcs is None:
        rcs = [0] * len(walls)
    runs = [
        bench.Run(wall=w, user=0, sys=0, rss_max_b=1000, rc=rc)
        for w, rc in zip(walls, rcs)
    ]
    return bench.Cell(workload=workload, mode=mode, n=len(runs), runs=runs)


# ---------------------------------------------------------------------------
# Tests.
# ---------------------------------------------------------------------------

def test_workload_load_smoke():
    """Positive: bundled workloads.toml loads and parses without raising."""
    workloads = bench.load_workloads(HERE / "workloads.toml")
    check("load_smoke: at least one workload", len(workloads) > 0,
          f"got {len(workloads)} workloads")
    check("load_smoke: fib33 present", "fib33" in workloads)
    check("load_smoke: fib33 has expr", "expr" in workloads.get("fib33", {}))
    check("load_smoke: hello-name has skip-by-default tag",
          "skip-by-default" in workloads.get("hello-name", {}).get("tags", []),
          "real-world workloads requiring full pkgs eval should default-skip")


def test_filter_only():
    """Filter: --only NAMES restricts to exactly the named workloads."""
    workloads = bench.load_workloads(HERE / "workloads.toml")
    f = bench.filter_workloads(workloads, only=["fib25"], tags=None,
                               skip_tags=None, nixpkgs_path="/dev/null")
    check("filter_only: only fib25", set(f.keys()) == {"fib25"})


def test_filter_skip_by_default():
    """Negative: workloads tagged skip-by-default are excluded unless
    explicitly named."""
    workloads = bench.load_workloads(HERE / "workloads.toml")
    f_default = bench.filter_workloads(workloads, only=None, tags=None,
                                       skip_tags=None,
                                       nixpkgs_path="/dev/null")
    check("filter_skip_default: hello-name absent",
          "hello-name" not in f_default)
    f_explicit = bench.filter_workloads(workloads, only=["hello-name"],
                                        tags=None, skip_tags=None,
                                        nixpkgs_path="/dev/null")
    check("filter_skip_default: hello-name present when --only",
          "hello-name" in f_explicit)


def test_filter_nixpkgs_unavailable():
    """Negative: when nixpkgs_path is None, nixpkgs/lib workloads are
    silently dropped (with a warning to stderr)."""
    workloads = bench.load_workloads(HERE / "workloads.toml")
    # Capture stderr to suppress the warning during the test.
    import io, contextlib
    buf = io.StringIO()
    with contextlib.redirect_stderr(buf):
        f = bench.filter_workloads(workloads, only=None, tags=None,
                                   skip_tags=None, nixpkgs_path=None)
    check("filter_no_nixpkgs: synthetic kept", "fib33" in f)
    check("filter_no_nixpkgs: lib workloads dropped",
          "lib-foldl-1k" not in f)
    check("filter_no_nixpkgs: warning emitted", "WARN" in buf.getvalue())


def test_render_expr():
    """Positive: @NIXPKGS@ and @SYSTEM@ substitution works."""
    out = bench.render_expr(
        "(import @NIXPKGS@ { system = \"@SYSTEM@\"; }).hello",
        "/path/to/pkgs", "x86_64-linux")
    check("render_expr: nixpkgs substituted",
          "/path/to/pkgs" in out and "x86_64-linux" in out)
    check("render_expr: no template markers remain", "@" not in out)


def test_wall_stats_basic():
    """Positive: min/p50/p95/mean/stddev computed correctly."""
    c = make_cell("test", "tw", [0.1, 0.2, 0.15, 0.12, 0.13])
    s = c.wall_stats()
    check("wall_stats: min", abs(s["min"] - 0.1) < 1e-9, f"got {s['min']}")
    # p50 of 5 sorted values [0.1, 0.12, 0.13, 0.15, 0.2] = sorted[2] = 0.13
    check("wall_stats: p50", abs(s["p50"] - 0.13) < 1e-9, f"got {s['p50']}")
    check("wall_stats: stddev > 0", s["stddev"] > 0)


def test_wall_stats_all_failed():
    """Regression: when ALL runs fail, stats return zeros (not crash)."""
    c = make_cell("test", "tw", [0.1, 0.2, 0.3], rcs=[1, 1, 1])
    s = c.wall_stats()
    check("wall_stats_all_failed: min=0", s["min"] == 0)
    check("wall_stats_all_failed: fail_count=3", c.fail_count() == 3)


def test_wall_stats_partial_failed():
    """Regression: partial failures don't poison stats from successful runs.

    A workload that flakes on 1/3 runs should still report meaningful
    stats for the 2 successful ones, BUT also surface fail_count=1 so
    the user can see the flakiness."""
    c = make_cell("test", "tw", [0.1, 0.2, 0.3], rcs=[0, 1, 0])
    s = c.wall_stats()
    check("partial_fail: min from successful runs",
          abs(s["min"] - 0.1) < 1e-9)
    check("partial_fail: fail_count=1 visible",
          c.fail_count() == 1)


def test_json_round_trip():
    """Regression: to_json output is parseable back as JSON."""
    cells = [make_cell("fib25", "tw", [0.05, 0.05])]
    js = bench.to_json(cells, {"date": "test", "git_rev": "abc"})
    parsed = json.loads(js)
    check("json_round_trip: parseable", "results" in parsed)
    check("json_round_trip: cell count", len(parsed["results"]) == 1)
    check("json_round_trip: workload preserved",
          parsed["results"][0]["workload"] == "fib25")


def test_baseline_diff_no_regression():
    """Positive: when current matches baseline, diff reports 0 flagged."""
    cells = [make_cell("fib25", "tw", [0.05, 0.05])]
    js = bench.to_json(cells, {"date": "x", "git_rev": "x"})
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
        f.write(js)
        baseline_path = Path(f.name)
    try:
        out = bench.diff_against_baseline(cells, baseline_path, threshold_pct=5)
        check("diff_no_regression: 0 flagged", "0 cells flagged." in out)
    finally:
        baseline_path.unlink()


def test_baseline_diff_regression():
    """Regression: when current is 20% slower than baseline, the diff
    flags REGRESSION and counts a non-zero flagged count."""
    base_cells = [make_cell("fib25", "tw", [0.10, 0.10])]
    js = bench.to_json(base_cells, {"date": "x", "git_rev": "x"})
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
        f.write(js)
        baseline_path = Path(f.name)
    try:
        # Current cells: 25% slower.
        cur_cells = [make_cell("fib25", "tw", [0.125, 0.125])]
        out = bench.diff_against_baseline(cur_cells, baseline_path,
                                          threshold_pct=5)
        check("diff_regression: flagged", "REGRESSION" in out)
        check("diff_regression: non-zero flagged count",
              "0 cells flagged." not in out and "flagged." in out)
    finally:
        baseline_path.unlink()


def test_baseline_diff_improvement():
    """Positive: when current is faster than baseline, diff flags
    IMPROVEMENT (so we can see good news too)."""
    base_cells = [make_cell("fib25", "tw", [0.20, 0.20])]
    js = bench.to_json(base_cells, {"date": "x", "git_rev": "x"})
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
        f.write(js)
        baseline_path = Path(f.name)
    try:
        cur_cells = [make_cell("fib25", "tw", [0.15, 0.15])]
        out = bench.diff_against_baseline(cur_cells, baseline_path,
                                          threshold_pct=5)
        check("diff_improvement: flagged", "IMPROVEMENT" in out)
    finally:
        baseline_path.unlink()


def test_run_one_failure_visible():
    """Regression: a workload that fails (non-zero rc) is reported with
    rc != 0 — NOT silently treated as a 0-time success.

    Runs `nix eval --expr 'throw "boom"'` which always fails.  We
    trust /usr/bin/time -l to be present (we're on macOS in this
    test set; Linux equivalent is `command time -v`)."""
    nix = bench.DEFAULT_NIX
    if not nix.exists():
        check("run_one_failure: SKIPPED (no nix binary)", True,
              "nix not built; build first to enable this test")
        return
    r = bench.run_one(nix, "tw", "throw \"benchmark-test-boom\"",
                      with_v3_timing=False, with_vm_stats=False, timeout=30)
    check("run_one_failure: rc != 0", r.rc != 0, f"got rc={r.rc}")
    check("run_one_failure: wall recorded", r.wall > 0)


def test_run_one_success_v3_direct():
    """Positive: NIX_V3_DIRECT_EVAL=1 + V3_TIMING=1 produces parseable
    phase data (this is the v3-direct fix landed in this session)."""
    nix = bench.DEFAULT_NIX
    if not nix.exists():
        check("run_one_v3_direct: SKIPPED (no nix binary)", True)
        return
    r = bench.run_one(nix, "v3-direct",
                      "let f = n: if n < 2 then n else f (n - 1) + f (n - 2); in f 20",
                      with_v3_timing=True, with_vm_stats=False, timeout=30)
    check("run_one_v3_direct: rc == 0", r.rc == 0)
    # Without the run.cc V3_TIMING fix, run_ms would be 0 because
    # v3-direct never fires the v3_hook.cc atexit handler.
    check("run_one_v3_direct: phase data captured", r.v3_run_ms > 0,
          f"v3_run_ms={r.v3_run_ms}; "
          f"if 0, the run.cc PhaseTimer is not firing")


# ---------------------------------------------------------------------------
# Driver.
# ---------------------------------------------------------------------------

def main():
    tests = [
        test_workload_load_smoke,
        test_filter_only,
        test_filter_skip_by_default,
        test_filter_nixpkgs_unavailable,
        test_render_expr,
        test_wall_stats_basic,
        test_wall_stats_all_failed,
        test_wall_stats_partial_failed,
        test_json_round_trip,
        test_baseline_diff_no_regression,
        test_baseline_diff_regression,
        test_baseline_diff_improvement,
        test_run_one_failure_visible,
        test_run_one_success_v3_direct,
    ]
    print(f"running {len(tests)} bench harness tests")
    for t in tests:
        print(f"\n{t.__name__}:")
        try:
            t()
        except Exception as e:
            global FAIL
            FAIL += 1
            print(f"  FAIL {t.__name__} (exception): {e}")
    print(f"\n=== {PASS} passed, {FAIL} failed ===")
    sys.exit(0 if FAIL == 0 else 1)


if __name__ == "__main__":
    main()
