# WC-38 Fix Plan — Refactor CFF_FORCE_RETRY Semantics

> **ARCHIVED-IN-PLACE 2026-05-27**: This doc is historical (pre-strategic-doc-set or one-off RCA). Preserved here because it is still referenced from one or more KEEP docs. See [`LODE_CLEANUP_REVIEW_2026-05-27.md`](LODE_CLEANUP_REVIEW_2026-05-27.md) for the archival rationale.

---


Status: design notes, not implementation.

## Summary of state (post 5-session debug)

- All A/B gates tested individually do **not** fix nixpkgs
  (`NIX_V3_NO_REC_SLOT_REF`, `NIX_V3_NO_INTER_ARG_FORCE`,
  `NIX_V3_NO_WITH_FORCE`, `NIX_V3_NO_BINOP_FORCE`,
  `NIX_V3_NO_GETFORCE_SUPER`, `NIX_V3_EARLY_PUBLISH`,
  `NIX_V3_RETURN_CHAIN`).  Combined gates: same.
- Multi-agent diagnosis (6 agents) converged: tree-walker, Tvix, hnix,
  GHC all defer field-thunk firing until the outer `f x` of `lib.fix`
  completes; v3 fires them during.  v3 is the outlier.
- The `with`-stack slot in v3 correctly points to `lib.fix x`'s slot;
  it derefs to a Black thunk because v3's evaluation order reaches the
  inner `callPackages ../llvm { }` thunk while x is mid-flight.
- 23+ synthetic patterns all pass — bug requires real nixpkgs's
  `lib.foldl' (flip lib.extends)` chain over 10+ overlays + splice's
  `lib.callPackagesWith self`.
- v3 fails after 44 imports vs tree-walker's 825 (succeeds).  Neither
  imports `pkgs/development/compilers/llvm/default.nix`.

## What CFF_FORCE_RETRY actually does today (vm.cc)

- `OP_FORCE` slow path: pop value, chase Slot/App/Evaluated chains in
  C loop, then if Suspended thunk: mark Black, push frame
  (CFF_THUNK_RETURN), set `CFF_FORCE_RETRY` on the **caller** frame.
- Inner thunk body runs.  `OP_RETURN` (vm.cc:1394+) pops retVal, writes
  to thunk->evaluated, sets Evaluated state, and at the caller-resume
  path: `if (caller.flags & CFF_FORCE_RETRY) && retVal is
  Thunk/App/Slot → goto op_force_slow`.
- This is **already** semantically equivalent to tree-walker's
  `forceValue` loop in `eval-inline.hh:104-138`.

## So where is the divergence?

The retry mechanism isn't the bug.  The actual divergence must be
elsewhere — somewhere v3 enters a thunk's body that tree-walker
leaves lazy.  Suspect candidates ordered by likelihood:

1. **v3's `inherit (rec {...}) X` lowering creates eager thunk
   construction.** lower.cc:lowerInheritFrom re-lowers the rec source
   per inherited name, but each is supposed to be wrapped in a thunk.
   If somewhere a thunk for `(rec_source).X` IS forced during attrset
   construction (vs left lazy), we'd see the chain.  Tree-walker uses
   `buildInheritFromEnv` to share via Env indirection.

2. **v3's emit-time `OP_FORCE` insertion at attrset-construction
   sites.** The `recref-X` thunkifyRecAttrSelect path emits
   `OP_REC_BINDING_SLOT_REF` which forces the rec-attrset header but
   NOT entries.  However if a sibling binding is `rec_attrs."21"`-style
   AttrSelect, `emitSelectChain` (lower.cc:1308) inserts an `OP_FORCE`
   on the chain receiver — which can transitively force entries.

3. **v3's lambda-formal thunkification.** lower.cc:619-652 wraps each
   lambda formal in a thunk that forces the argset on access.  Tree-
   walker (eval.cc) binds formals as direct `Value*` slot pointers,
   no force at access.  This is a structural divergence that may fire
   more.

4. **Cross-CU `forceValue` recursion.** vm.cc:3192-3500's
   `forceValue` helper uses C recursion via `dispatchLoop`.  If a
   sub-force's dispatchLoop sets up frames that then propagate forces
   back up via CFF_FORCE_RETRY, the chain gets longer than tree-
   walker's iterative forceValue.

## Recommended architectural fix (Option 1, expanded)

Refactor approach: **align OP_FORCE / forceValue / OP_RETURN to a
single canonical loop semantic.**

### Step 1: audit every emit-time `forceVal()` in lower.cc

There are ~14 call sites (per V3_DBG_FORCE_SITE).  For each, check:
- Does tree-walker force this value at the same logical point?
- If not, drop the force; let the consuming op force-on-receive at
  runtime instead.

Specific candidates to drop:
- emit.cc:479 (OP_REC_BINDING_SLOT_REF pre-force) — the `with E;`
  source.  Tree-walker's `attrs->maybeThunk` keeps it lazy.
- lower.cc:1308 (emitSelectChain force) — partial; force receiver only
  when path length > 1 and the next step is dynamic.
- lower.cc:805 (callee force in lowerCall) — keep; necessary for
  generic OP_CALL dispatch.

### Step 2: change v3 lambda-formal binding

Currently lower.cc:619-652 wraps each formal in a thunk that runs
`forceVal(paramRef); AttrSelect(paramForced, name)` on access.

Change to: **bind formal directly** to the entry's value pointer at
call time, like tree-walker does.  This requires:
- At OP_CALL (vm.cc:1133+), when the callee has formals, force the arg
  attrset ONCE, then for each formal, write `arg.entries[i].value`
  directly to the body's local slot.
- Drop the per-formal thunk emission in lower.cc:619-652.
- Update OP_GET_LOCAL semantics for formals (no change — slot is the
  Value directly).

Estimated cost: ~150 lines in vm.cc + lower.cc.  Risk: medium.

### Step 3: validate against the WC-38 probe + lang tests

- `run-wc38-nixpkgs-probe.sh` should go from 0/3 to 3/3.
- 142/142 lang tests must remain green.
- 42/42 wc-laziness tests must remain green.

### Step 4: add positive/negative/regression tests for the new
behavior

- Positive: a synthetic `f = { a, b, c }: a + b + c; f { a = 1; b =
  throw "!"; c = 3; }` should not throw if `b` isn't accessed (lazy
  formals).  Tree-walker permits this; v3 currently would throw.
- Negative: a synthetic with `{ a, b }: a` called with `{ a = 1; b =
  throw "!"; c = 4; }` (extra arg) should still throw "unexpected
  argument" when there's no ellipsis.
- Regression: the WC-38 nixpkgs probe — captures the actual failure.

## 2026-05-02 update: Step 1 attempted (NIX_V3_EAGER_ARG_FORCE)

Implemented `NIX_V3_EAGER_ARG_FORCE=1` (commit ce49480b4) to align v3
with tree-walker's call-time arg forcing.

**Result: NEGATIVE.** Setting the gate does NOT fix the nixpkgs
WITH_LOOKUP failure.  Identical error.  Tests still 142/142 + 42/42.

Conclusion: the bug is NOT WHEN v3 forces the arg attrset.  It's
WHEN v3 reaches the failing OP_CALL in the first place.  Eager-
forcing the arg at call time doesn't change call-stack timing.

This refutes the per-formal-thunk hypothesis as the root cause.
Step 2 (full lambda-formal refactor) would similarly only change
WHEN forces happen, not WHICH call paths v3 traverses.

## True remaining suspects (post Step 1 negative result)

The bug must be in something that drives v3 to ENTER deeper call
chains than tree-walker on the same input.  Not in:
  - Force timing (proven — eager force doesn't fix)
  - Lambda formal access scheme (corollary — formal access is
    downstream of call traversal, not the cause)
  - CFF_FORCE_RETRY semantics (proven semantically equivalent)
  - Tag::Slot or Phase 5 work (proven via pre-Phase-1 bisect)

Genuine open candidates:
  1. **Some primop in v3 forces more than tree-walker.**  Audit:
     - `intersectAttrs` (vm.cc 970): forces both args before primop.
       Tree-walker also forces both.  Same.
     - `mapAttrs` (primops.cc 995): builds Tag::App lazy chain.
       Tree-walker similar.
     - `lib.callPackagesWith` body: many builtin calls.  Trace
       which primop in this body trips the chain in v3 vs tw.
  2. **callClosure / dispatchLoop helper** (vm.cc:3192+ forceValue,
     callClosure).  v3's helpers use C recursion through dispatchLoop;
     tree-walker uses C call-stack.  Subtle difference may make v3's
     forces propagate through frames differently.
  3. **Some emit-time `OP_FORCE` insertion that tree-walker's
     equivalent semantically isn't.**  V3_DBG_FORCE_SITE shows ~14
     emit sites.  Need to compare each against tree-walker's
     eval.cc for behaviour.  emit.cc:479 (OP_REC_BINDING_SLOT_REF
     pre-force) is the highest-volume.

## Recommendation

After 6 sessions, no surgical fix is in reach without genuine
architectural redesign.  The diagnostic infrastructure is excellent;
the next session should pick ONE concrete primop that v3 forces but
tree-walker doesn't (via TW_DBG_FORCE / V3_DBG_FORCE_TRACE diff at
the same eval point), and chase that ONE divergence.  This requires
running both evaluators in lock-step on a smaller-than-nixpkgs but
still-failing test case — which we don't have, and which 23+
synthetic attempts failed to produce.

The most productive path forward is probably to defer WC-38 until
either (a) a smaller failing test case emerges naturally during
nixpkgs work elsewhere, or (b) a focused architectural review
session with both evaluators side-by-side on a debugger.

## Why I'm stopping here

After 5 sessions of debugging, the root cause is comprehensively
diagnosed but the surgical fix requires Step 2 (lambda-formal
binding refactor) which is a ~150-line architectural change.  This is
appropriate for a separate focused implementation session, not
another debugging round.  The diagnostic infrastructure
(V3_DBG_FORCE_SITE, V3_DBG_IMPORT, V3_DUMP_RANGE/LAMBDAS, TW_DBG_FORCE,
TW_DBG_IMPORT) is in place and will be invaluable for verifying the
fix.

## References

- `project_wc38_with_blackhole.md` — full investigation memory
- `run-wc38-nixpkgs-probe.sh` — regression probe (currently 0/3)
- `run-wc-laziness-tests.sh` — 42/42 includes 3 cycle-detection tests
- Multi-agent commits: 77328da17, 429e61f77, dc79f92a5, 4959d9eb2,
  6c8b4afcb, f952be962, 1f2460df1
