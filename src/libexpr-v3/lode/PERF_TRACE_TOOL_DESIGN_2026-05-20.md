# Perf-Trace Tool Design — 2026-05-20

> Critically-revised self-critique of the same-day research turn that produced
> the first sketch. Closes Item 5 ("documented CPU-profile workflow") of the
> 10-item debug story in `LESSONS_LEARNED_2026-05-15.md` §4.9; supplies the
> Boehm-heap-over-time signal that `ACTION_PLAN_2026-05-15.md` Phase 1.5's
> drvPath force-rate decomposition needs.

## Goal

A small, reproducible tool that runs the **same Nix expression** under two (or
more) evaluators — tree-walker (TW) and v3-direct — under external **CPU% and
RSS sampling over time**, plus an in-process **Boehm heap-size probe**, and
emits an **SVG overlay** plus a **JSONL trace** suitable for offline analysis.

Scope is single-machine, macOS-first (Linux works incidentally), no kernel
profiling, no stack samples (those are `samply`'s job — see §"Phase 2").

## Why now

Three independently-derived pulls point at the same gap:

1. **ACTION_PLAN Phase 1.5** (lines 65-90) lists "drvPath/outPath force-rate
   decomposition profile" as a TODO whose item (b) is *"GC scan time per force
   (Boehm arena watermark progression)"*. There is no current way to produce
   that signal. This tool is its instrument.

2. **`EXTEND_DERIVATION_INVESTIGATION_2026-05-18.md`** decomposes the 200×
   per-force hello.drvPath gap into four multiplicative factors. Factor (2) is
   "Boehm-arena scan amortized into each force". You cannot decompose
   multiplicative factors without per-factor instrumentation; we have none for
   factor (2) today.

3. **`LESSONS_LEARNED_2026-05-15.md` §4.9 Item 5**: existing profiling is
   `V3_TIMING` + `allocStats` + `bench.py` — all **scalar aggregates**.
   Time-series is the missing column in the debug-story matrix; the lessons
   doc explicitly names it as a gap.

## What we already have (and why it is not enough)

| Source | Shape | Time-series? | Boehm heap? | Cross-evaluator? |
|---|---|---|---|---|
| `bench.py` (`/usr/bin/time -l`) | min/p50/p95 wall + final max RSS | ✗ scalar | ✗ (RSS only) | ✓ |
| `V3_TIMING` | per-phase ms (lower/compile/run/bridge) | ✗ aggregate | ✗ | ✗ (v3 only) |
| `NIX_VM_STATS` / `NIX_VM_OPCOUNTS` | opcode + dispatch counters | ✗ aggregate | ✗ | ✗ (v3 only) |
| `NIX_V3_MAX_*` allocStats on breach | closures/thunks/lists/attrsets + Boehm bytes | ✗ one-shot | ✓ on breach | ✗ (v3 only) |
| `samply` (not installed) | external stack sampler, RSS | ✓ | ✗ (RSS only) | ✓ |
| `psrecord` (not installed) | external sidecar sampler | ✓ | ✗ (RSS only) | ✓ (single-PID at a time) |

The missing intersection: **time-series + Boehm-heap + cross-evaluator,
plotted comparably on the same axes**. No combination of existing tools
produces that artefact directly.

## Approaches surveyed

### A. `psrecord` (off-the-shelf, Python)

`pip install psrecord`; one command spawns a child, polls `psutil`, writes a
PNG via matplotlib. Single-process at a time; no native overlay across
processes. Workaround for overlay: invoke twice in `--log` mode and merge the
log files offline. Latest PyPI release predates 2024 (maintenance is best
described as "stable, not active"). Verdict: viable for a one-day spike;
not enough control for the long-term home.

### B. `samply` (off-the-shelf, Rust)

Stack sampler in the spirit of `perf`, produces HTML+SVG (Firefox profiler
format). macOS-native via Mach task ports; no kernel patches needed. RSS is
a side product of its sampling, not the headline. **Best complement, not
replacement**: pairs nicely with the custom RSS tool — phase 2 work,
discussed below.

### C. macOS Instruments / `sample` / `vmmap` snapshots

The system-native option. Heavy permissions (TCC + SIP), opaque output
format, hard to script for batch jobs, no SVG export. Useful for one-off
deep dives, not for the routine signal we want.

### D. Custom Python sidecar (`psutil`) + matplotlib SVG **[recommended]**

We spawn the evaluator child, attach a thread that polls
`psutil.Process(pid)` at a fixed cadence, capture stderr in parallel,
write JSONL trace + SVG. Matplotlib's SVG backend produces compact,
embeddable, diffable output. Heavy parts of the stack (`psutil`,
`matplotlib`) live in a flake devShell output — they don't poison
the main project closure.

### E. Custom Python sidecar (`psutil`) + hand-rolled `<svg>` writer

Same sampler as D; ~2× the rendering code; zero matplotlib dependency
(~200 MB nix closure saved). Drawback: every visual polish item (legend,
gridlines, dual axes, IQR ribbon) is hand-rolled. Recommended only if
the matplotlib closure becomes a friction point.

### F. In-process probe inside v3-eval only

A pthread sampling timer inside the binary that emits
`{t_us, GC_get_heap_size(), GC_get_free_bytes()}` to stderr periodically.
**Necessary as a complement** to D — Boehm heap size is not visible from
outside the process — but **not sufficient on its own**: TW would need
the same instrumentation, and the goal here is comparability.

## Critical-review corrections to the 2026-05-20 first sketch

The same-day research turn had several things wrong or oversold. Recording
them here so the next reader does not re-walk the path:

1. **VMS is not a Boehm signal.** The first sketch said
   "`memory_info().vms` (virtual — Boehm tracking)". Wrong. `vms` is the
   total virtual address space — every mapped library, every anonymous
   region, every JIT mmap. Boehm's arena is one contributor among many.
   For honest Boehm-heap tracking we **must** instrument v3-eval to emit
   `GC_get_heap_size()`; the external `vms` figure is not a substitute.

2. **`psutil.cpu_percent()` returns 0.0 on first call.** It is
   delta-based; the first sample is meaningless. Discard sample 0
   (or seed with a `cpu_percent(interval=None)` warm-up call before the
   child even starts). The first sketch did not mention this.

3. **`nix eval` may fork subprocesses** (flake fetches: git, curl). The
   first sketch implicitly assumed single-process. Default to
   `psutil.Process(pid).children(recursive=True)` aggregation and document
   the resulting RSS as "process-tree total". Otherwise we under-report on
   workloads that need a network fetch.

4. **Off-the-shelf `psrecord` is unmaintained-adjacent.** The first sketch
   called it "mature". Closer to: stable in maintenance mode since 2022.
   Fine for a one-day spike, not a long-term basis.

5. **`samply` was missing entirely from the first sketch.** It is the
   strongest off-the-shelf macOS option for stack samples + RSS together.
   Promoted to "Phase 2 complement" below.

6. **Matplotlib's closure cost is real**, not negligible: ~200 MB nix
   closure with numpy + freetype + Pillow transitively. Fine inside the
   bench devShell; do not assume it can be added to the main project
   devShell without weighing the cost.

7. **CPU% can exceed 100% under Boehm.** If libgc was compiled with
   `--enable-parallel-mark`, GC bursts can run multi-threaded and the
   sampler will report >100%. Symmetric across TW and v3 (both use the
   same libgc), so comparability holds — but the y-axis must be allowed
   to exceed 100%.

8. **Stderr-line timestamping was missing.** `V3_TIMING` emits phase
   boundaries that should appear as vertical markers on the curve. The
   sampler thread already has a wall-clock; tagging stderr lines with the
   same clock is ~10 LoC.

9. **N-run aggregation strategy was unstated.** A single trace is
   anecdotal. Default to median trace + IQR ribbon over N≥5 runs (mirrors
   `bench.py`'s default).

10. **Sub-100 ms workloads can't be traced usefully.** At 50 ms sampling,
    a 100 ms eval produces ~2 useful samples. Below ~500 ms, this tool is
    the wrong tool — `bench.py` is. State explicitly.

11. **Disk-cache effect not addressed.** v3 caches compiled bytecode in
    `~/.cache/nix/v3-cache.sqlite`. First-run vs warm-cache traces differ
    dramatically (a `lower+compile` phase appears in cold runs and
    disappears in warm). The tool must support `--cold` (clear cache
    between runs) and **default to clearing**, since cold is the more
    honest baseline.

12. **Thermal throttling.** Long-running benches on macOS laptops can hit
    thermal limits; the trace then looks like an early CPU plateau
    followed by step-down. Note in interpretation guidance; optionally
    record start/end temps from `pmset -g therm` if available.

13. **Time-axis alignment unspecified.** TW finishes in T₁, v3 in T₂≠T₁.
    Provide both views: absolute (overlay starting at t=0; v3's curve
    extends further) and normalized to [0,1] (peak shapes line up).
    Default absolute; normalize on flag.

## Recommended design

### File layout

```
src/libexpr-v3/bench/
├── bench.py                       # existing scalar harness — unchanged
├── perf-trace.py                  # NEW: time-series sampler + plotter
├── workloads.toml                 # shared with bench.py
└── samples/
    └── 2026-05-21/
        ├── hello-name.jsonl       # per-run trace (one line per sample)
        ├── hello-name.svg         # rendered overlay
        └── index.html             # gallery linking the SVGs
```

Inside `v3-eval` (small companion):

```
src/libexpr-v3/heap_trace.{hh,cc}  # NEW: pthread sampler for GC_get_heap_size
src/libexpr-v3/v3_eval_main.cc     # MODIFIED: start/join the sampler when
                                   #   NIX_V3_HEAP_TRACE=1
```

### Architecture

```
┌───────────────────────────────────────────────────────────────────┐
│  perf-trace.py                                                    │
│                                                                   │
│  ┌──────────────┐    spawn (env-gated mode)    ┌──────────────┐  │
│  │ main thread  │ ─────────────────────────────▶│ nix / v3-eval│  │
│  │ orchestrator │                                └──────────────┘  │
│  │              │       50 ms tick                                 │
│  │  ┌───────────┴────────┐                                         │
│  │  │ sampler thread     │ ◀── psutil.Process(pid).children(...)   │
│  │  │  cpu% rss vms thr  │                                         │
│  │  │  → in-mem deque    │                                         │
│  │  └────────────────────┘                                         │
│  │                                                                 │
│  │  ┌────────────────────┐    captures stderr line-by-line          │
│  │  │ stderr reader thr  │    with wall-clock timestamp,            │
│  │  │  V3_TIMING phases  │ ─ tags V3_TIMING + heap-trace lines      │
│  │  │  NIX_V3_HEAP_TRACE │                                         │
│  │  └────────────────────┘                                         │
│  │                                                                 │
│  │  on exit ── flush JSONL, render SVG, append to index.html       │
│  └─────────────────────────────────────────────────────────────────┘
```

### Sampling semantics

- Default cadence: 50 ms.
- Minimum cadence: 20 ms (`psutil` overhead becomes load-bearing below this).
- Aggregation: **process-tree total** (parent + children, recursive).
- Sampler thread runs as a daemon so a crashing main thread does not hang.
- First `cpu_percent()` is a warm-up call, discarded.
- Wall clock is `time.monotonic_ns()` for both psutil samples and stderr
  line tags, so the two streams share a single axis.

### Output formats

JSONL (one line per sample):
```json
{"t_ns": 12345678, "rss_b": 31457280, "vms_b": 8589934592,
 "cpu_pct": 98.2, "threads": 3, "boehm_heap_b": 402653184}
```

Plus one record per stderr-tagged event:
```json
{"t_ns": 23456789, "kind": "phase", "phase": "compile",
 "raw": "V3_TIMING lower=12 compile=84 run=412 bridge=0"}
```

SVG: three vertically-stacked subplots sharing the time axis —

1. CPU% (TW and v3-direct overlaid; median line + IQR ribbon across N runs).
2. Process-tree RSS in MB (same overlay treatment).
3. Boehm heap size in MB (v3 only; TW shown as flat "unmeasured" annotation).

Phase boundaries from `V3_TIMING` render as faint vertical guides; the
median end-of-run wall time is annotated at the right edge.

### Aggregation across N runs

For each (workload, mode) cell, run N times (default N=5). Resample each
trace to a common time grid (linear interpolation at sampler cadence),
then compute per-grid-point median + 25th/75th percentiles. Plot the
median as a solid line, IQR as a translucent ribbon. This is the standard
visualization for noisy time-series and reads correctly even for someone
who has not internalized it.

### In-process Boehm heap probe (~80 LoC, gated)

```
gate: NIX_V3_HEAP_TRACE — env-gated pthread sampler that emits
  "v3 heap-trace t_us=NNN heap=BBB free=CCC" lines to stderr at a fixed
  cadence (default 50 ms; override with NIX_V3_HEAP_TRACE_INTERVAL_MS).
  Retire when: in-process GC stats are exposed via a runtime API that
  external samplers can read without parsing stderr (i.e. when Stage 3
  nursery default-on lands and the GC has a stable telemetry surface).
```

Sampler thread is started inside `v3_eval_main.cc` before `runRootExpr`,
joined cleanly on exit or signal. The interval matches the external
sampler so traces align point-for-point.

### Make target

```
make perf-trace WORKLOAD=hello.name [MODES=tw,v3-direct] [N=5] [INTERVAL=50]
```

Outputs `src/libexpr-v3/bench/samples/$(date +%Y-%m-%d)/<workload>.{svg,jsonl}`
and updates the date-folder `index.html`.

## Open questions (decide before / during build)

1. **Matplotlib or hand-rolled SVG?** Default matplotlib; revisit if the
   devShell footprint becomes a problem.
2. **Default `--cold`?** Recommended yes (clear v3 disk cache between
   runs); `--warm` overrides. The "honest baseline" framing is what the
   action plan's measurement spike actually wants.
3. **Should the tool also drive `samply`?** Phase 2 work; emits a sibling
   stack-sample HTML and links it from the SVG caption.
4. **Where does `NIX_V3_HEAP_TRACE` live?** Inside `v3_eval_main.cc` only
   (path A) or also inside the `nix` CLI's v3-direct runner (path B)?
   Path B is needed for `nix eval --impure` traces; path A alone leaves
   that workload uninstrumented. Recommend: add to **both**, since the
   sampler is small and the cost is one shared `.cc`.
5. **Sub-process inclusion granularity.** Aggregate over the whole tree
   (recommended) or also emit per-PID series? Per-PID is more data and
   rarely needed; ship aggregate first, add per-PID on demand.
6. **Min wall floor.** For workloads <500 ms, the tool's signal degrades.
   Refuse to plot below a `--min-wall` threshold? Or plot with a banner?
   Recommend: plot with banner, since some interpretive value remains.
7. **Cross-machine reproducibility.** RSS curves differ between Apple
   silicon and x86_64. Note machine identity in the SVG footer (CPU
   model, RAM, OS version, libc, libgc version, nix HEAD sha).

## Rule 0 falsification anchor

Before the tool lands, name the hypothesis its first run will kill or
confirm:

> **Hypothesis to be killed or confirmed on first measurement**:
> *"v3-direct's force-rate gap on `hello.drvPath` is dominated by
> Boehm GC scan time (factor 2 of the
> `EXTEND_DERIVATION_INVESTIGATION_2026-05-18.md` decomposition); the
> CPU% curve shows long stretches of low-allocation high-scan and
> Boehm heap remains monotonically high during eval."*

Possible outcomes:

- **Boehm heap monotonically high throughout eval, CPU% shape matches
  GC-scan signature** → confirms factor 2 dominant → **commits Stage 3**
  (nursery default-on) as the highest-priority next step. Reduces
  ambiguity about how to spend the next 6 weeks.
- **Boehm heap drops between bursts**, GC is not the bottleneck →
  falsifies factor 2 dominance → forces re-decomposition; opcode
  dispatch (factor 1) or intermediate allocations (factor 3) take
  priority; **Stage 4 or IR Phase C jumps the queue over Stage 3**.
- **CPU% drops to near-zero in long stretches** with neither heap growth
  nor GC activity → falsifies "v3 is CPU-bound" entirely; the gap is
  somewhere outside CPU and Boehm — likely a system call (IFD,
  store-path resolution) we have not been tracking. **New top-level
  investigation opened.**

In all three outcomes the tool has paid for itself by closing a
multi-week ambiguity. If somehow none of the three apply, the tool
itself needs review (the sampler is wrong) and we file a meta-issue.

## Cost & sequencing

| Item                                                      | Cost      |
|-----------------------------------------------------------|-----------|
| `perf-trace.py` (sampler + JSONL writer + matplotlib SVG) | 1–2 days  |
| `heap_trace.{hh,cc}` + wire into `v3_eval_main.cc`        | 0.5 day   |
| Wire into `nix` CLI v3-direct path (path B above)         | 0.5 day   |
| Flake devShell: `bench` output with `python3.withPackages` (psutil, matplotlib) | 0.25 day |
| `make perf-trace` target + USAGE-equivalent prose in README of bench/ | 0.25 day |
| First Rule 0 measurement run on hello.drvPath             | same-day after landing |
| **Phase 2 (optional)**: `samply` integration, HTML index polish | 0.5 day |

**Total to first Rule 0 measurement**: ~3 working days.

This is small enough to land before the Phase 1.5 measurement spike's
2-week deadline trips its kill criterion.

## Where this slots in

- **ACTION_PLAN_2026-05-15.md Phase 1.5**: this tool is the instrument for
  the "drvPath/outPath force-rate decomposition profile" TODO (Phase 1.5
  bullet 5). Without it that TODO is non-executable.
- **ROADMAP_TO_VISION_2026-05-15.md cross-stage cadence**: this is
  instrumentation, not a stage; it sits parallel to Stages 1–9 and
  is referenced from "Current state benchmark" + "Cross-references".
- **LESSONS_LEARNED_2026-05-15.md §4.9 Item 5**: this closes the
  "documented CPU-profile workflow" gap. Item 5 should be updated to
  point here as the workflow.
- **EXTEND_DERIVATION_INVESTIGATION_2026-05-18.md** decomposition is
  unverifiable without this tool; the doc lists 4 factors but only
  factors 1 and 3 have existing instrumentation.

## What this is not

- **Not a stack profiler.** For "which function/opcode/primop took the
  time", `samply` (or eventually a v3-native sampler) is the answer.
  This tool answers "how did wall-time spend itself across CPU and
  memory pressure"; it is necessary but not sufficient for hot-path
  attribution.
- **Not a microbenchmark replacement.** `bench.py` stays the harness for
  ratio/p50/p95 reporting and CI regression flagging.
- **Not a fuzzer or correctness oracle.** Differential testing
  (Item 2 of the debug story) is unchanged.

## Copyright

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0.
