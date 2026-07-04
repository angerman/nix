# Observability audit — how v3 measures CPU, wall, live bytes, and memory-over-time

**Date:** 2026-06-03
**Status:** AUDIT + TOOLING. A complete, code-grounded inventory of every
in-process CPU/wall/memory measurement in the v3 VM, plus the external
harness and charting tooling. Three-agent fan-out (CPU·wall / memory·live
/ harness·charting), reconciled and spot-verified against code. Ships two
deliverables that close the two biggest gaps (§5).
**Author:** session synthesis (code-grounded at `33654b071`; agent findings
re-verified at `alloc.hh:81-89`, `run.cc:640-672`, `live_trace.cc:1490-1734`,
`heap_trace.cc`).

Companion docs:
- [`PERF_TRACE_TOOL_DESIGN_2026-05-20.md`](PERF_TRACE_TOOL_DESIGN_2026-05-20.md) — the perf-trace.py design (now shipped)
- [`L_MEASUREMENT_GAP_2026-05-28.md`](L_MEASUREMENT_GAP_2026-05-28.md) / [`L_TIME_SERIES_DATA_2026-05-29.md`](L_TIME_SERIES_DATA_2026-05-29.md) — the L(t) sampler this audits
- [`POST_F4_MEMORY_PROFILE_2026-06-02.md`](POST_F4_MEMORY_PROFILE_2026-06-02.md) — the R0 numbers whose `v3_arena` label §4 corrects
- [`R2_4D_EVAC_WALL_PLAN_2026-06-03.md`](R2_4D_EVAC_WALL_PLAN_2026-06-03.md) — Lever 4 (GC cadence) needs the where-is-peak-over-time signal this audits

---

## 0. One-paragraph answer

**CPU time is essentially unmeasured** — every timer in the VM is wall-clock
(`std::chrono::steady_clock`); the only CPU read in the subtree is one
`getrusage` in the resource-limit cap-check, consumed solely to throw.
**Live bytes** are measured precisely but **end-of-run only**
(`NIX_V3_LIVE_TRACE` transitive reachability walk → freeable). **Over-time
tracking exists**: `bench/perf-trace.py` (external psutil sidecar →
JSONL + matplotlib SVG) works and has committed outputs, and there are
three in-process time-series gates. But the two best in-process signals had
defects — the richest (`LIVE_TRACE_PERIODIC` per-Tag CSV) had **no plotter**
and is safepoint-starved on deep evals; and **no in-process series carried
resident RSS or CPU**. This audit ships fixes for both (§5).

---

## 1. CPU / wall-time measurement

Default sink for almost all of these is **human-formatted stderr, once at
end-of-run** (mixed units ns/µs/ms/s); only the GC CSVs are machine-parseable.

| Gate | Code | Measures | Granularity | Series? |
|---|---|---|---|---|
| `V3_TIMING` | `run.cc:58-121` | **wall** lower/optimise/compile/run (+ import-timing breakdown) | 4 coarse phases | end-scalar |
| `NIX_VM_OPCOUNTS` | count `vm.cc:3030`; dump `run.cc:886-932` | dispatch counts (no time) | per-opcode | end-scalar |
| `NIX_VM_OPCYCLES` | `vm.cc:3008-3029`; dump `run.cc:934-978` | **wall ns** per opcode (⚠ NOT cycles — see §4) | per-opcode | end-scalar |
| `NIX_V3_DBG_RETURN_BREAKDOWN` | `vm.cc:5743…`; `run.cc:980-1019` | **wall ns** OP_RETURN sub-phases | per-return-phase | end-scalar |
| `NIX_VM_PRIMOP_TIME` | `vm.cc:9929-9948`; dump `primops.cc:8529` | **wall ns** per primop (inclusive) | per-primop-name | end-scalar |
| GC mark/sweep split | `mark_sweep.cc:1331-1483` | **wall ms** mark vs sweep | per-GC-cycle | per-cycle line |
| GC evac split | `mark_sweep.cc:1315-1326` | **wall ms** move / verify / munmap | per-GC-cycle | per-cycle line |
| **`NIX_V3_GC_CYCLE_CSV=path`** | `mark_sweep.cc:817-885` | markMs, sweepMs, bytesFreed, **wallSincePrev_ms** | per-GC-cycle | **CSV time-series** (only when `NIX_V3_MAJOR_GC=1`) |
| `NIX_V3_MAX_WALL_TIME` | `limits.cc:686-742` | **wall** elapsed (cap only) | polled / opcode | threshold (not emitted) |
| `NIX_V3_MAX_CPU_TIME` | `limits.cc:745-765` | **CPU** `ru_utime+ru_stime` (cap only) | polled / opcode | threshold (not emitted) |
| `V3_DBG_CALLFLAKE_TIMING`, `V3_DBG_DESERIALIZE`, `NIX_VM_CACHE_SITES` | various | **wall** sub-phase timing | per-phase | end-scalar |

---

## 2. Live-bytes / memory measurement

| Gate | Code | Counts (precise) | Granularity | Series? |
|---|---|---|---|---|
| **`NIX_V3_LIVE_TRACE`** | `live_trace.cc:388-513` | **precise-root transitive reachability** live bytes; `freeable = cumulative_alloc − live`; SHIP-GREEN/MARGINAL/FALSIFIED verdict. *(238 MB hello / 1.43 GB HNE / 6.22 GB M5 freeable.)* | per-Tag | **end-of-run only** |
| `NIX_VM_STATS` RSS banner | `run.cc:644-670` | `peak_rss`(=`ru_maxrss`, **peak**) / `boehm_heap` / `boehm_free` / `boehm_unmapped` / `v3_arena`(=`bytesAllocated()`) / `elsewhere` | whole-run scalar | end-scalar |
| `allocStats()` byte counters | fields `alloc.hh:485-492`; dump `run.cc:459-472` | **cumulative bump bytes** per Tag (`total_alloc`), monotonic | per-Tag | end-scalar |
| per-site ATTR | `alloc.hh:2871-3055`, dumps `:3354-4199` | per-(file:line) bytes/counts (`NIX_V3_{BINDINGS,THUNKS,CLOSURES,LISTS,PAIRS,STRINGS}_ATTR`) | per-alloc-site | end-scalar |
| `mergeBindings` #821 | `run.cc:488-580` | per-site bytes/calls + na/nb histograms | per-site | end-scalar |
| `NIX_V3_BLOCK_PROBE` | `live_trace.cc:1418-1485` | per-block fill ratio; Immix line-occupancy @ 64/128/256/512 B; evac sparse-block opportunity | per-block / per-line | end-scalar |
| **`NIX_V3_LIVE_TRACE_PERIODIC=K`** (+`_OUT`) | impl `live_trace.cc:1505-1734`; site `vm.cc:2932-2945` | **per-Tag LIVE MB + L_resident + L_cumulative + wall_ms**, full walk every **K MB of arena alloc** | per-Tag | **CSV time-series** (safepoint-gated — §4) |
| RSS read primitives | `limits.cc:89-110` (current resident), `:296-312` | mach `resident_size` / `/proc/self/statm` (current) vs `ru_maxrss` (peak) | scalar | — |

---

## 3. External harness + charting

| Tool | Path | Captures | Series? | Plots? |
|---|---|---|---|---|
| **perf-trace.py** | `bench/perf-trace.py` | CPU% + RSS + VMS + threads (psutil, 50 ms) + Boehm heap (parses the in-process stderr probe) | **time-series** | **matplotlib SVG** (3 panels, median+IQR) |
| **`NIX_V3_HEAP_TRACE`** probe | `heap_trace.cc` | (was Boehm heap/free/total only; **now** + rss + cpu_ms — §5) | **wall 50 ms series** | via perf-trace.py / plot-v3-memory.py |
| bench.py | `bench/bench.py` | wall + maxRSS + V3_TIMING + dispatch counts | scalar-per-run | no (tables/CSV/json) |
| m5-cron.sh | `bench/m5-cron.sh` | wall + peak RSS + drvPath, **append-only ledger** | scalar-per-run | no (TSV ledger) |
| bench-v3-vs-tw.sh, measure-peak-noise-floor.sh, workload-heterogeneity.sh | `test/`, `bench/` | wall / peak_rss σ-envelope | scalar-per-run | no |

Committed proof perf-trace.py is real (not design-only):
`bench/samples/2026-05-20/hello-drvpath.svg` (105 KB) + JSONL with
~150 heap events/run + `-summary.md` (TW p50 649 ms vs v3 10204 ms).

---

## 4. Corrections (verified against code — these supersede looser claims)

1. **`v3_arena` is NOT "cumulative bump bytes."** `run.cc:657` prints
   `threadArena().bytesAllocated()` = mapped arena **block** bytes (16 MB
   granular), which **decrements under GC/evac**. The genuinely-cumulative
   number is the separate `total_alloc` line (`allocStats().bytes*`). The
   M5 "`v3_arena cumul = 7197` vs resident 4686" gap in `POST_F4_MEMORY_PROFILE`
   is **mapped-blocks vs resident** (macOS compresses cold pages), not
   cumulative-vs-peak — that doc's label should be corrected.
2. **`peak_rss` is `ru_maxrss` = run high-water**, not current resident.
   The watchdog's *current*-RSS read (mach `task_info`, `limits.cc:296-312`)
   is a different primitive.
3. **`NIX_VM_OPCYCLES` is mislabeled** — wall-ns per opcode (steady_clock,
   ~10-20 ns self-overhead), **not** hardware cycles. There is **no rdtsc /
   `__builtin_readcyclecounter` anywhere**; true cycles/IPC are unobtainable
   in-process.
4. **`-DV3_RELEASE` zeroing is real but LATENT, not active.** `alloc.hh:81-89`
   no-ops every counter bump under `V3_RELEASE` — **but the checked-in build
   config never defines it** (no match in `Makefile.config`/`config.status`).
   So the rich counters work in today's build; this only bites a future
   release build. (An agent overstated this as a current reality.)
5. **perf-trace.py's Boehm-heap panel is nearly useless.** `GC_get_heap_size`
   sits at ~403 MB / 99.9% free on hello — v3's real memory is the
   malloc-backed `threadArena` (the `elsewhere` bucket), invisible to Boehm.
   The honest memory-over-time signal is **resident RSS**, not Boehm heap.
6. **`LIVE_TRACE_PERIODIC` is safepoint-starved.** Sampled only at
   `exitDepth==0` (`vm.cc:2933-2944`, same constraint as the major-GC
   trigger); deep getFlake/M5 evals rarely unwind there, so M5 yields ~2
   samples. Usable for hello/HNE; **not** for M5 — the very workload the
   R2.4d cadence lever needs it for.

---

## 5. What this audit SHIPPED (the two biggest gaps closed)

### (c) Unified in-process series — `heap_trace.cc` now emits RSS + CPU
The `NIX_V3_HEAP_TRACE` sampler (50 ms wall thread) previously carried only
Boehm heap. It now also reads **current resident RSS** (mach `resident_size`
/ `/proc/self/statm`, mirroring `limits.cc::currentRssBytes` — current, not
peak) and **cumulative CPU ms** (`getrusage` user+sys). New line format
(`rss`/`cpu_ms` **appended** so perf-trace.py's 4-field regex is unchanged):

```
v3 heap-trace t_us=… heap=… free=… total=… rss=… cpu_ms=…
```

This makes a bare `NIX_V3_HEAP_TRACE=1 v3-eval …` run emit a full
CPU+RSS+heap time-series to stderr with **no external sidecar** — the
unified in-process series that didn't exist before. Single-file edit
(`heap_trace.cc` + its header doc) = minimal conflict surface. CPU% is
recovered by differentiating `Δcpu_ms/Δwall`.

### (b) Plotter for the richest signal — `bench/plot-v3-memory.py`
The per-Tag `LIVE_TRACE_PERIODIC` CSV (the only decomposed
where-is-memory-going-over-the-run data) had **no consumer** — the flush
banner literally told you to write your own Python. New plotter:
- `--live-periodic <csv>` → **stacked per-Tag live MB** (Bindings/Thunks/
  Closures/Lists/Pairs) + a total line, plus an **L(t)** panel
  (`L_resident`/`L_cumulative`); x-axis `alloc` (default) or `wall`. Labels
  the sample count and warns when sparse (the §4.6 safepoint caveat).
- `--heap-trace <log>` → resident RSS vs Boehm heap + CPU% (from the §5(c)
  fields; gracefully handles legacy 4-field logs).
- matplotlib Agg → SVG, graceful skip if matplotlib absent, psutil-free
  (reads files). Verified end-to-end (100 KB SVG, 4 populated panels).

---

## 6. Gaps that remain (ranked)

1. **`LIVE_TRACE_PERIODIC` safepoint-starvation is unfixed** — the data
   source is still sparse on M5. Options: (a) sample on a byte-threshold at
   a *shallower* hook, accepting C-stack-unsafe-walk risk only for the
   read-only LiveTracer; (b) accept RSS (now in the heap-trace series via
   §5c) as the M5 over-time proxy and reserve per-Tag decomposition for
   hello/HNE. The R2.4d cadence lever can use the §5(c) RSS series today.
2. **No Makefile / USAGE entry point** for perf-trace.py or plot-v3-memory.py;
   `psutil`/`matplotlib` unpinned (the smoke render used
   `nix-shell -p 'python3.withPackages(ps:[ps.matplotlib])'`). A
   self-documenting make target + a pinned bench python env would make these
   discoverable.
3. **No CPU-vs-wall split or per-optimizer-pass timing.** The 15 `opt_*.cc`
   passes have zero clock reads (one `optimise=` bucket). CPU% from §5(c) is
   whole-process, not per-function — a sampling profiler (`samply`/`perf`)
   wrapper is still unbuilt (only one-off `/usr/bin/sample` artifacts exist).
4. **No wall field in the main `NIX_VM_STATS` banner** — RSS and wall live
   under different gates; you can't get both from one invocation.

---

## 7. Cross-references

- [[perf-trace-tool-design-2026-05-20]] — the sidecar design (shipped)
- [[l-measurement-gap-2026-05-28]] / [[l-time-series-data-2026-05-29]] — the L(t) sampler
- [[post-f4-memory-profile-2026-06-02]] — the `v3_arena` label §4.1 corrects
- [[r2-4d-evac-wall-plan-2026-06-03]] — Lever 4 consumer of the §5(c) RSS series
- [[memory-first-class]] — RSS-primary framing
- [[head-5-counter-trap]] / [[same-host-bisect]] — measurement-discipline siblings
- Code: `heap_trace.cc` (§5c), `bench/plot-v3-memory.py` (§5b),
  `live_trace.cc:1490-1734` (periodic CSV), `run.cc:640-672` (NIX_VM_STATS),
  `alloc.hh:81-89` (V3_RELEASE), `vm.cc:2932-2945` (safepoint gate)

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
