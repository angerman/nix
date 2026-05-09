# v3-direct nixpkgs RCA progress (#548) — 2026-05-09

## Constraint reminder

This work is bound by `V3_NATIVE_CONSTRAINT_2026-05-09.md`: TW is
permitted only at FFI leaves.  Any candidate fix that introduces TW
into the eval path is rejected.

## Findings so far

### 1. The cycle path (validated via V3_DBG_FORCE_INSIDE_X)

```
[0]   <thunk>@2012   forced=overlays   — root thunk forces nixpkgs lib formals
[1-11] final@148      forced=prev       — extends layers cascade (5 levels)
[12]  super@564     forced=res        — allPackages's super: lambda body forces
                                         res (= all-packages.nix evaluation)
[13]  super@997     forced=adjacentPackages — stage.nix formal arg
[14]  list@3205     forced=<thunk>    — lib.lists.foldl' or similar
[15]  end@3257      forced=<thunk>    — lib's end-named lambda iteration
... 700+ Evaluated-thunk no-op forces in the `end` iteration ...
[768] attrs@3542    forced=<thunk>@39086 — recurseIntoAttrs lambda forces its
                                           `attrs` parameter (via OP_GET_LOCAL_FORCE 0)
                                           → THE failing thunk
                                           → OP_WITH_LOOKUP callPackage
                                           → cycle.
```

The failing thunk's body:
```
[39086] OP_WITH_LOOKUP callPackage
[39087] OP_LIT_PATH    17
[39088] OP_CALL
[39089] OP_SET_LOCAL   2
```

So `callPackage <path-17>`.  Path 17 is some specific package path
in nixpkgs.

### 2. Frame chain at cycle (validated via V3_DBG_WITH_CYCLE)

```
[81] call  final@148       — toFix's outer lambda
[82-86] thunk final@148    — extends's prev thunks (5 levels deep)
[87] call  super@395       — allPackages's super: lambda
[88] thunk <thunk>@546     — anonymous thunk
[89] thunk conflictingAttrs@524
[90] thunk super@140       — *** UNEXPECTED ***
[91] call  attrs@3542      — recurseIntoAttrs
[92] thunk <thunk>@39086   — failing thunk
```

### 3. The mystery: thunk at codeOff=140 named "super"

Frame 90 is a THUNK frame (CFF_THUNK_RETURN flag), descriptor name
"super", codeOffset=140.  Bytecode at 140:

```
[140] OP_GET_UPVALUE 3       — get upvalue 3 (== pkgs)
[141] OP_WITH_PUSH 0         — push pkgs onto with-stack
[142] OP_GET_UPVALUE 3       — get upvalue 3 again
[143] OP_MAKE_THUNK 15 data=[0,1]   — make entry-thunk #1 (with-target = 1)
... continues with many OP_MAKE_THUNK + OP_SET_LOCAL pairs ...
```

This is *exactly* the body shape of `super: with pkgs; { entry1=...,
entry2=..., }` — i.e., the inner curried lambda of all-packages.nix
or a similar overlay.

**But:** OP_MAKE_THUNK only sets `t->suspended.desc =
&cu->lambdas[funcIdx]` at vm.cc:1782 (the only writer).  Adding a
diagnostic that fires when OP_MAKE_THUNK creates a thunk with
`desc->name == "super"` produced **zero log entries** on the failing
run.  So OP_MAKE_THUNK is NEVER called with funcIdx pointing at a
"super"-named function.

Yet at cycle time, frame 90's thunk has desc->name=="super".

Hypotheses to investigate next session:

- **(H1)** Two LambdaDescriptors share the same memory.  e.g., a
  thunk fid's descriptor location is being written over by a
  lambda fid's descriptor population.  Check emit.cc's
  `unit.lambdas.resize(fid + 1)` (emit.cc:1252) and the population
  code around it.

- **(H2)** The descriptor IS for the all-packages.nix `super:`
  lambda body, and OP_MAKE_THUNK is being called with that lambda's
  fid via some path I haven't yet found.  My diagnostic only fires
  in the OP_MAKE_THUNK handler — there might be ANOTHER path that
  creates a thunk with this descriptor (e.g., a primop that wraps
  an Expr* into a thunk via another mechanism, like
  `treeWalkerToV3` → allocBridgeThunk?  But Bridge thunks don't
  set suspended.desc).

- **(H3)** The frame chain printer reads the descriptor through a
  union variant that's been zeroed/garbaged.  At state=Blackhole,
  the suspended union variant is technically inactive (the original
  Suspended-state desc is stale).  But OP_FORCE flips state in
  place without overwriting the union, so reads should be correct.
  Worth ASAN-checking the cycle path for UB.

### 4. Synthetic reproducers (negative results)

- `repro_step1.nix`: minimal extends + assert + with-pkgs. WORKS.
- `repro_step2.nix`: + recurseIntoAttrs entries.            WORKS.
- `repro_step3.nix`: + 50 mapAttrs-generated entries.       WORKS.
- `repro_step4.nix`: + inherit-from clauses.                WORKS.
- `test_letres.nix`: + stage.nix `let res = ... res self super` shape. WORKS.

None reproduce the cycle.  Full nixpkgs has some structure these
synthetics don't capture — likely depth + cross-CU import + lambda-
skip-eligible formals interactions.

### 5. Diagnostics committed

- `V3_DBG_FORCE_INSIDE_X`: log every Suspended-thunk force inside
  lib.fix x_thunk's lifetime.  Capped at 2000 entries.
- `V3_DBG_WITH_CYCLE`: dump frame chain + bytecode at OP_WITH_LOOKUP
  cycle throw.  Now also dumps body-start of "super"-named thunks
  on the chain.
- `V3_DBG_MK_THUNK_SUPER`: log every OP_MAKE_THUNK creating a
  "super"-named thunk.  CURRENTLY DOESN'T FIRE on the failing run
  — that's the hypothesis (H2)/(H1) starting point.

## Recommendation for next session

**Don't** continue trying to identify the unexpected thunk-vs-lambda
descriptor confusion via grep + reasoning.  **Do** instrument
emit.cc's `unit.lambdas.resize(fid + 1)` block and the descriptor
population to confirm (H1) — i.e., that no two functions share a
descriptor slot.  If they don't share, then (H2) is the answer and
we need to find the other thunk-creation path.

Once the descriptor mystery is resolved, the eval-order divergence
becomes diagnoseable: knowing WHICH function the cycle's thunk is,
we can trace its creation site in lower.cc and determine why v3
forces it (and TW doesn't).

## Update — afternoon 2026-05-09

### Resolved: H1/H2/H3 were ALL wrong — display artifact

After adding `V3_DBG_TRACE_THUNK_X` (records each thunk's
creation-time `desc` pointer / codeOff / name) and consulting it
at cycle-throw, the descPtr **matched** at every frame.  No
descriptor mutation, no union UB, no descriptor-table aliasing.

The "ip < codeOff" puzzle was a **display artifact** of
`OP_TAIL_CALL` (vm.cc:3176) which retargets `cur.cu`,
`cur.closure`, `cur.ip` in place but leaves `cur.thunk->suspended.desc`
pointing at the original thunk-body lambda.  Frame[87] was the 'res'
thunk (codeOff=431 in lib's CU) but its body had tail-called into
`super:` (codeOff=140 in all-packages.nix's CU); the frame's
*executing* desc ≠ the *thunk's* desc.

Cycle-dump diagnostic now prints both: `thunk-name='res' codeOff=431
EXEC=super codeOff=140` so future investigators don't repeat this.

### Real root cause — found

The eager-force site is in `super:` body around PC=219..222:

```
OP_GET_LOCAL 17        ; rec-binding[289] (a let-bound function)
OP_GET_LOCAL 18        ; thunk arg
OP_CALL                ; fn(arg) — EAGER
OP_SET_LOCAL 19
```

This is an `inherit (E) Y` clause where E is a **curried call** like
`callPackagesWith pkgs ./path { args }`.  AST shape:

```
ExprCall {
  fun = ExprCall {
    fun = ExprCall { fun = ExprVar(callPackagesWith), arg = pkgs },
    arg = ./path
  },
  arg = { args }
}
```

The lowerer's `pushInheritFromCache` thunkifies from-exprs only
when the from-expr's `c->fun` is a Var (immediate).  For curried
calls, the outer ExprCall's `fun` is itself an ExprCall, so the
rule misses them and the from-expr is lowered eagerly into the
parent scope's bytecode — exactly the OP_CALL we see.

### Fix landed

`lower.cc::isComplexFromExpr`: walk the `c->fun` chain to find the
rooted Var, regardless of `fromWith`.  Up to 8 hops.  Tested:

  - `run-direct-eval-tests.sh` — 26/26 pass
  - `run-lang-tests.sh` — 142/142 pass
  - `run-self-dot-thunkify-tests.sh` — 11/11 pass
  - `run-cutover-parity-tests.sh` — 142/142 parity (TW vs v3-direct)

Cycle on `(import nixpkgs {}).hello.name` advances:

  - Before: `cycle while resolving 'callPackage'` at frame depth 90
  - After:  `cycle while resolving 'texlive'` at frame depth 88

### Resolved: 'texlive' cycle (ExprSelect from-expr)

The texlive cycle was driven by `inherit (lib.systems) X Y` —
the from-expr is a simple `ExprSelect(Var(lib), systems)`.  Same
class of bug as the curried-call gap: TW's
`from->maybeThunk(state, up)` thunkifies unconditionally; v3's
`isComplexFromExpr` only matched ExprCall and ExprOpUpdate.

Fix: extend rule to match ExprSelect whose immediate head is an
ExprVar.  Pushes the cycle one step further to 'libsForQt5'.

### Remaining: 'libsForQt5' cycle (call-on-select shape)

`inherit (libsForQt5.callPackage path0) Y Z` — outer ExprCall
whose `fun` is `ExprSelect(Var(libsForQt5), callPackage)`.
Neither base rule matches: the Call walk hits ExprSelect and
stops; the Select rule looks at the head expression, which is
ExprCall.

Implemented: opt-in via `NIX_V3_THUNK_CALL_ON_SELECT_VAR=1`
which, after the call walk, additionally accepts an ExprSelect-
on-Var as a Var-rooted head.  Default-off because flipping it on
triggers a **runtime force-count explosion**:

  - hot descriptor: `lib/systems/parse.nix:64:44`
    (the `({inherit name;} // value)` OpUpdate inside setType)
  - `forced = 10.7M`, hot count = 6.2M (in 30 s)
  - `arena = 56 GB`, peak frame depth 3604

Bisecting with NIX_V3_INHERIT_FROM_THUNK_FILTER showed the
explosion fires even when the rule is restricted to a single
`inherit (libsForQt5) ...` clause — i.e. one clause is enough to
trip it.  That rules out a "many small thunks" hypothesis;
instead, ONE additional thunkified from-expr breaks SHARING in
the lib.fix iteration so setTypes reruns thousands of times.

Hypothesis (architectural): the thunk wrap captures `with pkgs;`
into `capturedWiths` at MakeThunk time (lexical-with chain).
Forcing the thunk re-pushes those withs and runs the body.
Under TW, the SAME thunk is shared across fix-point iterations
because TW's env-driven thunks share representation; under v3's
lexical-with chain, a fresh thunk is materialised per iteration
and each one forces independently.  This is the same bug class
as project_498 always-thunkify regression / project_516 lambda-
param Slot conflation.

For libsForQt5 to close cleanly we need either:
  (a) Tag::Slot sharing such that the from-expr thunk is
      memoised across fix-point iterations (cell-update protocol
      for from-expr thunks, mirroring the rec-attrset cell
      update at OP_RETURN).
  (b) Explicit shared from-expr cache outside the per-iteration
      lexical-with chain — a per-CU cache of (Expr*, env-shape)
      → thunk pointer that survives across `f x` iterations.

Both are STG-territory.  Tracked as #548c.

### Files committed this session

- 0931b77a3 — curried-call thunkify + diagnostic improvements
- eae55d149 — ExprSelect-with-Var-head + sync ip on OP_WITH_LOOKUP
- c8d90c521 — RCA memo update
- 78a24bb63 — opt-in call-on-Select-Var
  (NIX_V3_THUNK_CALL_ON_SELECT_VAR=1)

Cycle progression:
  callPackage → texlive → libsForQt5 (current floor)
  All regressions clean (170/170 across 4 suites).

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input
Output Group.
SPDX-License-Identifier: Apache-2.0
