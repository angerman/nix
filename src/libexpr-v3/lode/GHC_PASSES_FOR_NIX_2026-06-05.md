# GHC's optimization playbook vs v3: which passes transfer to Nix + the bytecode VM

**Date:** 2026-06-05
**Status:** RESEARCH / REFERENCE — critically reviewed against the v3 code at HEAD
(`eb53787cb`+) and against GHC's actual Core-to-Core pipeline. Every "v3 lacks X"
is grounded (§1). The §3 critical nuances are the load-bearing part — several
naive "just port the GHC pass" instincts are *wrong* for v3's representation.
**Triggering question:** "What GHC (and other) optimization passes do we still miss
and could add — let-floating? demand analysis? typing? Which make sense for Nix and
our bytecode VM?"
**Author:** session synthesis

Companion docs:
- [`EVAL_OPT_ROADMAP_2026-06-04.md`](EVAL_OPT_ROADMAP_2026-06-04.md) — the v3-framed catalog (inlining/cardinality/fusion/shape-sharing/…); this doc is the GHC-grounded *why* + the additions it surfaces (demand, SAT, w/w, simplifier-fixpoint, flow-typing)
- [`BENCH_V3_VS_TW_2026-06-04.md`](BENCH_V3_VS_TW_2026-06-04.md) — the per-element fold profile these passes target (thunks + 21 GET_UPVALUE/elem)
- [`QUADRATIC_BUILD_AND_FUSION_SURVEY_2026-06-04.md`](QUADRATIC_BUILD_AND_FUSION_SURVEY_2026-06-04.md) — fusion + the `++`/`//` builder (the RULES/Perceus discussion below)

---

## 0. The mapping principle (what transfers, and the one correction that reshapes it)

Nix is a **lazy, dynamically-typed, functional** language whose only structured
value is the attrset (plus list/lambda/scalars; **no ADTs, no type classes, no user
types**). GHC is the canonical optimizer for a lazy functional language, so its
*Core*-level passes are the right prior art — but they sort into four buckets:

| GHC pass family | Transfers to Nix? |
|---|---|
| **Laziness-exploiting** (demand/strictness, worker-wrapper, let-floating, SAT, eta/arity) | **Directly** — Nix is lazy like GHC. The wins live here. |
| **Type-exploiting** (type-class spec, type-directed unboxing) | **Not as-is** — Nix is untyped. Becomes *flow-based shape/Tag inference* (§2.5, the "typing" answer). |
| **ADT/`case`-exploiting** (case-of-case, SpecConstr, CPR, liberate-case) | **Mostly N/A** — Nix's only "constructor" is the attrset; dispatch is thin (`if`/select/`?`). |
| **Backend** (Cmm, register allocation, instruction selection) | **Low value** — v3 is an *interpreter*; its analog (register-VM/superinstructions) is capped by the ~1.5–2× interpreter ceiling (architecture review). Focus on Core-level. |

**THE correction that reshapes everything (verified in `value.hh`):** GHC's biggest
wins come from **two** mechanisms — (a) *thunk-elimination* (demand → strict args
need no thunk) and (b) *unboxing* (types → `Int#` in registers, CPR returns unboxed).
**v3 already stores scalars inline in its 16-byte tagged `Value`** (`mkInt` writes
`payload.i`; no heap box). So **(b) does not apply to v3** — there is no boxing of
ints/floats/bools to remove, CPR is a non-issue, and "unbox the accumulator" is
already true. **v3 inherits GHC's *thunk-elimination* win and not its *unboxing*
win.** This single fact re-prioritizes the whole list: the value of demand analysis
+ worker/wrapper for v3 is *killing `MkThunk` allocations*, full stop — not the
register-unboxing that motivates half of GHC's machinery.

---

## 1. v3's optimizer today (the grounded baseline)

`optimise(Module&)` runs a **fixed sequence** (with a few manual re-runs), not a
fixpoint simplifier:

| Has | v3 pass | GHC analog | Limit |
|---|---|---|---|
| ✓ | constantFold | const folding | same-block literals |
| ✓ | betaReduce | inlining (β) | **single-step, same-block, simple body** |
| ✓ | inlineTrivialBindings | (β alias) | `v = VarRef u` only |
| ✓ | commonSubexprElim | CSE | **block-local** |
| ✓ | fusePrimOpApps / primOpFold | const folding | App-chain→PrimOpCall; literal folds |
| ✓ | streamFusion | RULES (fusion) | **`foldl'∘map → __foldlMap` ONLY** |
| ✓ | ifThenFold | case-of-known-con | bool literal only |
| ✓ | genListUnroll | (unrolling) | n ≤ 8 |
| ✓ | appSpineFold | (β / arity) | saturated curried, pure args |
| ✓ | deadBindingElim(+occur) | dead-code | iterates to fixpoint |
| ✓ | opt_func_strictness + opt_strict_call_unthunk | **partial DmdAnal** | forward per-fn `strictArgs` (forced args) → call-site unthunk; **no cardinality/absence; bails at recursion** (= opt #2, in flight) |
| ✓ | eager-forced-let (opt #1), let-rec demotion | (demand-ish) | landed; dominance-sound |
| ✓ | eval/apply (gated `NIX_V3_EVAL_APPLY`) | STG eval/apply | in flight, gate-on GREEN |
| ✗ | — | **unified Demand Analysis** | only the forward strictness fragment above |
| ✗ | — | **Worker/Wrapper** | absent |
| ✗ | — | **Static Argument Transformation** | absent |
| ✗ | — | **Float-in / Float-out (full laziness)** | absent (only the narrow single-use-thunk lift in `opt_strict_call_unthunk`) |
| ✗ | — | **flow type/shape inference** | absent (runtime AttrSelect/RecSlot ICs only) |
| ✗ | — | **SpecConstr / call-pattern spec** | absent |
| ✗ | — | **RULES rewrite framework** | only the one hand-coded fusion |
| ✗ | — | **GVN** (cross-block CSE) | block-local only |
| n/a | — | CPR / unboxing | **N/A — scalars already inline (§0)** |

Scalars inline; attrsets are sorted key/value arrays with 4-way AttrSelect ICs.

---

## 2. The catalog (tiered by Nix + v3 fit)

### Tier 1 — transfer directly, highest value

**2.1 Unified Demand Analysis (GHC `DmdAnal`).** One *backward* analysis computing
**strictness** (forced) + **absence** (never demanded) + **cardinality** (used-once).
v3 has only the forward *strictness* fragment, split across two ad-hoc passes, and it
stops at recursion. **Build one demand analysis** instead: it subsumes opt #1/#2 and
adds (i) used-once → inline, killing the thunk *and* the share-cell; (ii) absent arg →
don't pass/thunk it. *v3 payoff: thunk-elimination only (§0) — but that's exactly the
remaining fold bottleneck (300003 thunks/elem).* *Catch: soundness (the
`CALLPACKAGE_BUG` over-eagerness class); the recursive fixpoint is the subtle part —
must be conservative (in doubt → not-strict).*

**2.2 Worker/Wrapper + Static Argument Transformation (SAT).** The recursion wins.
- **SAT** (GHC `-fstatic-argument-transformation`): drops arguments that are *constant
  across every recursive call* and captures them as free vars of a local worker. This
  is **literally `foldl'`** — `op`, `list`, `n`, `go` are static; only `i`/`acc` vary.
  SAT kills the per-iteration re-fetch/re-push of invariants (the **21 GET_UPVALUE/elem**
  the bench found). *Honest nuance: SAT is OFF-by-default in GHC (mixed results,
  interacts with inlining) — but it precisely targets v3's hot-loop shape, and it's the
  **safe, recursion-specific** form of LICM (vs the leak-prone float-out, §2.6).*
- **Worker/Wrapper**: split `f` → an inlinable wrapper (laziness boundary) + a strict
  worker. For v3 the worker's benefit is **strict args need no thunk** (NOT GHC's
  register-unboxing, §0). Driven by 2.1.
*Fit: very high — `lib` is recursion/HOF-heavy. v3 status: absent (the next lever after opt #2).*

**2.3 The Simplifier as a fixpoint (GHC's architectural core).** GHC runs *one*
Simplifier — inline + β + case-of-case + case-of-known + local let-float + eta +
const-fold + dead + RULES — **iterated to a fixpoint**, because each transform exposes
the next. v3 runs a fixed sequence with manual re-runs (`constantFold`×4, `inline`×4).
**Restructure the local transforms into a fixpoint core**, with the global analyses
(demand, SAT, CSE, float) interleaved between runs (GHC's shape). This *multiplies*
every other pass — especially inlining, which is the enabler for Nix's layered `lib`.
*Architectural; highest force-multiplier.*

**2.4 Eta-expansion + Call-Arity — fold into the in-flight eval/apply.** Eta-expanding
partial applications to full arity is what *feeds* the arity-aware (eval/apply)
convention more saturated calls; Call-Arity computes the safe arity. *Critical
soundness nuance (§3): eta-expansion is NOT unconditionally valid in a lazy language —
`\x -> e x` evaluates/shares `e` differently than `e`; it's only safe when the function
is **always** applied to ≥ its arity, which is exactly what Call-Arity proves.* Do this
*with* eval/apply, not separately.

### Tier 2 — Nix-adapted

**2.5 Flow-based type/shape inference — the "typing?" answer.** Nix is dynamically
typed, so GHC's type-*directed* opts don't transfer; the adaptation is a **forward
dataflow over the `Tag` lattice** (Int/Float/String/Bool/Attrs/List/Closure/Path)
inferring "`v` is int here / attrset of shape S / function of arity N." This is the
**V8/JS *type-feedback* idea, done STATICALLY** — which fits v3 because it compiles
*once* into a content-addressed cache (no JIT, no deopt machinery needed). It unlocks a
cluster: safe **operator opcode-lowering** (`<`→`OP_LESS`, `+`→`OP_ADD` *only when*
both sides are provably comparable/arithmetic — the soundness gate); **monomorphic
attr-select** (skip the binary search when the shape is known); and **arity** (feeds
2.4). *Critical ordering nuance (§3): a value's type is known only once it's **forced**
(laziness) — so flow-typing is most effective AFTER/with demand analysis (2.1). It is
**partial and opportunistic** (infer where provable, fall back to the polymorphic
primop otherwise) and **sound-only** (a wrong inference → wrong opcode → corruption).
Note: v3 does NOT need it for unboxing — scalars are already inline (§0).* *Fit: high —
it's the prerequisite enabler for opcode-lowering + monomorphic dispatch.*

**2.6 Let-floating — Float-IN first.**
- **Float-in (sinking)**: push a binding into the branch that uses it → no alloc/force
  on the other path. Nix is full of conditional bindings (`let x=…; in if c then …x… else cheap`).
  Cheap, safe, high-frequency. *Do this.*
- **Float-out (full laziness)**: hoist loop-invariant thunks so they compute once (LICM).
  *Caveat (GHC's hard-won lesson): full laziness can cause **space leaks** — floating a
  thunk lengthens its lifetime; GHC ships `-fno-full-laziness` for hot code. Prefer
  **SAT (2.2)** — the recursion-specific, leak-free version of the win you actually want.*

**2.7 RULES rewrite framework — the principled fusion family.** GHC `RULES`
(foldr/build, map/map, stream fusion) are declared rewrites the Simplifier applies. v3
has *one* hand-coded fusion. A small rule mechanism lets builtins/`lib` declare the rest
(`map∘filter`, `filter∘map`, `concatMap∘genList`, `mapAttrs∘filterAttrs`, build-fusion).
*Nuance: RULES need the inliner + simplifier (2.3) to expose the LHS, and each rule must
preserve Nix's laziness/eval-order/error semantics (a wrong rule is unsound).* *Fit:
high (lib combinator chains); the general form of the survey's biggest gap.*

**2.8 SpecConstr / call-pattern specialization.** Specialize a recursive function on the
*shape* of its arguments at the recursive call. For Nix: specialize `go` on the Tag/shape
of `acc`. Overlaps SAT (2.2) + HOF specialization + flow-typing (2.5); **code-bloat
cost**. Lower priority — pursue only if 2.1/2.2/2.5 leave a measured gap.

### Tier 3 — thin or N/A for Nix

| GHC pass | Verdict for Nix |
|---|---|
| Case-of-case / case-of-known-con | Thin — dispatch is `if`/select/`?` (v3 has `ifFold`). **But:** Nix encodes sum types as `{ type = "…"; … }` attrsets, so **attrset-shape analysis (2.5) is the Nix analog of constructor analysis** — `attrs.type == "x"` branches could become case-of-known-shape. The one place ADT-style opts re-enter, via shape. |
| **CPR / unboxing** | **N/A — scalars already inline (§0).** No boxed `Int` to deconstruct. |
| Liberate-case | Niche (unroll to expose constructors); little to expose in Nix. |
| Type-class specialization, unarisation (unboxed tuples) | N/A — no classes, no unboxed tuples. |

### Explicitly rejected (with reasons)

- **JIT / native codegen** — dispatch ≈5% of wall; interpreter ceiling dominates.
- **Persistent / HAMT attrsets** — the Chain/persistent-overlay path is **4×-falsified**
  (blocked on the 208-site `entries[]` audit, `EXIT_PHASE_C_4_FALSIFIED`).
- **Perceus / refcounted in-place** (Roc/Koka) — blocked by Boehm conservative GC (no
  refcount); the **static-linearity transient builder** (QUADRATIC doc) is the substitute.
- **Attrset shape-sharing / hidden classes (V8)** — *no GHC analog*, but keep it: it's
  the **biggest *memory* lever** and is untyped-attrset-specific (dedup key arrays + feed
  the AttrSelect IC).

---

## 3. Critical nuances (the part to not get wrong)

1. **v3 gets thunk-elimination, not unboxing (§0).** Verified: scalars are inline in
   the `Value`. So frame demand/w-w as `MkThunk`-killers; **drop the "unbox the
   accumulator"/CPR line entirely** — it's already inline, there's nothing to unbox.
2. **"Biggest" depends on the axis.** Demand + worker/wrapper is the biggest **wall**
   pass (thunk-elim drives the fold bottleneck); **shape-sharing** is the biggest
   **memory** pass (no GHC analog). Don't conflate them — and recall the bench says
   memory is the larger gap (4.4–5.3× vs 2.26× wall).
3. **Eta-expansion is unsafe in a lazy language without Call-Arity.** It can duplicate
   work / change strictness/sharing. Only safe when always-saturated → guard with arity
   analysis (this is why it belongs *with* eval/apply, 2.4).
4. **Flow-typing follows strictness, and is opportunistic + sound-only (2.5).** Types
   are known only post-force; infer where provable, else keep the polymorphic primop; a
   wrong inference is corruption. It's an *enabler* (opcode-lowering, monomorphic
   select), not a universal win.
5. **SAT is off-by-default in GHC** (mixed results) **but precisely fits v3's loop** and
   is the leak-free LICM. Float-out (full laziness) has the space-leak caveat — prefer
   float-in + SAT.
6. **The over-eagerness hazard spans 2.1/2.2/2.4/2.5** — strictness, w/w, eta, and
   type-spec can all force/inline/specialize something not demanded → non-termination or
   early `throw` (the `CALLPACKAGE_BUG` class). Every one needs per-pass gating +
   byte-identical drvPath + `--core`, per [[measure-twice-cut-once]].
7. **Don't import GHC's *backend* rationale.** Worker/wrapper, CPR, unarisation are
   half-motivated by native unboxing/registers, which an interpreter with inline-scalar
   tagged Values doesn't have. Keep only their *allocation* (thunk) payoff.

---

## 4. Recommended architecture + sequencing (mapped onto the live work)

1. **Unify the demand analysis** (strictness + absence + cardinality, recursion-correct
   fixpoint) — make it the *home* for opt #1/#2 rather than three ad-hoc passes. *The
   headline gap; opt #2 is already heading here.*
2. **Add Worker/Wrapper + SAT**, demand-driven — the recursion win (invariant capture =
   the 21 GET_UPVALUE/elem; strict args = no thunk). *The lever after opt #2.*
3. **Restructure the optimizer as a Simplifier-fixpoint** (local transforms iterated;
   global analyses interleaved) — multiplies inlining + fusion. *Architectural.*
4. **Fold eta-expansion + Call-Arity into the in-flight eval/apply** (2.4) — they feed it.
5. **Add flow type/shape inference** (2.5), *after* strictness — unlocks the operator
   opcode-lowering (T3.A) + monomorphic attr-select + shape-sharing.
6. **A RULES mechanism** (2.7) + **shape-sharing** (memory) — the structural Nix wins.

**The unifying GHC lesson:** a lazy-FP optimizer is dominated by **demand analysis +
worker/wrapper + a fixpoint simplifier + inlining**. v3 is doing the first two
piecemeal and the simplifier as a fixed sequence. Closing those — *as the thunk-
elimination engine they are for v3 (not GHC's unboxing engine)* — plus the Nix-specific
adaptations (flow-typing instead of a type system; shape-sharing with no GHC analog;
float-in over float-out; SAT for the recursion) is the high-ROI program. Everything else
(SpecConstr, CPR, case-opts, liberate-case, backend/regalloc, JIT) is thin, N/A, or
ceiling-bounded for an interpreter of a dynamically-typed lazy language.

---

## 5. Honest limits

- These are **wall + (some) memory** levers; the M5<4 GB watchdog still routes through
  GC + the `//` churn + caches, not this. (Demand-driven thunk-elim *does* cut
  allocation, so it helps memory too — but it's not the watchdog lever.)
- Several (unified demand, w/w, simplifier-fixpoint) are **substantial, entangled
  engineering** on hot paths — sequence them behind per-pass gates + the byte-identical
  oracle, exactly as the team is doing with eval/apply.
- Past the interpreter ceiling (~1.5–2×), wall returns diminish; **don't chase the last
  fold wall-% past where memory + determinism (AOT) offer more slope.**

---

## 6. Cross-references

- [[eval-opt-roadmap-2026-06-04]] — the v3-framed catalog this grounds in GHC theory
- [[bench-v3-vs-tw-2026-06-04]] — the fold profile (thunks + 21 GET_UPVALUE/elem) §2.1/§2.2 target
- [[quadratic-build-and-fusion-survey-2026-06-04]] — RULES fusion (§2.7) + Perceus/in-place discussion
- [[measure-twice-cut-once]] / [[falsification-rule]] — §3.6 gating; the §0 correction was found by verifying, not assuming
- Code: `value.hh` (inline scalars — the §0 correction); `opt_func_strictness.cc` / `opt_strict_call_unthunk.cc` (the partial DmdAnal = §2.1); the `opt_*.cc` set (§1, what's absent)
- GHC refs: *DmdAnal* + Worker/Wrapper (Peyton Jones & Partain); *Static Argument Transformation* (Santos); *SpecConstr* / *Call-Arity* (Breitner); *Let-floating: moving bindings to give faster programs* (Peyton Jones, Partain & Santos); the GHC Simplifier (Peyton Jones & Santos, "A transformation-based optimiser for Haskell")

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
