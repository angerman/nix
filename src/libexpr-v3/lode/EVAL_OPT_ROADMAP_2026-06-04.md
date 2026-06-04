# Eval-optimization roadmap: what to pursue after the fold/loop opts

**Date:** 2026-06-04
**Status:** ROADMAP / OPPORTUNITY MENU — the optimization catalog *beyond* the 5
fold/loop opts (+ strictness #2 + specialization). Code-grounded at HEAD
(`cd1da2577`+); ranked, with what v3 already has so "missing" is honest.
**Triggering question:** "So strictness, specialization — what other optimizations
should we look at after those 5?"
**Author:** session synthesis

Companion docs:
- [`BENCH_V3_VS_TW_2026-06-04.md`](BENCH_V3_VS_TW_2026-06-04.md) — the two-gaps baseline (wall 2.26× vs memory 4.4–5.3×) this roadmap is prioritized against
- [`QUADRATIC_BUILD_AND_FUSION_SURVEY_2026-06-04.md`](QUADRATIC_BUILD_AND_FUSION_SURVEY_2026-06-04.md) — the opt-pass survey + the `++`/`//` build-fusion lever (Tier 2 here)
- [`POST_PURE_PIPELINE_OPTS_2026-06-02.md`](POST_PURE_PIPELINE_OPTS_2026-06-02.md) — the AOT/determinism axis (orthogonal to this; that's *capability*, this is *speed+memory*)
- [`IR_OPTIMIZATION_PLAN_2026-05-18.md`](IR_OPTIMIZATION_PLAN_2026-05-18.md) — the original A–H pass plan (most of which landed; see §1)

---

## 0. The unifying principle (read first)

The 5 fold opts (#1 strict-let+seq-elim, #2 strictness-through-recursion, #3
eval/apply multi-arg calls, #4 LICM / worker-wrapper, #5 specialized list
iteration), plus **strictness** and **specialization**, are all instances of ONE
principle:

> **Don't materialize the thunk / closure / Bindings you don't need.**

Every item below is the same principle applied elsewhere. The consequence — and the
reason these are the right post-fold investment — is that they **compound on BOTH
axes**: each one reduces *allocation*, so it buys wall (the 2.26× gap) AND memory
(the 4.4–5.3× gap, the bigger one per [[memory-first-class]]). Contrast the
register-VM / dispatch / superinstruction work, which touches **wall only** and is
capped by the ~1.5–2× interpreter ceiling (architecture review; n-gram super-insns
measured ~1%). **Allocation-reduction is the higher-slope lever on both axes.**

---

## 1. What v3 already has (so "missing" is grounded)

Pipeline driver `opt_const_fold.cc:274 optimise(Module&)`:

| Pass | File | Scope/limit |
|---|---|---|
| constant fold | `opt_const_fold.cc:244` | arithmetic/comparison/bool on same-block literals |
| beta reduce | `opt_beta_reduce.cc:370` | **single-step, same-block, simple body only** |
| block CSE | `opt_cse.cc:123` | **block-local** |
| alias inline | `opt_inline.cc:203` | `v = VarRef u` collapse |
| primop fuse | `opt_primop_fuse.cc:67` | App-chain → `PrimOpCall` |
| primop fold | `opt_primop_fold.cc:282` | `length`/`head`/`tail`/`elemAt`/`toString`/`attrNames` on literals |
| stream fusion | `opt_stream_fusion.cc:339` | **`foldl'∘map → __foldlMap` ONLY** |
| if fold | `opt_if_fold.cc:85` | `if literal` |
| genList unroll | `opt_genlist_unroll.cc` | n ≤ 8 |
| app-spine fold | `opt_app_spine_fold.cc:423` | saturated curried call, pure args |
| DCE | `opt_dce.cc` | dead pure bindings |
| strictness | `opt_func_strictness.cc` / `opt_strict_call_unthunk.cc` | opt-in; **bails at the recursive boundary** (= opt #2) |
| non-rec-let demote | `cli/lower_v3.hh` (`cd1da2577`) | ✅ landed — non-rec `let` → local thunk, no heap cell |
| AttrSelect IC / RecSlot IC | `bytecode.hh` CompilationUnit | runtime, 4-way / 1-way |

Unboxed ints/floats/bools (tagged 16-B Value) are already in. So the catalog below
is the genuinely *missing* or *under-developed* set.

---

## 2. The catalog (tiered)

### Tier 1 — enablers (do first; they multiply everything else)

**T1.A — General inlining (size-bounded, cross-block).** The *master* opt for Nix.
v3 has only single-step `betaReduce` + `appSpineFold`. Nix is pathologically layered
(`lib.mkIf`, `optionalAttrs`, `mapAttrs`, fixed-points, hundreds of 1-line wrappers);
a real inliner pulls small callees into the caller and exposes const-fold,
strictness, AND fusion at once. **Specialization is the HOF case of this** — inline a
HOF with a known lambda arg (`foldl' (a:b:a+b)`) → the callback's strictness becomes
visible → unthunk its arg + fuse the loop (the route that breaks "op's arg is
fundamentally lazy" + fold-opt #5). Without an inliner, strictness/fusion only fire
on what's already in one block. *Effort: high (heuristics, laziness-correct cloning);
highest leverage.*

**T1.B — Cardinality / used-once analysis (the strictness companion).** Strictness =
*"is it forced?"*; cardinality = *"how many times used?"* A `let`/arg used **≤ once**
→ inline it, killing BOTH the thunk and the share-cell. Removes the "every `let x = e`
is a heap thunk" tax that fold-opt #1 fixes only for the seq-forced case. GHC unifies
strictness+cardinality in one demand analyzer — that's the right home for #1/#2 + this
(one analysis, not three passes). *Effort: medium; pairs naturally with the #2 work.*

### Tier 2 — Nix-specific structural wins (highest combined wall+memory payoff)

**T2.A — The stream-fusion family + build-fusion.** `__foldlMap` (foldl'∘map) is the
ONLY combinator fusion today. Un-fused pervasive lib chains: `map∘filter`,
`filter∘map`, `concatMap∘genList`, **`mapAttrs∘filterAttrs`** (the module-system
workhorse), `listToAttrs∘map`. Each fusion deletes an intermediate list/attrset.
**Build-fusion** (`++`/`//`-folds → allocate-once / transient builder; see
[[quadratic-build-and-fusion-survey-2026-06-04]]) is the same family and attacks the
`a // b` churn that is **84% of arena bytes** (#746). This is where inlining →
specialization → fusion converge. *Effort: medium per fusion; the transient builder
is the bigger piece (needs static linearity — no refcount under Boehm).*

**T2.B — Attrset shape sharing (hidden classes / BiBOP).** The big Nix-specific lever
not yet pursued. Every derivation attrset shares ~the same key-set
(`name`/`version`/`src`/`buildInputs`/…); every NixOS config node shares a shape.
Store the key vector ONCE per shape (a descriptor) + values in a flat array → dedups
the Bindings *key arrays* (a large slice of the 84% Bindings memory) AND makes lookup
monomorphic/shape-cached (faster, feeds the AttrSelect IC). V8-style hidden classes;
v3 has `shapeCell` hooks to build on. Memory AND wall, tailored to Nix's
attrset-dominated heap. *Effort: high (representation change); see §4 caveat.*

### Tier 3 — cheap, safe peepholes (FileCheck-able; opportunistic)

**T3.A — Operator opcode-lowering.** **Verified 2026-06-04:** `i >= n` (and a bare
`1 < 2`) lowers to `PrimOpCall "__lessThan" + Not`, **not** the `OP_LESS` opcode that
exists in the VM — so every per-element comparison pays LIT_PRIMOP-push + CALL_PRIMOP
+ a C++ `primLessThan` instead of an inline opcode. A peephole
`PrimOpCall(__lessThan/add/sub/…) → ir::Less/Add/… → OP_*` eliminates the call
(cheaper than LICM-hoisting the push). Confirm `OP_LESS` handles the comparable-type
polymorphism (int/float/string/path) before assuming it's free. *Effort: low; per-element win in every loop.*

**T3.B — Algebraic simplification (breadth).** `x // {}`→`x`, `[] ++ x`→`x`,
`if c then true else false`→`c`, `length (genList f n)`→`n`, redundant double-`force`
/ `seq` of WHNF, `toString` of a string. v3 has *some* via primop-fold/const-fold;
broaden it. *Effort: low.*

### Tier 4 — frontier (high ceiling, hard; gate on need)

**T4.A — Cross-module / import inlining.** `lib` is imported *everywhere*, but its
tiny functions can't be inlined into consumers today (separate CUs). Inlining across
the import boundary (interacts with the content-addressed bytecode cache) is the
whole-program frontier — potentially huge on real workloads, hard to get right.

**T4.B — Global value numbering (cross-block CSE).** v3's CSE is block-local;
repeated attr-path reads (`config.x.y` many times) + comparisons across blocks aren't
shared. Partly subsumed at runtime by the AttrSelect IC. *Effort: medium; moderate value.*

---

## 3. Sequencing + rationale

1. **Tier 1 (inlining + cardinality) first** — enablers multiply the value of
   strictness, fusion, AND specialization. Doing fusion/specialization before a real
   inliner fires them on far fewer sites.
2. **Tier 2 (fusion family + shape-sharing) next** — the highest combined wall+memory
   payoff, specific to how Nix code is actually shaped.
3. **Tier 3 peepholes ride alongside** any of it (independent, cheap).
4. **Tier 4 last**, gated on whether Tiers 1–2 left a gap worth the frontier cost.

**The decisive contrast:** Tiers 1–3 all reduce *allocation* → they buy BOTH the wall
gap (2.26×) and the memory gap (4.4–5.3×, the bigger one). The register-VM / dispatch
work buys wall only and is ceiling-bounded. **Single highest-leverage start:
T1.A general inlining** — it's the enabler that makes strictness, fusion, and
specialization dramatically more effective and directly deletes the lib-wrapper
thunk/closure churn on every real workload.

---

## 4. Honest limits / risks

- **Over-eagerness is the recurring hazard.** Strictness (#2), cardinality (T1.B),
  and inlining (T1.A) can all force/inline something that wasn't demanded → turn a
  terminating lazy program non-terminating, or surface a `throw` early. This is the
  `CALLPACKAGE_BUG` class. Every one needs the *conservative* direction (force/inline
  only when demanded on ALL paths) + per-pass gating + byte-identical drvPath gates +
  `--quick`/`--core`. Per [[measure-twice-cut-once]].
- **Shape-sharing (T2.B) is a representation change** that touches the ~208 sites
  reading `Bindings::entries[]` directly — the same audit that blocks the 4×-falsified
  Chain/HAMT persistent-overlay (see `EXIT_PHASE_C_4_FALSIFIED`). Note it's
  *orthogonal* to persistence (it shares KEY arrays, not the overlay structure) and
  doesn't need O(1) `//`, but it inherits the same entries[]-audit prerequisite.
- **Cross-module inlining (T4.A)** interacts with content-addressing — inlining across
  CU boundaries must keep the cache key a pure function of source.
- **None of this is the AOT/determinism axis** ([[post-pure-pipeline-opts-2026-06-02]])
  nor the GC/M5-watchdog arc — those are separate programs; this is the eval-engine
  optimization catalog.

---

## 5. The Phase-2 checkpoint (fold opts → native-op decision)

Once the 5 fold opts (+ strictness) land, **re-run the `NIX_V3_NO_BC_FOLDL` A/B**
(bytecode `foldl'` vs the C++ `primFoldl` 11M-insn floor). That resolves the deferred
question — *native list-iterator opcode (fold-opt #5) vs prefer-C++-`primFoldl` vs
"close enough, keep bytecode"* — with residual data, rather than building
worker/wrapper LICM speculatively.

---

## 6. Cross-references

- [[bench-v3-vs-tw-2026-06-04]] — the two-gap baseline this is prioritized against
- [[quadratic-build-and-fusion-survey-2026-06-04]] — T2.A build-fusion + the opt-pass survey
- [[post-pure-pipeline-opts-2026-06-02]] — the orthogonal AOT/determinism axis
- [[memory-first-class]] — why the memory gap (and thus allocation-reduction) is higher-slope
- [[measure-twice-cut-once]] / [[falsification-rule]] — §4 gating
- Code anchors: §1 table (the `opt_*.cc` pipeline); `cli/lower_v3.hh` (`cd1da2577` demotion);
  `OP_LESS`/`OP_ADD` opcodes in `bytecode.hh` (T3.A target); `shapeCell` (T2.B);
  `EXIT_PHASE_C_4_FALSIFIED_2026-05-30.md` (the entries[]-audit prerequisite)

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
