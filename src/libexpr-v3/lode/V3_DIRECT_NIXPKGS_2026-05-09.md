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

## What the actual fix needs (corrected — *not* via TW)

**Earlier draft of this memo proposed routing through TW for
mid-flight observation.  That was wrong.** The inversion plan
(#454 → #457 → #458) is explicit: v3 owns eval, TW is leaf-only
(FFI boundary — store ops, derivation, path resolution).  Borrowing
TW's force at a Tag::Slot deref would re-create the cross-VM
fresh-thunk pattern documented in `EVAL_ORDER_DIVERGENCE_2026-05-08.md`
and undo the whole point of the slot architecture.

The fix has to be **v3-native**.  Two angles, both legitimate:

### Angle A: match TW's eval-order (eliminate the divergence)

TW completes the assert + res forcing without firing the inner
`with pkgs; callPackage` thunk.  v3 fires it.  Find the v3-specific
eager force and remove it.

The kill-switch sweep above showed the cycle is robust to every
v3 optimisation toggle, so the divergence is in the *core* lower
or VM dispatch path — not in any opt-pass.  Candidates worth
instrumenting (haven't been ruled out yet):

- **OP_ATTRS_INIT** ordering: TW's `ExprAttrs::eval` builds the
  binding map but invokes `maybeThunk` on every entry — does v3's
  OP_ATTRS_INIT build the same shape, or does it force entry
  values in some path (dynamic-name attrs, inherit-from)?
- **Inherit-from from-expr lowering**: hidden-from-expr thunks
  fire when ANY of `inherit (X) a b c` is referenced.  If v3
  references one during construction (e.g., for the `assert
  conflictingAttrs == {}` evaluation) but TW doesn't, that's the
  divergence.  `lib.attrNames` in the assert error message is a
  candidate — it forces the attrset top-level which could trigger
  hidden-thunk evaluation.
- **OP_CALL on a primop with strict args**: `lib.intersectAttrs`
  forces both args; v3's primop dispatch might evaluate them in
  a different order than TW.
- **OP_RETURN cell-update**: if a let-rec entry's cell update
  cascades into evaluating sibling entries (chain effect), that
  could force more than TW does.

The most promising next instrumentation is to log every OP_FORCE
inside the lib.fix x_thunk's lifetime and identify the FIRST force
that has no analog in TW.  Done by tracking the "outer thunk
name" set at OP_FORCE entry and printing the trip when it's "x".

### Angle B: native mid-flight observation (no TW)

If we can't eliminate the divergence, give v3 a way to observe
partial state during construction without touching TW.  The slot
mechanism (#458) gets us part of the way: heap-stable cells with
OP_RETURN-time updates.  Extending it would require:

1. **Per-Bindings progressive view**: each rec attrs's Bindings*
   is mutated as `OP_ATTRS_REC_SET` fires.  An in-progress reader
   (forcing through Tag::Slot) sees the entries that have been
   set so far.  Already true for `rec { x = 1; y = self.x; }`
   patterns — the slot mechanism handles them.
2. **Progressive write for the let-rec body's intermediate
   values**: when a `let prev = ...; in body` is mid-evaluating
   `body` (e.g., an OP_UPDATE), and a foreign Slot reader hits
   its outer thunk, return whatever the body has accumulated.
   This is hard because the OP_UPDATE doesn't have a "result so
   far" — it's transient operand-stack state.

Angle A is cleaner if we can find the divergence.  Angle B is a
deeper structural change.

## Recommended next step

Instrument the FIRST eager force during lib.fix x_thunk's
forcing; trace through to its lowering origin.  That tells us
which lowering decision creates an OP_FORCE that TW doesn't have
analog for.  THAT is the v3-native fix: change the lowering to
match TW's lazy structure.

Until that lands: users wanting full nixpkgs eval can use
`NIX_USE_V3=1` (without `NIX_V3_DIRECT_EVAL`), which works under
#547's STG default-on flip.  But that's a workaround, not the
goal — the goal is v3-direct fully working on nixpkgs.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input
Output Group.
SPDX-License-Identifier: Apache-2.0
