# v3-direct + STG: full nixpkgs cycle — investigation

## Status

After flipping STG default-on for the runtime gates (#547 Phase 2),
v3-fhook on full nixpkgs works (BLACKHOLE → OK).  v3-direct still
cycles on full nixpkgs:

```
$ NIX_V3_DIRECT_EVAL=1 nix eval --impure --expr 'builtins.typeOf (import <nixpkgs> {})'
error: v3 OP_WITH_LOOKUP: cycle while resolving 'callPackage'
```

The fix needs the architectural slot-bridge work (#509 STG-13,
PUBLISH_RECOVERY_USE_AUDIT_2026-05-08.md Gap A).  Quick-toggle tests
ruled out per-optimization causes.

## Frame trace (post-#547 flip, 94 frames deep)

```
[93] thunk codeOff=39086 — failing thunk: OP_WITH_LOOKUP callPackage; OP_LIT_PATH 17; OP_CALL
[92] call  codeOff=3538  name='attrs' — recurseIntoAttrs lambda body
                                         (attrs // { recurseForDerivations = true; })
[91] thunk codeOff=140   name='super' — some `super:` lambda body wrapped as thunk
[90] thunk codeOff=523   name='conflictingAttrs' — let-rec entry from stage.nix:159
[89] thunk codeOff=545              — anonymous thunk
[88] call  codeOff=394   name='super' — stage.nix's allPackages's super: lambda
[87..82]   codeOff=148   name='final' — extends layer thunks (5 levels)
[81] call  codeOff=1170  name='super' — outermost extends call
... lib.fix x_thunk forcing path ...
```

## Cause chain

1. We force `pkgs` (= `lib.fix toFix`'s x_thunk) — state goes Black.
2. Body runs the chained extends → reaches `allPackages` overlay's
   `super:` lambda body (stage.nix:148-163):
   ```nix
   let
     res = import ./all-packages.nix {...} res self super;
     conflictingAttrs = lib.intersectAttrs res super;
   in
   assert lib.assertMsg (conflictingAttrs == { }) "..."; res;
   ```
3. The `assert` evaluates `conflictingAttrs == {}` eagerly.
4. Forcing `conflictingAttrs` runs `lib.intersectAttrs res super`,
   which forces `res`.
5. Forcing `res` enters all-packages.nix's lambda 14 body (`with
   pkgs; { ... }`), which builds the merged attrset.
6. **Somewhere during that body's evaluation**, `recurseIntoAttrs`
   is invoked with an arg, and `attrs //` forces the arg.  That
   arg's body uses `with pkgs; callPackage`.
7. `pkgs` derefs to lib.fix's x_thunk (still Black from step 1).
   Cycle.

## Why TW doesn't cycle here

Same code on TW: produces `"set"`.  TW's evaluation completes step
5 without entering step 6.  Either TW's eval-order doesn't trigger
the inner force, or TW handles partial-state observation gracefully
where v3 doesn't.  The audit memos identify TW's mechanism as
slot-pointer-into-env semantics with per-Value tBlackhole tracking
(EVAL_ORDER_DIVERGENCE_2026-05-08.md), where v3 uses heap cells +
state machine on Thunk objects.

## Kill-switch sweep results (post-#547)

All produce CYCLE on nixpkgs typeOf:

| switch                          | result |
|---------------------------------|--------|
| NIX_V3_NO_INVERT_EVAL=1         | CYCLE  |
| NIX_V3_NO_INLINE_REC_SLOT=1     | CYCLE  |
| NIX_V3_NO_REC_SLOT_CAPTURE=1    | CYCLE  |
| NIX_V3_NO_OPTIMISE=1            | CYCLE  |
| NIX_V3_NO_INTRINSIC_RECOGNISE=1 | CYCLE  |
| NIX_V3_NO_LIFT_LAMBDA=1         | CYCLE  |
| NIX_V3_NO_LAMBDA_SKIP=1         | CYCLE  |
| NIX_V3_NO_BINOP_FORCE=1         | CYCLE  |

The cycle is robust to optimisation toggles.  This is structural,
not a tweakable knob.

## Tactical fixes attempted (each failed)

- **#546**: split `OP_ATTRS_REC_INIT` into rec-vs-let-in-body
  variants to stop publishing `{prev}` onto outer thunks.  Gone.
- **lowerBinOp ir::Update skip eager force**: removed redundant
  emit-time force on `attrs // {...}` since OP_ATTRS_UPDATE auto-
  forces.  Symptom unchanged — the cycle isn't the eager force at
  emit time, it's the runtime force inside OP_ATTRS_UPDATE itself
  that triggers the chain.  Reverted.

## What the actual fix needs

Per `PUBLISH_RECOVERY_USE_AUDIT_2026-05-08.md` Gap A and
`EVAL_ORDER_DIVERGENCE_2026-05-08.md`:

The slot-bridge work (STG-13, currently pending): when v3 forces
through a Tag::Slot whose backing thunk is currently Black, fall
back to TW's slot-pointer-into-env path.  TW handles
mid-construction observation via per-Value tBlackhole tracking;
v3's per-Thunk state machine doesn't have an equivalent.

Concrete shape (sketched, not implemented):

1. At OP_FORCE / OP_WITH_LOOKUP, when the value is a Tag::Slot to
   a thunk in Blackhole state, AND we're inside a v3 eval whose
   outer frames include that thunk's force frame, route the deref
   through TW: ask TW's `forceValue(*nix::Value*)` on the equivalent
   TW value, which sees the Black via TW's mechanism.
2. TW either returns a partial-shape Value (if its env binding is
   mid-construction with a partial slot) or throws the same cycle
   error.

Effectively v3 borrows TW's mid-flight observation path for the
narrow case where v3-direct's eval-order pushes us into one.  Not
a workaround — TW IS the canonical reference for these semantics.

## Recommended next step

Don't keep instrumenting v3-direct's nixpkgs path tactically.  Pick
up STG-13 (slot-bridge): plumb a TW fallback into Tag::Slot deref
when the backing thunk is Black on the current vm's frames.

Until then, **users wanting full nixpkgs eval should use the v3-
fhook path** (`NIX_USE_V3=1`, no `NIX_V3_DIRECT_EVAL`), which
is the user-facing default after #547 and works correctly under
STG default-on.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input
Output Group.
SPDX-License-Identifier: Apache-2.0
