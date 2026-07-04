# Diagnostic infrastructure audit — why GC isn't shipping + what's missing

**Date:** 2026-05-29
**Author:** session synthesis (3-agent deep audit + critical review + code verification)
**Status:** STRATEGIC AUDIT — replaces the implicit assumption that "we have enough diagnostics"
**Triggering question:** "Why is our GC not working as expected? Do we have good enough fine-grained tracking for wall-time/cycle attribution, allocation attribution, and lifetime tracking?"

Companion docs:
- [`PHASE_4_PRELIM_FALSIFIED_2026-05-29.md`](PHASE_4_PRELIM_FALSIFIED_2026-05-29.md) — what failed today
- [`GC_DECISION_2026-05-29.md`](GC_DECISION_2026-05-29.md) — the PIVOT-IMMIX decision under audit here
- [`EXIT_GC_SPIRAL_PLAN_2026-05-29.md`](EXIT_GC_SPIRAL_PLAN_2026-05-29.md) — proposed pause
- [`L_TIME_SERIES_DATA_2026-05-29.md`](L_TIME_SERIES_DATA_2026-05-29.md)
- [`STAGE_6_FALSIFIERS_RESULT_2026-05-29.md`](STAGE_6_FALSIFIERS_RESULT_2026-05-29.md)
- [`NIX_PROFILER_DESIGN_2026-05-21.md`](NIX_PROFILER_DESIGN_2026-05-21.md) (Stage 15 wall)
- [`NIX_MEMORY_PROFILER_DESIGN_2026-05-27.md`](NIX_MEMORY_PROFILER_DESIGN_2026-05-27.md) (Stage 15 memory sibling)
- [`MEASURE_TWICE_CUT_ONCE_2026-05-23.md`](MEASURE_TWICE_CUT_ONCE_2026-05-23.md) §5.7-§5.8

---

## 1. TL;DR — verified headline finding

**The F2 sweep-cost verdict that pivoted the team from flat MS to Immix was a PROJECTION, not a measurement.** Verified at [`live_trace.cc:818`](../live_trace.cc):

```cpp
const double projectedSweepMs = markMs;  // O(N_live) too
```

`projectedSweepMs = markMs` is literally a copy. The "40-54% sweep wall" claim is the **mark wall doubled, then printed as if it were the sweep cost**. Meanwhile real `markMs` and `sweepMs` ARE computed at [`mark_sweep.cc:868-880`](../mark_sweep.cc) from separate timestamps and emitted under `NIX_VM_STATS` — but the F2 verdict consumed the projection-banner, not the real-measurement banner.

**Six GC falsifications, six aggregate measurements treated as structural ground truth.** This is the meta-pattern that's hidden the underlying cause for 3 weeks. The team has world-class instrumentation (20+ env-var-gated systems across allocation + wall + GC); none of it produces **per-cycle distributions** or **per-source-position attribution** by default.

**The data needed to make grounded GC decisions LIVES IN HEAP STATE** (PosIdx fields populated at every Bindings alloc; mark_sweep.cc timing locals; per-Tag live bytes during BFS walk). The dumpers / aggregators that would consume it are mostly **not built**.

**Single most important action:** before committing to Immix or any further GC redesign, build the **per-cycle GC CSV** (~half a day) and **per-PosIdx live-bytes rollup** (3-5 days). These re-grade the current pivot decision and convert "GC is the lever" from a Tag-level pattern-match into a source-level measurement.

---

## 2. Why GC isn't working as expected — root cause synthesis

### 2.1 The mechanical reason: bytes freed don't translate to RSS

Per Agent 1's deep code-read of `mark_sweep.cc` cross-referenced with `PHASE_4_PRELIM_FALSIFIED §2.2`:

> v3_arena unchanged at 587.20 MB

The flat MS implementation IS marking and sweeping correctly. Cells are being identified as dead and added to `freeListBins_`. **But:**

- Dead cells go into `unordered_map<size_t, vector<void*>> freeListBins_` ([`alloc.hh:1854`](../include/v3/alloc.hh)) — they remain at their original arena address
- The OS does not unmap pages until WHOLE blocks are freed
- Block-level free requires all cells in the block to be dead simultaneously
- For v3's allocation pattern (Bindings dominate 84%, persistent through propagation), blocks are densely live; whole-block-free fires rarely

**Net result:** flat MS reclaims bytes LOGICALLY (free-list bookkeeping) but not PHYSICALLY (RSS doesn't drop). This is why the +21.88 MB regression occurs: bookkeeping overhead (cell-start bitmap, line-mark bitmap, transient BitmapMarker, freeListBins) adds RSS while the supposed reclamation lives only in pointer arithmetic.

### 2.2 The same will happen to Immix

Per Agent 1: "Immix wins WALL (cheaper allocator); does NOT win PEAK RSS without defragmentation." Same OS-unmap mechanic.

- Immix line-mark bookkeeping is *more* overhead than flat MS (cell-start bitmap + line-mark bitmap + freeSpans vector)
- Immix's "whole-block-free" requires all lines in the block to be empty, which has the same density problem as flat MS's cell-density problem
- Defragmentation (Immix's evacuation feature) would help but is deferred per `GC_DECISION §5.4`

**Implication:** the team is about to spend 4-6 weeks on Immix to learn the same lesson, **unless defragmentation is in scope** (which doubles the effort estimate).

### 2.3 The common-mode across 6 falsifications

Agent 1's table, validated:

| Falsification | What was attacked | Where bytes actually live (HNE) |
|---|---|---|
| Ditch-Boehm wall | Boehm wall cost | Boehm wall ≈ 0 ms (`gc_total_ms=0`) — nothing to ditch |
| Boehm tuning §6.2 | Boehm page release | 402 MB Boehm 99.9% free already; v3 arena registered as Boehm root blocks unmap |
| Periodic GC | Boehm trigger frequency | v3 doesn't allocate into Boehm |
| Arena dereg | Boehm scan cost | Removed scan but no RSS effect (v3 wasn't in scan path materially) |
| Cheney semispace | Arena copy-out | Copy peak `(1+L)·N` > baseline; high L makes it strictly worse |
| Flat MS | Per-cell reuse | Bytes freed sit in `freeListBins_` resident; no whole-block-free triggers |

**The pattern:** every variant assumed the bottleneck was in the GC mechanism. The bottleneck is in **what gets allocated** (84% Bindings, 82% of which is `mergeBindings`) and **what fails to be physically reclaimed** (free-list-resident bytes don't reduce RSS without compaction or workload-driven whole-block death).

### 2.4 The L(t) data we now have

Per `L_TIME_SERIES_DATA_2026-05-29.md`:
- HNE: 0.48 → 0.59 → 0.58 → 0.48 → 0.47 → **0.41** (range 0.18; L_min=0.41)
- hello: 0.54 → 0.69 (monotone increasing)
- No L < 0.30 observed

**Interpretation Agent 1 surfaced (worth promoting):** the HNE L drop 0.59 → 0.41 means **~700 MB of dead bytes accumulate after the peak-L moment.** If MS fired ONCE at the L=0.41 trough, it could reclaim those bytes — but the current trigger fires at "arena hits 256 MB" which is much earlier in the eval, when L is still ~0.5. **By the time arena reaches its true peak at 1593 MB, MS has already run and reclaimed bytes are sitting in free-list bins.**

**This means trigger policy is itself a lever the team hasn't explored.** Late-firing or trough-detecting GC could reclaim significantly more than the current early-threshold firing achieves.

But — caveat — without per-cycle GC CSV, we can't even tell when current GC fires. The "trigger policy is a lever" hypothesis has no measurement to ground it.

### 2.5 Implementation issues found in mark_sweep.cc

Per Agent 1's code review, three concrete inefficiencies in the current flat MS implementation:

1. **`clearCellStartBitFor` is O(blocks × deadCells)** ([`alloc.hh:1750-1765`](../include/v3/alloc.hh)). Called per dead cell from [`mark_sweep.cc:762`](../mark_sweep.cc). On HNE with ~100 blocks × ~10M dead cells = 1 billion block-comparisons. **Hidden quadratic.** Fix: pass blockIdx in (already known by `sweepOneBlock`).

2. **`anyMarkInRange` calls `findBlockConst` per call** ([`mark_sweep.cc:161-190`](../mark_sweep.cc), binary search at line 213). Called once per cell during sweep. `sweepOneBlock` already knows the block — should pass resolved `BlockBitmap*`. O(log nBlocks × nCells) vs achievable O(nCells).

3. **`memset(p, 0, bytes)` on every free-list pop and Immix span alloc** ([`alloc.hh:1058, 1069, 1671`](../include/v3/alloc.hh)). Phase 3.6 reuse-safety fix. When the same cell gets reused 10×, that's 10× zeroing cost.

**Implication:** the +40.7% wall regression on hello.drvPath under flat MS may include significant cost from the O(blocks × deadCells) quadratic. **Fixing it might rescue flat MS before declaring it dead.** Estimated effort: ~1 day. The team pivoted to Immix BEFORE fixing this. Recommend: try the fix; re-measure.

---

## 3. Wall-time / cycle attribution diagnostic state

### 3.1 What we have

22 instrumentation systems, comprehensively inventoried by Agent 2. Highlights:

| System | What it produces | Strength |
|---|---|---|
| `V3_TIMING=1` | Phase breakdown: lower/optimise/compile/run/vm/bridge | Outer scope; rolls up imports |
| `importTimingTotals` | Per-import-phase | Inner import detail; covers what V3_TIMING rolls up |
| `NIX_VM_OPCOUNTS=1` | Per-opcode dispatch count + top-12 | Drove bigram fusion work |
| `NIX_VM_OPCYCLES=1` | Per-opcode total ns + avg ns/op | M5 primop profile + opcode hot-list |
| `NIX_VM_BIGRAMS=1` | 256×256 (prev_op, cur_op) matrix | Identified Phase 4b/5b fusion targets |
| `NIX_VM_PRIMOP_TIME=1` | Per-primop total ns + top-15 | Real per-primop wall (inclusive of nested forces) |
| `NIX_V3_BRIDGE_TIMING=1` | Per-BridgeKind (6 kinds) ns | Killed #661 + downgraded AR15 |
| `V3_DBG_ALLOC_DUMP=1` | Per-LambdaDescriptor allocCount/forceCount/callCount | Per-Nix-lambda granularity (via PosHandle) |
| `NIX_V3_HEAP_TRACE=1` | Boehm heap(t) every 50ms via sampler thread | Time-series for Boehm subset only |
| `bench/perf-trace.py` | External psutil CPU%/RSS/threads/Boehm-heap, JSONL + SVG | Cross-evaluator TW vs v3 |
| `NIX_VM_STATS=1` | Per-cycle GC banner: closures/thunks/bindings counts + markMs + sweepMs + reclaim% | **markMs/sweepMs ARE measured** ([`mark_sweep.cc:874-880`](../mark_sweep.cc)) |

### 3.2 Capability matrix (12 questions)

Per Agent 2's table — what we CAN answer today:

| Question | Verdict |
|---|---|
| Where does this Nix expression spend wall-time? | **Partial** (per-opcode + per-LambdaDescriptor count, but not per-source-line wall) |
| Which lib function is slowest? | **Partial** (V3_DBG_ALLOC_DUMP ranks by forceCount not wall ns) |
| Per-CompilationUnit cycle attribution | **No** |
| **Per-source-position cycle attribution** | **No** (the load-bearing miss; ~30% MVP per `NIX_PROFILER_DESIGN`) |
| Per-primop cycle attribution | **Yes** (`NIX_VM_PRIMOP_TIME`) |
| TW-bridge wall attribution | **Yes** (`NIX_V3_BRIDGE_TIMING`) |
| Pre-eval/eval/post-eval breakdown | **Partial** (V3_TIMING covers lower/compile/run; serialize not split) |
| Time-series wall(t) | **No** (only L(t) and Boehm-heap(t); not wall-by-bucket over time) |
| Sweep cost attributed to source | **No** |
| GC trigger reason | **Partial** (single trigger mechanism today; binary fires/didn't-fire) |
| Cold-cache vs warm-cache attribution | **Partial** (importTimingTotals separates hits/misses; no per-eval baseline) |
| Per-thread breakdown | **N/A** (single-threaded) |

### 3.3 The diagnostic gap for the Phase 4 / Immix decision specifically

Per Agent 2's deep dive, the team has:

✓ Per-cycle markMs + sweepMs lines (real measurement at `mark_sweep.cc:874-880`)
✗ Per-cycle CSV — lines go to stderr one at a time; **no machine-readable aggregator**
✗ Per-cycle distribution — PHASE_4's "40-54% sweep" is a **SUM over all cycles**, not a histogram. **Were SOME cycles fast and SOME slow? Unknown.**
✗ Per-block sweep cost — `sweepOneBlock` returns counts but doesn't time per block
✗ Sweep cost decomposed by size-class — structural reasoning, not measured
✗ Trigger latency — wall between alloc-cross-threshold and first GC mark is unmeasured
✗ Bytes-actually-reused per cycle — free-list entries dumped but no per-cycle reuse counter
✗ First cycle vs steady-state — cycle index not tracked

**The data needed for the "real" sweep cost breakdown is computed in-process (`tSweepEnd - tMarkEnd`) and printed to stderr but never aggregated.** The PHASE_4 verdict consumed the aggregate (sum across cycles ÷ wall) when it could have consumed the distribution (per-cycle).

---

## 4. Allocation + lifetime tracking diagnostic state

### 4.1 What we have

Per Agent 3's inventory, ~12 alloc-related systems:

| System | Tracks | Granularity |
|---|---|---|
| `NIX_VM_STATS` | All allocStats (per-Tag, per-opcode, per-mergeBindings histogram) | Per-Tag + per-opcode + per-hand-rolled-site |
| `NIX_V3_BINDINGS_ATTR` | Per-Bindings origin via posHandle + label | Per-source-position + per-C++-site (#746) |
| `NIX_V3_THUNKS_ATTR` | Per-Thunk origin (file:line via `__builtin_FILE/LINE`) | Per-C++ source position |
| `NIX_V3_CLOSURES_ATTR` | Per-Closure origin | Per-C++ source position |
| `NIX_V3_PAIRS_ATTR` | Per-ValuePair origin | Per-C++ source position |
| `NIX_V3_LISTS_ATTR` | Per-ListVec origin + size | Per-C++ source position |
| `NIX_V3_STRINGS_ATTR` | Per-allocChars origin | Per-C++ source position (today's spike) |
| `NIX_V3_LIVE_TRACE` | LIVE bytes per Tag at end-of-run | Per-Tag (one-shot) |
| `NIX_V3_BLOCK_PROBE` | Live bytes per 16 MB arena block; line-occupancy | Per-block + per-line |
| `NIX_V3_LIVE_TRACE_PERIODIC=K` | L(t) every K MB alloc | Time-series (total only; not per-Tag) |
| `V3_DBG_ALLOC_DUMP` | Per-LambdaDescriptor allocCount/forceCount/callCount | Per-Nix-lambda |
| `V3_DBG_HOT_FORCE` | Per-Thunk forces counter | Per-Thunk instance |

### 4.2 Capability matrix (20 questions)

Per Agent 3 — what we CAN answer today:

| Question | Verdict |
|---|---|
| Bytes allocated by Tag | **Yes** |
| Bytes allocated by C++ alloc site | **Yes** |
| Bytes allocated by Nix source position (file:line) | **Partial** (only Bindings has Entry::pos linkage; Thunks via LambdaDescriptor; Closures/Lists/Pairs/Strings have NO Nix PosIdx) |
| Live bytes by Tag at end-of-eval | **Yes** |
| L(t) time-series | **Partial** (total only; not per-Tag in CSV; 2-7 samples) |
| Per-object allocation time | **No** (no `allocTick`/`birthCycle` field anywhere) |
| Per-object first-touch / first-force time | **No** |
| Per-object last-touch time | **No** |
| Per-object LAG/USE/DRAG/VOID | **No** (biographical profiler not built) |
| Per-object retainer | **No** (`LiveTracer::enqueue` records seen set; no predecessor) |
| Per-object dominator | **No** |
| **Per-source-position retention** | **No** (data exists in heap; aggregator not built) |
| Per-eval-phase allocation breakdown | **No** |
| User code vs nixpkgs library attribution | **No** |
| Per-thunk forced count | **Yes** (instance + per-descriptor) |
| Per-Closure: used-upvalues vs captured | **No** |
| String duplication count | **No** (NIX_V3_STRINGS_ATTR counts bytes, not content hashes) |
| Per-CompilationUnit attribution | **No** |
| Cross-process: cache reload vs fresh alloc | **No** (only via differential measurement) |
| Allocation-rate over time | **Partial** (computable from periodic CSV) |

### 4.3 The big miss: per-PosIdx live-bytes rollup

Agent 3's headline. The team has:
- `Bindings::Entry::pos` (PosIdx32 inlined in pad slot per #752) — populated at every Bindings allocation
- `LambdaDescriptor::posHandle` — populated for every Lambda
- `LiveTracer` that walks all live Bindings + Thunks transitively

**The data needed for "lib/lists.nix:1213 retains 35 MB" is already in the heap.** What's missing is the aggregation step in the `walkBindings` / `walkThunk` callbacks. Per [`NIX_MEMORY_PROFILER_DESIGN §5.1`](NIX_MEMORY_PROFILER_DESIGN_2026-05-27.md), this is the **3-5 day Phase 1** that was proposed 2 days ago and not yet built.

**Without this, every GC design rests on Tag-level evidence** (84% Bindings dominance). With this, the team can answer:
- Is the 400 MB live Bindings residual on HNE from 10 source positions or 10,000?
- Concentrated → BiBOP / page segregation works
- Dispersed → it doesn't
- Does cardano-node M5 retention live in user code (overlay leak) or library code (mkDerivation chain)?

---

## 5. The meta-pattern across 6 falsifications

Each agent independently surfaced the same pattern. Synthesized:

**"Measure aggregate / make structural claim."** Six GC falsifications, six aggregate measurements:

| Falsification | Aggregate measurement | Distribution we needed |
|---|---|---|
| Ditch-Boehm wall | gc_total_ms=0 (aggregate) | Sufficient — falsification was right |
| Boehm tuning §6.2 | boehm_unmapped=0 (aggregate) | Sufficient — falsification was right |
| Periodic GC | 9× wall regression (aggregate) | Per-trigger pause distribution — missing |
| Arena dereg | +0.2 MB / +1.4 MB peak (aggregate) | Sufficient — falsification was right |
| Cheney | +486 MB peak inferred (n=1 sample) | Per-cycle peak distribution — MISSING |
| Flat MS | +40.7% wall, +21.88 MB peak (aggregate) | Per-cycle distribution + per-block sweep + per-source retention — ALL MISSING |

Per `MEASURE_TWICE_CUT_ONCE §5.7` (and proposed §5.8 in [`L_MEASUREMENT_GAP_2026-05-28.md`](L_MEASUREMENT_GAP_2026-05-28.md)): "moment-vs-distribution conflation." We have a documented anti-pattern that we keep falling into because the instruments default to aggregate.

**The instruments themselves enable the pattern.** `NIX_VM_STATS` produces a banner of totals at end-of-run. `NIX_V3_BRIDGE_TIMING` produces totals. `V3_DBG_ALLOC_DUMP` produces top-N by count. **None of them produces a distribution by default.** Per-cycle GC stats exist but go to stderr as separate banners with no aggregator. Per-PosIdx data exists in the heap but no walker reads it.

The team is doing the meta-thing right (measure-twice methodology, pre-commit thresholds, Rule 0 falsifications). But the **measurements that feed those decisions are aggregate-shaped while the decisions are distribution-shaped**.

---

## 6. Recommended diagnostic fill-in

Ranked by impact-per-effort. All buildable in <2 weeks combined.

### 6.1 Build #1: Per-cycle GC CSV (~half day)

**Action:** Add `NIX_V3_GC_CYCLE_CSV=path` env var. On each `runMajorMarkSweep` invocation, write one CSV row: `cycleIdx, trigger_reason, markMs, sweepMs, blocksScanned, blocksFreed, liveCells, deadCells, bytesFreed, freeListEntryCount, allocSincePrev, wallSincePrev`. The data is already computed at `mark_sweep.cc:874-960`; only the row format + file handle are new.

**Why first:** rescues the F2 verdict. If even ONE of the markMs/sweepMs distributions shows a long-tail outlier, the "flat MS is too slow" verdict may be wrong, and the rescue path is "bound the outlier" not "switch GC family."

**Pre-committed deliverable:** CSV from a single hello.drvPath run under `NIX_V3_MAJOR_GC=1` shows per-cycle distribution. If p99 sweepMs / p50 sweepMs > 5, the aggregate verdict was inappropriate.

### 6.2 Build #2: Per-PosIdx live-bytes rollup (3-5 days)

**Action:** Extend `LiveTracer::walkBindings` ([live_trace.cc:185+](../live_trace.cc)) and `walkThunk` to aggregate bytes into `unordered_map<uint32_t /* posHandle */, LivePosEntry>`. At dump time, resolve to file:line:col via `resolvePosSnapshot`. Emit top-20 retainers by bytes.

**Why second:** converts every future GC discussion from "84% Bindings dominance" (Tag-level) to "lib/fixed-points.nix:95 retains 85 MB, lists.nix:1213 retains 35 MB, ..." (source-level). The instrument that should have existed BEFORE Cheney.

**Pre-committed deliverable:** for hello.drvPath, top-20 retainers should sum to ≥80% of Bindings+Thunk bytes; source-positions should be visually meaningful (nixpkgs function names).

### 6.3 Build #3: Per-Tag L(t) (<1 day)

**Action:** Extend periodic CSV ([live_trace.cc:1428-1488](../live_trace.cc)) with per-Tag columns: `live_closures_mb, live_thunks_mb, live_bindings_mb, live_lists_mb, live_pairs_mb`. The LiveTracer already separates per-Tag internally — just plumb through.

**Why third:** answers whether HNE's 0.41 → 0.59 → 0.41 swing is one Tag growing or all-at-once. Routes per-generation policy.

### 6.4 Build #4: Phase decomposition (1-2 days)

**Action:** Add `phaseBytesParse / phaseBytesLower / phaseBytesEvalPrimop / phaseBytesEvalMain / phaseBytesSerialize` counters; phase-switch on entry/exit to each. The 990 MB elsewhere bucket on HNE was decomposed by differential measurement (`NIX_V3_NO_DISK_CACHE=1`); this gives in-process attribution.

**Why fourth:** direct prerequisite for [`EXIT_GC_SPIRAL_PLAN`](EXIT_GC_SPIRAL_PLAN_2026-05-29.md) Day 2 cache-eviction PoC. Tells us how much of HNE's 1593 MB arena IS cache reload vs eval growth.

### 6.5 Build #5: Fix flat MS quadratic + re-measure (~1 day)

**Action:** Fix the O(blocks × deadCells) in `clearCellStartBitFor` by passing blockIdx from `sweepOneBlock`. Re-run Phase 4 SHIP gate.

**Why fifth:** the +40.7% wall regression may include significant cost from this quadratic. Fixing might rescue flat MS without the 4-6 week Immix detour. Cheap to try.

### 6.6 Build #6: Alloc-tick field (2-3 days)

**Action:** Add `uint32_t allocTick` to Thunk + Bindings (Bindings has padding budget per #752). Bump a global tick at K-MB safepoints. At live-walk, emit age histograms.

**Why sixth:** foundation for LAG/USE/DRAG/VOID biographical classification. Enables "is HNE 0.41 a fast-churn workload or slow-drift" diagnostic — the distinction that decides generational vs flat.

### 6.7 Build #7: Retainer-edge sampling (~1 week)

**Action:** During `LiveTracer::enqueue` ([live_trace.cc:113-118](../live_trace.cc)), when `seen.insert(p).second == true`, record `predecessor[p] = currentParent`. Sample 1/N edges to bound memory. At dump, walk back from chosen object to root.

**Why seventh:** answers "what RETAINS the 400 MB Bindings residual?" If chain is `flake.nix → callPackage → stdenv.lib`, points at lazy-promotion fix. If chain is `getFlake → ImportCache → ...`, points at cache eviction (orthogonal to GC). Different answer routes different work.

**Cumulative effort: ~2 weeks single-engineer.** Each item has measurement-first acceptance criteria.

---

## 7. Critical review of agents

I push back where each agent was incomplete or wrong:

### 7.1 Agent 1 (GC failure root-cause)

**Strong:**
- Code-verified the `projectedSweepMs = markMs` projection (I verified independently at `live_trace.cc:818`)
- Quantitative breakdown of the +21.88 MB regression
- Identified the O(blocks × deadCells) hidden quadratic
- Sharp distinction: Immix wins WALL, not PEAK

**Where I push back:**
- Agent 1 says "this is the load-bearing single finding." It is — for the F2 verdict. But it doesn't fully explain the Phase 4 measurement, which used actual markMs/sweepMs. The agent conflates F2 (pre-impl projection) with PHASE_4 (post-impl aggregate). Both have integrity issues; they're different ones. F2 = projection masquerading as measurement. PHASE_4 = aggregate sum masquerading as distribution.
- Agent 1 didn't read `GC_DECISION_2026-05-29.md` directly (probably couldn't access). The Immix decision context is partially inferred.

### 7.2 Agent 2 (wall-time profiling)

**Strong:**
- Comprehensive 22-system inventory
- Sharp gap analysis for the Phase 4 Immix decision specifically
- Cheap concrete fix (`NIX_V3_GC_CYCLE_CSV` <1 day)
- Linked instruments to the falsifications they would have caught

**Where I push back:**
- Agent 2's "headline finding" (per-cycle GC CSV) is correct but understates the scope. Per-PosIdx retention (Agent 3) is arguably bigger — it changes WHAT GC variants are even worth considering, while per-cycle CSV changes HOW WELL we can measure variants we've already picked.
- The "perf-trace.py is cross-evaluator" claim is true but the integration with NIX_VM_STATS / Boehm-stderr is fragile.

### 7.3 Agent 3 (alloc + lifetime)

**Strong:**
- Identified the per-PosIdx aggregator gap (3-5d build vs years of design discussion)
- Sharp framing: "the data exists; only the consumer is missing"
- Walked the 6 falsifications and identified which would have been caught earlier with better instrumentation (3 of 6)

**Where I push back:**
- Agent 3's "1 week retainer-edge sampling" is optimistic. Sampling within `LiveTracer::enqueue` is straightforward but USING the data (walking back from chosen object) requires UI work that's underestimated.
- Per-Tag L(t) at <1 day is right; that should be priority-2 not the recommendation-5 it's listed as. Easy win.

### 7.4 Synthesis-level pushback

All three agents independently land on "the instrumentation is aggregate-shaped while the decisions are distribution-shaped." This convergence is strong. **But:** none of the agents asked the second-order question: **why does the team keep building aggregate instruments?**

My hypothesis: aggregate instruments are CHEAPER to add (one increment per op + one banner-print at exit). Distribution instruments require row-format design, file handles, CSV consumers downstream. The path of least resistance is aggregate. **The methodology fix isn't "build better instruments once" — it's "default to distribution-shape for any new diagnostic."**

---

## 8. Honest limits

- **The 6 falsifications are real failures of approach, not just instrumentation.** Even with perfect diagnostics, v3's heap profile (84% Bindings, L=0.5-0.75, 990 MB elsewhere bucket) is hostile to copying GC and challenging for non-moving. Better diagnostics give grounded decisions; they don't guarantee a successful GC ships.
- **My verification of `projectedSweepMs = markMs` is at one line of code.** The F2 verdict text in `STAGE_6_FALSIFIERS_RESULT_2026-05-29.md` (which I haven't read in full) might cite multiple data sources. The "40-54% sweep wall verdict was projection" claim should be cross-checked by reading that doc.
- **The recommended builds are diagnostics, not fixes.** Building them doesn't ship RSS reduction. They UNBLOCK informed decisions about which fix to ship.
- **Phase decomposition #4 may be wrong-shaped.** Phases aren't strictly serial; primops fire during eval; bytecode load happens at multiple times. Implementation is finickier than I describe.
- **Retainer-edge sampling has UX risk.** Showing a back-walk chain to a Nix author requires resolving file:line context that may not survive serialization round-trips.
- **The team has ground-truth I don't.** Some instruments may be deprecated; some may be planned-but-not-checked-in. Verify before building.

---

## 9. Recommendation

**Before any further GC variant implementation (including Immix), build Diagnostics #1 + #2 + #3 (Per-cycle GC CSV + Per-PosIdx live-bytes + Per-Tag L(t)). Combined effort: ~5-6 days.**

These three would:
1. Re-grade the current Immix pivot decision with actual sweep distribution (Build #1)
2. Convert "84% Bindings" from a Tag-level pattern-match into a source-level lever (Build #2)
3. Make the L(t) data we just shipped multi-dimensional (Build #3)

If after Build #1 the sweep distribution shows a long-tail outlier, the Immix pivot may be wrong — flat MS with the quadratic fixed could ship. If Build #2 reveals concentrated retention (10 PosIdx own 80% of live bytes), per-site fixes route the work. If Build #3 shows one Tag growing during eval, generational policy is justified.

**This is the EXIT_GC_SPIRAL_PLAN Week 0 work, refined.** Instead of three measurement spikes about lever yields, do three diagnostic builds that LET us see the levers properly. Then run the original Week 0 measurements with grounded instrumentation.

---

## 10. Cross-references

### This audit
- Agent 1 (GC failure root-cause) — code-verified; load-bearing finding at `live_trace.cc:818`
- Agent 2 (wall/cycle profiling) — comprehensive inventory; Build #1 recommendation
- Agent 3 (alloc + lifetime) — per-PosIdx gap; Build #2 recommendation

### Strategic docs
- [`EXIT_GC_SPIRAL_PLAN_2026-05-29.md`](EXIT_GC_SPIRAL_PLAN_2026-05-29.md) — pause GC; this audit refines Week 0
- [`GC_PAUSE_2026-05-29.md`](GC_PAUSE_2026-05-29.md) (if landed) — the meta-conclusion
- [`MEASURE_TWICE_CUT_ONCE_2026-05-23.md`](MEASURE_TWICE_CUT_ONCE_2026-05-23.md) §5.7 + §5.8 candidate — moment-vs-distribution rule
- [`NIX_PROFILER_DESIGN_2026-05-21.md`](NIX_PROFILER_DESIGN_2026-05-21.md) — Stage 15 wall (Build #2 informs)
- [`NIX_MEMORY_PROFILER_DESIGN_2026-05-27.md`](NIX_MEMORY_PROFILER_DESIGN_2026-05-27.md) — Build #2 is its Phase 1

### Code anchors
- [`live_trace.cc:818`](../live_trace.cc) — `projectedSweepMs = markMs` (the verified projection bug)
- [`mark_sweep.cc:874-880`](../mark_sweep.cc) — real markMs/sweepMs locals (unused by F2)
- [`alloc.hh:1750-1765`](../include/v3/alloc.hh) — O(blocks×deadCells) `clearCellStartBitFor`
- [`alloc.hh:1645-1649`](../include/v3/alloc.hh) — `freeListAdd` per-cell unordered_map insert
- [`live_trace.cc:185+`](../live_trace.cc) — walkBindings (where Build #2's PosIdx aggregation goes)
- `Bindings::Entry::pos` (`alloc.hh:133`) — data ready for Build #2
- `LambdaDescriptor::posHandle` — data ready for Build #2

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
