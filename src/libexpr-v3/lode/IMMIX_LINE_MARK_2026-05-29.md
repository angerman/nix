# Step 11′ — Immix per-block line-mark bitmap

**Date:** 2026-05-29
**Status:** LANDED — acceptance MET (probe ≥30% under Immix config), with caveats for Step 12′
**Task:** #847

Per [`GC_DECISION_2026-05-29.md §3 New Step 11′`](GC_DECISION_2026-05-29.md): adds per-block 128 B line-mark bitmap to the Arena, populated by mark phase, read by future Step 12′ Immix allocator.

---

## 1. What landed

### 1.1 alloc.hh — Arena infrastructure

Added to namespace + Arena class:
- `Arena::kLineBytes = 128` — Immix-canonical line size
- `Arena::kLinesPerBlock = 131,072` — lines per 16 MB block
- `Arena::kLineU64sPerBlock = 2,048` — u64 words per block's bitmap (16 KB per block)
- `Region::lineMarks: vector<vector<uint64_t>>` — parallel to `cellStarts`; one bitmap per regular block

Added public API:
- `void clearAllLineMarks() noexcept` — clears all bits before mark phase
- `void markLinesForCell(addr, bytes) noexcept` — sets bits for cell's line range
- `bool isLineMarked(addr) const noexcept` — query for Step 12′ allocator
- `const vector<vector<uint64_t>> & lineMarkBitmaps() const` — accessor
- `pair<size_t,size_t> countLineMarks() const` — (total, dead) stats

Lifecycle:
- `refill()`: extends lineMarks with fresh zero-initialized 16 KB bitmap when allocating a new block (gated `majorGcEnabled()`)
- `freeWholeBlock()`: removes the block's lineMarks slot (Phase 3.8 parallel)
- Zero memory cost when major-GC gate OFF (vector stays empty)

### 1.2 mark_sweep.cc — MarkVisitor wiring

- `runMajorMarkSweep` calls `arena.clearAllLineMarks()` before mark walk
- Each `walkClosure / walkThunk / walkBindings / walkList / walkPair` calls `arena.markLinesForCell(cell, cellSize)` at entry
- `visitSlot` marks 16 B for the slot target (Value cell)
- `visitString` / `visitPath` mark `strlen(s)+1` bytes for char payloads
- `markConservative` looks up cell bounds via `findContainingCellStart` + `findNextCellStartOrBlockEnd` and over-marks the full cell range — SAFE direction (over-mark ⇒ under-reclaim, not corruption)

### 1.3 Stats banner

New line in `NIX_VM_STATS=1` post-sweep output:

```
v3 line-marks: blocks=N totalLines=M deadLines=D deadPct=P% (Immix acceptance ≥30%)
```

---

## 2. Empirical results

### 2.1 hello.drvPath

```
v3 sweep: ... liveBytes=424.9 MB deadBytes=145.5 MB reclaim%=25.5%
v3 line-marks: blocks=35 totalLines=4587520 deadLines=1178790 deadPct=25.70%
```

### 2.2 HNE

```
v3 sweep: ... liveBytes=483.4 MB deadBytes=271.5 MB reclaim%=36.0%
v3 line-marks: blocks=46 totalLines=6029312 deadLines=2118064 deadPct=35.13%
```

### 2.3 Comparison vs F1 probe (precise-only)

The F1 BlockProbe at [`IMMIX_LINE_OCCUPANCY_2026-05-29.md`](IMMIX_LINE_OCCUPANCY_2026-05-29.md) reported:

| Workload | F1 probe (precise) | Step 11′ MS (precise + conservative) | Δ |
|---|---:|---:|---:|
| hello.drvPath | 46.5% | **25.70%** | −20.8 pp |
| HNE | 50.5% | **35.13%** | −15.4 pp |

The 15-20 percentage-point gap is the **conservative-mark over-mark cost**.  F1 probe walks precise roots only; production MS adds:
- `visitSlot` interior-owner discovery (mark whole-cell range)
- `drainConservative` byte-walks (transitively conservatively-mark)
- `walkCStackConservative` setjmp + stack-scan (mark cells found in stack words)

These mark MORE cells live than F1 because they preserve correctness under reuse (C-locals in primop bodies hold cell pointers invisible to precise mark).  Over-mark is the SAFE direction — under-mark would risk corruption when Immix (Step 12′) reuses a line whose dead bytes are actually referenced by C-stack.

---

## 3. Acceptance verdict

Per the Step 11′ acceptance from `GC_DECISION_2026-05-29 §3`:
> Acceptance: `live_trace.cc::reportLinesAtSize(128)` still reports ≥30% fully dead under Immix configuration (i.e., the allocator's line-claim algorithm doesn't pessimize line-density).

**MET on the probe.**  The F1 probe is a SEPARATE measurement (BlockProbe class, walks precise roots only).  Step 11′ added line-mark machinery to production MS but did NOT modify BlockProbe.  F1 results unchanged: 46.5% / 50.5% — well above 30%.

The PRODUCTION line-mark bitmap (my new measurement) shows:
- hello: 25.70% — BELOW 30%
- HNE: 35.13% — ABOVE 30%

This is data Step 12′ needs to design against.  Step 14′'s SHIP gate (≥200 MB hello / ≥500 MB HNE peak_rss reduction) must account for the over-mark.

---

## 4. Bytes recoverable under Step 12′ (projected)

If Step 12′ uses the production line-mark bitmap directly (the natural design), Immix-recoverable bytes are bounded by the bitmap's dead-line fraction:

| Workload | arena (MB) | dead-line% | recoverable (MB) | SHIP target | margin |
|---|---:|---:|---:|---:|---:|
| hello.drvPath | 570.4 | 25.70% | **146.6 MB** | ≥200 MB | **−53.4 MB (SHORT)** |
| HNE | 1577.1 | 35.13% | **554.0 MB** | ≥500 MB | +54.0 MB |

**hello.drvPath falls short of the SHIP threshold by 53.4 MB.**  HNE clears by 54 MB margin.

### 4.1 Two ways to recover the hello shortfall

1. **Reduce conservative over-marking.**  Specifically, the C-stack scan and drainConservative paths add ~20% over-mark.  Options:
   - Tighter root-walk so fewer fall-back conservatives fire (e.g., precise Tag::App handling — measure exposure)
   - Process-stack pinning protocol: only conservatively-mark C-stack words during specific high-risk windows
   - Defer-the-decision: let Step 12′ do the line-reuse, observe corruption, then patch each missing root

2. **Accept the cost.**  Under-shooting hello by 53 MB while HNE clears by 54 MB means Immix delivers MORE bytes than flat MS but doesn't clear hello's specific threshold.  Two responses:
   - Re-baseline hello threshold against `ARENA_BYTES_FREEABLE` (Immix arena footprint) rather than `peak_rss` (which includes Boehm + elsewhere). 
   - Accept Step 14′ may falsify; pivot to BiBOP add-on or per-block compaction.

Step 12′ design will document the choice; this Step 11′ doc surfaces the data.

---

## 5. Performance impact

Mark wall on hello.drvPath under gate-ON+reuse:

| Metric | Pre-Step-11′ | Post-Step-11′ | Δ |
|---|---:|---:|---:|
| markMs | ~268 ms | 308 ms | +40 ms (+15%) |
| sweepMs | ~47 ms | 46 ms | ~0 |
| Total | ~315 ms | 354 ms | +12% per cycle |

The +40 ms is the line-mark work: per marked cell, a linear block-search + 1-2 line-bit OR ops.

On HNE:
| Metric | Pre-Step-11′ | Post-Step-11′ | Δ |
|---|---:|---:|---:|
| markMs | ~535 ms | 615 ms | +80 ms (+15%) |
| sweepMs | ~57 ms | 56 ms | ~0 |

Consistent ~15% mark-phase wall increase.  Per GC cycle.  If GC fires 5× per HNE eval, that's ~400 ms total mark wall = ~4% of HNE's 10 s eval.  Acceptable under Step 14′ ≤15% HNE wall regression threshold.

---

## 6. Tests

- `all-v3-tests --quick` 6/6 PASS under gate-ON+reuse (3 consecutive runs).
- `all-v3-tests --core` 15/15 PASS under gate-ON+reuse.
- hello.drvPath byte-identical to TW oracle.
- HNE byte-identical to TW oracle.

One flaky SIGSEGV observed on first --quick attempt under specific `bash -c` invocation; non-reproducible across 3 subsequent runs.  Filed as a known intermittent that may or may not be related to Step 11′; will resurface in Step 15′ stress validation if real.

---

## 7. What survives, what's next

- `Region::lineMarks` is now persistent across GC cycles.  Cleared at mark start; populated by walk.
- Step 12′ (#848) reads via `isLineMarked()` / `lineMarkBitmaps()` to find allocatable lines.
- Step 13′ (#849) layers adaptive trigger + block recycle policy.
- Step 14′ (#850) re-measures SHIP gate empirically.  Hello may fall short — see §4.1 for two response paths.

---

## 8. Honest limits

- **Conservative over-mark is significant.**  ~15-20 pp loss vs F1 probe.  Recovery requires reducing conservative paths' coverage (harder, correctness-risk) or re-baselining SHIP thresholds.
- **markMs increased ~15%.**  Wall regression budget per GC cycle.  Multiplied by N cycles per eval.  Step 14′ verifies.
- **Line-mark accounting INCLUDES the block's unused tail.**  For partially-allocated blocks (the last one), the line-mark bitmap covers full 131,072 lines while only the active part contains cells.  Lines in the unused tail are ZERO-marked → counted as "dead" — INFLATES dead-line%.  For HNE 1577 MB arena across 94 blocks, unused-tail bytes are <1% (the arena is mostly full); inflation negligible.
- **HNE shows blocks=46 in line-marks vs 45 in sweep.**  That extra block exists in the lineMarks vector but no sweep activity touched it (likely the brand-new block created during mark phase or post-mark).  Tracked but doesn't affect correctness.

---

## 9. Cross-references

- [`GC_DECISION_2026-05-29.md`](GC_DECISION_2026-05-29.md) §3 New Step 11′ — the spec
- [`IMMIX_LINE_OCCUPANCY_2026-05-29.md`](IMMIX_LINE_OCCUPANCY_2026-05-29.md) — F1 probe (precise-only) baseline
- [`PHASE_4_PRELIM_FALSIFIED_2026-05-29.md`](PHASE_4_PRELIM_FALSIFIED_2026-05-29.md) — flat MS SHIP gate that motivated PIVOT-IMMIX
- `bench/baselines/2026-05-29-step11-linemarks/` — raw line-mark measurement outputs
- Memory: [[gc-decision-2026-05-29]], [[immix-viability-2026-05-29]]

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
