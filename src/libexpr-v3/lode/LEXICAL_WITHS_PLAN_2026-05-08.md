# Lexical-With Chain — Design Doc

Date: 2026-05-08
Related: #528 (self-dot inherit-from), #529 (with-from inherit-from), INVERSION_PLAN

## What's wrong with the current approach

v3 today captures `with`-scopes for closures and thunks via
`snapshotCurrentWiths(vm)` at make-time:

```cpp
size_t base = vm.frames.back().withStackBase;
size_t top  = vm.withStack.size();
return withStack[base .. top];
```

This is a *runtime* snapshot of the current frame's slice of the
flat with-stack.  In principle it matches lexical scope — each frame's
slice is the lexical with-chain visible at that frame.  But it has
several failure modes:

1. **The snapshot is value-only, not source-of-truth.**  At snapshot
   time the value of each `with`-target may be a Tag::Slot pointing
   at a heap cell that's mid-construction (a let-rec entry whose
   thunk is still Black, or an extends-chain `prev`).  When the
   captured-with is later forced for OP_WITH_LOOKUP, the slot may
   have been *cell-updated* by some unrelated thunk's OP_RETURN to
   a value that resembles the source-shape but isn't what TW would
   look up against.

2. **Tail-call resets withStackBase.**  `OP_TAIL_CALL` pops the
   current frame's local with-pushes and resets its withStackBase to
   the new top before pushing the callee's captured-withs.  A thunk
   created in tail-call body sees only callee captured-withs — by
   construction correct, but very fragile if any code path captures
   pre-tail-call withs into a closure that then escapes.

3. **The snapshot walks no AST.**  v3 stores the captured slice as
   opaque values; nothing connects it back to which `ExprWith` each
   came from.  If a future bug mismatches lexical structure with
   runtime push-order, we have no static check.

## TW's approach and what we want to mirror

TW threads `prevWith` as a member of every `Env`.  An `ExprWith` ast
node carries `prevWith` pointing at the next outer `ExprWith` (set
at parse time via `bindVars`).  Closures hold an Env directly, so
the prevWith chain rides along with the closure's lexical scope.

Lookups for unbound names walk Env's prevWith chain at runtime.
Forcing a `with`-target happens lazily inside the lookup (TW's
`forceAttrs` on the `ExprWith.attrs` value).

The *static structure* — which ExprWiths are visible to which Expr
— is fully determined at parse/lower time.  v3 currently *does not
materialise* this static structure into the IR; it relies on
runtime-stack-discipline to reproduce it.

## What we will build (B + C)

A per-thunk and per-closure **lexical-with chain** that v3 computes
at lower time and materialises at make-time.  Concretely:

* `ir::MkThunk` and `ir::MakeClosure` get a new field
  `lexicalWiths: std::vector<ir::VarId>`.  Each VarId resolves to
  the IR binding for one enclosing `ExprWith`'s `attrs` expression.
  Order: outermost-first (root of file) to innermost-last.

* The lowerer maintains a stack of "currently-active With scopes"
  with each scope holding the IR VarId of the with-target.  When an
  expression is thunkified, we copy the chain into the IR node.

* `emit.cc` emits, before `OP_MAKE_THUNK` / `OP_MAKE_CLOSURE`:
    for varId in lexicalWiths: emitVarRef(varId)
  followed by the existing freeVars push and the make instruction.
  The opcode encodes `(nUpvalues, nWiths)`.

* `LambdaDescriptor` gets a `nWithTargets` field (uint16) so
  runtime can split.

* `vm.cc`'s OP_MAKE_THUNK / OP_MAKE_CLOSURE handlers consume both:
  the top `nWithTargets` entries fill `capturedWiths`; the next
  `nUpvalues` fill the upvalue tail (or vice-versa, whichever
  matches existing emit order).

* `snapshotCurrentWiths` is *removed from the make-path*.  It stays
  as a fallback for cases we can't statically determine (e.g. some
  primop bridges).  The top-of-call entry (`run`) keeps its
  `withStackBase=0` initialiser unchanged.

## Why this fixes the bug class

The runtime with-stack is reduced from "source of truth" to
"performance accelerator for the contiguous lookup walk in OP_WITH_LOOKUP."
Closure-creation no longer depends on it being correct.  The
captured-withs of any closure or thunk are now exactly the
lexical chain, materialised value-by-value at make-time.

If the value-at-make-time is itself wrong (slot points at the
wrong cell), that's a separate slot-aliasing bug — but at least the
*shape* of the captured chain is correct, which is testable
independently.

## Step-by-step implementation

### Step 1: Tests up front (positive + negative + regression)

Write `run-lexical-withs-tests.sh` covering:
- positive: closure created inside `with X;`, called outside, sees X.
- positive: thunk created inside `with X;`, forced outside, sees X.
- positive: nested withs — closure inside `with A; with B;` sees both.
- positive: closure inside `with A;` returns another closure, the
  inner is later called and STILL sees A.
- negative: closure created OUTSIDE `with X;`, called INSIDE, does
  NOT see X (lexical-not-dynamic).
- negative: a closure that captures `with X;` doesn't accidentally
  see Y from a sibling/unrelated call frame.
- regression: existing self-dot tests, lang tests, parity, on-demand-root.

### Step 2: Add `Scope::Kind::With` push tracking with-target VarId

Verify (or add) Scope::Kind::With push in `lowerWith`.  Each entry
holds the lower-time `withTargetVarId`.

### Step 3: Add `lexicalWiths` to MkThunk + MakeClosure IR nodes

Plain `std::vector<ir::VarId>` field, populated at thunkify / makeClosure.

### Step 4: Populate `lexicalWiths` at thunkify / makeClosure time

Walk the lower-time scopes stack, collect WithVar entries in order.

### Step 5: Emit pushes the with-targets before make

Before pushing freeVars, emit `emitVarRef(withVar)` for each
lexicalWith.  Adjust opcode operand or data-words to encode count.

### Step 6: Runtime consumes from stack

OP_MAKE_THUNK / OP_MAKE_CLOSURE pop `nWithTargets + nUpvalues`
values.  Build `capturedWiths` ListVec from the with-targets;
build upvalue tail from the upvalues.

### Step 7: Bytecode + disk-cache schema bump

Bump `kSchemaVersion` so old caches don't deserialise into the new
shape.

### Step 8: Verify

Run the full suite.  All green or document failures for
follow-up.

## Risks / Open questions

* **disk-cache invalidation.**  The schema bump invalidates any
  pre-existing v3 disk-cache entries.  Acceptable; the daemon will
  re-lower next eval.

* **closure capture order.**  Need to be careful that
  `lexicalWiths` are pushed BEFORE / AFTER upvalues consistently
  with what the runtime expects.

* **synthetic thunks.**  The lowerer creates synthetic thunks for
  thunkifyForAttr, thunkifyRecAttrSelect, hidden inherit-from
  thunks, etc.  Each synthetic thunk creation site needs to
  populate `lexicalWiths` correctly.  Search for `m.functions.emplace_back()`
  uses in lower.cc.

* **TAIL_CALL interaction.**  Tail-call resets withStackBase.  Our
  approach captures lexical with-targets at make-time, not via
  the runtime stack; so tail-call no longer matters for closure
  creation.  But OP_WITH_LOOKUP still walks the runtime stack —
  ensuring it stays correct under tail-call is a separate concern
  (already the existing semantics).

## Implementation status — 2026-05-08

LANDED (commit `af8715433`):

* `Scope::Kind::With` push with `withTargetVar` records the lowered
  attrs VarId for each enclosing `with`.
* `ir::MkThunk`, `ir::Lambda`, `ir::LetRec::Entry`,
  `ir::LetRec::HiddenEntry` carry `lexicalWiths: vector<VarId>`.
* `ir::Function::nWithTargets` and `LambdaDescriptor::nWithTargets`
  carry the count to runtime / disk cache.
* Lowerer's `collectLexicalWiths()` populates the chain at every
  thunkify / make-closure site.  Per-entry letrec thunks share the
  chain captured ONCE at the LetRec construction site (matches
  lexical position).
* `computeFreeVars`'s `collectExprRefs` adds `lexicalWiths` to the
  binding's outer-block refs so they propagate up to the maker
  function's freeVars when crossing frame boundaries.
* `emit.cc` pushes `lexicalWiths` BEFORE `freeVars`; OP_MAKE_CLOSURE /
  OP_MAKE_THUNK carry TWO data words now (`nUpvalues` then
  `nWithTargets`).
* `vm.cc` OP_MAKE_CLOSURE / OP_MAKE_THUNK pop upvalues into
  `upvalues[]`/`tail[]` first (top of stack), then with-targets into
  `capturedWiths` ListVec.  Falls back to `snapshotCurrentWiths` only
  when `nWithTargets == 0` (synthetic call paths that didn't go
  through the lowerer).
* `serialize.cc` schema bumped 4→5; `disasm.cc` and vm diagnostic
  walkers updated for the new 3-word encoding.
* `opt_inline.cc` rewriteVar honours `lexicalWiths`.

Tests (all green except KNOWN-FAIL flips that should be tightened):

* `run-lexical-withs-tests.sh` 15/15 — positive + negative + regression
  including formals-with-pkgs and curried with-escape.
* `run-self-dot-thunkify-tests.sh` 11/11.
* `run-direct-eval-tests.sh` 26/26.
* `run-lang-tests.sh` 142/142.
* `run-cutover-parity-tests.sh` 140/142 (pre-existing).
* `run-on-demand-root-tests.sh` 142×4 / 142×4 (all four modes).
* `run-broader-thunkify-tests.sh` 4/4, `run-456-chase-cycle-tests.sh`
  7/7, `run-evalscope-tests.sh` 4/4, `run-disk-cache-tests.sh` 5/5,
  `run-bridge-attr-lookup-tests.sh` 21/21,
  `run-bridge-thunk-after-force-tests.sh` 8/8,
  `run-bridge-stack-uaf-tests.sh` 6/6,
  `run-lazy-bridge-arg-tests.sh` 9/9,
  `run-bridge1-shortcut-tests.sh` 17/17,
  `run-tw-lambda-bridge-tests.sh` 12/12,
  `run-mutual-circular-formals-tests.sh` 10/10,
  `run-wc-laziness-tests.sh` 85/85,
  `run-fix-inherit-from-self-tests.sh` 5/5,
  `run-gate-removal-tests.sh` 35/35.
* `run-458-rec-slot-capture-tests.sh` 18/20 (same 2 nixpkgs#hello.name
  fails as pre-existing).

KNOWN-FAIL → flipped-to-pass (tests asserting the failure mode are
now stale; assertions need updating):

* `run-intrinsic-recognition-tests.sh` d3b default: full lib.fix +
  extends now returns 11 without `NIX_V3_SELF_DOT_MAX_LEVEL=2`
  workaround.  The lexical chain materialises the fix-point's `self`
  with-targets correctly at level=1.
* `run-inherit-from-laziness-tests.sh` p2 v3-skip: v3 owns the
  evaluation cleanly without TW fallback (`runThrew=0`); the post-#496
  fallback is no longer needed under the lexical chain.
* `run-on-demand-root-shapes.sh` #455 NEGATIVE-OF-POSITIVE:
  `NIX_V3_NO_CALL_HOOK_EAGER=1` no longer trips the auto-eager guard.

RESOLVED post-landing diagnostic (commit `298870af2`):

* `OP_GET_UPVALUE: no closure context` on `(import <nixpkgs>
  {}).lib.id 5` was caused by `opt_inline.cc rewriteVar` not walking
  the new `lexicalWiths` vectors on `LetRec::Entry` /
  `LetRec::HiddenEntry`.  The trivial-binding inliner aliased some
  VarIds, but the LetRec entries' `lexicalWiths` retained pre-alias
  VarIds, which then leaked into fn[0]'s freeVars during
  computeFreeVars (the stale VarId pointed at a now-dead binding
  whose only "user" was the un-rewritten `lexicalWiths`).  Lambda /
  MkThunk lexicalWiths were already rewritten in the main #530
  commit.

  After the fix, `(import <nixpkgs> {}).lib.id 5` reverts to the
  PRE-existing `OP_WITH_LOOKUP: name 'callPackage' not found in
  with-scope` failure documented under
  project_493_step3d_with_stack — proving the lexical chain change
  is fully transparent, and the remaining failure is unchanged from
  before this branch.

REMAINING (unchanged from before #530):

* `OP_WITH_LOOKUP: name 'callPackage' not found in with-scope` —
  documented in project_493_step3d_with_stack memory.  Likely a
  with-stack carriage gap through bridged TW thunks in the by-name-
  overlay machinery.  Independent of the lexical-with chain.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
