# v3 Optimizer — Comprehensive Report (2026-05-07)

> **ARCHIVED-IN-PLACE 2026-05-27**: This doc is historical (pre-strategic-doc-set or one-off RCA). Preserved here because it is still referenced from one or more KEEP docs. See [`LODE_CLEANUP_REVIEW_2026-05-27.md`](LODE_CLEANUP_REVIEW_2026-05-27.md) for the archival rationale.

---


A study of the existing optimization pipeline in
`src/libexpr-v3/`, cross-referenced with the published literature on
optimization for lazy functional bytecode VMs (STG, V8, Zinc, PyPy
adaptive specialiser, OCaml Lambda passes), and concluding with
ranked recommendations. Four parallel agents produced the underlying
data; I synthesized and reconciled.

---

## 1. Executive summary

v3 has a conventional 6-pass IR-level optimizer plus a small set of
emit-time superinstructions and one polymorphic inline cache for
attribute selection. The passes are **soundness-first to a fault** —
each pass picks a narrow whitelist of IR shapes it can prove safe
to rewrite, so what's there is correct, but a substantial chunk of
the lazy-VM literature's standard moves is missing.

The single largest gap, both by code volume and by performance
leverage, is the absence of **two foundational analyses**:

- **Occurrence counting** — per-binder use-counts (dead / once /
  many). GHC's `OccurAnal`. None in v3 today; almost every interesting
  pass downstream needs it.
- **Demand / strictness analysis** beyond syntactic WHNF detection.
  GHC's `DmdAnal` + worker/wrapper. v3's `elimRedundantForce` is a
  one-step approximation; a real demand lattice would generalise it
  across calls and across blocks.

Once these foundations exist, at least four high-leverage passes
unlock: hidden-class shapes for attrsets, selector thunks,
single-entry thunk elision, and a real heuristic inliner. None of
these are speculative — they're what the GHC and V8 literature
converged on three decades ago for the same shapes of code.

Below: the current pipeline mapped in detail, then the literature
filtered to Nix's specific constraints, then ranked recommendations.

---

## 2. Current state

### 2.1 IR-level pipeline

Six passes, fixed order, single forward pass each except DCE (which
fixed-points up to 8 rounds). Pipeline driver in
`opt_const_fold.cc:279–300`:

```
constantFold        // 1. arithmetic/comparison/boolean over literals
commonSubexprElim   // 2. block-local CSE on deterministic ops
elimRedundantForce  // 3. strictness (Force{v} → VarRef{v} when v is WHNF)
inlineTrivialBindings // 4. VarRef alias collapse
fusePrimOpApps      // 5. App-chain over LitPrimOp → PrimOpCall
deadBindingElim     // 6. DCE, fixed-point
```

`NIX_V3_NO_OPT=1` disables the whole pipeline;
`NIX_V3_NO_OPT_STRICT=1` disables pass 3 only. No per-pass disable
harness for the others.

#### Pass-by-pass summary

**1. Constant folding** (`opt_const_fold.cc`, 302 lines).
Block-local. Folds `Add/Sub/Mul/Div/Eq/NEq/Less/Not` over `Lit*`
operands; chases `VarRef` chains within the same block to find
literals. Skips folds that would throw at runtime (div-by-zero,
INT64_MIN/-1, integer overflow checked via `__builtin_*_overflow`).
Skips `ConcatStrings` (string contexts). Sound by construction; never
fires on operations that involve forces or side effects.

**2. CSE** (`opt_cse.cc`, 151 lines). Block-local. Hashes
expressions by `(variant_index, operands)`; second occurrence is
rewritten to `VarRef{firstSeen}`. Whitelist:
`Add/Sub/Mul/Div/Eq/NEq/Less/Not/HasAttr`. Explicitly excluded:
`Force`, `App`, `PrimOpCall`, `AttrSelect`, `AttrSelectDyn`,
`HasAttrDyn`, `And/Or/Impl`, `If/With/Assert`, `Lambda`, `MkThunk`,
attrset/list construction. The exclusions are correct — most have
either observable side effects, throw-position semantics, or
allocation identity that matters.

**3. Strictness / redundant-force elimination**
(`opt_strictness.cc`, 184 lines). Block-local. For each `Force{v}`,
chases `VarRef` chains and checks whether the producer is a member
of a 22-entry "produces WHNF" whitelist (literals, lambdas,
arithmetic, comparison, attrset/list/lambda construction). If so,
rewrites the `Force` to a `VarRef`. Cross-block analysis is the
documented gap.

**4. Inline trivial bindings** (`opt_inline.cc`, 223 lines).
Module-wide. Identifies `Binding{var, VarRef{u}}` aliases,
path-compresses chains, rewrites every operand to point at the
terminal target, drops the alias bindings. **Note:** this is *only*
alias collapse. There is no inlining of literals, small expressions,
or single-use lambdas. A-normal form constraint means operands are
always VarIds.

**5. Primop fusion** (`opt_primop_fuse.cc`, 131 lines). Module-wide.
Two-pass scan: first collects `LitPrimOp` bindings (skipping ones
with `lazyArgs != 0`); second walks App chains and rewrites
saturated chains to `PrimOpCall`. Soundness depends on `lazyArgs`
being correct per primop.

**6. DCE** (`opt_dce.cc`, 164 lines). Module-wide; fixed-point up to
8 rounds. Erases bindings whose VarId has zero references AND whose
RHS is in a "pure" whitelist (literals, VarRef, LitPrimOp, Lambda,
MkThunk, AttrSet, ListExpr). All other bindings (Force, App,
PrimOpCall, AttrSelect, etc.) are preserved unconditionally.
`Function::paramVars` are added to the reference set defensively.

### 2.2 Bytecode-level optimization

There is **no post-emit peephole pass**. All bytecode-level
optimization happens at lower-time / emit-time as deliberate fast
paths.

**Superinstructions** (`bytecode.hh:107–118`, `emit.cc:217–240`):

- `OP_GET_LOCAL_FORCE` — fuses `OP_GET_LOCAL` + `OP_FORCE`.
  Complete coverage of `Force(VarRef)` for locals.
- `OP_GET_UPVALUE_FORCE` — same for free variables / upvalues.
- `OP_TAIL_CALL` — rewrites `OP_CALL` in tail position. Complete for
  the trivial case (last instruction); partial for tail-in-then
  branches when the following `OP_JUMP` doesn't target `codeEnd`.

**Fast-path type-test and list-op opcodes** (`bytecode.hh:204–234`):

- `OP_IS_NULL/BOOL/INT/FLOAT/STRING/PATH/LIST/ATTRS/FUNCTION` —
  inline type predicates. 13 cases.
- `OP_HEAD/TAIL/LENGTH/ELEM_AT` — list selectors emitted directly
  by the lowerer when it recognises the primop call shape.

**Inline cache** (`bytecode.hh:297–308`, `vm.cc:2761–3080`):

- 4-way polymorphic IC for `OP_ATTRS_SELECT`, keyed on
  `(Bindings*, slot)`, round-robin eviction on miss. Hit rate
  reported 80–95 % on stable attrset shapes. Megamorphic miss path
  is inline binary search on sorted entries.

**Selector lambda fast path** (#424; `emit.cc:906–953`,
`vm.cc:1676–1696`). Emit-time pattern detection: if the body is
exactly `OP_GET_LOCAL 0 / OP_ATTRS_SELECT / OP_RETURN`, the
descriptor stores `selectorSym` and dispatch skips frame allocation
entirely. Restricted to arity-1, no formals, no upvalues. Gated
behind `NIX_V3_SELECTOR_LAMBDA=1` for stability.

**Numeric path**: `vm.cc:1082–1198` does multi-case dispatch on
operand types per `OP_ADD`/`OP_SUB`/etc. with `__builtin_expect`
hints toward int-int. There is **no** typed `OP_ADD_II` / `OP_ADD_FF`
opcode — type checks happen at every numeric op.

**Encoding**: fixed 32-bit instructions; top 8 opcode + low 24
operand. Multi-word instructions follow with extra `uint32_t` data.
Constant pools (`addIntConst`, `addFloatConst`, `addStringConst`)
**do not deduplicate** — relies on IR const-folding to prevent most
duplicates. Branches are 24-bit absolute (no short/long
distinction). Dispatch is `switch(op)` with branch hints; computed-
goto is mentioned as future work in `vm.cc:2`.

### 2.3 Analyses computed but under-utilised

`ir.cc:287–456` runs **free-variable analysis** before emit (needed
for FAM closure layout). It is *not* consumed by any IR pass — passes
operate before `computeFreeVars` runs. Each pass independently builds
its own `BlockMap` / `KeyHash` / reference set.

There is no shared:

- Use-def chain
- Occurrence count
- Dominance / postdominance
- Escape analysis
- Cardinality analysis
- Type / shape inference
- Liveness / interval analysis

The redundant-walk cost is asymptotically fine (each pass is O(N) in
bindings), but the **absence** of these analyses is the structural
limit — it's what gates almost every optimization the literature
considers standard.

### 2.4 Soundness posture

Every pass picks a narrow whitelist. Where there's doubt, the
binding is preserved. The result is a correct optimizer that
can't break observable laziness, but that **systematically
under-fires**: most IR shapes are touched by zero passes.

Concrete example: a `let x = a + b in x + 1` where neither `a` nor
`b` is a literal goes through the pipeline unchanged. Const-fold
doesn't fire (operands aren't literals). CSE doesn't fire (no
duplicate). Strictness doesn't fire (no Force). Inline-trivial
doesn't fire (not a VarRef alias). Primop-fuse doesn't fire (no
PrimOp). DCE doesn't fire (the result is used). The optimizer makes
zero changes to perfectly tractable IR.

---

## 3. Literature mapping

The published literature on optimizing lazy functional bytecode VMs
is mature. Filtering to what applies to v3's specific constraints
(no algebraic data types; attrsets instead; lazy elements but
strict-spine lists; per-evaluation eval, not long-running JIT
warmup):

### 3.1 STG / GHC family — directly applicable

- **Demand / strictness analysis with worker/wrapper** (Sergey,
  Vytiniotis, Peyton Jones, TOPLAS 2017; GHC `DmdAnal.hs`).
  *Generalises `elimRedundantForce` across calls.* HIGH.
- **Occurrence analysis** (GHC `OccurAnal.hs`). *Per-binder counts;
  prerequisite for almost everything else.* FOUNDATIONAL.
- **Selector thunks** (Peyton Jones 1987, Marlow & Peyton Jones JFP
  2006). *`let v = a.b.c in …` becomes a thunk that on entry does
  the chained select and self-replaces. Maps directly onto the
  Nix `attrSelect`-of-let pattern.* HIGH.
- **Beta reduction / inlining** with size-cost heuristic (Peyton
  Jones & Marlow, JFP 2002). *v3 has only alias collapse today.*
  HIGH.
- **Full laziness (let-floating outwards) and let-floating inwards**
  (Peyton Jones, Partain, Santos, ICFP 1996). *Hoist let-bindings
  out of lambdas to allocate once per closure-creation rather than
  once per call; sink into branches to avoid dead allocation.*
  MEDIUM.
- **Case-of-case** (Peyton Jones & Santos, SCP 1998). *Maps onto
  `attrSelect (if c then a else b) "x"` — real flake-output pattern.*
  MEDIUM.
- **Eta expansion** (GHC `Arity.hs`). *Uncurried Nix lambdas limit
  the gain; still useful for partial-app chains.* MEDIUM.
- **Term-level CSE across blocks** (GHC `CSE.hs`). *v3 is
  block-local today.* MEDIUM.
- **CPR / Constructed Product Result** (Baker-Finch et al., JFP
  2004). *Functions returning attrsets immediately destructured.*
  MEDIUM.
- **SpecConstr** (Peyton Jones, ICFP 2007). *No ADTs; the analog is
  shape-specialisation, see V8 family below.*
- **Deforestation / shortcut fusion** (Gill et al., FPCA 1993).
  *Nix lists are eager-spine; gain is marginal.* LOW.

### 3.2 V8 / dynamic-language family — directly applicable

- **Hidden classes / shapes for attrsets** (Chambers, Ungar, Lee,
  SELF, OOPSLA 1989; modern V8 docs). *The single biggest unlanded
  structural win for v3. Pairs with the existing PIC.* HIGHEST EV.
- **Polymorphic inline caches** (Hölzle, Chambers, Ungar, ECOOP
  1991). *v3 has 4-way for `OP_ATTRS_SELECT`. Extend to function
  call sites and `with`-lookup.* MEDIUM.
- **Quickening / adaptive specialisation** (Brunthaler, ECOOP 2010;
  CPython 3.11+ "Adaptive Specialising Interpreter", Shannon 2021).
  *Opcode self-modifies after warmup. Generalises the PIC pattern.*
  MEDIUM-HIGH.

### 3.3 Bytecode-interpreter family — mechanical wins

- **Superinstructions** generated systematically (Ertl & Gregg,
  JILP 2003). *v3 has 5 hand-coded; profile-guided generation
  identifies more.* MEDIUM.
- **Direct threading via computed goto** (Bell, CACM 1973; Berndl
  et al., CGO 2005). *5–15 % on the dispatch fraction; mechanical;
  comment in `vm.cc:2` already flags it as planned.* MEDIUM-HIGH.
- **Stack caching** (Ertl, PLDI 1995). *Top-of-stack in registers.
  Wait until superinstructions stabilise.* MEDIUM.
- **Typed numeric opcodes** (`OP_ADD_II`, `OP_ADD_FF`, `OP_EQ_II`).
  *Saves 3 type-check branches per int-int hot loop. ~3–5 % on
  fib-style benchmarks.* MEDIUM.

### 3.4 Lazy-specific — structural wins

- **Single-entry thunks** (Marlow & Peyton Jones, JFP 2006). *Skip
  the update barrier on cardinality-1 thunks. Each Nix `let` binding
  used exactly once is a candidate.* HIGH; depends on cardinality.
- **Lazy black-holing** (Marlow et al., PLDI 2001 / Haskell 2009).
  *Defer the blackhole-install until GC. Single-thread Nix gets
  the write-elision benefit. Deferred until profiling shows
  write-amp matters.* MEDIUM.
- **Cardinality / one-shot analysis** (Sergey et al., POPL 2014).
  *Underpins single-entry thunks.* HIGH.

### 3.5 Skipped (rationale)

- SpecConstr (no ADTs)
- Stream fusion / deforestation (eager-spine lists)
- Trace JIT (PyPy/LuaJIT) — wrong workload shape (attrset-fixpoint
  construction, not loops)
- Full SSA conversion — overkill; use-def map keyed on binder-id is
  enough
- Dominators / loop analysis — no imperative loops in lazy IR
- Blanket lambda lifting — selective only, see GHC literature

---

## 4. Top recommendations

Ordered by leverage × feasibility, with prerequisites called out.

### 4.1 Foundational analyses (must come first)

| Pass | Cost | Unlocks |
|---|---|---|
| **Occurrence analysis** | ~250 LOC, none | Heuristic inliner, single-entry thunks, smarter DCE, demand analysis |
| **Use-def map keyed on binder-id** | ~300 LOC | Cross-block CSE, demand analysis, escape analysis |

These have no direct user-visible perf number on their own. Build
them anyway; almost every entry below depends on at least one of
them.

### 4.2 Top 8 ranked recommendations

| # | Pass | Expected gain | Cost | Prerequisites | Notes |
|---|---|---|---|---|---|
| 1 | **Hidden-class shapes for attrsets** | 10–20 % on nixpkgs | ~600 LOC + runtime | Existing PIC framework | Replaces sorted-name binary-search with shape-indexed offset access. Pairs natively with the 4-way IC. |
| 2 | **Selector thunks** | 5–10 % | ~300 LOC | Occurrence | `let v = pkg.a.b.c in …`-class pattern. v1/v2/v3 all currently lack this; a textbook lazy-eval move. |
| 3 | **Demand analysis + worker/wrapper** | 5–10 % | ~700 LOC + ~400 LOC | Occurrence + use-def | Generalises `elimRedundantForce` across calls and blocks. Eliminates thunk allocation for arguments forced on every path. |
| 4 | **Heuristic inliner** | 3–7 % directly, more via unlocking | ~500 LOC | Occurrence + size cost | Replaces alias-only `inlineTrivialBindings` with a real inliner. Critical: a chain of `let f = \x: …; let g = f a; in …` collapses iteratively, which fires further folds. |
| 5 | **Direct threading via computed goto** | 5–10 % on M-series | ~50–100 LOC | None | Mechanical. The single cheapest "real" perf win on the list. Already in the code as a planned item. |
| 6 | **Single-entry thunk elision** | 2–4 % | ~150 LOC | Cardinality (from occurrence) | Skip the update barrier on cardinality-1 thunks. |
| 7 | **Quickening for the PIC pattern** | 3–5 % | ~300 LOC | Hidden classes | Opcode self-rewrites to a monomorphic variant after the first hit. |
| 8 | **Typed numeric opcodes** (`OP_ADD_II`, etc.) | 3–5 % on numeric loops | ~200 LOC + new opcodes | None | Saves 3 branches per arithmetic op. Independent of IR work. |

Cumulative expected impact on cardano-node-class workloads if all 8
land: **25–40 %**, with the heaviest single contribution from
hidden classes (#1).

### 4.3 Secondary wins (smaller but cheap)

- **Cross-block CSE** (~150 LOC, MEDIUM gain on lib patterns).
- **Selective let-floating** (in + out) (~400 LOC, depends on
  occurrence, MEDIUM gain).
- **Multi-arg selector lambda extension** (~150 LOC; current #424
  is arity-1 only; extend to `\x y: y.f`-class patterns).
- **`OP_REC_BINDING_SLOT_REF` + `OP_FORCE` peephole fusion** (~50
  LOC; small).
- **`OP_LIT_INT_RANGE` for small ints 0–255** (~100 LOC; ~0.5–1 %
  code-size).
- **Constant pool deduplication** (~50 LOC; small).
- **AttrSelect chain fusion** to `OP_ATTRS_SELECT_PATH` (~200 LOC;
  ~1 % on `pkgs.haskell.compiler.ghc98.foo`-class chains).

### 4.4 Don't (yet)

- Blanket lambda lifting (selective only after measurement).
- Stream-fusion / deforestation (wrong list semantics).
- Full SSA (overkill; use-def map suffices).
- Trace JIT (wrong workload).
- Lazy black-holing (current eager+revert is fine until write-amp
  measured).

---

## 5. Strategic notes

**The compounding pair.** Occurrence analysis + demand analysis
together are the structural bottleneck. Almost every other
recommendation either requires one of them or becomes much
safer/cheaper with them. Build these first even though they have no
direct perf number.

**The shape pair.** Hidden classes (#1) + quickening (#7) together
convert the dynamic per-site lookup pattern into near-static
dispatch. This is where v3 can credibly beat both v1 and v2 — the
existing 4-way PIC is good infrastructure, but pointer-identity
keying limits it. Shapes give it polymorphic reach. Quickening
removes the dispatch-time check on warm sites entirely.

**The mechanical pair.** Direct threading (#5) and typed numeric
opcodes (#8) are zero-risk, no-prerequisite wins. They have nothing
to do with the architectural moves. Both should land in any sprint
that touches `vm.cc`.

**`with`-scope hazard.** Every static analysis has to handle `with`
specifically — it shadows identifiers at runtime in ways that defeat
naïve free-variable / occurrence analysis. The existing CSE handles
this by simply not crossing `with` boundaries; reuse the same
pattern in occurrence and demand analysis. Cheapest: bail to dynamic
at any `with`-shadowing site.

**Soundness floor.** The current pipeline's narrow whitelists are
fundamentally correct. The recommendations above each preserve
laziness explicitly:

- Selector thunks preserve sharing (the GHC RTS short-circuit story).
- Demand analysis only marks bindings strict if they're forced on
  every path — never speculative.
- Single-entry elision applies only when cardinality is provably 1,
  not just statically once.
- Quickening keeps the slow path; it just adds a fast branch.

There is no recommendation in this report that requires changing
the laziness contract. Every win is a refinement, not a
sacrifice.

---

## 6. Concrete next sprint

Two-week proposal:

1. **Occurrence analysis pass** (~3 days). New file `opt_occur.cc`,
   inserted between current passes 4 (inline) and 5 (primop-fuse).
   Annotates each `Binding` with `OccInfo {dead, once, once_unsafe,
   many}`. Used by passes 6 (DCE smarter) and any future inliner.
2. **Direct threading** (~1 day). `#ifdef __GNUC__`,
   `&&label_OP_X` table, replace the switch in `vm.cc:934`. Gated
   on compiler support; falls back to current switch.
3. **Typed `OP_ADD_II / OP_SUB_II / OP_MUL_II / OP_EQ_II`** (~1
   day). New opcodes in `bytecode.hh`; lowerer emits them when
   IR-level type-inference tags both operands as int-only; runtime
   handlers ~10 lines each.
4. **Selector thunk lowering** (~3 days). Detect `let v = e.x.y.z
   in …` at lower-time when `v` has occurrence ≤ many and `e` is
   a non-literal. Emit a `MkSelectorThunk` IR node that lowers to
   a dedicated `OP_MAKE_SELECTOR_THUNK`. Runtime forces `e`,
   walks the path, self-replaces.

After this sprint: foundational analysis available, ~10–15 %
mechanical perf win, selector thunks as the first non-trivial
lazy-eval addition. Hidden classes (the biggest single win) becomes
the headline feature for the *following* sprint, with occurrence
analysis already in place to support its shape-transition tracking.
