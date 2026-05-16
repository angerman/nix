# Roadmap to Vision — 2026-05-15

A detailed, ordered, step-by-step path from v3's current state (≈30-35% of the architectural vision shipped) to the stated end-state: an **STG-inspired, V8-influenced bytecode VM** for Nix with a custom generational GC, cppnix parser reuse, thin FFI, and pure-bytecode evaluation.

Companion docs:
- `ACTION_PLAN_2026-05-15.md` — immediate 8-week corrective plan (Phases 0-4). Stage 1 below points to it.
- `ALIGNMENT_SCORECARD_2026-05-15.md` — vision-vs-reality scorecard updated quarterly.

The roadmap covers Stages 1 through 8 (~44 weeks, target end ≈ 2027-Q1).

---

## Strategic ordering rationale (why this order)

Stage order is fixed by dependencies. Each stage *unlocks* the next; out-of-order execution wastes effort.

1. **Correctness before optimization.** V8-style PICs are useless if the VM cannot complete the workload. → Action plan first.
2. **Pure-eval before nursery default-on.** The nursery's value is proportional to allocation traffic *through v3*, not TW. If TW does the work, the nursery is dead weight. → Push TW out of the picture before measuring nursery wins.
3. **Uniform STG before shapes.** V8 hidden classes assume consistent allocation paths per source construct. Today's emit-time eager/lazy asymmetries mean the same source position produces different runtime shapes. → Fix lowering uniformity before tagging shapes.
4. **Shapes before PICs.** A PIC needs a shape key. No shapes → no inline cache.
5. **PICs before selector thunks.** Selector thunks share work; the shape system is what tells you that two uses share the same source.
6. **Thin FFI is parallel, ongoing.** Reducing the bridge surface happens alongside Stages 2-7; it has explicit checkpoints in Stages 2 and 8 but no standalone phase.

Diagram of dependencies (arrows = "must complete before"):

```
[Action Plan (Stage 1)]
        |
        v
[Stage 2: Pure-bytecode eval] ----+
        |                         |
        v                         v
[Stage 3: Nursery default-on]   [Stage 8: Thin FFI ongoing]
        |                         |
        v                         |
[Stage 4: Uniform STG-shape]      |
        |                         |
        v                         |
[Stage 5: Hidden classes]         |
        |                         |
        v                         |
[Stage 6: PICs]                   |
        |                         |
        v                         |
[Stage 7: Selector thunks] <------+
```

---

## Stage 1 — Action plan completion (Weeks 1-8, prerequisite)

Out of scope here. Refer to `ACTION_PLAN_2026-05-15.md`.

**Exit (≈2026-07-10)**:
- `hello.name` evaluates correctly in v3-direct.
- Bench within 1.3× of TW on `lib-evalModules-100`.
- `vm.cc` split into ≤6 files, each ≤2 000 LoC.
- Env-var count ≤30.
- C1-C8 silent semantic gaps closed; `.err.exp` diffing in test runner.

Until these are met, **do not start Stage 2.**

---

## Stage 2 — Achieve pure-bytecode evaluation (Weeks 9-14)

### Goal

Retire `NIX_V3_SKIP_INSTALLABLE_PREEVAL` entirely. v3-direct evaluates all real nixpkgs workloads without TW pre-eval. Bridge layer reduced from ~370+ LoC in vm.cc to a clean FFI surface.

### Why now

The architectural promise is "most evaluation in the pure bytecode VM." Today TW pre-eval is default-on (commit 1ac5795b0). Until that's reversed, v3 is decorative — TW does the real work and v3 is along for the ride.

Doing this before Stage 3 matters because: the nursery's value scales with allocation traffic. If TW is doing the work, the nursery sees no traffic, and any perf measurement of "nursery on/off" is noise.

### Prerequisites

- Action plan Phase 2 closed (cycle-handling architectural decision committed: either CELL_EVERYWHERE default-on or fixed THUNK_ALL).
- Action plan Phase 3 closed (closure-pool reckoning).
- Bench harness running weekly.

### TODOs

- [ ] **Flip the gate**. Set `NIX_V3_SKIP_INSTALLABLE_PREEVAL=1` for one week of development. Run the v3-direct workload sweep. Catalogue every workload that breaks. (1 day to set up; 1 week observation.)
- [ ] **Triage breakages**. For each failure, classify: (a) v3 bug, (b) TW dependency we forgot existed, (c) bridge-layer escape hatch we can delete. (2 days.)
- [ ] **Fix category (a)** v3 bugs. Each fix follows the action plan's discipline: regression test in-commit, no new env-var without retirement criterion.
- [ ] **For category (b)**, identify the v3 equivalent: a primop bridge, IFD path, attribute traversal, etc. Implement the v3-native version. (2-3 weeks total across this and the next item.)
- [ ] **For category (c)**, delete the escape hatch and re-test.
- [ ] **Audit `bridge_yield.cc`** call sites. Each one is either (i) a legitimate FFI to cppnix store/derivation primitives — keep, but move to `ffi.cc` — or (ii) a v3-can't-handle escape hatch — delete after v3 handles. (4 days.)
- [ ] **Audit the ~370 LoC of bridge plumbing in vm.cc** (`v3CallBridge1`, `v3ForceAttr`, `clearBlackMarksOnException`, etc.). Move legitimate FFI to `ffi.cc`. Delete escape hatches. (1 week.)
- [ ] **Delete the `NIX_V3_SKIP_INSTALLABLE_PREEVAL` gate** entirely.
- [ ] **Document the new FFI surface**: one `.hh` file listing every cppnix entry point v3 calls. (1 day.)
- [ ] **Bench**. Re-run real-world workloads (hello.name, attrNames-on-nixpkgs, cardano-node, libsForQt5.kdevelop) and commit a `bench/baselines/stage-2-exit.json`.

### Exit criteria

- `NIX_V3_SKIP_INSTALLABLE_PREEVAL` is deleted (not just default-off).
- `bridge_yield.cc` is deleted or reduced to <100 LoC.
- vm.cc bridge plumbing is <100 LoC; ≤5 call sites total.
- Full nixpkgs `attrNames` completes in v3-direct.
- cardano-node evaluates in v3-direct.
- `ffi.cc` surface is documented.

### Verification

Weekly bench harness: must show v3-direct completing all real-world workloads. If any workload regresses to "cannot complete," Stage 2 is not done.

### Kill criterion

If after 6 weeks ≥3 workload categories still need TW pre-eval, the cycle-handling architecture chosen in Action Phase 2 was wrong. **Go back to that decision.** Do not paper over with new gates.

### What this stage unlocks

- Stage 3 can be measured meaningfully (allocation traffic now flows through v3).
- Stage 8 (thin FFI) has its first big checkpoint behind it.
- The project can honestly claim "pure-bytecode evaluation" for the first time.

---

## Stage 3 — Nursery default-on, closure-pool retired (Weeks 15-20)

### Goal

`NIX_V3_NURSERY` becomes opt-OUT (rename to `NIX_V3_NO_NURSERY`). Closure-pool is either deleted or simplified to a small fast-path without sentinel bits. The Cheney nursery is the primary allocator for v3 values.

### Why now

A generational GC is in the stated vision. Today the closure-pool fills the void — a hand-rolled recycling pool with `_pad = 0xFA5E`, `CFF_FAKECLO_TAINTED`, `kFakeCloMagic` sentinel infrastructure. A5/A6 bugs prove the recycle protocol is ill-defined. The Cheney design (`CHENEY_NURSERY_DESIGN.md`) is most of the way there — Phase A (allocator) and Phase C (scavenge) landed. What's missing is Phase D (write barriers) and Phase E (scavenge frequency policy).

Doing this before Stage 4 matters because: Stage 4 will push allocation rate up by 5-10× (every binding becomes a thunk). The nursery has to absorb that, or Stage 4 will look like a perf regression.

### Prerequisites

- Stage 2 closed (real allocation traffic to measure against).
- `V3_DBG_GC_STRESS` (random forced GC) functional.

### TODOs

- [ ] **Re-read `CHENEY_NURSERY_DESIGN.md`**. Identify the three Phase D options (A/B/C in that doc).
- [ ] **Profile the workload**: how often does v3 actually write through old→new pointers in nixpkgs eval? Use `perf` or Instruments to measure. (3 days.)
- [ ] **Choose Phase D path** based on the profile:
  - If old→new writes are rare: card-table or per-page dirty-bits (cheapest barrier).
  - If old→new writes are frequent and concentrated: targeted barriers at known write sites only.
  - If old→new writes are pervasive: full Steele-style barrier on every write.
  Document the choice in a one-page `NURSERY_PHASE_D_DECISION.md`. Close `CHENEY_NURSERY_DESIGN.md` with a RESOLVED row.
- [ ] **Implement Phase D**. (1.5-2 weeks.)
- [ ] **Implement Phase E (scavenge frequency)**. Heuristic options: every N allocations, every M dispatch-loop entries, watermark-based on nursery occupancy. Pick one, with rationale. Bench-tune the threshold. (3-4 days.)
- [ ] **Stress-test**. `V3_DBG_GC_STRESS=1` forces a scavenge after every 10-100 allocations. Run lang tests + nixpkgs eval; verify no value corruption. (1 week of run-and-fix.)
- [ ] **Closure-pool decision**:
  - Option α: delete the pool entirely; everything allocates through the nursery. Simplest. Bench-measure.
  - Option β: keep the pool as a hot-path-only allocator for `cl_force` / `forceValue` Suspended thunks (the single highest-frequency closure shape). No recycle protocol; freshly nursery-allocated each call. No sentinel bits.
  Decide based on Stage 2 bench numbers + α-vs-β micro-bench. Default: choose α unless β shows ≥5% on canonical bench.
- [ ] **Flip default**: rename `NIX_V3_NURSERY` to `NIX_V3_NO_NURSERY`; default-OFF (i.e. nursery default-on).
- [ ] **Retire `_pad = 0xFA5E`, `CFF_FAKECLO_TAINTED`, `kFakeCloMagic`** and related sentinel infrastructure.
- [ ] **Add property-test**: under `V3_DBG_GC_STRESS`, run randomized expression evaluation and assert (a) no crash, (b) result matches non-stressed run. (3 days.) This is the property-test framework that the scorecard called out as an orphan; landing it here lets it cover all subsequent stages.

### Exit criteria

- Nursery is default-on.
- Closure-pool is either deleted or simplified (no sentinel bits).
- A5-class corruption is structurally impossible (adversarial stress test passes 1000+ runs).
- Allocation throughput on alloc-heavy workloads ≥ TW.
- Bench within 1.2× TW on canonical (no regression vs Stage 2 baseline).

### Kill criterion

If Phase D write-barrier work exceeds 4 weeks, or measurements show no allocation benefit over the closure-pool, defer Stage 3 and skip ahead to Stage 4 — uniform STG with closure-pool as the allocator. Re-attempt Stage 3 after Stage 6 (PICs), when reduced allocation pressure may make the choice clearer.

### What this stage unlocks

- Stage 4 can ship without an allocation-pressure cliff.
- A property-test framework exists for the remaining stages.

---

## Stage 4 — Uniform STG-shape (Weeks 21-28)

### Goal

Eliminate emit-time eager/lazy asymmetries. Every binding is lazy by default in `lower.cc`; a strictness-analysis pass un-thunkifies where provably safe. The S5 / eager-inherit-from bug class becomes structurally impossible.

### Why now

The recurring bug cascade `#496 → #497 → #498 → #516 → #546 → #548 → #577 → #583` is one root cause manifesting under N labels: lower.cc makes per-construct decisions about thunkifying, and those decisions don't match TW's blanket `maybeThunk`. The fix is architectural: move the laziness decision out of `lower.cc`'s heuristics and into a separate optimizer pass that runs on the IR.

Doing this before Stage 5 matters because: V8 hidden classes assume that the same source position produces the same runtime shape. Today's per-construct thunkification means a single source position can produce a thunk *or* a forced value depending on context. That's incompatible with shape-keying.

### Prerequisites

- Stage 3 (nursery can absorb the 5-10× allocation spike).
- Action plan Phase 1 closed (iterative forceValue, so deep thunk chains don't blow the C-stack).

### TODOs

- [ ] **Audit `lower.cc` thunkification sites**. Find every place lower.cc decides "thunkify this or not." Document in `LOWERING_THUNKIFY_AUDIT_2026-XX-XX.md`. Expect 8-15 sites. (3 days.)
- [ ] **Audit `opt_strictness.cc`** (185 LoC). What does it currently identify? Likely partial — perhaps just "binding immediately followed by a force." Document its coverage. (1 day.)
- [ ] **Design the uniform lowering**. Emit ALL bindings as thunks by default. Have a single explicit "force this argument" annotation for known-strict primops (e.g. `OP_ADD` integer args). (4 days; one-page design doc.)
- [ ] **Expand strictness analysis**. The pass needs to identify:
  - Definitely-forced bindings (single force on a path from definition to use, no conditional).
  - Inlinable-once bindings (one use, no force conditional).
  - Loop-invariant bindings (forced in every iteration; safe to pre-force outside).
  Reference: GHC's strictness analyzer + the optimizer plan doc. (2 weeks.)
- [ ] **Wire strictness pass into `optimise()`** after const-folding, before primop-fuse. (1 day.)
- [ ] **Re-implement lowering** with uniform-thunk-default. (1 week.)
- [ ] **Lang test green**. All 142 must pass at every commit during this stage. (Ongoing.)
- [ ] **Real-world bench**. Expect: allocation rate up 5-10×, perf neutral or +/-15% on canonical workloads thanks to the nursery + strictness pass.
- [ ] **Retire all per-construct laziness env-var gates**: `NIX_V3_INHERIT_FROM_THUNK_ALL`, `NIX_V3_NO_INHERIT_FROM_THUNK_ALL`, `NIX_V3_INHERIT_FROM_THUNK_FILTER`, etc. (1 day, after the above lands.)
- [ ] **Retire the `OP_ATTRS_REC_INIT` split** (#546): if all bindings are uniform thunks, the rec-attrs-vs-let-in-body distinction collapses to a single opcode. (3 days.)
- [ ] **Close `CALLPACKAGE_BUG_2026-05-09.md`, `EVAL_ORDER_DIVERGENCE_2026-05-08.md`**, `CELL_UPDATE_EVERYWHERE_2026-05-12.md` with RESOLVED rows.

### Exit criteria

- All per-construct laziness env-var gates deleted (-10+ from the inventory).
- `opt_strictness.cc` is a documented pass with full IR coverage.
- The S5 / eager-inherit-from / cycle bug class is structurally impossible to reintroduce.
- Bench within 1.2× of TW on canonical workloads.
- All lang tests pass.

### Kill criterion

If uniform-thunk allocation overhead exceeds 2× TW even with Stage 3's nursery on, strictness analysis isn't pulling its weight. Diagnose: is the strictness pass missing patterns it should catch, or is allocation itself the bottleneck? If the former, expand the pass; if the latter, Stages 5-7 might need to ship before Stage 4 is considered done.

### What this stage unlocks

- Stage 5: shape tagging now has a uniform allocation path to key off.
- All cycle/blackhole work from Action Plan Phase 2 becomes structurally retire-able.
- The cleanest part of the codebase (the optimizer) becomes the most load-bearing.

---

## Stage 5 — Hidden classes / attrset shapes (Weeks 29-34)

### Goal

Attrsets carry a shape descriptor. Same source position produces the same shape across runs. Same shape → cacheable lookup paths. This is V8's foundational optimization.

### Why now

Strictness analysis (Stage 4) made allocation paths uniform. Now they can be tagged with shape. Without uniformity, shapes would fragment.

Doing this before Stage 6 matters because: PICs are the optimization that uses shapes. No shapes → no PIC.

### Prerequisites

- Stage 4 (uniform allocation paths).

### TODOs

- [ ] **Shape representation design doc** (`SHAPE_DESIGN_2026-XX-XX.md`). Options:
  - Interned name-set (sorted vector of `Symbol*`, hash-consed). Simplest. Probably right.
  - Transition tree (V8-style "hidden class chain"). More complex, supports incremental addition.
  - Bloom filter + name-set. Cheap "shape miss" detection.
  Pick one. (3 days.)
- [ ] **`Shape*` table + interning**. Global hash-consed table; each `Shape` has a canonical pointer. (3 days.)
- [ ] **Attrset cell carries a `Shape*`** (or shape ID, 32-bit interned index). Modify `Bindings` / attrset representation. (1 week.)
- [ ] **`OP_ATTRS_BUILD` emits shape-tagged attrsets**. At each emit site, compute the shape at compile time and bake it in. (3 days.)
- [ ] **`OP_ATTRS_SELECT` shape capture**. At first execution of each SELECT site, record the observed `(shape_id, slot_index)` in an inline cache slot tied to the bytecode position. Do NOT yet act on the cache — just observe. (3 days.)
- [ ] **Telemetry pass**. After 1 week of usage in development, query: what fraction of OP_ATTRS_SELECT sites observe ≤2 shapes? ≤4? Megamorphic (>10)? This validates whether shapes have predictive power. (1 day.)
- [ ] **Bench**. Should be near-neutral; if it's a regression, shape representation is too heavy. Tune. (2-3 days.)

### Exit criteria

- Every attrset has a shape descriptor.
- 95%+ of OP_ATTRS_SELECT sites observe ≤2 shapes on real workloads (validates predictive value).
- Bench within ±5% of Stage 4 baseline (no-op overhead until Stage 6 lights it up).

### Kill criterion

If attrsets at the same source position routinely produce 10+ distinct shapes, the shape system isn't capturing the V8-applicable pattern. Options:
- Redesign shape with name-subset matching (allow shape A to "match" shape B if A ⊂ B).
- Skip Stage 6 (PICs) and go directly to Stage 7 + 8.
- Abandon V8-style optimization for Nix; declare Stage 5+6+7 not-applicable; pivot to direct-threading dispatch instead.

### What this stage unlocks

- Stage 6: PICs have a cache key.
- Stage 7: selector thunks can determine sharing equivalence via shape.

---

## Stage 6 — Polymorphic Inline Caches (Weeks 35-40)

### Goal

`OP_ATTRS_SELECT` and `OP_CALL` hot paths cache `(shape → slot)` and `(closure → entry)` at the bytecode site. Cache invalidation on shape change.

### Why now

This is where the V8-style perf win materializes. Without it, Stage 5 is observation-only overhead.

### Prerequisites

- Stage 5.

### TODOs

- [ ] **`OP_ATTRS_SELECT` 1-PIC**: each select site has one cache slot `(Shape*, slot_index)`. Fast path: check shape pointer equality; on hit, direct slot access. On miss, fall back to dictionary lookup + update cache. (1 week.)
- [ ] **2-PIC**: each select site has two cache slots. Switch to 2-PIC when first-slot miss happens on a recurring second shape. (3 days.)
- [ ] **Megamorphic fallback**: after 5+ misses across distinct shapes, mark the site megamorphic; fall back to dictionary lookup permanently. (1 day.)
- [ ] **`OP_CALL` PIC**: cache `(closure_shape → fast_path)` at callsite. Closure shape is encoded by lambda position + capture set. (1 week.)
- [ ] **Cache invalidation**: when a shape is retired (rare; only on shape table garbage-collection), invalidate dependent PICs. Initially: don't GC shapes (they're cheap). Address only if shape table grows unbounded. (1 day to defer.)
- [ ] **Bench measurement**. This is THE perf checkpoint. Expected: 1.5-2× on attribute-heavy workloads (lib-evalModules, real nixpkgs lookups). (1 week of bench + tune.)

### Exit criteria

- Bench shows ≥30% improvement on `lib-evalModules-100` vs Stage 5 baseline.
- PIC hit rate ≥85% on real nixpkgs.
- vm.cc opcode dispatch for ATTRS_SELECT and CALL has a clean fast-path/slow-path split.

### Kill criterion

If PICs add <15% perf even at 85% hit rate, dispatch overhead in `vm.cc` is the dominant cost, not attribute-lookup overhead. Insert a **Stage 6.5** (direct threading / computed-goto opcode dispatch) before continuing.

### What this stage unlocks

- The V8-style perf win is realized.
- Stage 7's selector thunks can use shape sharing as evidence of common-subexpression-ness.

---

## Stage 7 — Selector thunks (Weeks 41-44)

### Goal

`inherit (a) b; inherit (a) c` shares the force of `a`. Repeated attribute-path access shares work. The pattern is structurally recognized by the optimizer — no nixpkgs-specific heuristics.

### Why now

Selector thunks are a known-good optimization for Nix workloads (cppnix has them; some Nix forks implement them). They depend on identifying sharing opportunities; the shape system (Stages 5-6) provides the mechanism.

This is the smallest stage — ~3 weeks. It's the natural cleanup after PICs.

### Prerequisites

- Stages 4, 5 (shape system identifies sharing equivalence).

### TODOs

- [ ] **Detection**. `lower.cc` identifies `inherit (X) a, b, c, ...` patterns. (2 days.)
- [ ] **`OP_SELECTOR_THUNK` opcode**. Wraps a target expression + a list of selector names. (3 days.)
- [ ] **Force protocol**. Forcing a selector thunk forces the target once; subsequent selections become attrset SELECT via the cached shape. (3 days.)
- [ ] **Lang tests + perf bench**. (1 week.)
- [ ] **Extend to attribute-path repetition**: `let p = a.b.c; in p + p` is the same opportunity. `opt_cse.cc` may already handle it; verify. (3 days.)

### Exit criteria

- Bench shows ≥10% on `inherit (X) a b c d`-heavy workloads (some lib/* modules; texlive).
- Pattern is recognized structurally; no `texlive`-specific or nixpkgs-specific heuristics.

### What this stage unlocks

- All stated optimization layers from the vision are in place.

---

## Stage 8 — Thin FFI surface + primops classification (parallel, Weeks 9-44)

### Goal correction (important)

The goal is NOT "shrink primops.cc to <3 000 LoC." That was a misframing in the initial roadmap draft. **v3-native primops are architecturally correct** because:

1. **GC ownership.** v3 Values live in v3's nursery (or whatever v3 allocator); TW Values live in Boehm-managed memory. Marshalling between them requires copy or wrapping at every primop call boundary.
2. **Hot-path cost.** `primMap`, `primFilter`, `primAttrNames`, `primFoldlPrime`, `primConcatMap` etc. are called millions of times in real nixpkgs eval. Each call materializing v3 List/Bindings → TW List/Bindings → v3 List/Bindings would dominate eval time.
3. **GC safety.** Crossing the boundary mid-eval means a v3 GC scavenge cannot safely move v3 Values that are temporarily held by a TW primop, and a Boehm collection cannot safely move TW Values held by a v3 primop. Either marshall-by-copy (slow) or pin (correctness hazard).

Therefore the FFI surface is for **system boundaries**, not for pure data ops:

**FFI-bridged** (talk to cppnix):
- Store operations (deriving, addToStore, paths, IFD).
- File I/O (`readFile`, `readDir`, `findFile`).
- Process / network primitives (`fetchurl`, `fetchTarball`, etc., to the extent they survive in modern Nix).
- Path normalization and store-path validation.
- Eval-state operations that need cppnix's parser/state (`builtins.fromJSON`, `import` at runtime, etc.).
- Symbol/Name interning shared with cppnix where mutual visibility is required.

**v3-native primops** (stay v3-native):
- All pure list ops: `map`, `filter`, `foldl'`, `head`, `tail`, `length`, `elemAt`, `concatMap`, `genList`, `partition`, `groupBy`.
- All pure attrset ops: `attrNames`, `attrValues`, `hasAttr`, `getAttr`, `mapAttrs`, `listToAttrs`, `catAttrs`.
- All pure string ops: `split`, `replaceStrings`, `substring`, `stringLength`, `toString`, `concatStringsSep`.
- All arithmetic + comparison.
- `__toString` dispatch, `<-?`, `//`, `++`, `+` over strings/paths.
- Anything that takes only `Value*` (or v3 Cells/Bindings/Lists) and returns `Value*` of pure-data type.

### Why parallel

This stage is cleanup work distributed across Stages 2-7. It doesn't gate any stage but each stage has natural checkpoints to land FFI-clarification commits.

### TODOs (distributed)

- [ ] **(Stage 2)** Bridge audit + reduction (already in Stage 2 TODOs). The target here is removing `bridge_yield.cc` and the ~370 LoC bridge plumbing in vm.cc — these are v3-can't-do-it ESCAPE HATCHES, not the same thing as FFI primops.
- [ ] **(Stage 2)** Document the FFI surface in one `.hh` file (`ffi.hh` already exists; promote it to canonical). Lists every cppnix entry point v3 calls. (1 day.)
- [ ] **(Stage 3)** **Primops classification audit**. Classify every entry in `primops.cc` into (a) v3-native pure data op (stays), (b) FFI-bridged system primitive (stays, may move to a thin wrapper around `ffi.cc`), (c) duplicates work from cppnix that should be FFI-bridged. Document in `PRIMOPS_CLASSIFICATION_2026-XX-XX.md`. (4 days.) Expected: ~80% category (a), ~15% category (b), ~5% category (c).
- [ ] **(Stage 3-4)** For category (c) — actual duplication: replace with FFI thin wrapper. These are typically things that already require cppnix state (e.g. `builtins.fromJSON` uses a JSON parser cppnix has). Do NOT do this for pure ops just because cppnix has its own implementation.
- [ ] **(Stage 4)** Consolidate inline-opcode primop duplication. `primHead`, `primTail`, `primLength`, `primElemAt` are re-implemented inline in vm.cc OP_HEAD/TAIL/LENGTH/ELEM_AT. Factor to shared inline functions in a v3-internal header, OR accept duplication with explicit `// keep in sync with primops.cc:NNN` anchors. (2 days.) This is D4b in the scorecard.
- [ ] **(Stage 5-6)** Shape-system migrations: as shapes mature, primops that take attrsets (`attrNames`, `mapAttrs`, etc.) can use the shape descriptor for fast iteration instead of walking the cell. Migrate them. (3-5 days per primop, distributed.)
- [ ] **(Stage 7)** Final FFI surface review. `ffi.hh` should describe a stable, minimal surface. Documented contract. (2 days.)

### Exit criteria (at end of Stage 8, ≈Week 44)

- `ffi.hh` is the documented FFI surface (one canonical header listing every cppnix entry point v3 calls).
- `bridge_yield.cc` deleted.
- vm.cc bridge plumbing <100 LoC.
- Every entry in `primops.cc` is classified as native vs FFI; classification is in-source as a doc comment.
- Category (c) duplication eliminated (expected delta to primops.cc: ~500-1 000 LoC removed; final size likely ~7 000-7 800 LoC and that is fine).
- No v3 file has more than 100 lines of FFI plumbing (excluding `primops.cc` itself, where v3-native primops are correct and intentional).

---

## Cross-stage standing cadence (preserved throughout)

- **Action plan's weekly cadence continues**: Monday bench re-run; Friday env-var delta audit; net gate count must monotonically decrease.
- **Quarterly**: re-score `ALIGNMENT_SCORECARD_2026-05-15.md`. Trigger an alignment review (not just a stage review) if drift criteria fire.
- **Per-stage exit**: append a one-line RESOLVED row to the relevant scorecard component + this roadmap.
- **Per-commit**: every gate added has an inline retirement criterion; every "Phase/Stage follow-up" has a victory condition; no new RCA letter on an open one.

---

## End-state target (≈ 2027-Q1, after Stage 7)

The scorecard at the end of Stage 7 should read:

| # | Component | Status |
|---|-----------|--------|
| 1 | Parser reuse | ✅ |
| 2 | Bytecode VM core | ✅ (vm.cc ≤2 500 LoC; clean dispatch) |
| 3 | Optimizer pipeline | ✅ (occur + strictness wired) |
| 4 | STG thunk states | ✅ |
| 5 | STG-shape uniformity | ✅ (uniform lowering + strictness analysis) |
| 6 | V8 hidden classes / shapes | ✅ |
| 7 | Polymorphic Inline Caches | ✅ |
| 8 | Selector thunks | ✅ |
| 9 | Generational GC | ✅ (nursery default-on; closure-pool retired) |
| 10| Thin FFI | ✅ (ffi.cc documented surface; bridge_yield gone) |
| 11| Pure bytecode evaluation | ✅ (NIX_V3_SKIP_INSTALLABLE_PREEVAL deleted) |
| 12| Bytecode disk cache | ✅ |

**Bench**: v3 ≥1.0× TW on `lib-evalModules-100`, ≥1.0× on `fib33`, ≥0.7× on full nixpkgs `attrNames` (i.e. v3 is faster — the V8/STG win).

**Code health**: vm.cc ≤2 500 LoC; `bridge_yield.cc` deleted; bridge plumbing in vm.cc ≤100 LoC; env-var count ≤20; lode/ has ≤5 active docs. primops.cc remains substantial (~7 000-7 800 LoC) — that's intentional and correct, because v3-native pure-data primops are part of the design.

**Process**: no RCA letter has been opened that wasn't closed within 7 days for the past 6 months.

---

## Meta-kill criterion (project-level decision point)

**At end of Stage 2 (≈ Week 14, ≈ 2026-08-21)**: if `NIX_V3_SKIP_INSTALLABLE_PREEVAL` cannot be deleted — i.e. v3-direct cannot evaluate real workloads without TW pre-eval — the architecture choice "v3-direct as primary" is wrong.

The honest options at that point are:
- **Pivot to "v3 as a JIT-style optimizer for hot paths, TW remains primary"**. Narrower, defensible, less ambitious. The action plan's wins (gate retirement, vm.cc decomp, correctness fixes) all still apply.
- **Abandon v3 and revisit later**. When one of (a) cppnix major version refactor, (b) better profiler tooling, (c) sponsored full-rewrite resourcing lands. The lode/ archive remains valuable as design study for a future attempt.

**Stages 3-7 only make sense if Stage 2 actually closes.** If Stage 2 misses, do NOT silently proceed to Stage 3.

---

## Risk register

| Risk                                                                 | Likelihood | Impact | Mitigation                                                                          |
|----------------------------------------------------------------------|------------|--------|-------------------------------------------------------------------------------------|
| Stage 2 reveals more TW-dependencies than expected                    | Medium     | High   | Kill criterion triggers project-level pivot; not a sunk-cost continuation           |
| Phase D nursery write barriers blow out timeline                      | Medium     | Medium | Stage 3 kill criterion defers nursery; uniform STG can still ship on closure-pool   |
| Strictness analysis can't catch enough patterns                       | Low        | High   | Reference GHC + the optimizer doc; this is well-studied territory                   |
| Shapes fragment beyond predictive value on real nixpkgs               | Medium     | High   | Stage 5 has a measure-before-acting checkpoint; kill criterion redirects to Stage 7 |
| PICs add little perf due to dispatch overhead                         | Low        | Medium | Insert Stage 6.5 (direct threading) if measured                                     |
| Stage 4 allocation spike exceeds nursery absorption                   | Medium     | Medium | Stage 3 must close before Stage 4; if not, Stage 4 paces back                       |
| New env-vars accumulate during stages (action plan rules violated)    | High       | Medium | Weekly env-var audit; PRs that violate are reverted, not amended                    |
| Researcher-archaeology pattern returns (lode/ doc proliferation)      | Medium     | Medium | "No new design doc until previous closes" rule; quarterly re-evaluation             |

---

## Estimated effort summary

| Stage                                  | Weeks | Cumulative |
|----------------------------------------|-------|------------|
| 1 — Action plan completion             | 8     | 8          |
| 2 — Pure-bytecode eval                 | 6     | 14         |
| 3 — Nursery default-on                 | 6     | 20         |
| 4 — Uniform STG-shape                  | 8     | 28         |
| 5 — Hidden classes / shapes            | 6     | 34         |
| 6 — Polymorphic Inline Caches          | 6     | 40         |
| 7 — Selector thunks                    | 4     | 44         |
| 8 — Thin FFI (parallel)                | 0     | 44         |

**Total**: ≈44 weeks from 2026-05-15. Target completion: ≈ 2027-Q1.

This is **aggressive** for one engineer; comfortable for two. The single-engineer path implies fewer parallel Stage 8 commits and slower bench-tuning iteration. Pad timeline by ~25% (≈55 weeks, ≈ 2027-Q2) for a realistic single-engineer estimate.

---

## Appendix — Stage completion log

Append one row per stage as exits land. Format: `Stage N — RESOLVED YYYY-MM-DD — [met / met-with-caveats / missed]: <one-line summary>`.

- Stage 1 — [pending, target ≈ 2026-07-10]
- Stage 2 — [pending, target ≈ 2026-08-21]
- Stage 3 — [pending, target ≈ 2026-10-02]
- Stage 4 — [pending, target ≈ 2026-11-27]
- Stage 5 — [pending, target ≈ 2027-01-08]
- Stage 6 — [pending, target ≈ 2027-02-19]
- Stage 7 — [pending, target ≈ 2027-03-19]
- Stage 8 — [pending, completes parallel with Stage 7]

---

## Cross-references

- Action plan: `ACTION_PLAN_2026-05-15.md`
- Scorecard: `ALIGNMENT_SCORECARD_2026-05-15.md`
- What worked / what didn't: `LESSONS_LEARNED_2026-05-15.md`
- Nursery design: `CHENEY_NURSERY_DESIGN.md`
- Optimization plan (input for Stage 4-6): `OPTIMIZATION_PLAN.md`
- Occurrence analysis plan: `OPT_OCCUR_PLAN_2026-05-08.md`
- Reduction audit: `REDUCTION_AUDIT_2026-05-09.md`
- Cleanup audit: `CLEANUP_AUDIT_2026-05-09.md`
- V3-NATIVE constraint origin: commit `cf12c1880`
