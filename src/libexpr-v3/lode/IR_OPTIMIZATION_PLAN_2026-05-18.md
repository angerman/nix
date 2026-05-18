## IR optimization plan + test status — 2026-05-18

Answers to three user questions: (1) cardano-node feasibility,
(2) IR-level optimization plan with detailed reasoning, (3) test
infrastructure status.

---

## 1. cardano-node flake feasibility

**Answer: NO, not today.** Three independent blockers:

| Blocker | What's missing | Path to fix |
|---|---|---|
| `builtins.getFlake` | v3 doesn't expose libfetchers's getFlake primop | Bridge it through TW (FFI leaf) |
| IFD (haskell.nix-required) | v3 doesn't drive `import (derivation ...)` paths through the build daemon | Implement IFD bridge or accept TW-driven IFD |
| Per-op throughput | ~200x slower than TW; haskell.nix needs ~60s of TW eval just to traverse | Phase 2 IR optimization (this doc) |

**Quantification.** Direct attempts and their outcomes:

```
$ TW flake metadata github:IntersectMBO/cardano-node             0.12s ✓
$ TW flake show github:IntersectMBO/cardano-node (no IFD)         1.22s
   → IFD-required error (haskell.nix needs --option allow-import-from-derivation true)
$ TW flake show github:IntersectMBO/cardano-node (with IFD)       >60s (still incomplete)
$ TW eval (builtins.getFlake "...").packages....cardano-node.name 11.3s ✓
$ v3 eval same query
   → error: v3 OP_ATTRS_SELECT: attribute 'getFlake' not found
$ v3 eval via fetchTarball + import
   → timeout >30s
```

cardano-node's flake is intrinsically heavy because of haskell.nix's
IFD-driven plan synthesis. Even TW takes minutes to fully traverse it.
Plotting cardano-node as a v3 target requires both bridging the
flake-fetch primops AND closing the per-op throughput gap.

**Reasonable adjacent target: cardano-cli build expression directly**
(skipping the flake layer), or a non-haskell.nix nixpkgs-derived
derivation. Or: re-attempt cardano-node AFTER Phase 2 IR optimizations
land.

---

## 2. IR-level optimization plan (deep reasoning)

### 2.1 Current state of the optimizer pipeline

`opt_const_fold.cc:optimise(Module &)` runs (in order):

1. **`constantFold`** — literal arithmetic over LitInt/Float/Bool. Same-block only. Skips non-foldable (div-by-zero, overflow). ~30 LoC of folding rules.
2. **`commonSubexprElim`** — hash-cons identical RHSs within a block; replace duplicates with VarRef to canonical.
3. **`elimRedundantForce`** — drops `Force(VarRef→Force(x))` chains and Force-of-known-WHNF.
4. **`inlineTrivialBindings`** — path-compresses VarRef aliasing chains.
5. **`fusePrimOpApps`** — `App(App(LitPrimOp p, a), b)` → `PrimOpCall(p, [a, b])` when arity matches.
6. **`deadBindingElim`** (or `deadBindingElimViaOccur` via gate) — removes bindings whose VarId is never referenced.

The pipeline runs ONCE (no fix-point iteration).

### 2.2 What's measurably missing (per the 200x slowdown investigation)

The hello.drvPath investigation identified four contributing factors:

**(A) Per-iteration intermediate allocations** dominate the wall-time.
TW's primDerivationStrict iterates Bindings in C with zero
intermediates; v3's Option 4 bytecode wrapper allocates ~90
intermediate Nix values per derivation (3 × N for N=attrs/drv,
because `map → filter → listToAttrs` materializes between each).

**(B) Bytecode primops are not fused.** `foldl' op init (map f xs)`
walks the list twice (and allocates an intermediate list); the
fused-loop form (`foldl' (acc: x: op acc (f x)) init xs`) would
match TW's primConcatMap-style fast paths.

**(C) Pure primops dispatch through OP_CALL_PRIMOP** even when the
argument shapes are statically known. `builtins.length [1 2 3]` is
a constant; `builtins.elemAt [1 2 3] 0` is a constant. The constant
folder doesn't recognise these patterns.

**(D) Closures get materialised even when used once.** `(x: x + 1) y`
allocates a closure, then App, then OP_CALL pushes a frame, then
OP_RETURN tears it down. Beta-reduction at IR level would inline the
body.

### 2.3 Step-by-step plan (8 phases, ranked by leverage)

Each phase has: hypothesis killed, exit criterion, dependency.

#### Phase A — Beta reduction (1-shot, no captures)

**Hypothesis killed**: "every immediate-call lambda needs runtime
closure/frame alloc."

**Pass**: walk `App(VarRef→Lambda, arg)` patterns; when the lambda's
freeVars are all VarRef-reachable in scope AND the lambda is referenced
once, inline the body with the formal substituted by VarRef→arg.

**Exit criterion**: `(x: x + 1) 5` lowers to no MAKE_CLOSURE / OP_CALL.
Measurable: `fib33` (heavy curried-apply) goes from 1.6× TW → ≤1.4× TW.

**Dependency**: none. Lands before any of B-H.

**Effort**: 1-2 days. Touches `opt_inline.cc`.

**Risk**: side-effect lambdas (throw, abort) — only inline when the
lambda body is provably pure. Already have a purity classifier in
opt_dce.cc.

#### Phase B — Pure primop constant folding

**Hypothesis killed**: "static-data primop calls need runtime dispatch."

**Pass**: extend `constantFold` to recognize `PrimOpCall(p, [args])`
where args are all literal LitInt/LitFloat/LitBool/LitString and
`p` is a known-pure primop (length, elemAt, head, tail, attrNames,
toString of literal, etc). Compute the result at compile time.

**Exit criterion**: bench shows `length [1 2 3 4 5]` lowers to LIT_INT 5,
no OP_CALL_PRIMOP emitted.

**Dependency**: A (to ensure inlining surfaces these patterns).

**Effort**: 2-3 days (per-primop folder table; matching ListExpr/AttrSet/
LitString shapes).

#### Phase C — Stream fusion (`foldl' (acc: x: ...) init (map f xs)` → single loop)

**Hypothesis killed**: "list operations require N + N walks + intermediate
list alloc."

**Pass**: a peephole over App-chains looking for `foldl' op init (map f xs)`,
`foldl' op init (filter p xs)`, etc. Rewrite to:
```nix
let
  g = acc: x: op acc (f x);  # or `acc: x: if p x then op acc x else acc`
in foldl' g init xs
```

The fused form runs ONE traversal with ONE intermediate (the accumulator).
Mirrors GHC's `build/foldr` rule.

**Exit criterion**: bench attrset-build-1k (the test that regressed +60.4%
on my early bytecode primop work) goes from baseline to ≤30% over TW.

**Dependency**: A, B.

**Effort**: 4-5 days. Touches lower.cc + new opt_fusion.cc. The peephole
needs to be conservative: only fuse when the inner function is pure
(no `throw`, no side-effecting primop).

**Risk**: semantics of error propagation (`foldl' _ _ (map throw xs)` —
the map's throw fires lazily; fused form might re-order).

#### Phase D — Closure-free lambda lifting

**Hypothesis killed**: "every lambda allocs a Closure even when it
captures nothing."

**Pass**: detect `Lambda` IR nodes with `freeVars.empty()` AND
`lexicalWiths.empty()`. These can be allocated once globally
(per-CompilationUnit) and reused without per-creation alloc. Add an
"intern" step in emit.cc that registers a singleton Closure pointer.

**Exit criterion**: smaller alloc counts on benchmarks; no semantic
change.

**Dependency**: A.

**Effort**: 1-2 days.

**Risk**: low. Captures are tracked precisely.

#### Phase E — Selector-thunk specialization at IR level

**Hypothesis killed**: "every `x: x.foo` lambda runs through generic
OP_CALL dispatch."

Note: v3 already has a `desc->selectorSym` fast-path at OP_CALL
runtime (vm.cc:8042). But the recognition is at LOWER time; if my IR
optimizations introduce new selector-shaped lambdas (via beta
reduction or fusion), they need to mark them.

**Pass**: after Phase A inlining, scan resulting Lambda IR nodes for
the `paramVar: AttrSelect(paramVar, sym)` body shape. Mark them with
`selectorSym = sym`. emit.cc already wires this into LambdaDescriptor.

**Exit criterion**: nixpkgs hot path benchmarks measure fewer
"generic OP_CALL" dispatches vs "selector-lambda" ones.

**Dependency**: A.

**Effort**: 0.5 day.

#### Phase F — Static App-spine folding

**Hypothesis killed**: "App-chains can't be flattened until runtime."

**Pass**: when an App's `fun` resolves to a statically known Lambda
AND all upstream Apps in the spine have known-pure args, beta-reduce
the entire spine.

`((f a) b) c` where `f = (x: y: z: x + y + z)` → `a + b + c`.

**Exit criterion**: PartialApp / Tag::PrimOpApp counters in
`allocStats` drop measurably on the bench corpus.

**Dependency**: A.

**Effort**: 1-2 days.

#### Phase G — Pure if-then-else folding

**Hypothesis killed**: "branch elimination only happens at runtime."

**Pass**: `If(LitBool(true), then, else)` → emit `then` (and drop else).
Trivial; complements constant-fold.

**Exit criterion**: bench shows fewer OP_BRANCH_FALSE in compiled
bytecode for static-branch shapes.

**Dependency**: B (to ensure conditions resolve to literals).

**Effort**: 0.5 day.

#### Phase H — IR-level loop unrolling (small known lists)

**Hypothesis killed**: "`genList f 4` always builds a 4-element
list with App entries."

**Pass**: when `genList f n` is statically known and `n ≤ 8`,
unroll to `[f 0, f 1, ..., f (n-1)]` (a ListExpr). Each element
becomes a regular IR node, subject to Phase A inlining.

**Exit criterion**: `genList id 4` lowers to a 4-element ListExpr
with no OP_CALL_PRIMOP genList.

**Dependency**: B.

**Effort**: 1 day.

### 2.4 Pipeline ordering

After landing all phases:

```
1. constantFold              (extend: known-pure primops)         [B]
2. betaReduce                (1-shot lambda inlining)             [A]
3. constantFold              (run again — beta-reduced shapes may newly constant-fold)
4. commonSubexprElim         (unchanged)
5. elimRedundantForce        (unchanged)
6. inlineTrivialBindings     (unchanged)
7. streamFusion              (foldl'/map/filter fusion)           [C]
8. genListUnroll             (small static genList)               [H]
9. ifThenFold                (static-condition If)                [G]
10. lambdaLift               (capture-free Lambda → singleton)    [D]
11. selectorRecognize        (mark x: x.sym lambdas)              [E]
12. fusePrimOpApps           (unchanged)
13. staticAppSpineFold       (fully static App spines)            [F]
14. deadBindingElim          (unchanged)
```

Fix-point iteration: re-run steps 1-3 + 7 + 13 until no more changes,
capped at a max-iterations safety guard. Most modules converge in 1-2
iterations.

### 2.5 Estimated impact (rough order of magnitude per phase)

| Phase | Workload best-case win | Risk class |
|---|---|---|
| A — beta-reduce | fib33 1.5× → 1.2× | low |
| B — primop constant-fold | bench microcorpus 1.1× → 1.05× | low |
| C — stream fusion | attrset-build-1k 1.6× → 1.0× | medium |
| D — capture-free lift | 5-10% alloc reduction overall | low |
| E — selector at IR | nixpkgs lib selectors faster | low |
| F — static App spine | curried-apply benches 1.2× → 1.0× | low |
| G — static-If fold | minimal (already rare at IR) | trivial |
| H — small genList unroll | nixpkgs map-of-small-list faster | low |

Cumulative target: hello.drvPath 30× → ≤3× over TW. Combined with
the existing Option 4 wrapper, the per-op overhead drops from ~200×
to ~20-30× (where the remaining gap is intrinsic to bytecode dispatch
vs C iteration).

### 2.6 Phase 2+ extensions (DEFERRED — measure first)

- **Result caching for mkDerivation-style fix-points** — hypothesis E
  from `EXTEND_DERIVATION_INVESTIGATION_2026-05-18.md`. Requires
  measurement of WHAT TW caches before commit.
- **Direct-threaded bytecode** — eliminates the switch dispatch
  overhead. ~10-15% per-op win. Touches vm.cc dispatch loop only.
- **JIT** — Stage 12 candidate in ROADMAP_TO_VISION; not before
  Phase 1.5 measurement spike concludes.

---

## 3. Test infrastructure status

### 3.a Regression tests — STRONG

| Category | Count | Notes |
|---|---|---|
| Lang tests (`run-lang-tests.sh`) | 143 (142 pass) | upstream nix functional/lang/ runs through v3-eval |
| Property tests (`property_tests.py`) | 580 (580 pass) | 58 categories × 10 cases, TW oracle |
| Derivation parity (`derivation-parity.sh`) | 20 (20 pass) | drvPath byte-for-byte vs TW; NEW this session |
| Iterative-force depth (`iterative-force-depth.sh`) | 10 hard | let-chain, curry-apply, app-spine; 5000-deep |
| Let-rec publish split (`run-let-rec-publish-split-tests.sh`) | meson-integrated | #546 regression guard |
| evalscope handles (`v3-evalscope-handles`) | 33 checks | EvalScope handle invalidation |
| drv-preflight (`v3-drv-preflight`) | meson-integrated | libnixstore drvPath validation |
| Repro fixtures | 12 `.nix` files | repro-455 / 495 / 583 / a12b / hello-name |
| Shell-based test scripts | 46 in `test/run-*.sh` | bug-specific repros + harnesses |

**Meson-integrated (auto-run on `ninja test`)**: v3-smoke, v3-drv-preflight,
v3-evalscope-handles, v3-bench-harness-self-tests, v3-lint-no-inline-getenv,
v3-let-rec-publish-split, v3-iterative-force-depth. **7 meson-integrated.**

**NOT meson-integrated** (but committed): the 46 shell scripts + 12 repro
fixtures. Some are run by hand; some by CI hooks. Coverage gap: no
single `make v3-test` target runs the full suite.

### 3.b Performance benchmarks — EXISTS but UNDER-USED

| Mechanism | Status |
|---|---|
| `bench/bench.py` + `workloads.toml` | EXISTS; 30+ workloads (fib25/30/33, ackermann, lib-evalModules, etc.) |
| `bench-v3-vs-tw.sh` | EXISTS; per-workload median timing |
| `bench/baselines/*.json` | DIR EXISTS but no recent baseline committed |
| `V3_TIMING=1` per-phase | EXISTS (lower/optimise/compile/run/vm/bridge ms) |
| `V3_DBG_FORCES` periodic stats | EXISTS (forced/allocated/ratio/arena/hot-thunk) |
| `NIX_VM_STATS=1` final dump | EXISTS (closures/thunks/lists/etc.) |
| `NIX_SHOW_STATS=1` (TW side) | EXISTS — used in investigation this session |
| CI-integrated perf regression detection | **MISSING** |
| Recent committed baseline | **MISSING** (last bench/baselines/* is days old) |
| Bench corpus for derivation parity (drvPath / outPath wall-time) | **MISSING** |

**Recommendation**: re-baseline today's HEAD + add hello.name /
hello.drvPath / derivation-chain-10 / derivation-chain-30 to
workloads.toml. Commit baseline. Wire bench into CI with
±10% regression threshold.

### 3.c Positive/negative tests for this session's lessons — INCOMPLETE

Lessons learned this session and their test status:

| Lesson / fix | Positive test | Negative test | Notes |
|---|---|---|---|
| genericClosure WHNF-on-callback-return (b0a0ff2e1) | derivation-parity covers indirectly | — | should add explicit `genericClosure (op = ...)` parity test |
| OP_ATTRS_SELECT_IC iterative force (a499fc2b0) | iterative-force-depth | NIX_V3_NO_PATH_COMPRESS A/B | acceptable |
| seq lower.cc fast-path (7f107525d) | lang tests cover seq | — | should add explicit perf test (seq many vs C seq) |
| isTrueValue Tag::Slot chase | derivation-parity catches semantic | V3_DBG_EXPECTED_BOOL diagnostic | weak — need a fixture that EXACTLY exercises the path |
| removeAttrs element WHNF (in commit 7adc7e61f) | derivation-parity catches | — | should add `removeAttrs s (map f xs)` fixture |
| __derivationFromPreprocessed FFI leaf (Option 4) | 20-case derivation-parity | NIX_V3_NO_BC_DERIVATION_HYBRID A/B | good |
| Tag::App memoization (d3e41c13d) | indirect; lang/property/parity | NIX_V3_NO_APP_MEMO A/B | good |
| primDerivation wrapper hello.name | implicit (Option 4 hybrid test) | — | should add a hello-name perf bench |

**Coverage gaps to close**:

1. **`test/repro-genericClosure-bytecode-filter.nix`** — the exact
   shape that broke pre-b0a0ff2e1: bytecode `filter` invoked inside
   `genericClosure { operator = ... }`. Run under v3-direct, expect
   correct list.

2. **`test/repro-removeAttrs-lazy-elements.nix`** — the shape that
   broke before adding element-force: `removeAttrs s (map f xs)`.

3. **`test/repro-isTrueValue-slot.nix`** — synthetic Tag::Slot reaching
   OP_NOT via the OP_ATTRS_SELECT_IC writeback chain. Hard to
   construct synthetically; consider keeping as
   `repro-hello-drvpath-bool.nix` if a small repro can't be isolated.

4. **`test/repro-app-memo-regression.nix`** — positive: a `map`
   result accessed twice returns same Bindings ptr (App memo hit).
   Negative: `NIX_V3_NO_APP_MEMO=1` shows the same semantic result
   (parity preserved).

5. **`bench/workloads.toml` additions**:
   - `hello-name-real-nixpkgs`: `(import <nixpkgs>{}).hello.name`
   - `derivation-chain-30`: the synthetic chain used this session
   - `bench-attrset-build-1k`: the workload that regressed before
   - `derivation-strict-parity`: drvPath of stdenv-style drv

6. **Meson-integrate the shell test scripts**: each `run-*.sh`
   needs a `test()` block in meson.build so `ninja test` covers them.
   Today only 7 are integrated; 46 exist.

7. **`test/REPROS.md`** (already in the lessons doc as gap #10):
   manifest mapping issue → fixture. Build this index.

### 3.d Recommended test-infrastructure work (concrete)

Priority order:

1. **`test/all-v3-tests.sh`** master runner that invokes every
   `run-*.sh` + `derivation-parity.sh` + property tests + lang tests.
   Exit non-zero if anything fails. CI-integrate.

2. **Wire all shell scripts into meson** (`ninja test` covers them).

3. **Add the 5 missing repros listed above** plus the bench-workload
   additions.

4. **Re-baseline `bench/baselines/`** today and commit (date-stamped).

5. **Build `test/REPROS.md`** — one row per fixture: file, lesson,
   issue link.

6. **CI perf-regression detection**: bench harness compares against
   baseline JSON; fails if any workload regresses >15%.

---

## Summary

| Question | Status | Effort to close |
|---|---|---|
| cardano-node eval | NO (3 blockers) | Bridge getFlake (1 day) + IFD support (1 wk) + Phase A-H (4 wks) |
| IR optimization plan | DETAILED (8 phases) | 4-6 weeks of focused work |
| Regression tests | GOOD (~680 cases) | Meson-integrate the 46 unintegrated scripts (1 day) |
| Perf benchmarks | EXISTS but UNUSED | Baseline + CI integration (2 days) |
| Lessons-learned coverage | PARTIAL (some yes, some gaps) | 5 missing repros + REPROS.md (2 days) |

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0
