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

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input
Output Group.
SPDX-License-Identifier: Apache-2.0
