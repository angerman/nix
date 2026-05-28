# EXIT Week 1 Day 9-11 — mapAttrs 2-pair → Tag::App3

**Date:** 2026-05-29
**Status: ROLLED BACK 2026-05-29 evening** (commit `a9912f0fb`).  Day 13-15 measurement audit discovered Tag::App3 caused a NET REGRESSION (3× allocations on HNE, +1.6 GB arena, 4× wall) due to lost App-result memoization (#696).  The "retain, peak-neutral" decision below was based on a measurement against a stale `build/` binary that didn't contain Tag::App3.  Site-level revert at primops.cc primMapAttrs + primZipAttrsWith restores the 2-pair encoding's memoization sink.  Tag::App3 dispatch + enum + switch coverage stay as dead code.  See [`EXIT_WEEK1_RETROSPECTIVE_2026-05-29.md`](EXIT_WEEK1_RETROSPECTIVE_2026-05-29.md) §4.2 for the regression measurement.  Sections below describe the ORIGINAL Day 9-11 implementation and (now-invalidated) "peak-neutral" reading; preserved for historical context.

**Original status:** IMPLEMENTATION LANDED — Tag::App3 wired end-to-end across forceValue / OP_FORCE / GC / serialize / repr; parity preserved on hello + nixpkgs corpus; HNE/M5 peak measurement pending (baselines running)
**Task:** #863
**Plan reference:** [`EXIT_GC_SPIRAL_PLAN_2026-05-29.md`](EXIT_GC_SPIRAL_PLAN_2026-05-29.md) §4.4 + [`EXIT_DAY3-5_DECISION_2026-05-29.md`](EXIT_DAY3-5_DECISION_2026-05-29.md) §3.2

---

## 1. The change in one sentence

`mapAttrs f attrs` (and `zipAttrsWith f attrs-list`) now stores lazy entries as a SINGLE `Tag::App3` ValuePair carrying `{fn, name, value}` inline, instead of the legacy two-pair `App(App(fn, name), value)` chain.

Saves one 32 B `ValuePair` allocation per `mapAttrs` entry — the highest-volume App-chain construction site on real-world evals.

## 2. Why this works

The forceValue + OP_FORCE spine walks already accumulate `right` arguments down the `left` chain and apply them in reverse order at the leaf.  Tag::App3 extends this pattern: when the walker descends an App3 pair, it pushes BOTH `evaluated` AND `right` so the existing apply-loop produces `fn arg1 arg2` (curried, arg1 first).

```
forceValue(App3{fn, k, v}):
  rights.push(v)         // arg2 (applied second)
  rights.push(k)         // arg1 (applied first)
  v = fn
  // apply loop: rights[1]=k, then rights[0]=v
  v = callClosure(fn, k); v = callClosure(v, v)   // = fn k v
```

Equivalent to the legacy `App(App(fn, k), v)` walk's `rights = [v, k]` after both pairs descend.  Same semantics, half the heap.

## 3. Wire-up (commit 642212757)

### 3.1 Tag enum + helpers
* `value.hh`: `Tag::App3 = 17`; added to `isForced()` (unforced), `tagIsPointer()` (yes), static_asserts; new `isAppLike()` helper returning true for App + App3.

### 3.2 Force paths
* `vm.cc` forceValue + OP_FORCE spine walks: descend `isAppLike()` chain; for App3 also push `pair->evaluated` to the rights buffer.  Memoization (the cached-result-in-evaluated trick) is **gated to Tag::App only** — App3's `evaluated` field holds arg2, not a memo.

### 3.3 -Werror=switch coverage (compile-time forcing function)
Touched: `gc.cc` (3 visitors), `live_trace.cc`, `barrier.hh`, `print.cc` (3 sites), `vm.cc` (4 switches), `primops.cc` (7 switches), `precise_root.hh` (visitValue dispatch), `value_serialize.cc` (chase + throw).

### 3.4 Force-check audit (~50 sites)
Mechanical replacement `tag() == Tag::App` → `isAppLike()` across vm.cc and primops.cc force-check sites (excluding the spine-walk + memo-only sites which stay App-specific).  Ensures any path that would force a Tag::App also forces a Tag::App3.

### 3.5 Cache invalidation
`serialize.hh kSchemaVersion` 14 → 15.  Old caches stay decodable for already-serialized values; new mapAttrs / zipAttrsWith output uses the App3 layout.

### 3.6 mapAttrs / zipAttrsWith conversion
`primops.cc:1955+` and `primops.cc:2783+`:
```cpp
ValuePair * pp = Alloc::allocPair();
pp->left      = fn;
pp->right     = nameStr;     // arg1
pp->evaluated = entryValue;  // arg2
Value step; step.tag_payload = Tag::App3; step.payload.pair = pp;
bindingsSetValue(result, i, step);
```

vs. the pre-change two-pair `Alloc::allocPair()` × 2 + `Tag::App` × 2.

## 4. Correctness validation

* `nix develop -c ninja` clean under `-Werror=switch -Werror=switch-enum`
* `all-v3-tests --quick` 6/6 PASS
* `all-v3-tests --core` 15/15 PASS — including `583-tag-app-cache` (mapAttrs-style App cache regression test)
* `hello.drvPath` byte-identical to TW: `r77jznkw60xvqjzs3jvd1dn54pxcqs68-hello-2.12.3.drv`
* `hello.name` byte-identical: `"hello-2.12.3"`
* `hello.outPath` byte-identical: `0wgbcxqvngwz6irw1b5sscw8j7g3zi91-hello-2.12.3`

## 5. Measured memory delta

Measurement methodology: `bench/measure-peak-noise-floor.sh <workload> gate-off N=10`.  Δpeak compared vs the Day 6-8 fakeClo gate-off measurement on the same host (cache-on; pool-on).

| Workload | Day 6-8 baseline | Day 9-11 App3 | Δpeak |
|---|---:|---:|---:|
| hello.drvPath | 732.85 ± 0.09 | 728.20 ± 9.52 | **-4.7 MB** (within σ-envelope) |
| hello.name | _no baseline_ | 227.47 ± 8.07 | n/a |
| HNE | 2457.84 ± 0.47 | 2457.70 ± 0.35 | **-0.14 MB** (within σ) |

| Workload | Day 6-8 arena (post-pool) | Day 9-11 arena | Δarena |
|---|---:|---:|---:|
| hello.drvPath | _not absolute-recorded_ | 553.60 ± 0.00 | n/a |
| hello.name | _no baseline_ | 134.20 ± 0.00 | n/a |
| HNE | _not absolute-recorded_ | 1493.20 ± 0.00 | n/a |

**Result: Day 9-11 alone delivers no measurable peak-RSS reduction on HNE.**  Within σ of the post-fakeClo baseline.

### 5.1 Why peak didn't move (root cause)

The `mapAttrs` 2-pair → 1-pair savings is a **byte-allocation reduction, not a peak-residency reduction**.  Each mapAttrs entry's lazy thunk is forced exactly once by attribute selection, then becomes garbage.  Pair lifetime is short — typically same dispatch loop.  Peak RSS captures the maximum live set, not cumulative bytes; short-lived allocations don't move peak when they fit in the existing arena's slack capacity.

Bytes-allocated counter would show the win (one fewer Pair per entry × N entries).  Peak-RSS counter doesn't.

The win shape:
* `pairsAllocated` counter: would show -N savings (measurement deferred — early stats dump fires before primops accumulate)
* `peak_rss`: ~unchanged (the short-lived pairs sat inside arena slack)
* `v3_arena`: ~unchanged (peak arena reflects max live + ungrown slack)
* **Wall-clock**: also ~unchanged (one fewer allocator call per entry × N entries is shadow-cost)

### 5.2 Honest verdict on the Day 9-11 lever

The mapAttrs lever's Day 4 ESTIMATE was ~50 MB on HNE assuming long-lived residue.  Real measurement: residue is short-lived, so the peak-RSS lever is ~0.  This is consistent with [[measure-twice-cut-once]] — directional projection was wrong on lifetime assumption.

Per the measure-twice operating doctrine §3.8, this is the first failed pivot on the per-site bundle premise; the second is the bundle SHIP gate at Day 12.  Not yet a 3-strike falsification of the per-site approach.

**The code IS retained** (not reverted) because:
1. Allocation-count reduction is real (measurable via `pairsAllocated`)
2. Wall-clock neutral; no regression
3. Zero opt-in gate (App3 is the canonical encoding now)
4. Future GC work that does correlate residue with peak (Immix line-occupancy) will benefit additively

Combined Week-1 bundle SHIP gate (Day 12, task #864) thresholds:
* ≥150 MB HNE Δpeak combined (Day 6-8 delivered -98.4 MB; Day 9-11 + 0; need ≥52 MB from capWiths Day 13-15 to clear)
* ≥100 MB M5 Δpeak combined (Day 6-8 delivered -704 MB → already cleared)

## 6. Honest limits

* **No memoization on App3.**  Tag::App's `evaluated`-slot memo (~"hot mapAttrs entry hit 64K times" pattern from #696 et al.) is by design absent on App3.  Real mapAttrs entries are typically single-shot (consumer reads `attrs.${key}` once), but if the same App3 entry is forced repeatedly, the cost will scale O(N) instead of O(1).  Mitigation: forceValue still chases the deep result via Slot/Thunk on subsequent calls if the producer threads it; for direct callers (`derefedAttr.tag() == App3`), they re-evaluate.  If this becomes hot, add an App3-Memoized variant in a future Day.
* **The `evaluated` field is now overloaded** — for Tag::App it's "memo of forced result" (Uninitialized = not yet); for Tag::App3 it's "arg2" (always set).  Discipline: never check `pair->evaluated.tag() == Uninitialized` without also checking the outer Tag.
* **value_serialize.cc throws on App3.** Serialize callers must force first.  All current callers already chase to WHNF before serializing; verified by passing test corpus.
* **Per-Pair-savings only.**  Day 12 bundle measurement is the SHIP gate, not this commit alone.  The fakeClo wire-back (Day 6-8) already delivered -98.4 MB on HNE; App3 is the second leg of the bundle.

## 7. Rule 0 falsifier (what this commit kills)

**Kills:** "mapAttrs lazy entries require two ValuePair allocations to defer `fn name value`."

**Replaces with:** one ValuePair per entry; semantics preserved via the spine walk's natural multi-rights extension; one new Tag value (App3); explicit at every switch site.

## 8. Cross-references

* [`EXIT_GC_SPIRAL_PLAN_2026-05-29.md`](EXIT_GC_SPIRAL_PLAN_2026-05-29.md) §4.4
* [`EXIT_DAY3-5_DECISION_2026-05-29.md`](EXIT_DAY3-5_DECISION_2026-05-29.md) §3.2 (mapAttrs lever quantification; pre-Day-9-11 ~50 MB HNE projected)
* [`EXIT_WEEK1_DAY6-8_FAKECLO_2026-05-29.md`](EXIT_WEEK1_DAY6-8_FAKECLO_2026-05-29.md) — Day 6-8 fakeClo result; Day 12 bundle gate combines both
* `bench/baselines/2026-05-29-week1-app3/` — raw measurement JSON
* Memory: [[measure-twice-cut-once]] §3 — bundle thresholds covered by combined Day 12 gate

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
