# "Ditch Boehm" as a PERF project — FALSIFIED — 2026-05-27

## REFRAMING 2026-05-27 (user clarification)

The user's intent for GC work is **memory usage**, not wall time. This document falsified the WALL premise (Boehm GC = 0 ms on M5 + hello), which was the original IDEAL_GC_DESIGN motivator. With memory as the primary axis, the 400 MB Boehm-reserved heap IS the headline target — well above the `[[memory-first-class]]` 50 MB SHIP threshold. The "ditch Boehm" project remains live as a MEMORY project.

The reframing changes nothing measurement-wise — Boehm still consumes 0 ms wall, the 400 MB heap is still 99.4% free at peak. But the project's SHIP gate is now `[[memory-first-class]]` peak-RSS reduction, not wall improvement.

User chose precise-root foundation (~1-2 wk) as the next step. See `GC_PRECISE_ROOT_FOUNDATION_2026-05-27.md` for the survey + decomposition.

---


**Date:** 2026-05-27 (morning, after M5 cron + M5 primop profile)
**Status:** FALSIFIED via measurement-first spike — wall-perf premise does not hold; memory + architectural cases remain but require different framing
**Companion docs:** [`IDEAL_GC_DESIGN_2026-05-26.md`](IDEAL_GC_DESIGN_2026-05-26.md) (original design), [`BOEHM_DEPENDENCY_2026-05-21.md`](BOEHM_DEPENDENCY_2026-05-21.md) (dependency audit), [`GC_BUILD_VS_BUY_2026-05-21.md`](GC_BUILD_VS_BUY_2026-05-21.md) (Whippet vs hand-roll), [`IFD_S4_FALSIFIED_2026-05-27.md`](IFD_S4_FALSIFIED_2026-05-27.md) (same measurement-first pattern this turn)

## Headline

Boehm GC consumes **0 ms total** on cardano-node M5 (1 collection over a ~30 s eval). hello.drvPath: same. **There is no wall to recover by replacing Boehm.**

Per `[[measure-twice-cut-once]]` + `[[falsification-rule]]`: the multi-month "ditch Boehm" project as a perf-improvement effort cannot ship — there is nothing to measure as a SHIP gate.

## Measurement

Added to `run.cc` (this commit):
```cpp
GC_word boehmGcNo = GC_get_gc_no();
unsigned long boehmGcMs = GC_get_full_gc_total_time();
```

Reports under `NIX_VM_STATS=1`:
```
v3-direct boehm: gc_count=N gc_total_ms=M
```

### M5 result

```
v3-direct memory: peak_rss=3772.1MB boehm_heap=402.9MB boehm_free=400.5MB v3_arena=6392.1MB elsewhere=0.0MB
v3-direct boehm: gc_count=1 gc_total_ms=0
```

29.57 s wall. 1 GC collection. 0 ms total time in GC.

### hello.drvPath result

```
v3-direct memory: peak_rss=753.5MB boehm_heap=402.9MB boehm_free=402.8MB v3_arena=587.2MB elsewhere=0.0MB
v3-direct boehm: gc_count=1 gc_total_ms=0
```

Same pattern: 1 GC cycle, 0 ms.

## Why Boehm GC is essentially free on these workloads

1. **v3's main allocator is `threadArena()` (per-thread bump allocator), NOT Boehm.** The v3 arena holds closures, thunks, Bindings, lists, pairs — the bulk of allocation. M5 v3 arena = 6.4 GB; Boehm heap = 0.4 GB (1/16 of arena).

2. **Boehm is only used at the FFI boundary** — when v3 returns a Value to TW (`v3ToTreeWalker`). The bridge telemetry shows 80 bridges / 1.3 ms wall on M5 (`[[bridge-telemetry-2026-05-26]]`). The Boehm-allocated bridge values are SMALL and FEW.

3. **The 402.9 MB Boehm heap is RESERVED**, not USED. `boehm_free=400.5 MB` means 99.4 % of the heap is empty. Boehm has nothing to collect because nothing is being allocated into it.

4. **Without allocation pressure, Boehm's incremental collector doesn't run.** 1 collection at process exit (probably triggered by atexit or by hitting a generation threshold) is the only GC cycle.

## What ditching Boehm would and would not deliver

| Dimension | Original claim | Measured reality | Verdict |
|-----------|---------------|------------------|---------|
| **Wall improvement** | "few %" (per BOEHM_DEPENDENCY §3) | **0 ms / 0 %** | FALSIFIED |
| **Memory peak** | "400 MB reserved heap" | 402.9 MB confirmed | REAL but small relative to v3 arena (6.4 GB on M5) |
| **Architectural foundation** | precise-root + moving GC + generational | not measurable; structural | TRUE but multi-month |
| **AOT/snapshot reliability** | "Boehm conservative scan fragile" | not measured | UNVERIFIED |

The wall claim is the dominant motivator in IDEAL_GC_DESIGN §"Why hand-roll" and "Why Whippet". Falsifying the wall claim means the multi-month engineering investment lacks a measurable SHIP gate.

## What this DOES NOT mean

- **NOT** that Boehm is fine forever. The architectural-foundation case (precise roots + moving GC) is real but bounded by Stage 13 (parallel eval) or R6 (Whippet trigger) firing.
- **NOT** that Boehm's 400 MB peak RSS is acceptable. It's a real memory tax. The right framing: BOEHM TUNING spike to recover some of it.
- **NOT** that the IDEAL_GC_DESIGN analysis was wrong. It was based on speculation about GC cost; the measurement settles the speculation.

## Recommended bounded next step: Boehm tuning spike (~1 day)

Per `[[memory-first-class]]`: a 50 MB peak RSS reduction with zero wall regression ships. Boehm tuning candidates:

1. **`GC_set_initial_heap_size`** — the 402.9 MB initial reservation is over-allocated for small workloads. Lowering the initial size + letting Boehm grow on demand could save ~300 MB on hello.drvPath at zero wall cost.
2. **`GC_set_no_dls(1)`** — disables dynamic-library symbol scanning. On macOS this may save startup time + RSS pages.
3. **`madvise(MADV_DONTNEED)` on unused Boehm pages** — release the 400 MB free pages back to the OS so they don't count toward peak RSS.
4. **`GC_set_full_freq(N)`** — adjust full-collection frequency. Currently default; could be tuned for v3's known shape (few FFI bridges per eval).

**Pre-committed SHIP threshold** (per [[threshold-recalibration-rule]]):
- SHIP if ≥50 MB peak RSS reduction with ≤2 % wall regression on hello.drvPath + M5
- TUNE if 20-50 MB reduction
- REVERT WITH DATA if <20 MB reduction

The tuning spike is bounded, falsifiable, and measurement-driven. NOT the same as ditching Boehm.

## Architectural foundation work (separate, deferred)

IDEAL_GC_DESIGN_2026-05-26.md §"No-regret foundations to START NOW" lists work that's valuable INDEPENDENT of the wall claim:
- Precise root infrastructure (~1-2 wk; tag bits + emit.cc stack maps + GC_ROOT macros + STL helpers)
- Phase E v0.2 missed-root fix (~1-3 d)
- Selective nursery (~2-3 d)
- T1.3 allocation-site attribution (~1-2 d)

These are bounded foundation steps that could ship per their own thresholds. None requires "ditching Boehm" as a prereq.

## Pre-committed retirement criterion (per [[falsification-rule]])

For "ditch Boehm" as a wall-perf project to revive:
- Workload measurement showing Boehm GC total time ≥3 % of wall (currently: 0 %)
- AND independent evidence that the Boehm collection cost would be eliminated by the replacement (not just shifted to a different GC's overhead)

Neither condition is currently met. Closing the project as completed-falsified.

## What this lands in this commit

- Measurement instrumentation (`run.cc`): `boehmGcNo` + `boehmGcMs` reported under `NIX_VM_STATS=1`
- This writeup (`GC_DITCH_BOEHM_FALSIFIED_2026-05-27.md`)
- Memory entry `[[gc-ditch-boehm-falsified-2026-05-27]]`

No code changes to GC infrastructure. No commitment to Boehm tuning spike (that's a separate decision).

## Cross-references

- [[falsification-rule]] — Rule 0; cheap spike preceded multi-month commitment
- [[measure-twice-cut-once]] — anti-pattern (premise speculation) avoided
- [[threshold-recalibration-rule]] — applied here: original "few %" wall claim corrected to "0%" via measurement
- [[bridge-telemetry-2026-05-26]] — supports premise: FFI surface is sub-millisecond
- [[ifd-s4-falsified-2026-05-27]] — same measurement-first pattern this turn
- [[m5-primop-profile-2026-05-27]] — M5 wall is in OP_TAIL_CALL + OP_CALL_PRIMOP + OP_ATTRS_SELECT_DYN, not in GC
- [[memory-first-class]] — Boehm tuning spike framed under this rule
- lode/IDEAL_GC_DESIGN_2026-05-26.md — original design (wall premise now falsified; memory + foundation cases remain)
- lode/BOEHM_DEPENDENCY_2026-05-21.md — dependency audit
- lode/GC_BUILD_VS_BUY_2026-05-21.md — Whippet vs hand-roll (decision deferred since premise is falsified)

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
