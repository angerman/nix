# Sample-driven cross-section: TW vs v3-direct on hello.name (2026-05-18)

After Phase 1.6 landed, used `sample(1)` to profile the v3-vs-TW gap
on the same workload that had regressed 2× since Phase-1-MET.  Data
captured at HEAD `f045e3185`; sample tables archived under
`bench/samples/sample-{tw,v3,v3-noopt,drv}-top30.txt`.

## Workload

```
(import <nixpkgs> { system = "aarch64-darwin"; }).hello.name
   → "hello-2.12.3"
```

Wall time (3 runs each, fresh process):
- TW: 0.72s, 0.73s, 0.73s.
- v3-direct (Phases A-H on): 1.74s, 1.75s, 2.01s.
- v3-direct (NIX_V3_NO_OPT=1, all phases off): 1.49s, 1.52s, 1.52s.

Two findings up front:
1. v3-direct's "Phase A-H off" baseline is **2.07× TW** (1.50s vs 0.72s).
   So most of the gap is structural, not optimizer overhead.
2. Phases A-H add 250-500 ms (15-30%) on top.  Real but secondary.

## Top-N hot leaves

`sample` runs for 3 s and groups equal-stack samples; the table below
is the "Sort by top of stack" section's leaves with ≥5 samples,
excluding `__psynch_cvwait` / `__workq_kernreturn` (mutex + worker
idle, present at similar rates in both binaries).

### TW

| Samples | Function |
|---|---|
| 14 | `yylex` (parser) |
| 8 | `nix::parser::BisonParser::parse` |
| 5 | `nix::Bindings::iterator::operator++` |
| 5 | `nix::EvalState::callFunction` |
| 5 | `nix::ExprSelect::eval` |
| 5 | `nix::ExprVar::eval` |
| 5 | `nix::ExprVar::maybeThunk` |
| 5 | `nix::parser::BisonParser::basic_symbol<...>::clear` |
| 5 | `std::__pop_heap` (Bindings sort) |

Total identifiable v3-vs-TW-meaningful hot work: ~50 samples,
distributed across many functions at 5 each.  **Eval itself
disappears into the noise floor** — TW finishes fast and waits.

### v3-direct (Phases A-H on)

| Samples | Function |
|---|---|
| **39** | `unordered_map<PosKey, uint32_t>::find` (attr-pos lookup) |
| **25** | `primIntersectAttrs` |
| **23** | `unordered_map<PosKey, uint32_t>::emplace` (attr-pos record) |
| **22** | `dispatchLoop` |
| **18** | `mergeBindings::'lambda'` |
| **15** | `unordered_map<VarId, const Expr*>::emplace` (IR opt `defs` map) |
| 12 | `unordered_map<unsigned int>::emplace` (likely a related opt map) |
| 8 | `CompilationUnit::~CompilationUnit` (teardown) |
| 25+ | `_xzm_free` / `_free` / `_platform_memmove` / `__bzero` (allocator) |

Total identifiable concentrated work: ~160 samples — **3.2× TW's
concentration**.  v3 spends real CPU in identifiable hotspots; TW
doesn't.

### v3-direct, NIX_V3_NO_OPT=1 (Phases A-H off)

| Samples | Function |
|---|---|
| **45** | `PosKey::find` |
| 29 | `dispatchLoop` |
| **17** | `mergeBindings::'lambda'` |
| **13** | `primIntersectAttrs` |
| **13** | `PosKey::emplace` |
| 10 | `mergeBindings::'lambda0'` |
| 13 | `std::vector<string>::__assign_with_size` (somewhere in primops) |
| 11 | `unordered_map<unsigned int>::emplace` |
| 16 | `yylex` |

Note: the giant `<VarId, std::variant<...80 Expr alternatives...>>`
hash table from the OPT-on profile is **absent** here — confirms it
was the IR-opt `defs` map, accounting for ~27 samples (~5% of v3
profile) of pure Phase-A-H overhead.

## Cross-section verdict — where the 2× regression lives

The bulk of v3-direct's extra time over TW is **NOT** in the
optimizer pipeline.  It's three structural costs:

1. **`PosKey` hash table — 60+ samples (~5% v3 profile)** is the
   biggest single contributor.  `recordAttrPos` fires every time
   OP_ATTRS_REC_SET / OP_ATTRS_REC_INIT_TAIL / mergeBindings runs
   (vm.cc:5526, 5598, 5670, 5754, 5793 + the merge sites at 658,
   659, 664, 665).  Stores `(Bindings*, SymbolId) → pos` so
   `builtins.unsafeGetAttrPos` can look it up later.  For
   `hello.name`, the lookups never happen — the recording is pure
   waste.

2. **`mergeBindings` lambda + `primIntersectAttrs` — ~45 samples
   combined**.  Both are attrset operations in the Option-4
   derivation hybrid path.  Merging is N×log(N); `mergeBindings`'s
   lambda is the per-entry comparator + insertion path that builds
   the merged Bindings.  Hot because each derivation in the
   hello-stdenv chain triggers a merge to produce the output
   attrset.

3. **IR-opt `defs` map — ~27 samples** (visible only with Phases
   A-H on).  Each opt pass (beta-reduce, primop-fold, app-spine,
   etc.) builds a `unordered_map<VarId, const Expr*>` per block,
   throws it away after the pass.  With 10+ bytecode-primop CUs at
   startup, each running ~12 passes, that's ~120 map constructions
   per process.  The variant value type has 42 alternatives so each
   insert has high constant cost.

## hello.drvPath has the same pattern but deeper

`sample-drv-top30.txt` (the hello.drvPath capture from the earlier
hang investigation) shows ~60% of CPU in `treeWalkerToV3` recursive
self-calls (primops.cc:4408 ↔ 4467).  Same FFI-bridge cost class as
`mergeBindings`; the difference is depth.  drvPath traverses the
full transitive derivation graph; name only the top derivation.

## Action options ranked by ROI

| # | Optimization | Estimated win | Effort | Risk |
|---|---|---|---|---|
| 1 | **Replace `attrPosTable` global with a `Bindings::posPtr` side-pointer** (allocated only when at least one entry has a known pos). Eliminates the hash on every recordAttrPos + lookupAttrPos. | ~60 samples → ~5% wall on hello.name; bigger on derivation-heavy workloads | 1 day | Med — touches every Bindings construction site |
| 2 | **Gate `recordAttrPos` behind `NIX_V3_RECORD_ATTR_POS` (default off)**.  `unsafeGetAttrPos` returns 0 ("unknown") when gate is off.  Trades feature for perf. | ~60 samples; matches #1's payoff but trivial to land | 0.25 day | Low — only `unsafeGetAttrPos` semantically affected; nixpkgs uses it sparingly for error messages |
| 3 | **Refactor `mergeBindings` lambda body** — the inner lambda is captured-by-reference in the comparator path; understand exactly why it's hot (alloc? sort? equality?). | ~15-25 samples | 0.5 day | Low |
| 4 | **Cache IR `defs` map across passes within a single optimise() call** — passes serially consume the same block defs.  Build once, reuse N times. | ~15-25 samples; bigger on programs with many small blocks | 0.5-1 day | Low — internal opt-pass change |
| 5 | **Convert `treeWalkerToV3` to iterative with a shared seen map** — fixes the hello.drvPath unbounded recursion class.  Also speeds hello.name slightly. | drvPath: 30-50% reduction.  name: ~10-20 samples (the small slice attributable). | 1-2 days | Med — careful work-stack design |

The smart cheap move is **#2** first (1 day → ~5% on hello.name +
~5-10% on derivation workloads where every entry of every attrset
is recorded), then **#1** as a follow-up once we know what
nixpkgs paths actually rely on attr-pos info.  **#5** is the
biggest single floor-mover for drvPath/outPath but is the
largest commit.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group.  SPDX-License-Identifier: Apache-2.0.
