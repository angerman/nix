# Nursery Phase D — Decision (Path α with per-container dirty bits)

**Date**: 2026-05-21.
**Status**: Decision memo informing the Phase D implementation start.
**Decides**: §13 of `NURSERY_PHASE_D_DESIGN_2026-05-18.md` (Path α vs Path β).
**Decision**: **Path α** — per-container dirty bits on Bindings, ValuePair, and a thin wrapper for standalone Value cells.  Targeted barrier instrumentation at three distinct write-site classes (no card table).  Migrate to Path β (card-marking) only if Stage 4 / Stage 5 introduces enough new inter-gen write sites that the per-site audit becomes intractable.

## Strategic context (added 2026-05-21 after re-reading lode/)

This memo sits inside two larger architectural decisions made the same day:

1. `GC_BUILD_VS_BUY_2026-05-21.md` — commits to **hand-rolling v3's GC for the
   next 6-12 months** rather than adopting Whippet / MMTk / GHC RTS.  The
   doc's falsifier claim 3 sets a **6-week ceiling on Phase D**; my 2.5-3-week
   estimate must hold under that ceiling.  If Phase D blows past 6 weeks, the
   build-vs-buy claim falsifies and Whippet adoption gets reconsidered.

2. `BOEHM_DEPENDENCY_2026-05-21.md` — frames Stage 3 as **Layer 1 of a
   three-layer memory model**:
   - Layer 1 (committed; this work): nursery default-on → ~50-70%
     hello.drvPath peak RSS reduction.  Phase D is the gating work.
   - Layer 2 (Stage 8-adjacent, post-Phase-1.5): de-root v3 arena from Boehm
     by copy-out at FFI exit — `GC_add_roots` calls removed.
   - Layer 3 (not committed): Whippet tenured replacement.  Trigger-gated
     post-Stage-7 by Phase 1.5 measurement.

My Phase D Stage-3 exit criteria (§6 below) match Layer 1's structural
hypothesis:
- v3_arena 956 MB → ≤300 MB (3.2× reduction).
- Peak RSS 1895 MB → ≤1000 MB (1.9× reduction).
These are exactly the BOEHM_DEPENDENCY Layer 1 falsifier claim 2 ("Stage 3
reduces hello.drvPath peak RSS by ≥40%").  If we miss these, the Layer 1
hypothesis falsifies and Phase D wasn't enough on its own — pushes us toward
Layer 2 acceleration.

The recent two-round GC audit (`RCA_VALUEPAIR_EVALUATED_2026-05-21.md`,
`GC_AUDIT_ROUND_2_2026-05-21.md`) surfaced missed-root classes that
are v3-specific (`Tag::Slot`, cell-update, `Thunk::shapeCell`,
`forceWriteTarget`, bridge thunks).  Per build-vs-buy §5.3, no
off-the-shelf GC framework would have caught these — they need custom root
walkers regardless.  The **diagnostic infrastructure** (BRUTE + AUDIT +
GC_STRESS, all landed) is the real mitigation; the framework choice is
secondary.  This validates the Path α direction: targeted per-site barriers
+ stress-mode validation, rather than a framework's generic mark-region.

## 0. Why this memo exists

`NURSERY_PHASE_D_DESIGN_2026-05-18.md` ended at §13 with two paths (α incremental dirty bits vs β card-marking) and a "weakly held" preference for β.  Three new datums have landed since:

1. `5a3e489c9` — NIX_VM_STATS three-way RSS decomposition.  Bindings dominate the v3 arena at 84% (807 MB of 956 MB on hello.drvPath).  ValuePairs 3%, Thunks 5%, Closures 3%.  See `project_702_rss_decomp_2026-05-21.md`.
2. `d123d0be2` — `V3_DBG_GC_STRESS=N`.  STRESS=1000 on the full v3 suite + `run-brute-audit.sh` 15-case battery passes cleanly.  Confirms scavenger correctness under aggressive frequency.
3. `0d67df1ea` — refined BRUTE classifies LIVE vs DEAD hits.  ZERO LIVE hits across nixpkgs through firefox.  All "missed-root" candidates were arena-bloat false positives.

These narrow the design question:
- The write-site distribution is **highly concentrated**, not spread across many classes.  84% of writes land in Bindings entries.
- Phase D's main correctness risk (missed barrier site) is bounded by a manageable audit.
- Path β's "scales to many write sites" advantage is theoretical until Stage 4 actually adds them.

## 1. Audit summary (grep-based, 2026-05-21)

### Class A — Bindings entry writes (`b->entries[i].value = X`)

**~28 sites** across `bytecode_primops.cc`, `vm.cc`, `primops.cc`, `print.cc`.  All follow the pattern:

```
auto * b = Alloc::allocBindings(n);          // tenured
// ... populate names ...
b->entries[i].value = X;                     // X may be nursery
```

These dominate by volume (84% of arena bytes on hello.drvPath).

**Subset of representative sites:**
- `primops.cc:1644, 1777, 2058, 2275, 2288, 2578, 3330, 3414, 5145, 5826, 6030, 6814, 6887, 6943, 7518, 7965, 8004, 8074`
- `vm.cc:6460, 6477, 6552, 6835, 9535` — OP_ATTRS_REC_INIT etc.
- `bytecode_primops.cc:312` — install path 3 (primop replacement).
- `print.cc:106` — deepForce in-place rewrite (already wrapped via DeepForceGuard).

### Class B — ValuePair::evaluated writes

**~7 sites** in `vm.cc`.  All Tag::App memoization paths:
- `vm.cc:5823, 5846` — OP_FORCE compress-chain.
- `vm.cc:10632` — OP_CALL_PRIMOP App result memo.

ValuePair is **always tenured** (`Alloc::allocPair` calls `threadArena().alloc`).  Every write to `pair->evaluated = X` where X is a nursery pointer is an inter-gen write.

### Class C — Thunk::evaluated writes

**~7 sites** in `vm.cc`.  Pattern: `t->evaluated = retVal` at OP_FORCE return.
- `vm.cc:5433, 5602, 5994, 11030, 11049`.

Thunks are nurseryOrArena-allocated (mostly nursery early in eval, promoted to tenured via scavenge for survivors).  A TENURED Thunk that gets its `evaluated` field written with a NURSERY pointer is the same inter-gen hazard.

### Class D — Standalone Value cells (`Thunk::cell` indirect)

**~3 sites** in `vm.cc`.  Pattern: `*(fr.thunk->cell) = retVal`.

Cells are allocated via `Alloc::allocValue()` (always tenured).  Writing a nursery pointer through `*cell` is an inter-gen write but the cell itself is tenured.

Currently `Thunk::cell` can point either into a Bindings entry's `value` slot (covered by Class A's barrier) OR a standalone `Value *` (uncovered).  Audit shows both shapes occur; `vm.cc:8117/8118` shows cell being set from arena `cellTarget`; `vm.cc:9554/9558` shows similar.

### Sites NOT requiring barriers

- **Closure upvalue writes**: only at construction time (immutable post-creation).  No mutation post-promotion.
- **List literal element writes**: only at construction.
- **Bindings.entries[].name writes**: SymbolId, not Value — no GC concern.
- **Nursery-to-nursery writes**: same-generation, no barrier needed.

## 2. Decision: Path α — per-container dirty bits

### 2.1 Mechanism

Three barrier macros, one per write-site class.  All centralized so future write sites just call the helper:

```cpp
// Bindings entry write barrier.
inline void bindingsSetValue(Bindings * b, uint32_t i, Value v) {
    b->entries[i].value = v;
    if (isNurseryPointer(v) && !threadNursery().contains(b))
        b->dirty = 1;
}

// ValuePair memoization barrier.
inline void pairSetEvaluated(ValuePair * p, Value v) {
    p->evaluated = v;
    if (isNurseryPointer(v))            // ValuePair always tenured
        p->dirty = 1;
}

// Thunk::evaluated memoization barrier.
inline void thunkSetEvaluated(Thunk * t, Value v) {
    t->evaluated = v;
    if (isNurseryPointer(v) && !threadNursery().contains(t))
        t->dirty = 1;
}
```

Plus a thin wrapper for standalone-cell writes:

```cpp
// Cell write — covers the OP_RETURN cell-write path for both
// Bindings-internal cells AND standalone allocValue() cells.
inline void cellWrite(Value * cell, Value v, Thunk * thunkOwner) {
    *cell = v;
    if (isNurseryPointer(v)) {
        // If the cell sits inside a Bindings, mark the Bindings;
        // otherwise mark the standalone-cell registry (which the
        // scavenger walks).
        if (thunkOwner && thunkOwner->cellContainer)
            thunkOwner->cellContainer->dirty = 1;
        else
            threadStandaloneCellRegistry().add(cell);
    }
}
```

### 2.2 Header field changes

- `Bindings`: add `uint8_t dirty` (consume one byte of existing 4-byte size padding; no struct grow).
- `ValuePair`: currently 48 bytes (3 × Value @ 16 bytes).  Add 1 byte → padded to 56 bytes.  Per-pair cost: 8 bytes.  On hello.drvPath (621780 pairs × 8 bytes) = +4.9 MB total.  **Acceptable**.
- `Thunk`: already has a state byte; consume 1 padding byte for `dirty`.  Zero size change.
- `Thunk::cellContainer`: NEW pointer field `Bindings *` (nullable).  +8 bytes per Thunk.  On hello.drvPath (663125 thunks × 8) = +5.3 MB.  **Acceptable**.

**Total header overhead at hello.drvPath peak**: ~10 MB.  Trivial vs the 956 MB arena.

### 2.3 Scavenge update

The Scavenger gains a fourth root class: dirty-tenured-containers.  After natural roots are walked, iterate the tenured arena (via `Arena::blockRanges()`) and, for each known-tenured Bindings / ValuePair / Thunk header with `dirty == 1`:
- Walk its inter-gen-pointer field.
- Reset `dirty = 0`.

The arena walk DOES need object-layout awareness (the dirty-bit position differs per class).  Path β would need similar but at card granularity.  Path α restricts the walk to specific header bytes — same complexity, similar work.

**Alternative**: a thread-local "dirty list" maintained by the barrier writes.  Each barrier that flips `dirty 0 → 1` push-backs the container pointer onto a vector; scavenge walks the vector.  Simpler than arena walk; the vector resets to empty after each scavenge.

**Recommendation**: thread-local dirty list.  ~10K entries per scavenge cycle (rough estimate from #702 data); negligible cost.

### 2.4 Standalone cell registry

For Class D's uncovered cells: a thread-local `std::vector<Value *>` of cells whose containing object isn't a Bindings (the cellContainer == null case).  Populated at `Alloc::allocValue()` if we can't guarantee the cell will be written exclusively with tenured values.  Walked by the scavenger.

Alternative per §10 Q5 of the design doc: **eager-promote standalone cells at allocation** — when a Value pointer crosses through `allocValue()` and is later written with a nursery pointer, immediately promote the pointed-to nursery object.  Cheaper to implement; nursery objects that escape to cells get tenured immediately.  Trade: small early-promotion penalty per cell vs avoiding a separate registry.

**Recommendation**: standalone-cell registry, walked by scavenger.  Simpler model; the cost is sub-MB.

## 3. Why not Path β (card-marking)

Path β was the design doc's weakly-held preference based on "Stage 4 strictness will introduce more write sites."  Three reasons to defer Path β:

1. **The audit shows write sites are concentrated**.  84% of arena = Bindings.  Path α's "3 macros" handle 99% of the write volume cleanly.  Path β's generality is unused.

2. **Card-marking needs object-layout-aware scanning** inside cards.  v3 has tagged Values (16-byte aligned, tag in low bits), but the tenured arena interleaves Closures (variable-size FAM), Thunks (variable-size FAM), Bindings (variable-size FAM), ValuePairs (fixed 48B).  Scanning a card requires either:
   - Per-card metadata "what's at this offset" — extra space.
   - Per-arena-object header parsing — comparable to Path α's per-header dirty bit.
   - Conservative pointer-shaped-word scan — slow + false positives.

   None of these is cheaper than Path α's "per-container dirty bit + walk dirty list."

3. **Stage 4 strictness is months away**.  When it lands, we can re-evaluate.  Adding card-marking now would defer Stage 3 by 1-2 weeks for theoretical generality.

If Stage 4 adds 5+ new inter-gen write sites that don't fit cleanly into the Bindings/Pair/Thunk classes (e.g. new mutable v3-type with its own write protocol), THEN consider migrating to card-marking.  The Path α infrastructure remains useful as long as Bindings dominates allocations.

## 4. Phase D delivery plan (revised, post-decision)

Total effort: **2.5 - 3 weeks focused work** (down from design doc's 3-4 weeks because the audit is already done and Path β infrastructure isn't being built).

| Step | Effort | Risk |
|---|---|---|
| **Step 1** — Header field additions (`Bindings::dirty`, `ValuePair::dirty`, `Thunk::dirty`, `Thunk::cellContainer`). | 0.5 day | Low |
| **Step 2** — Barrier helpers (`bindingsSetValue`, `pairSetEvaluated`, `thunkSetEvaluated`, `cellWrite`). | 1 day | Low |
| **Step 3** — Convert all ~28 Bindings-write sites in primops.cc / vm.cc / etc. to use the barrier helper.  Mechanical refactor; grep + sed-able. | 2-3 days | Medium (any miss = silent corruption — caught by GC_STRESS) |
| **Step 4** — Same for ~7 ValuePair-evaluated sites and ~7 Thunk-evaluated sites. | 1 day | Medium |
| **Step 5** — Standalone-cell registry: declaration + insertion at allocValue() + scavenger walk. | 1 day | Low |
| **Step 6** — Scavenger: dirty-list walk (per-thread vector of dirty containers).  Reset dirty bits at end of scavenge. | 2 days | Medium (subtle correctness — what if a container is on the dirty list AND in the natural walk? — needs dedup) |
| **Step 7** — Disable Phase C's "walk all tenured" path (the original Phase C v1 walked the full reachable graph; with dirty bits we skip un-dirty containers).  Test that AUDIT stays clean. | 1 day | Medium |
| **Step 8** — FFI promote-at-exit (`ensureTenured` helper at FFI exit points).  Closes inter-evaluator boundary. | 1-2 days | Low |
| **Step 9** — Run `./all-v3-tests.sh --brute` + `V3_DBG_GC_STRESS=10,100,1000` + nixpkgs slice.  Triage any LIVE BRUTE / AUDIT hits.  Each hit indicates a missed-barrier site — easy to track via the failing workload. | 1 week of run-and-fix | High (correctness validation) |
| **Step 10** — Measure: hello.drvPath under nursery default-on (Phase D enabled).  Expect v3_arena to drop from 956 MB to ~150-300 MB; expect peak RSS to drop from 1895 MB to ~600-1000 MB.  If not, missed barriers OR something else dominates "elsewhere". | 1 day | Medium |
| **Step 11** — Flip default: rename `NIX_V3_NURSERY` → `NIX_V3_NO_NURSERY`, default-OFF (i.e. nursery default-on).  Documented retirement criterion gates flip. | 0.5 day | Low (if Step 10 passes) |
| **Step 12** — Retire `_pad = 0xFA5E`, `CFF_FAKECLO_TAINTED`, closure-pool sentinel infrastructure per Stage 3 plan. | 1-2 days | Low |

Cumulative: ~15 working days plus validation loop.  Coordinate with Action Plan's standing cadence.

## 5. Open decisions deferred to implementation

- **isNurseryPointer fast path**: range-check via `threadNursery().contains(p)`?  Or store a per-Value flag?  Range check is cheap (two pointer compares); start there, optimize later if hot.
- **Dirty-list deduplication**: a Bindings can be dirtied many times.  Either dedup at insertion (hash set per scavenge cycle) or accept duplicate walks (the scavenger's natural dedup catches it).  Start with the simpler "accept duplicates."
- **Per-thread vs global dirty list**: per-thread (thread_local).  Matches the per-thread arena model.
- **Action plan's Stage 4 prereq**: confirm strictness analysis doesn't introduce mutation patterns we haven't seen.  Add to Stage 4 design checklist.

## 6. Validation criteria for Phase D completion

Phase D is **done** when:

1. `./all-v3-tests.sh --brute` (10/11 PASS, only lint nit) under nursery default-on.
2. `./all-v3-tests.sh` + `V3_DBG_GC_STRESS=10` for 4 hours of continuous run without LIVE BRUTE / AUDIT hits.
3. hello.drvPath v3_arena drops from 956 MB → ≤ 300 MB (3.2× reduction).
4. hello.drvPath peak RSS drops from 1895 MB → ≤ 1000 MB (1.9× reduction).
5. firefox.name byte-identical to TW under nursery default-on.
6. cardano-node `.packages.<sys>.cardano-node` evaluates to the same drvPath as TW.
7. Lang/property/derivation-parity all pass under nursery default-on.

Stage 3 default-on flip gate: all 7 criteria met.

### Boehm-three-layer falsifier mapping

Criteria 3 and 4 above are the **direct falsifiers for
`BOEHM_DEPENDENCY_2026-05-21.md` claim 2** ("Stage 3 reduces hello.drvPath
peak RSS by ≥40%").  My 1895 → ≤1000 MB target IS the ≥40% reduction.

Additionally, after Phase D lands:

8. **Boehm mark-phase cost ratio**: extract `GC_get_full_gc_total_time()`
   before and after Phase D.  Compare against Layer-1 hypothesis from
   BOEHM_DEPENDENCY §6 critique 3 (Stage 3 cuts mark cost ~2×).  If mark
   cost doesn't drop, Layer 2 (de-root v3 from Boehm) becomes more urgent
   sooner.

9. **Tenured-leak watermark on cardano-node**: peak `v3_arena` after Phase D
   ships.  Trigger condition for `GC_BUILD_VS_BUY_2026-05-21.md` §12
   ("Adopt Whippet for tenured?"): **if > 1 GB, candidate Stage 16 enters
   the roadmap**.  If ≤ 1 GB, hand-rolling remains the path.

10. **Phase D landing time ≤ 6 weeks**: build-vs-buy claim 3 falsifier.
    My 2.5-3-week estimate gives generous headroom; if validation loop
    drags past 6 weeks, pause and revisit framework adoption.

## 7. Risks NOT yet covered

- **C-stack invisibility multiplier**: design doc §11 critique 7 — Phase D's per-scavenge speedup is amortized over scavenge frequency, which is capped by the `exitDepth == 0` gate.  If scavenges fire rarely, Phase D's reclamation is bursty.  Mitigation: characterize scavenge frequency post-Phase-D via NIX_VM_STATS heap series.  If frequency is too low to unlock the 5-10× GC factor, a `Rooted<Value>` shadow stack becomes a follow-on task (Stage 3.5).
- **TW interop hazard**: design doc §6.  Promote-at-FFI-exit (Step 8) addresses it, but the per-call cost depends on FFI frequency.  hello.drvPath has ~100 derivationStrict calls; firefox.name has ~5000.  Cost: O(depth of Value graph promoted per call).  Most are leaf-shaped (string, number); should be sub-microsecond per call.
- **Boehm's own GC racing with v3 scavenge**: design doc §10 Q9.  Address via `GC_disable()` / `GC_enable()` around scavenge.  Add to Step 6.

## 8. Cross-references

### Strategic context (same-day documents)

- `GC_BUILD_VS_BUY_2026-05-21.md` — commits to hand-rolling for the next
  6-12 months.  Phase D ceiling = 6 weeks (claim 3); my ≤3-week estimate
  must hold.
- `BOEHM_DEPENDENCY_2026-05-21.md` — three-layer memory model.  Phase D is
  Layer 1; Layers 2/3 trigger-gated post-Phase-1.5.
- `ROADMAP_TO_VISION_2026-05-15.md` Stage 3 — this work; Stage 3's exit
  criteria match §6 above.

### Design + audit precursors

- `NURSERY_PHASE_D_DESIGN_2026-05-18.md` — original ultrathink design doc;
  §13 left the choice open between Path α / β.  This memo closes it.
- `RCA_VALUEPAIR_EVALUATED_2026-05-21.md` + `GC_AUDIT_ROUND_2_2026-05-21.md`
  — the missed-root audits that defined Phase 1.7.  Per build-vs-buy §5.3,
  these classes don't get caught by ANY framework adoption; they need
  custom root walkers + diagnostic infrastructure (BRUTE/AUDIT/STRESS,
  all landed).
- `CHENEY_NURSERY_DESIGN.md` — the canonical reference for Stage 3.

### Measurement instruments + this-session work

- `project_702_rss_decomp_2026-05-21.md` — Bindings dominate at 84%; key
  datum behind Path α.  RSS three-way decomposition tooling.
- `project_brute_in_ci_2026-05-21.md` — diagnostic gating Phase D
  validation.
- `project_gc_stress_2026-05-21.md` — `V3_DBG_GC_STRESS=N`.
- `ACTION_PLAN_2026-05-15.md` Phase 1.5 — measurement spike;
  `BOEHM_DEPENDENCY` §9 trigger conditions decode post-Stage-3.
- `ACTION_PLAN_2026-05-15.md` Phase 1.7 — closed by this session's commits;
  Stage 3 correctness baseline confirmed clean.

### Operational constraints

- `feedback_v3_nursery_cstack_safety.md` — the `exitDepth == 0` gate that
  bounds scavenge frequency.  Phase D doesn't unlock this; per design
  doc §11 critique 7, the `Rooted<Value>` shadow stack is a potential
  follow-on if Phase D's reclamation is too bursty.
- `feedback_v3_native_constraint.md` — V3-NATIVE rule; FFI promote-at-exit
  (Step 8 of this memo's delivery plan) is the boundary mechanism.

## 9. Copyright

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.
