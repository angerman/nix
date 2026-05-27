# T1.3 Pairs + Lists attribution — two new levers found

**Date**: 2026-05-27
**Origin**: `MEMORY_REDUCTION_AVENUES_2026-05-26.md` Category 1 T1.3
extension.  Templated from #746 BINDINGS_ATTR + parallel to
`T1_3_THUNKS_ATTR_2026-05-27.md` + `T1_3_CLOSURES_ATTR_2026-05-27.md`.
**Status**: infrastructure landed; HNE measurement complete;
**TWO REAL ACTIONABLE LEVERS IDENTIFIED**.

## TL;DR

`NIX_V3_PAIRS_ATTR=1` and `NIX_V3_LISTS_ATTR=1` enable per-site
attribution dumps at end-of-run.  On HNE:

* **Pairs**: 6 sites, 2.6 M allocs, 118.9 MB tracked
* **Lists**: 21 sites, 1.1 M allocs, 51.1 MB tracked

Top patterns reveal two new levers:

| Lever | Bytes on HNE | Site | Description |
|-------|--------------|------|-------------|
| **mapAttrs 2-pair App chain** | 100.2 MB (42% of pairs) | primops.cc:1965 + 1970 | Each mapAttrs entry allocates 2 ValuePair-s to build `Tag::App (Tag::App fn name) value` |
| **Tiny `capturedWiths` ListVec** | 12.8 MB (25% of lists) | vm.cc:3831 (OP_MAKE_THUNK) | 544 K tiny lists (avg-size 1.04, max 2) for per-thunk `with`-chain snapshot |

## Headline measurement (HNE)

### Pairs (top 6, sorted by alloc-count)

```
6 distinct origins, 2597189 total allocs, 118.9 MB tracked
                  (sizeof(ValuePair)=48)
  primops.cc:1970   1094597  50.11 MB
  primops.cc:1965   1094597  50.11 MB
  primops.cc:1468    185814   8.51 MB
  vm.cc:4140         176099   8.06 MB
  primops.cc:2783     23041   1.05 MB
  primops.cc:2788     23041   1.05 MB
```

### Lists (top 8, sorted by total bytes)

```
21 distinct origins, 1088705 total allocs, 51.1 MB tracked
  vm.cc:7035        172800  24.47 MB  avg=8.78  max=261    -- OP_LIST_CONCAT
  vm.cc:3831        544299  12.82 MB  avg=1.04  max=2      -- thunk capturedWiths
  vm.cc:6991        178852   5.13 MB  avg=1.38  max=339    -- (force-path capturedWiths)
  primops.cc:1463   100243   3.60 MB  avg=1.85  max=3633   -- (genList family)
  primops.cc:684     21450   3.14 MB  avg=9.08  max=3894
  primops.cc:2771    23041   0.56 MB  avg=1.11  max=7
  vm.cc:3422         18238   0.47 MB  avg=1.19  max=2
  primops.cc:725       995   0.25 MB  avg=15.87 max=3633
  ...
```

## Lever 1 — mapAttrs 2-pair App chain (100 MB on HNE)

### Source (primops.cc:1955-1979)

```cpp
Bindings * result = Alloc::allocBindings(src->size);
for (uint32_t i = 0; i < src->size; ++i) {
    SymbolId sym = src->entries[i].name;
    Value nameStr = mkStringValueOwned(std::string(vmSymName(state, sym)));
    // Build a Tag::App chain that, when forced, applies `fn name value`.
    ValuePair * pp1 = Alloc::allocPair();
    pp1->left  = fn;
    pp1->right = nameStr;
    Value step1; step1.tag_payload = Tag::App; step1.payload.pair = pp1;
    ValuePair * pp2 = Alloc::allocPair();
    pp2->left  = step1;
    pp2->right = src->entries[i].value;
    Value step2; step2.tag_payload = Tag::App; step2.payload.pair = pp2;
    result->entries[i].name  = sym;
    bindingsSetValue(result, i, step2);
}
```

Two ValuePair allocations per attribute, both 48 bytes.  At
1,094,597 attributes processed on HNE = 2,189,194 pairs × 48 B
≈ **100 MB**.  Mostly transient (App pairs get memoized into
ValuePair::evaluated and the source 2-pair chain becomes garbage).

### Lever options

1. **Eager apply when `fn` is identity / projection** — common
   `mapAttrs (_: v: v) xs` patterns can elide the chain entirely.
2. **3-arg App representation** — instead of a 2-pair chain encoding
   `App (App fn name) value`, introduce a Tag::App3 with 3-Value
   storage.  Saves one ValuePair per entry (50 MB on HNE).  Touches
   the App evaluator + serialise + GC walkers.
3. **In-place App chain reuse** — if mapAttrs's source attrset is
   not shared, the SAME 2 pairs could be reused per slot (write-
   through pattern).  Architecturally awkward.

Option 2 is the cleanest with measurable yield.  Estimated effort
1-2 days.  Pre-committed SHIP threshold: ≥ 30 MB peak RSS
reduction on HNE + no `--core` regression.

## Lever 2 — Tiny capturedWiths ListVec (13 MB on HNE)

### Source (vm.cc:3831, OP_MAKE_THUNK)

```cpp
if (nWiths > 0) {
    ListVec * lws = Alloc::allocList(nWiths);
    for (uint16_t i = nWiths; i > 0; --i)
        lws->elems[i - 1] = pop(vm);
    listPostConstructBarrier(lws);
    t->suspended.capturedWiths = lws;
}
```

Each MAKE_THUNK with non-empty captured-`with` chain allocates a
`ListVec` of size `nWiths`.  HNE: 544,299 allocs, avg-size 1.04,
max 2.  Each is 16-32 bytes (ListVec header + 1-2 Value elements);
the per-alloc overhead (header + cache-line fill + per-alloc
bookkeeping) dominates.

### Lever options

1. **Inline into Thunk for nWiths ≤ 2** — extend Thunk struct with
   `Value inlineWiths[2]` + a flag.  544 K × ~24 B saved = 13 MB
   on HNE.  Touches the alloc + Phase D barriers + scavenger.
2. **Singleton pool for the 1-element case** — most common case
   (avg 1.04 suggests nWiths==1 is dominant).  A per-`with`-target
   memoized 1-element ListVec singleton could deduplicate.  Touches
   Phase D barriers.
3. **Compress in Thunk via small-list-inlined union** — generalised
   form of (1) supporting up to 2-3 elements inline.

Option 1 (small-list inline) is the cleanest with measurable yield.
Estimated effort 2-3 days (Phase D barrier audit is the careful
part).  Pre-committed SHIP threshold: ≥ 10 MB peak RSS reduction
on HNE + no `--core` regression.

## Cross-comparison: T1.3 across all types

| Type     | Sites | Top-site % | Lever found |
|----------|-------|-----------|-------------|
| Bindings | 23    | 82% (mergeBindings) | Chain Phase C — falsified ×3 |
| Thunks   | 1     | 99.999% (OP_MAKE_THUNK) | None (single site); use LambdaDescriptor instead |
| Closures | 4     | 44% / 39% / 16% | **fakeClo pool dead code** — needs revival OR nursery default-on |
| Pairs    | 6     | 42% / 42% | **mapAttrs 2-pair App** — 100 MB; 3-arg App rep |
| Lists    | 21    | 48% / 25%     | **tiny capturedWiths** — 13 MB; inline-in-Thunk |

**T1.3 produced 3 new concrete levers** (fakeClo, mapAttrs, captWiths).
The cumulative bytes-on-HNE: 144 + 100 + 13 = **257 MB potentially
recoverable** across these three independent levers — comparable to
Stage 6's 797 MB.

## Implementation note

Both T1.3 dumps fire ONCE per primop-install pass which produces
multiple small dumps in the v3-direct invocation chain (per the
T1_3_THUNKS_ATTR_2026-05-27 pattern).  The MEANINGFUL one is the
LAST dump in the output, which captures the main eval's allocations.

(Consider extending the live_trace.cc arena-watermark gate to also
filter T1.3 dumps to once-per-meaningful-pass — future polish.)

## Cross-references

* `lode/MEMORY_REDUCTION_AVENUES_2026-05-26.md` Category 1 — T1.3
  fully closes (now with 3 real levers across non-Bindings types)
* `lode/T1_3_THUNKS_ATTR_2026-05-27.md` — Thunks (no lever)
* `lode/T1_3_CLOSURES_ATTR_2026-05-27.md` — fakeClo finding (144 MB)
* `lode/HNE_BUCKET_DECOMP_2026-05-27.md` — workload context
* `lode/SESSION_ARC_2026-05-27.md` — overall arc
* `include/v3/alloc.hh` — Pair/List Origin tables + dump functions
* `primops.cc:1955-1979` — mapAttrs 2-pair App lever site
* `vm.cc:3825-3838` — OP_MAKE_THUNK tiny capturedWiths lever site
* `[[memory-first-class]]` — pre-committed SHIP thresholds
* `[[falsification-rule]]` — this commit identifies TWO new levers
  (rather than falsifying / confirming existing hypotheses)

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
  Input Output Group.
SPDX-License-Identifier: Apache-2.0
