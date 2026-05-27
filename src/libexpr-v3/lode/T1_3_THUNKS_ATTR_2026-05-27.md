# T1.3 Thunks attribution — concentrated at OP_MAKE_THUNK

**Date**: 2026-05-27
**Origin**: `MEMORY_REDUCTION_AVENUES_2026-05-26.md` Category 1,
"T1.3 per-alloc-site for Thunks/Closures/ListVecs/Strings".
Templated from #746 BINDINGS_ATTR.
**Status**: infrastructure landed; HNE measurement complete; finding
documented.

## TL;DR

`NIX_V3_THUNKS_ATTR=1` enables per-Thunk source-attribution dump at
end-of-run.  HNE measurement: **all 3.8 M Thunks (304.9 MB tracked
of 319.7 MB total) allocated from a SINGLE C++ site: `vm.cc:3707`
(`OP_MAKE_THUNK` opcode body)**.

This CONFIRMS the existing per-LambdaDescriptor attribution
(`V3_DBG_ALLOC_DUMP` via `LambdaDescriptor::allocCount`) is the
correct finer-grained lever for Thunks specifically.  C++-site
attribution is too coarse for the Thunks bucket — all roads lead
to `OP_MAKE_THUNK`.

## Why the result is what it is

The Bindings-attribution finding pattern was: 23 distinct C++ sites
across 1.06 M Bindings allocations on HNE; top-3 sites = 92.5 % of
bytes.  Multiple primops + bytecode opcodes call `Alloc::
allocBindings()`, producing real C++-site dispersion.

Thunks are different.  Bytecode-emitted Thunks all funnel through
`OP_MAKE_THUNK` (`vm.cc:3707`).  Primop-side Thunk creation
(`allocBridgeThunk`, fix-point intrinsics) is rare (~48 of 3.8 M
allocations on HNE = 0.001 %).  Therefore C++-site attribution
ROBUSTLY reports a single site.

The interesting per-Thunk variance is at the BYTECODE / AST level,
not at the C++-callsite level: which Nix expressions (lambdas, let-
bindings, lazy attrs) cause OP_MAKE_THUNK emission.  That's what
`LambdaDescriptor::allocCount` tracks today.

## Headline measurement

```
v3-direct thunks-attr: 1 distinct origins, 3826591 total allocs, 304.9 MB tracked
  origin (file:line)                              allocs         MB  avg-nUp  max-nUp
  ../src/libexpr-v3/vm.cc:3707                  3826591    304.93     1.72        9
```

* 3.8 M allocations = 99.999 % of all 3826639 thunks allocated
* avg nUpvalues = 1.72; max = 9
* 305 MB tracked vs 320 MB total stats (small under-count: tracked
  excludes `allocBridgeThunk` + primop-side thunks)

## Implementation note

Followed the BINDINGS_ATTR pattern:
* `ThunkOrigin { const char * file; uint32_t line; uint32_t nUpvalues; }`
* `thunkOriginTable()` — `std::unordered_map<const Thunk *, ThunkOrigin>`
* `thunkAllocSiteRecord(t, file, line, nUp)` callback from
  `allocThunkSuspended` (uses `__builtin_FILE` + `__builtin_LINE`
  default args)
* `dumpThunksAttribution(stderr, topN=20)` rollup printer
* Gate: `NIX_V3_THUNKS_ATTR=1` enables both recording AND dump.
  Zero cost when unset (one cached-bool branch).

Cost when enabled: per-thunk-alloc `unordered_map::operator[]` is
1-2 µs, on the order of 1-2 % of total eval wall on HNE.  Comparable
to BINDINGS_ATTR's cost on hello.drvPath.

## What this means for the next memory lever

Per HNE_BUCKET_DECOMP §"v3_arena decomposition", Thunks are 320 MB
on HNE — second-largest bucket after Bindings (713 MB).  The
attribution finding above means:

* **Per-C++-site fix is not viable** — there's only one site.
* **Per-Nix-source fix needs LambdaDescriptor attribution** —
  already exists via `V3_DBG_ALLOC_DUMP`.  Future Thunks-reduction
  work should start from that existing infrastructure.
* **Structural Thunk slimming is the orthogonal lever** —
  permanently stripping `Thunk::forces` (4 B / Thunk) and
  `Thunk::shapeCell` (8 B / Thunk) per `MEMORY_REDUCTION_AVENUES`
  Category 2 would save 4 B × 3.8 M = ~15 MB on HNE and 8 B × 3.8 M =
  ~30 MB respectively.  Both behind feature gates today.

## Cross-references

* `lode/MEMORY_REDUCTION_AVENUES_2026-05-26.md` Category 1 —
  T1.3 was item #3 in this category; this commit closes it
* `lode/HNE_BUCKET_DECOMP_2026-05-27.md` — Thunks = 320 MB context
* `lode/SESSION_ARC_2026-05-27.md` — overall session arc
* `include/v3/alloc.hh:1417-1450` — `ThunkOrigin` + table API
* `include/v3/alloc.hh:1804-1900` — `dumpThunksAttribution`
* `vm.cc:3700-3711` — OP_MAKE_THUNK (the 99.999 % allocation site)
* `closure.hh:97-206` — Thunk struct (the slimming target per
  Category 2)
* `[[falsification-rule]]` — this commit confirms a hypothesis
  rather than falsifying one (the avenues doc Category 1 prediction
  that T1.3 might surface a concentration: confirmed, but
  concentration is at the bytecode-op level, not the C++ level)

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
  Input Output Group.
SPDX-License-Identifier: Apache-2.0
