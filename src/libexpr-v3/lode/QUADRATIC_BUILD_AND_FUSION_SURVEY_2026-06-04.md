# The O(N²) `++`/`//` build cost + the stream-fusion survey

**Date:** 2026-06-04
**Status:** ANALYSIS + SURVEY — code-grounded at HEAD (`0e4b9678e`). Answers two
questions: (1) is repeated `++`/`//` accumulator-copy a real problem we should
optimize in the VM, and how; (2) which combinator fusions exist vs. are missing.
**Triggering question:** "if `repeated ++/// copy the accumulator each step` is a
real problem, shouldn't we optimize this in our bytecode VM? + survey the fusions."
**Author:** session synthesis

Companion docs:
- [`BENCH_V3_VS_TW_2026-06-04.md`](BENCH_V3_VS_TW_2026-06-04.md) — **read §0.1 below first**: the live `fold-add-1M` 17× work targets a DIFFERENT, complementary fold cost (the curried-calling-convention per-element alloc)
- [`POST_PURE_PIPELINE_OPTS_2026-06-02.md`](POST_PURE_PIPELINE_OPTS_2026-06-02.md) — the ranked opt menu (determinism/AOT is the standout; this is a *wall* lever, orthogonal)
- [`EXIT_PHASE_C_4_FALSIFIED_2026-05-30.md`](EXIT_PHASE_C_4_FALSIFIED_2026-05-30.md) — the 4× Chain-Bindings falsification ledger (Route A blocker)
- [`R2_4D_EVAC_WALL_PLAN_2026-06-03.md`](R2_4D_EVAC_WALL_PLAN_2026-06-03.md) — §3 "mergeBindings is NOT the lever it was billed as" + the GC-subsumes-churn point
- [`MEASURE_TWICE_CUT_ONCE_2026-05-23.md`](MEASURE_TWICE_CUT_ONCE_2026-05-23.md) — the wall-proxy-first discipline this analysis defers to

---

## 0. TL;DR

* **Yes, it's real and present.** Every `//` allocates a fresh `Bindings` and copies
  both operands (`mergeBindings`, vm.cc:1064; `OP_ATTRS_UPDATE`:8620 / `OP_ATTRS_UPDATE_TAIL`:8693).
  Every `++` allocates a fresh `ListVec` and copies (`OP_LIST_CONCAT`, vm.cc:7175).
  **No in-place reuse anywhere.** A chain of N merges that grows the accumulator is O(N²).
* **It's pervasive, not just "the reverted set."** The *installed* bytecode primops
  `filter`/`sort`/`concatMap`/`partition` build their result with `acc ++ [x]` per
  element → **internally O(N²)** (their own comments admit it: bytecode_primops.cc:451
  "Worst-case O(N²) due to repeated ++", :470 "regression intentional"). It is also why
  `concatLists`/`listToAttrs`/`removeAttrs`/`intersectAttrs` were **reverted** from
  bytecode (bytecode_primops.cc:395-414; +60.4% on attrset-build-1k). And per #746 the
  `a // b` churn is **84% of arena bytes** on hello.drvPath.
* **Three fix routes, with verdicts:**
  - **A — persistent / structural sharing (Chain Bindings / HAMT):** the textbook fix,
    but a **4×-falsified graveyard** here (vm.cc:1166-1216) — blocked on a ~208-site
    `entries[]` audit. Do not reopen without the prereq.
  - **B — in-place mutation when the accumulator is provably *unique* (a "transient"
    builder):** the **keystone**, largely untried. Blocked by v3's **Boehm conservative
    GC (no refcount)** → requires *static linearity inference*, not runtime transients.
    Captures the linear fold-build idiom; does NOT cover shared fixpoint overlays.
  - **C — fusion: rewrite fold-of-append → allocate-once builder:** safe, bounded, no
    representation change. Narrower (recognizable shapes only). This is the survey's
    biggest gap (§3).
* **This is a WALL lever, not the memory lever.** The Nofl-Immix GC subsumes the churn
  for *peak RSS* (the garbage collects); it does NOT remove the O(N²) *copying work*.
  M5<4 GB still routes through bridge eradication + GC, not this.
* **The convergence:** the #1 missing fusion (build-fusion) **is** the #1 O(N²) fix.
  Routes B and C are one body of work.

### 0.1 Relationship to the `fold-add-1M` work (BENCH_V3_VS_TW_2026-06-04)

`fold-add-1M` (17.5× vs TW) decomposes into TWO per-element **constant-factor** costs
(both O(N) with big multipliers), distinct from THIS doc's asymptotic O(N²):

1. **The per-iteration `OP_ATTRS_LET_REC_INIT` heap cell (size-1) — ✅ FIXED, `cd1da2577`.**
   This was NOT the curried calling convention (the bench's first hypothesis, revised in
   `81b96c405`); it was `foldl'`'s OWN inner `let next = op acc (elemAt list i); in …` —
   a *non-recursive* `let` that the lowerer boxed as a heap let-rec cell. Fixed by
   demoting non-recursive `let` to plain local thunk bindings (`cli/lower_v3.hh`, gate
   `NIX_V3_NO_LETREC_DEMOTE=1`, fixture `letrecDemote-nonrec-pos.nix`). **Verified
   2026-06-04:** the installed `foldl'` over 200k elems goes `attrsets 200001 → 1`,
   byte-identical, ~1.04× wall. **General win** — every non-recursive `let` in nixpkgs
   now skips a heap cell, not just `foldl'`.
2. **The curried-call thunks/closures** (`~2 closures / 4 thunks / 2 pairs` per element,
   from `op acc` partial-application boxing on the `callClosure` path) — the **remaining,
   dominant** per-element cost; the **arity-aware uncurried calling convention** (GHC
   eval/apply) axis, still open. (Fix #1's wall delta is modest precisely because the
   let-rec attrsets were numerous-but-small vs these thunks/closures.)

That is the **constant factor** (O(N) with a huge multiplier) and it bites even on a
**scalar** accumulator (`a: b: a + b`), where there is *no* `++`/`//`. **THIS doc is the
asymptotic (O(N²)) cost** that bites only when the accumulator is a **growing container**
(`filter`/`concatMap`/`listToAttrs`/overlay merges). They are orthogonal and additive:

- The calling-convention fix removes the ~7-allocs-per-element constant — and is correctly
  the **higher priority** (it's the 17× headline AND helps every HOF, scalar or not). It is
  **in flight** (uncommitted `emit.cc`/`closure.hh`/`vm.cc`/`primops.cc` at the time of writing).
- **After it lands, the O(N²) build-copy in this doc becomes the *next* fold ceiling** for
  growing-accumulator folds: `filter`/`concatMap`/`sort` stay O(N²) regardless of calling
  convention, because that cost is the `++`/`//` accumulator copy, not the call.

**Sequence:** the calling-convention fix first (theirs, in flight) → then this doc's Routes
B/C for the build case. Don't conflate the two: a perfect eval/apply convention does NOT
make `filter` O(N); a transient/fused builder does.

---

## 1. The problem, confirmed in code

### 1.1 `//` always copies

`mergeBindings` (vm.cc:1064) is a two-pass *allocate-exact-size + copy* (the #747
two-pass fixed the over-allocation slack, NOT the copy). Both `OP_ATTRS_UPDATE`
(8620) and the 98%-dominant `OP_ATTRS_UPDATE_TAIL` (8693) route through it. The only
fast paths are the #748 empty-operand short-circuits (`{} // b` → `b`, vm.cc:1163-1164).
There is **no uniqueness fast path** — even when the LHS is a fold accumulator with no
other reference, it is copied.

vm.cc:1090 already *instruments* the canonical hot case (`nb=1`, single-attr override)
and notes: *"a smart patched representation could amortise parent copy."* Scoped, not built.

### 1.2 `++` always copies

`OP_LIST_CONCAT` (vm.cc:7175) allocates a fresh `ListVec` of `len(a)+len(b)` and copies
both, then `listPostConstructBarrier`. No in-place grow.

### 1.3 The O(N²) is inside the installed bytecode primops

| Primop | Source (bytecode_primops.cc) | Cost |
|---|---|---|
| `filter` (:457) | `foldl' (acc: x: if pred x then acc ++ [x] else acc) []` | O(N²) |
| `concatMap` (:376) | `foldl' (acc: x: acc ++ fn x) []` | O(N²) |
| `partition` (:435) | `foldl'` with `++` into a pair accumulator | O(N²) |
| `sort` (:476) | insertion sort: `before ++ [x] ++ after` per insert | O(N²) |

The comments call these regressions "intentional" (C-stack-safety > asymptotics for the
small lists nixpkgs typically sorts/filters). But the asymptotic ceiling is real, and
it's the same mechanism that blocked the reverted set.

### 1.4 Scope of impact

This single mechanism gates four things at once:
1. asymptotics of `filter`/`concatMap`/`sort`/`partition`;
2. the reverted-set migration (`concatLists`/`listToAttrs`/`removeAttrs`/`intersectAttrs`);
3. every user/lib `foldl' (acc: x: acc ++/// …)` in nixpkgs — overlay merges, the module
   system, `lib.foldr`-style builds;
4. arena allocation volume (#746: `mergeBindings` = 84% of arena bytes).

---

## 2. The three routes

### Route A — persistent / structural sharing (Chain Bindings / HAMT)

Make `//` O(1)-construct + O(depth)-lookup via `Chain{parent, overlay}`, or O(log N) via
a HAMT (PERF_STRATEGY's parked "Stage 11"). **Verdict: 4×-falsified graveyard.**
vm.cc:1166-1216 + `EXIT_PHASE_C_4_FALSIFIED`: every attempt died with
`attribute 'buildPythonApplication' missing`, because ~208 sites read `Bindings::entries[]`
directly, bypassing the chain abstraction. Correctness requires a 208-site audit routing
all reads through a chain-aware accessor — multi-session, high-risk. Per measure-twice,
3 failed pivots = falsification; this is closed until the prereq is done.

### Route B — in-place mutation when the accumulator is provably unique (THE keystone)

In `foldl' (acc: x: acc ++ [f x]) [] xs`, each intermediate `acc` is dead the instant the
next is built (strict `foldl'` overwrites it). If the VM knew the old `acc` had no other
reference, `acc ++ [x]` could **grow its backing array in place** (amortized O(1)) → O(N).

**The deepest blocker:** v3 uses **Boehm conservative GC — there is no refcount** to check
uniqueness at runtime. Unlike Clojure transients / Roc/Koka Perceus (refcount-driven),
v3 must prove uniqueness **statically**: the accumulator parameter is used *linearly*
(exactly once, as LHS of `++`/`//`) and does not *escape* (not captured in a closure;
returned only as the fold result). For the canonical fold-build shape this IS statically
provable. Then emit a **transient-builder** opcode/primop: allocate one growable
container, mutate per step, freeze at end.

**Payoff is broad:** fixes `filter`/`concatMap`/`sort`/`partition` internally, un-blocks the
reverted set, and speeds user-level fold-builds — while keeping the C-stack-safety that
motivated the bytecode primops.

**Partial infra exists:** `OP_ATTRS_UPDATE_TAIL` + the forceWriteback/shapeCell machinery
+ the per-site `mergeBindings` histogram (vm.cc:1086-1107) + the explicit "amortise parent
copy" note (vm.cc:1090).

**The catch:** linearity does NOT hold for shared **fixpoint overlays**
(`prev // overlay final prev` in `lib.fixedPoints` / the module system), where the
accumulator escapes into `final`. That is exactly Route A's territory and exactly why A
keeps failing. So B captures the *linear fold-build* idiom (large, common) but NOT the
*shared-overlay* idiom.

### Route C — fusion: rewrite fold-of-append → allocate-once builder

`foldl' (acc: x: acc ++ [g x]) []` ≡ `map g`; `foldl' (acc: x: acc // {n=v;}) {}` ≡
`listToAttrs`-shape; `listToAttrs (map …)` → one builder. Route these to the existing
allocate-once C++ builders (already O(N)). Safe, bounded, no representation change, no GC
dependency — but narrower (recognizable shapes only). This is §3's biggest gap.

---

## 3. The stream-fusion / optimizer survey

**Pipeline** (`ir::optimise`, opt_const_fold.cc:274), in order:
`constantFold → betaReduce → constantFold → CSE → elimRedundantForce → inline →
fusePrimOpApps → primOpFold → constantFold → inline → streamFusion → ifThenFold →
constantFold → inline → genListUnroll → appSpineFold → constantFold → inline → DCE →
(Stage-4 strictness, opt-in)`. 16 `opt_*.cc` files, ~5.7 KLoC.

**What IS fused / rewritten today:**

| Pass | Recognizes | Emits |
|---|---|---|
| `streamFusion` (opt_stream_fusion.cc:339) | **`foldl' op z (map f xs)`** — the *only* multi-combinator fusion | `__foldlMap op z f xs` (one pass, no intermediate list); `__foldlMap` def bytecode_primops.cc:332 |
| `genListUnroll` (opt_genlist_unroll.cc) | `genList f n`, n ∈ 0..8 literal | N-element ListExpr of `MkThunk(App(f,i))` |
| `primOpFold` (opt_primop_fold.cc:282) | `length`/`head`/`tail`/`elemAt`/`toString`/`attrNames`/`stringLength` on literals | the literal result |
| `appSpineFold` (opt_app_spine_fold.cc:423) | saturated curried call, pure args | one N-way substitution |
| `betaReduce` (opt_beta_reduce.cc:370) | `App(Lambda, arg)`, simple body | inlined body |
| `fusePrimOpApps` (opt_primop_fuse.cc:67) | App-chain over `LitPrimOp` reaching arity | one `PrimOpCall` |
| `constantFold`/`CSE`/`inline`/`ifThenFold`/`DCE` | literals; identical subexprs; aliases; `if literal`; dead bindings | standard |

**Headline: `__foldlMap` is the SOLE combinator fusion** (inner = `map` only; outer =
`foldl'` only; map-result must be use-once). **There is NO build-fusion** — searched the
optimizer for `OP_LIST_CONCAT`/`OP_ATTRS_UPDATE`/builder/accumulator rewrites: none.

**The gaps (ranked by nixpkgs frequency + whether it also fixes the §1 O(N²)):**

1. **Build-fusion** — `listToAttrs ∘ map`, `foldl'`-of-`++`/`//`, and `filter`/`concatMap`'s
   internal `++`. **ABSENT.** *Same thing as Route C — the single highest-value gap*, because
   it's both the most common shape AND the O(N²) fix.
2. **`map ∘ filter` / `filter ∘ map`** — ubiquitous in `lib`. Not fused.
3. **`foldl' ∘ filter`, `foldl' ∘ concatMap`** — streamFusion only knows `map` inner; no
   `__foldlFilter`/`__foldlConcatMap`.
4. **`any ∘ map` / `all ∘ map`** — map materializes first.
5. **`mapAttrs ∘ filterAttrs`, `mapAttrs' ∘ …`** — the attrset pipeline that dominates the
   module system / `lib.attrsets`; entirely un-fused (neither is even a bytecode primop).
6. **`concatMap ∘ genList` / `concatLists ∘ map`** — genList unrolls only n≤8.
7. **`genList` large constant n** — only n≤8 unrolled.

---

## 4. Recommendation + the measure-twice gate

**Sequencing:** C first (bounded fusion of recognizable build shapes — low risk), then B
(transient builder via static linearity — captures `filter`/`concatMap`/`sort` + the
reverted set + linear user folds). A stays parked behind its 208-site audit.

**The keystone realization:** extend `streamFusion` from "`foldl'∘map → __foldlMap`" to a
small family — `__foldlFilter`, `__listToAttrsMap` (allocate-once), and
`filter`/`concatMap`/`partition` rewritten against a **transient list builder** — at which
point Route B (transient builder) and §3 (the fusion family) are one body of work.

**Pre-committed measure-twice gate (per [[measure-twice-cut-once]]):** before building
either, measure the **wall proxy** — instrument `++`/`OP_LIST_CONCAT` + `mergeBindings`
copy-bytes-per-eval on hello / HNE / M5 (the `mergeBindings` per-site histogram infra is
already in `alloc.hh`). Pre-commit a "% of eval wall in `++`/`//` copying" threshold before
the rewrite. This is a wall lever; frame and gate it RSS-neutral / wall-primary per
[[memory-first-class]].

**Honest positioning:** the team's current ranked standout (POST_PURE_PIPELINE_OPTS) is
determinism/AOT, and the memory lever is bridge eradication. This O(N²) work competes with
neither but is also not today's declared #1. It is a real, scoped, high-leverage *wall*
lever (the `mergeBindings` instrumentation shows it was anticipated) — pending the wall
proxy.

---

## 5. What this analysis kills / confirms (Rule 0)

* **Confirmed:** `++`/`//` copy with no in-place reuse; the O(N²) is *inside* installed
  bytecode primops, not just the reverted set.
* **Confirmed:** Route A (persistent) is falsified-and-blocked, not merely untried.
* **Names the untried lever (B):** static-linearity transient builder, with its real
  blocker (no refcount under Boehm) and its real limit (shared overlays excluded).
* **Killed framing:** "just make `//` O(log N)" as a free win — it's the 4×-falsified path.

---

## 6. Cross-references / code anchors

- `vm.cc:1064` `mergeBindings` (two-pass copy); `:1086-1107` per-site histogram; `:1090`
  "amortise parent copy" note; `:1163-1164` empty short-circuit; `:1166-1216` Chain
  falsification; `:8620`/`:8693` OP_ATTRS_UPDATE(/_TAIL); `:7175` OP_LIST_CONCAT.
- `bytecode_primops.cc:332` `__foldlMap`; `:376/:435/:457/:476` concatMap/partition/filter/sort
  (the `++`-O(N²) sources); `:395-414` reverted-set rationale.
- `opt_stream_fusion.cc:339` the sole fusion; `opt_genlist_unroll.cc` n≤8; `opt_const_fold.cc:274`
  pipeline driver.
- [[measure-twice-cut-once]] — §4 wall-proxy gate. [[memory-first-class]] — wall-primary framing.
- [[falsification-rule]] — §5.

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
