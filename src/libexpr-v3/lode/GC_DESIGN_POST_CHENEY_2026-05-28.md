# v3 GC design after Cheney falsification — post-Cheney path forward

**Date:** 2026-05-28
**Author:** session synthesis (3-agent research: academic GC literature + real-world VM survey + v3 infrastructure inventory + critical synthesis)
**Status:** strategic design — replaces [`STAGE_6_PRECISE_GC_DESIGN_2026-05-27.md`](STAGE_6_PRECISE_GC_DESIGN_2026-05-27.md) which was falsified on 2026-05-28 per [`STAGE_6_CHENEY_FALSIFIED_2026-05-27.md`](STAGE_6_CHENEY_FALSIFIED_2026-05-27.md)
**Triggering question:** "Why would a copying GC create a 2× peak? Carefully plan out a better GC solution."

Companion docs:
- [`STAGE_6_CHENEY_FALSIFIED_2026-05-27.md`](STAGE_6_CHENEY_FALSIFIED_2026-05-27.md) — the falsification this doc replaces
- [`IDEAL_GC_DESIGN_2026-05-26.md`](IDEAL_GC_DESIGN_2026-05-26.md) — pre-Stage-6 ideal sketch (still load-bearing for the RSS-primary rule)
- [`DATA_STRUCTURE_AUDIT_2026-05-21.md`](DATA_STRUCTURE_AUDIT_2026-05-21.md) — 84% Bindings dominance finding
- [`HNE_BUCKET_DECOMP_2026-05-27.md`](HNE_BUCKET_DECOMP_2026-05-27.md) + [`LIVE_FRACTION_SPIKE_2026-05-27.md`](LIVE_FRACTION_SPIKE_2026-05-27.md) — measured L
- [`MEASURE_TWICE_CUT_ONCE_2026-05-23.md`](MEASURE_TWICE_CUT_ONCE_2026-05-23.md) §3 — pre-commit thresholds required

---

## 1. TL;DR

**The 2× peak under Cheney is intrinsic. v3's L=0.5-0.75 makes any copying tenured GC structurally worse than the current Boehm baseline.** Five Rule-0 GC falsifications now (ditch-Boehm wall, Boehm tuning §6.2, periodic-GC, standalone arena-dereg, Cheney) all converge on a single conclusion: **v3's heap profile demands a non-moving tenured collector.**

**Primary recommendation: mark-sweep tenured with per-size-class free lists (OCaml-pattern), reusing Stage 3's precise root walker + Stage 1's tagIsPointer. Effort: 2-3 weeks; LoC budget ~2 KLoC.** No 2× peak; wall cost competitive with Cheney at L≥0.5 (Blackburn ISMM'04). Phase E v0.2 nursery retained as opt-in young region but deprioritized (young L=0.43-0.58 is weak-generational; sticky mark-bits may displace it later).

**Plan B: Immix mark-region (Whippet-pattern).** Higher LoC budget (~4 KLoC), better space efficiency, opportunistic defragmentation. Decide between MS and Immix via a 1-2 day line-occupancy simulation BEFORE committing weeks to either.

**Scope reality:** even perfect GC on the v3 arena leaves the "elsewhere" 500-990 MB cache bucket untouched. **Mark-sweep alone does NOT get cardano-node M5 under the 4 GB watchdog.** Cache eviction + strictness analysis remain orthogonal levers required.

---

## 2. Why a copying GC creates a 2× peak

### 2.1 The mechanic (Cheney 1970)

A Cheney semispace collector divides the heap into TWO regions of equal size, FROM-space and TO-space. Allocation happens in FROM as a bump pointer. When FROM fills:

1. Walk roots, copy each reached object from FROM to TO, leave a forwarding pointer in FROM
2. Scan copied objects in TO, copy their reached children
3. When all reachable objects copied, swap roles: TO becomes new FROM, OLD FROM is freed
4. Continue allocation in new FROM

**During the copy phase (step 1-2), both FROM and TO are simultaneously resident.**

### 2.2 The math

For an arena of size N with live fraction L (= live bytes / total arena bytes):

| Phase | FROM resident | TO resident | Total resident |
|---|---|---|---|
| Pre-GC (steady state) | N (all live + dead) | 0 (unmapped) | **N** |
| During copy | N (still mapped; bytes haven't been freed yet) | growing 0 → L·N | up to **N(1+L)** |
| Post-GC | 0 (freed) | L·N (only live) | **L·N** |

**Peak / steady-state ratio = (1+L) / L = 1 + 1/L.**

| L | Peak factor | Peak / SS ratio | Example v3 workload |
|---|---|---|---|
| 0.05 | 1.05× | 21× | GHC nursery; copying nearly free |
| 0.10 | 1.10× | 11× | V8 young-gen typical |
| 0.30 | 1.30× | 4.3× | mid-life Java old-gen |
| **0.50** | **1.50×** | **3.0×** | **v3 HNE arena (L=0.50)** |
| **0.74** | **1.74×** | **2.35×** | **v3 hello.drvPath arena (L=0.74)** |
| 0.90 | 1.90× | 2.11× | hot lib.fix overlay |

### 2.3 Why v3's L is structurally high

Per [`LIVE_FRACTION_SPIKE_2026-05-27.md`](LIVE_FRACTION_SPIKE_2026-05-27.md) + [`HNE_BUCKET_DECOMP_2026-05-27.md`](HNE_BUCKET_DECOMP_2026-05-27.md), measured at end-of-eval:

| Workload | Arena bytes | Live bytes | L (overall) | L (Bindings) |
|---|---|---|---|---|
| hello.drvPath | 525 MB | 286 MB | **0.54** | **0.72** |
| HNE | 1.38 GB | 617 MB | **0.44** | **0.59** |
| synthetic genList | 125 MB | <1 KB | 0.000003 | n/a |
| cardano-node M5 | est. ~2.5 GB | est. ~1.1 GB | **est. 0.44** | est. 0.55-0.65 |

Three structural reasons L is high on real workloads:

1. **Bindings dominate (~84% of arena per [`DATA_STRUCTURE_AUDIT_2026-05-21.md`](DATA_STRUCTURE_AUDIT_2026-05-21.md)).** Once constructed, attribute sets stay alive because flake outputs reference them transitively. The stdenv attrset chain alone is hundreds of MB of persistently-live Bindings.

2. **mergeBindings is 82% of Bindings bytes on HNE** (per [`HNE_BUCKET_DECOMP_2026-05-27.md`](HNE_BUCKET_DECOMP_2026-05-27.md): 585 MB at `vm.cc:1228`). Each `a // b` over-allocates `na+nb` upfront, fills `k` actual entries; the live result chain has high L within itself.

3. **ImportCache pins whole subtrees indefinitely** (no eviction). Every `import (path)` result is held; haskell.nix imports dozens of large subtrees.

By contrast, **young-gen L=0.43-0.58** (Phase E v0.1 measurement, per memory entry [[phase-e-v0-2-landed]]): even there, half of allocations survive — squarely in the "weak generational" regime, not the "strong generational" regime where copying is nearly free.

### 2.4 What happens when you build Cheney on top of this

Measured 2026-05-28 (commit `eda44711a` Stage 6 Day 4):

| Mode | peak_rss | v3_arena | delta vs gate-OFF |
|---|---|---|---|
| gate-OFF | 753 MB | 587 MB | (baseline) |
| **gate-ON honest** | **1239 MB** | 436 MB | **+486 MB (1.65× peak)** |

The arena bytes column dropped (587 → 436 MB; the copy IS reclaiming dead) but the simultaneous OLD+NEW residency during scavenge pushes peak above gate-OFF by exactly the (1+L) factor the math predicts.

**Pre-committed SHIP threshold ≥200 MB reduction. Honestly measured: NEGATIVE 486 MB. SHIP gate NOT MET.**

### 2.5 Why prior measurements (yesterday's "-722 MB / 95.8%") were artifactual

Per [`STAGE_6_CHENEY_FALSIFIED_2026-05-27.md`](STAGE_6_CHENEY_FALSIFIED_2026-05-27.md): strings + paths in arena pages were dangling post-scavenge (the scavenger didn't forward `Tag::String` / `Tag::Path` const-char payloads). The OS hadn't reused those pages, so reads returned correct bytes by accident. The "31.4 MB peak" was a measurement after the dangling-page state where the OS still hadn't paged in OLD+NEW simultaneously because the bytes were "still there." When `fwdChars` properly installed (this commit), the honest peak measurement appeared.

**Lesson: any GC measurement on copying collectors must verify correctness via byte-identity AND measure peak DURING the collection, not just before/after.**

---

## 3. Design space survey

Three agents in parallel researched: (a) academic GC literature, (b) real-world VM implementations, (c) v3's existing infrastructure constraints. Critical synthesis:

### 3.1 Convergence

| Design | Agent A (lit.) | Agent B (real-world) | Agent C (v3) | Consensus |
|---|---|---|---|---|
| Mark-sweep tenured | strong fit at L≥0.5 | OCaml + GHC nonmoving + Julia + Boehm-incumbent | 60% substrate ready | **✓ recommended** |
| Mark-region (Immix) | best peak × wall product | Whippet `mmc` default | adapts existing walker | **✓ recommended (Plan B)** |
| Mark-compact | works but multi-pass | HotSpot Parallel Old; V8 selective | could fit Bindings size class | secondary |
| Cheney semispace | falsified for L≥0.4 | only BEAM at L≈0 | **falsified 2026-05-28** | **✗ killed** |
| Reference counting | barrier storm at v3 alloc rate | Tvix uses Rc/Arc; cycles a concern | Bindings churn = high barrier rate | ✗ excluded |
| Concurrent GC | latency-oriented | not justified single-threaded eval | overkill | deferred |

**Strong agreement: non-moving tenured.** The split is between flat mark-sweep (OCaml-pattern) and Immix mark-region (Whippet-pattern).

### 3.2 The MS vs Immix decision

| Criterion | Flat mark-sweep + free-list | Immix mark-region |
|---|---|---|
| LoC | ~1.5-2 KLoC | ~3-4 KLoC |
| Peak factor | 1.0× (+ ~3% mark bitmap) | 1.0× (+ ~3% line bitmap) |
| Wall per cycle | O(N) sweep | O(live lines) mark + O(blocks) sweep skip |
| Fragmentation | size-class fits help; Robson worst-case 0.5× | line/block; opportunistic compaction |
| Variable-size Bindings | needs per-size-class bins | spans lines naturally |
| Allocator path | free-list pop; bump pointer fallback | bump-pointer within reclaimed lines |
| Implementation risk | low (well-precedented OCaml) | medium (line/block bookkeeping) |
| Production examples | OCaml, GHC nonmoving, Julia, Boehm | Whippet, JikesRVM Immix |
| **Best for v3 if** | want minimal LoC, ship in weeks | want best space efficiency, can spend extra weeks |

**Decision criterion (per measure-twice):** before committing weeks to either, run Agent A's cheap experiment — Immix line-occupancy simulation on hello.drvPath. **Threshold: ≥30% lines fully dead per cycle → Immix has bump-realloc advantage worth its complexity. <30% → flat MS dominates on LoC/risk.**

### 3.3 Where the agents are incomplete

Critical review identified gaps the agents didn't fully address:

**1. Scope reality (none of the agents).** Mark-sweep on the arena ONLY attacks the `v3_arena` bucket. The "elsewhere" bucket is independent:

| Workload | v3_arena | Boehm | elsewhere | peak_rss |
|---|---|---|---|---|
| hello.drvPath | 587 MB | 403 MB | 0 MB | 753 MB |
| HNE | 1594 MB | 403 MB | **990 MB** | 2987 MB |
| M5 | est. ~2.5 GB | 403 MB | est. ~3 GB | 5760 MB |

The HNE elsewhere bucket (990 MB) is **ImportCache + SQLite + bytecode disk-cache in-memory shadow** (per [`HNE_BUCKET_DECOMP_2026-05-27.md`](HNE_BUCKET_DECOMP_2026-05-27.md): 500-950 MB of that 990). **A v3-side GC cannot touch it.** Cache eviction is the orthogonal lever.

Post-perfect-GC projection on M5: ~3500 MB peak. Still over 4 GB watchdog only if elsewhere bucket shrinks. **Mark-sweep alone does not solve M5.**

**2. Sweep cost on variable-size Bindings (Agent A's untested claim).** Blackburn ISMM'04 says "MS wins on wall too at L=0.75" but that was Java workloads with fixed-size headers. v3's Bindings have variable-size FAM trailers (24 B Entry × N entries). Sweep needs per-cell size lookup. **This must be measured on the actual v3 heap, not assumed.**

Estimate: 1.5 GB arena × 16 cell-bytes / 64-bit-per-mark-bit ≈ 24 MB bitmap. Sequential scan at 30 GB/s ≈ 50 ms per major GC. Acceptable if major GC fires <10×/eval; marginal if >50×/eval.

**3. Phase E v0.2 nursery compatibility (Agent C said "compatible" but didn't analyze).** Young-gen L=0.43-0.58 is weak-generational. Copying-nursery cost ≈ L·N_nursery = 0.5 × 16 MB = 8 MB copied per minor GC. Frequency depends on alloc rate; for v3's 6.5M allocs/eval at ~40 B/alloc = 260 MB of nursery allocation → 16 minor GCs × 8 MB = 128 MB total copy work. **Compared to MS sweep work over a 16 MB nursery: O(16 MB) bitmap = ~0.5 ms per cycle × 16 = 8 ms.** Sticky mark-bits would be 16× cheaper than copying for this nursery shape.

**Implication: Phase E v0.2 was a partial answer. Sticky mark-bits over a flat MS tenured displaces it. But this is a Phase 2 optimization — start with MS tenured + retain Phase E as opt-in nursery; revisit nursery design once tenured is stable.**

**4. The 5-falsification common-mode (Agent C surfaced this).** "Mechanism confusion: cost ≠ frequency." Mark-sweep is vulnerable too:
- Boehm tuning: assumed lower cost increases frequency. False.
- Arena-dereg: same.
- Cheney: assumed copy reclaims live AND lowers peak. The arithmetic was always against this.
- Periodic-GC: mechanism right, timing wrong.
- B2 nursery default-on: cost ∝ alloc volume, not universal.

**For MS, the analogous risk: assuming smaller post-GC residency translates to lower peak.** It does if:
- Allocation slows down BEFORE next eval segment (which it does — peak is hit during specific phases)
- Sweep + free-list reclamation IS faster than re-allocating new pages from OS
- No new "elsewhere" bucket grows to take MS's reclaimed space

These are testable assumptions. Each needs a pre-committed verification step.

### 3.4 Production-VM precedent

Agent B's pattern across 10 systems:

| System | Tenured choice at L≥0.5 |
|---|---|
| OCaml | mark-sweep + free list (~2 KLoC) |
| GHC (since 8.10) | non-moving concurrent mark-sweep |
| V8 Orinoco | mark-compact with selective evacuation |
| HotSpot G1 | region-based mark + selective evacuation |
| HotSpot Parallel Old | mark-compact (sliding) |
| Julia | non-moving mark-sweep (FFI constraints) |
| Whippet `mmc` | Immix mark-region |
| Chez Scheme | generational copying + BiBOP segregation |
| Boehm (v3 incumbent) | conservative mark-sweep |
| BEAM | Cheney (only because L≈0 per-process) |

**Zero production VMs run flat Cheney at L≥0.5. The literature consensus from Blackburn ISMM'04: "copying GC needs ~2× live size to be competitive; v3's measured 1.65× regression on hello.drvPath is exactly the predicted penalty."**

---

## 4. Recommended design

### 4.1 Architecture

**Two-tier hybrid: mark-sweep tenured + Phase E v0.2 nursery (opt-in, retained as-is).**

```
┌────────────────────────────────────────────────────────────────┐
│  v3 process memory layout                                       │
├────────────────────────────────────────────────────────────────┤
│  Phase E nursery (16 MB, opt-in NIX_V3_NURSERY=1)              │
│    bump-pointer alloc; copying scavenge on overflow             │
│    write barriers (Phase D) record tenured→nursery edges        │
├────────────────────────────────────────────────────────────────┤
│  Tenured arena (variable; bulk of allocations)                  │
│    block-based (32 KB blocks); per-block mark bitmap            │
│    per-size-class free lists (BiBOP-lite for Bindings)          │
│    mark phase: walkAllV3Roots (Stage 3) + RootVisitor (Stage 5) │
│    sweep phase: walk blocks, populate free lists                │
│    triggered at exitDepth==0 above threshold (default 256 MB)   │
├────────────────────────────────────────────────────────────────┤
│  Boehm (external-tag: bridge nix::Value*, store paths)          │
│    unchanged; v3 GC does not touch                              │
├────────────────────────────────────────────────────────────────┤
│  Caches (orthogonal levers; not GC-managed)                     │
│    ImportCache + SQLite + CU disk-cache in-memory shadow        │
│    eviction policy is separate ROADMAP item                      │
└────────────────────────────────────────────────────────────────┘
```

### 4.2 Mark phase

**Reuse Stage 3 precise-root walker + Stage 5 GcRoot registry.**

```cpp
// Pseudocode
class MarkVisitor : public RootVisitor {
    BitmapMarker & marker;
public:
    void visitValue(Value & v) override {
        if (!tagIsPointer(v.tag)) return;
        void * ptr = v.payload.pointer;
        if (marker.tryMark(ptr)) {
            worklist.push(ptr);  // process transitively
        }
    }
    // visitClosure/Thunk/Bindings/List/Pair/Slot/String/Path: same pattern
};

void runMajorGC(VMState & vm) {
    BitmapMarker marker(arena);
    MarkVisitor v(marker);
    walkAllV3Roots(vm, v);
    while (!worklist.empty()) {
        void * obj = worklist.pop();
        // Walk obj's fields via visitor pattern (reuse RootVisitor dispatch)
        scanObject(obj, v);
    }
    sweepArena(arena, marker);
}
```

Per Agent C inventory:
- `walkAllV3Roots` (Stage 3, commit `02c95eba0`): ✓ ready
- `RootVisitor` interface: ✓ ready (designed for moving GC, but mark-bit recording is strictly simpler)
- `tagIsPointer` (Stage 1, commit `6f854fa2c`): ✓ ready
- `bridge_root_registry` (commit `bad371821`): ✓ ready (external-tag tracking)

**No new mark-phase infrastructure needed.** Mark visitor records bits instead of forwarding.

### 4.3 Sweep phase

**Per-block sweep with per-size-class free lists.**

Arena is already block-organized (32 KB blocks per [`STAGE_6_IMPLEMENTATION_GUIDE_2026-05-27.md`](STAGE_6_IMPLEMENTATION_GUIDE_2026-05-27.md) Day 1 refactor `40e779601`). Sweep walks blocks:

```cpp
struct Block {
    char * start;
    char * end;
    BitmapMarker::WordType * markBits;  // ~3% of block size
    // ...
};

void sweepBlock(Block & blk, FreeListSet & lists) {
    char * cursor = blk.start;
    while (cursor < blk.end) {
        // Determine object size at cursor (Tag-dispatched; FAM-aware for Bindings)
        size_t sz = objectSizeAt(cursor);
        if (!isMarked(cursor, blk.markBits)) {
            lists.add(cursor, sz);  // insert into size-class free list
        }
        cursor += sz;
    }
}
```

**Object-size discovery is the load-bearing detail.** Two designs:

**Design A: per-cell size header** (Boehm-style; +4 B per cell). Cheapest to sweep; bloats memory.

**Design B: typed pages (BiBOP-lite for Bindings).** Bindings (84% of arena) get dedicated blocks with uniform Entry layout; Tag determines per-block stride. Other Tags share a "mixed" pool with per-cell size headers. Saves 4 B per Bindings cell × ~1M cells on HNE = 4 MB.

**Recommendation: Design B (BiBOP-lite).** Bindings dominate enough that segregating them is worth the per-Tag dispatch.

### 4.4 Free-list allocation

Allocation slow path checks size-class free list before bump-allocating new block:

```cpp
void * Arena::alloc(size_t sz, Tag tag) {
    auto & lists = freeListsForTag(tag);
    if (auto * cell = lists.tryPop(sz)) {
        return cell;  // reuse swept cell
    }
    // Fallback to bump allocation
    if (current_block_->remaining() < sz) {
        refillBlock();
    }
    return current_block_->bump(sz);
}
```

Free-list pop is O(1); fragmentation is bounded by per-size-class bins (Boehm uses ~30 size classes; OCaml has a few).

### 4.5 Trigger policy

**Major GC fires when** `arena.bytesAllocated() >= threshold && exitDepth == 0`. Threshold tunable via `NIX_V3_MAJOR_GC_THRESHOLD_MB` (default 256 MB; clamped 16-32768 MB).

Adaptive threshold: after each GC, set next threshold to `max(initial, 2 × postGCArenaBytes)`. This is the classical "double if collection didn't free much" heuristic; prevents tight-loop GC when L approaches 1.

### 4.6 What survives from the Cheney work

Per [`STAGE_6_CHENEY_FALSIFIED_2026-05-27.md`](STAGE_6_CHENEY_FALSIFIED_2026-05-27.md), the following infrastructure is **retained as scaffolding**:

| Component | Reuse in MS |
|---|---|
| `walkAllV3Roots` + `RootVisitor` | ✓ identical (mark instead of copy) |
| `tagIsPointer` + Stage 1 static_asserts | ✓ identical |
| `bridge_root_registry` (external-tag) | ✓ identical |
| `fwdCell` (walkThunk standalone discovery) | partially reusable for mark-phase pointer discovery |
| `fwdChars` (Tag::String/Path payload) | not needed — MS doesn't move; payloads stay in place |
| Sorted-index for Bindings byte-range | not needed — MS doesn't use forwarding tables |
| `NIX_V3_MAJOR_GC=1` env gate | ✓ same gate, different mechanism |
| Dynamic threshold (re-fire prevention) | ✓ identical |
| Phase D write barriers | needed ONLY if generational; defer |
| Phase E v0.2 nursery | retained as opt-in; eventually displaced by sticky-bits |

**Carcass to remove:**
- Typed forwarding tables (`forwardingClosure_`, `forwardingThunk_`, `forwardingBindings_`, ...)
- `Region::backup_` / `swapRegions()` machinery
- Copy-loop infrastructure in `MajorScavenger`

The retired carcass is ~600 LoC. The retained scaffolding is ~400 LoC. Net new MS code: ~1.5-2 KLoC.

---

## 5. Pre-commit falsifiers (mandatory per measure-twice §3)

Before committing to weeks of mark-sweep implementation, run these cheap experiments. Each has a pre-committed threshold; the decision must be made BEFORE seeing results.

### 5.1 Falsifier #1: MS vs Immix line-occupancy simulation (1-2 days)

**Question:** does Immix's mark-region bump-realloc deliver enough space-savings vs flat MS to justify the +2 KLoC?

**Experiment:** instrument `live_trace.cc` to partition arena into 128 B logical lines; for each line compute "live" = ≥1 live byte. Report fraction of lines fully dead per cycle.

**Pre-committed threshold:** ≥30% lines fully dead → Immix path. <30% → flat MS path.

**Cost:** ~200 LoC over existing live-trace infrastructure. 1-2 days.

**Why this is load-bearing:** Immix's allocator advantage IS bump-realloc within reclaimed lines. If most lines have surviving fragments, that advantage evaporates.

### 5.2 Falsifier #2: Sweep-cost projection (0.5 day)

**Question:** Agent A's claim "MS wins on wall too at L=0.75" — does it hold for v3's variable-size Bindings?

**Experiment:** measure time to walk arena + dispatch on each cell's Tag + record size, WITHOUT actually freeing. Project across N major GCs per eval.

**Pre-committed threshold:** sweep cost <5% of eval wall on hello.drvPath. If >10%, MS may be wall-slower than Cheney despite RSS win; reconsider Immix.

**Cost:** ~50 LoC measurement helper. 0.5 day.

### 5.3 Falsifier #3: Post-GC peak verification (0.5 day before commit)

**Question:** does smaller post-GC arena residency actually translate to lower peak RSS, or does allocation refill take us right back to the high-water mark?

**Experiment:** under a 5-MB synthetic MS prototype, measure peak RSS at 10 checkpoints during hello.drvPath eval.

**Pre-committed threshold:** peak RSS at the last 3 checkpoints must be ≥150 MB below gate-OFF peak. If <100 MB savings, sweep work doesn't translate to peak benefit; the "smaller residency → lower peak" assumption fails.

**Cost:** ~100 LoC prototype + scripted hyperfine. 0.5 day.

**Why this is load-bearing:** kills the same "cost ≠ frequency" common-mode that falsified Boehm tuning + arena-dereg.

### 5.4 Falsifier #4: BiBOP-lite for Bindings (0.5 day spike)

**Question:** does segregating Bindings into dedicated pages reduce per-cell overhead enough to justify the per-Tag complexity?

**Experiment:** modify allocBindings to use a separate page pool; measure peak RSS delta on hello.drvPath.

**Pre-committed threshold:** ≥15% RSS reduction (~110 MB on hello) → BiBOP justified. <5% → drop BiBOP from design.

**Cost:** ~200 LoC + rerun. 0.5 day.

### 5.5 Decision flow

```
Run Falsifier #1 (Immix line-occupancy)
├── ≥30% lines dead → Implement Immix path (~4 KLoC, 4-6 wk)
└── <30%      → Continue:
                Run Falsifier #2 (Sweep cost)
                ├── >10% wall → Reconsider Immix despite #1 result
                └── ≤5% wall  → Continue:
                                Run Falsifier #3 (Peak verification)
                                ├── <100 MB savings → STOP. Address elsewhere bucket first.
                                └── ≥150 MB savings → Continue:
                                                       Run Falsifier #4 (BiBOP)
                                                       ├── ≥15% delta → Implement MS + BiBOP (~2 KLoC, 2-3 wk)
                                                       └── <5% delta  → Implement flat MS (~1.5 KLoC, 2 wk)
```

**Total falsifier cost: 2.5-3 days BEFORE committing weeks of implementation.** Per measure-twice §3.1: "cheap directional proxy → pre-committed threshold → binary decision."

---

## 6. Implementation plan (post-falsifiers, primary path: flat MS)

Assumes Falsifier #1 returns <30% (most likely outcome given Bindings dominance + persistent live), and #2/#3/#4 pass.

### 6.1 Phase 0 — Cheney carcass removal (1-2 days)

- Remove forwarding tables from `move_gc.cc`
- Retire `Region::backup_` machinery from `alloc.hh`
- Keep `bridge_root_registry`, `fwdCell`, `fwdChars`-as-fallback, `RootVisitor`, dynamic threshold
- Verify `all-v3-tests --quick` 6/6 + `--core` 15/15 pass after removal

### 6.2 Phase 1 — Mark phase scaffolding (2-3 days)

- New file `mark_sweep.cc` + `include/v3/mark_sweep.hh`
- `class MarkVisitor : public RootVisitor` — reuses Stage 3 walker
- `class BitmapMarker` — per-block mark bit storage (block-local bitmap; 3% overhead)
- Mark-only mode (no sweep): `NIX_V3_MARK_ONLY=1` env gate; measures live-set without reclamation
- Acceptance: marked-bytes within ±5% of `live_trace.cc` output (cross-validation)

### 6.3 Phase 2 — Sweep + free lists (3-5 days)

- Per-Tag size dispatch in `objectSizeAt(cursor)`
- BiBOP-lite for Bindings (separate page pool per Falsifier #4 result)
- Per-size-class free-list bins (~16 bins, log-spaced)
- Sweep loop walks blocks; populates free lists
- Acceptance: sweep wall <5% of eval wall on hello.drvPath (per Falsifier #2)

### 6.4 Phase 3 — Allocation slow-path (2 days)

- `Arena::alloc()` checks free-list before bump
- Trigger major GC at `bytesAllocated() >= threshold && exitDepth == 0`
- Adaptive threshold (double-if-no-progress)
- Acceptance: 6/6 + 15/15 tests pass under `NIX_V3_MAJOR_GC=1`

### 6.5 Phase 4 — Honest measurement (2 days)

- Hyperfine on hello.drvPath, HNE, M5 under both gate-OFF and gate-ON
- Verify byte-identical drvPath outputs
- **Pre-committed SHIP gate: ≥200 MB peak_rss reduction on hello.drvPath; ≤10% wall regression; byte-identical correctness.**
- If SHIP gate not met: revert behind opt-in gate (default-OFF); write falsification doc; pivot to Immix (Plan B) or scope reduction.

### 6.6 Phase 5 — Production hardening (3-5 days)

- Stress mode: NIX_V3_MAJOR_GC_STRESS=N forces GC every N opcodes
- Run `all-v3-tests --brute --core` under stress
- Validate on nixpkgs flake eval matrix (10+ packages)
- Flip from opt-in to default-ON if all gates clear

**Total Phase 0-5: ~2-3 weeks (single engineer, focused).**

### 6.7 If Falsifier #1 returns ≥30% (Plan B: Immix)

Substitute Phase 1-3 with Immix line/block bookkeeping. LoC budget ~4 KLoC; effort ~4-6 weeks. Same Phase 4 + 5.

---

## 7. Scope reality — what mark-sweep CANNOT solve

**Honest projection of peak RSS under perfect mark-sweep tenured:**

| Workload | Pre-GC peak | Post-MS projection | TW oracle | M5 watchdog gap |
|---|---|---|---|---|
| hello.drvPath | 753 MB | **~450 MB** (live 286 + slack ~165) | ~140 MB | n/a |
| HNE | 2987 MB | **~1800 MB** (live 617 + slack ~200 + elsewhere 990) | ~145 MB | n/a |
| **cardano-node M5** | **5760 MB** | **est. ~3500 MB** | 856 MB | **still over 4 GB watchdog?** |

Projection notes:
- "Live + slack" = post-GC arena residency. Slack ≈ free-list overhead + fragmentation, est. 25-30%.
- Elsewhere bucket (ImportCache + SQLite + CU shadow): **NOT touched by MS.** This is 990 MB on HNE, est. ~1500 MB on M5.
- Boehm bucket (~400 MB): mostly empty already; not the lever.

**For M5 to hit <4 GB, mark-sweep is NECESSARY BUT NOT SUFFICIENT.** Orthogonal levers required:

1. **Cache eviction (Phase 4b LRU per ROADMAP):** 500-950 MB on HNE; est. larger on M5. Blocked by 3 prerequisites per [[#701-deferred-emittreeattrs]].
2. **Strictness analysis (Stage 4 v4+):** reduces N upstream. Per [[stage4-v4-2]], 0 elisions on hello today; requires cross-function strictness.
3. **Bindings allocation pattern fixes** (Stage 7 candidate): mergeBindings is 82% of Bindings on HNE; persistent over-allocation. ChainBindings Phase C falsified (3 pivots); needs different approach.

**Mark-sweep is the foundation that lets these other levers translate to peak RSS reduction.** Without MS reclaiming dead arena, even successful cache-eviction would just leave reclaimed cache memory unused while arena waste persists.

---

## 8. Risks + mitigations

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Sweep cost dominates wall at L=0.74 | medium | ship-blocker | Falsifier #2 measures pre-implementation; abort if >10% |
| Post-GC peak doesn't drop as projected | medium | ship-blocker | Falsifier #3 verifies before commitment |
| Variable-size Bindings fragments worse than projected | low-medium | wall regression over time | BiBOP-lite (Falsifier #4); periodic compaction Phase 6 if needed |
| BiBOP per-Tag dispatch overhead | low | wall regression | Falsifier #4 measures delta |
| Phase E nursery interaction breaks | low | correctness | Stress mode + brute audit per Phase 5 |
| External-tag (bridge nix::Value*) corruption | medium | crash | bridge_root_registry already handles; existing test coverage |
| Common-mode pattern repeats (smaller residency ≠ lower peak) | medium | repeat-falsification | Falsifier #3 is specifically designed to catch this |
| M5 still >4 GB after MS ships | high | ROADMAP commit | Scope-reality §7 acknowledges this; orthogonal levers required |
| LoC blows past 2 KLoC budget | medium | timeline | Cap at 3 KLoC; if exceeded, revisit Immix decision |

---

## 9. Acceptance criteria (pre-committed per measure-twice)

### Phase 4 SHIP gate (after MS implementation)

**Quantitative thresholds (binary decisions, no post-hoc adjustment):**

- [ ] **Peak RSS reduction ≥200 MB on hello.drvPath** (gate-ON vs gate-OFF, hyperfine ≥10 runs, σ documented)
- [ ] **Peak RSS reduction ≥500 MB on HNE** (gate-ON vs gate-OFF, ≥10 runs)
- [ ] **Wall regression ≤10% on hello.drvPath** (gate-ON vs gate-OFF, ≥10 runs)
- [ ] **Wall regression ≤15% on HNE** (gate-ON vs gate-OFF, ≥10 runs)
- [ ] **byte-identical drvPath outputs** (gate-OFF, gate-ON, TW oracle on hello + HNE + M5)
- [ ] **`all-v3-tests --quick`** 6/6 PASS under both gates
- [ ] **`all-v3-tests --core`** 15/15 PASS under both gates
- [ ] **`all-v3-tests --brute --core`** PASS under `NIX_V3_MAJOR_GC=1` (live-counts = 0 across all workloads)

### Phase 5 default-ON gate

- [ ] All Phase 4 criteria pass with **≥3 consecutive measurements** at different times (host-noise smoothing)
- [ ] Stress mode validation: 1000+ forced GCs per eval, no crashes, no leaks
- [ ] nixpkgs flake matrix: 10+ packages eval byte-identical to TW
- [ ] cardano-node M5 peak RSS measurement (informative; not gate-blocking — see scope-reality §7)

### Falsification thresholds

If after Phase 4:
- Peak RSS reduction <100 MB → **FALSIFIED**. Write `MARK_SWEEP_FALSIFIED.md`; revert behind opt-in; pivot to Immix or scope-pivot to cache-eviction first.
- Wall regression >25% → **FALSIFIED**. Same pivot.
- Correctness regression → unconditionally revert; root-cause before re-attempting.

---

## 10. Honest limits

- **Scope:** MS attacks v3_arena only. Cache eviction + strictness analysis remain unaddressed and required for M5 4 GB watchdog.
- **Per measure-twice §5.7:** every "RESOLVED" claim must include independent verification step before pivoting. Mark-sweep "shipped" doesn't mean "M5 problem solved" — verify via cardano-node M5 measurement explicitly.
- **The 5-falsification common-mode** (cost ≠ frequency) applies to MS too. Falsifier #3 is the specific test against this.
- **LoC estimates are ranges** based on Whippet (10 KLoC C for Immix) and OCaml (2 KLoC C for MS major). v3's specific shape may shift 30%.
- **Wall projections** are LITERATURE-derived (Blackburn ISMM'04); v3-specific measurement is Falsifier #2.
- **Phase E v0.2 nursery interaction** is "presumed compatible" per Agent C; actual cross-test under stress is Phase 5 work.
- **Sticky mark-bits as future Phase E displacement** is a Phase 6+ optimization, not in this design.
- **Process-exit-after-eval assumption** (no fragmentation accumulation) holds for current CLI usage but not for `nix repl` daemon mode or IDE language servers. If those become primary workloads, periodic compaction needed.
- **Immix consideration in Plan B**: Whippet is the proven implementation but C-only and embedding it would be heavyweight. A from-scratch Immix in v3 might cost 5-7 KLoC, not 4.
- **Mark-bitmap storage**: 3% overhead is best case (per-block adjacent); if storage is external hash, overhead is 8-16 B per object × 6.5M objects = 50-100 MB. Design must keep bitmap local to block.
- **External-tag objects** (bridge nix::Value*, store paths) remain Boehm-managed. v3 MS doesn't touch these. Boehm's existing behavior on this small subset (~400 MB heap, mostly empty) is unchanged.

---

## 11. Cross-references

- [`STAGE_6_CHENEY_FALSIFIED_2026-05-27.md`](STAGE_6_CHENEY_FALSIFIED_2026-05-27.md) — the falsification this design replaces
- [`STAGE_6_PRECISE_GC_DESIGN_2026-05-27.md`](STAGE_6_PRECISE_GC_DESIGN_2026-05-27.md) — superseded
- [`STAGE_6_IMPLEMENTATION_GUIDE_2026-05-27.md`](STAGE_6_IMPLEMENTATION_GUIDE_2026-05-27.md) — Day 1-4 work; carcass removal in §6.1 references this
- [`IDEAL_GC_DESIGN_2026-05-26.md`](IDEAL_GC_DESIGN_2026-05-26.md) §1 — RSS-primary framing rule (this doc operates under)
- [`DATA_STRUCTURE_AUDIT_2026-05-21.md`](DATA_STRUCTURE_AUDIT_2026-05-21.md) — 84% Bindings dominance
- [`HNE_BUCKET_DECOMP_2026-05-27.md`](HNE_BUCKET_DECOMP_2026-05-27.md) — mergeBindings 82%, elsewhere 990 MB
- [`LIVE_FRACTION_SPIKE_2026-05-27.md`](LIVE_FRACTION_SPIKE_2026-05-27.md) — measured L
- [`NURSERY_PHASE_D_DECISION_2026-05-21.md`](NURSERY_PHASE_D_DECISION_2026-05-21.md) — Path α barriers
- [`ARENA_DEREG_FALSIFIED_2026-05-27.md`](ARENA_DEREG_FALSIFIED_2026-05-27.md) — sibling falsification; `bridge_root_registry` reused here
- [`BOEHM_TUNING_FALSIFIED_2026-05-27.md`](BOEHM_TUNING_FALSIFIED_2026-05-27.md) — common-mode lesson
- [`GC_DITCH_BOEHM_FALSIFIED_2026-05-27.md`](GC_DITCH_BOEHM_FALSIFIED_2026-05-27.md) — wall premise falsified; Boehm stays
- [`MEASURE_TWICE_CUT_ONCE_2026-05-23.md`](MEASURE_TWICE_CUT_ONCE_2026-05-23.md) §3, §5.7 — pre-commit thresholds; methodology audit
- [`MEMORY_REDUCTION_AVENUES_2026-05-26.md`](MEMORY_REDUCTION_AVENUES_2026-05-26.md) — orthogonal levers (cache eviction, strictness)
- [`ROADMAP_TO_VISION_2026-05-15.md`](ROADMAP_TO_VISION_2026-05-15.md) Stage 6 — to be updated reflecting MS replaces Cheney

### Code surfaces
- `include/v3/precise_root.hh` + `precise_root.cc` — mark phase substrate (reuse as-is)
- `include/v3/bridge_root_registry.hh` + `bridge_root_registry.cc` — external-tag (reuse as-is)
- `include/v3/alloc.hh` (Arena + Region) — sweep target; remove `backup_`/`swapRegions`
- `move_gc.cc` — retire Cheney carcass; keep `fwdCell`/`fwdChars`-as-fallback
- `include/v3/value.hh` — Tag enum + tagIsPointer (reuse as-is)
- `live_trace.cc` — falsifier infrastructure (#1, #3 reuse)

### Prior-art references (from agents, distilled)

**Academic foundational:**
- McCarthy, J. "Recursive functions of symbolic expressions" CACM 1960 (mark-sweep origin)
- Cheney, C.J. "A nonrecursive list compacting algorithm" CACM 1970 (the 2× peak design)
- Ungar, D. "Generation Scavenging" 1984 (generational origin)
- Demers et al "Combining generational and conservative GC" POPL 1990 (sticky mark-bits)
- Boehm-Demers-Weiser "Mostly parallel garbage collection" PLDI 1991
- Hertz & Berger "Quantifying the Performance of GC vs Explicit Memory Management" OOPSLA 2005 — copying needs ~3-5× heap headroom
- Blackburn, Cheng, McKinley "Myths and Realities" SIGMETRICS 2004 — MS wins at L≥0.5
- Blackburn & McKinley "Immix" PLDI 2008 — mark-region with selective evacuation
- Jones, Hosking, Moss *The Garbage Collection Handbook* CRC Press 2011 — definitive reference

**Production VMs:**
- Doligez & Leroy "A Concurrent, Generational GC for ML" POPL 1993 — OCaml mark-sweep
- Gamari & Dijkstra "Alligator Collector" ISMM 2020 — GHC nonmoving
- V8 blog series "Trash talk" / "Orinoco" — V8 mark-compact + selective evacuation
- Marlow et al "Parallel Generational-Copying GC" ISMM 2008 — GHC RTS reference
- Wingo, A. "Whippet" blog series 2022-2025
- Yang & Wingo "Nofl: A Precise Immix" arXiv 2025
- Blackburn et al "Reconsidering Garbage Collection in Julia" ISMM 2025
- Dybvig "Development of Chez Scheme" ICFP 2006 — BiBOP

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
