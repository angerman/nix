# Path B Investigation — 2026-05-16

User picked Path B (action plan Phase 2): "finish CELL_EVERYWHERE,
fix the lib.fix outer-x case, retire THUNK_ALL".  This doc records
the investigation state.

## Cycle source isolated

The repro `builtins.isAttrs (import <nixpkgs>{})` under
`NIX_V3_NO_INHERIT_FROM_THUNK_ALL=1 + NIX_V3_CELL_EVERYWHERE=1`:

  - Time: 0.6 s (no longer the 44 s hot loop)
  - Error: `v3 OP_WITH_LOOKUP: cycle while resolving 'libsForQt5'`
  - Position: `pkgs/top-level/stage.nix:150:7`
  - With-stack at fire: `with[0] = Tag::Slot → Tag::Thunk (state=1
    Blackhole)`

stage.nix:150 is the `allPackages` fix-point continuation:

```nix
allPackages = self: super:
  let
    res = import ./all-packages.nix { ... } res self super;
    conflictingAttrs = lib.intersectAttrs res super;
  in
  assert lib.assertMsg (conflictingAttrs == { }) ...;
  res;
```

`res` is the fix-point of all-packages applied to itself.  The
`with self;` look-up of `libsForQt5` fires inside all-packages.nix
while `res`'s outer thunk is Black.

## Frame stack at cycle (V3_DBG_WITH_CYCLE=1)

```
[93] thunk 'res' codeOff=454 EXEC=super     ← allPackages' res
[92] thunk 'conflictingAttrs' codeOff=513
[91] <thunk> codeOff=529
[90] call 'msg' codeOff=148
[89] call 'super' codeOff=398
[88] thunk 'prev' codeOff=183 EXEC=final    ← lib.extends's prev
[87] thunk 'prev' codeOff=183 EXEC=final
[86] thunk 'prev' codeOff=183 EXEC=final
[85] thunk 'prev' codeOff=183 EXEC=final
[84] thunk 'prev' codeOff=183 EXEC=final
[83] call 'final' codeOff=148               ← lib.extends's final lambda
[82] thunk 'x' codeOff=205                  ← lib.fix's x = f x
```

Five `prev` frames stacked — the overlay-composition cascade.
The cycle source is the chain `lib.fix x → final → prev → ... →
allPackages res → conflictingAttrs → with self; libsForQt5`.

## Why CELL_EVERYWHERE alone doesn't help

Per the existing infrastructure (commit `20adafe31` / Phase 1.5):

1. shapeCell-write fires at OP_ATTRS_REC_INIT[_TAIL] and
   OP_ATTRS_UPDATE_TAIL on the **innermost** `CFF_THUNK_RETURN`
   frame.
2. shapeCell-read fires in `forceValue`'s Black branch.

For `let x = f x` in lib.fix:
  - `x`'s body is `f x` — a function call, not an attrset literal.
  - Inside `f`'s body, `prev // overlay final prev` fires
    OP_ATTRS_UPDATE_TAIL.  But the **innermost** `CFF_THUNK_RETURN`
    at that moment is `prev` (frame 88), not `x` (frame 82).
  - So `prev->shapeCell` is updated; `x->shapeCell` stays at the
    sentinel `Tag::Thunk(x)`.

When OP_WITH_LOOKUP forces the with-source (which is `x`):
  - forceValue's Black-branch shapeCell-recovery checks
    `shapeVal.tag() == Tag::Thunk && shapeVal.payload.thunk == x` —
    yes, still the sentinel.  Recovery doesn't fire.
  - BlackholeError thrown.

So: `count of shapeCell-recovery events under V3_DBG_CELL_EVERYWHERE
on the repro: 0`.

## Why "cross-thunk propagation" was rejected

Per `CELL_UPDATE_EVERYWHERE_2026-05-12.md` §"Why not the cross-thunk
hack?" — propagating shapeCell up the THUNK_RETURN chain was tried
(Phase 1.5b, reverted) and broke things:

  > Writing the same inner Bindings to all outer thunks' shapeCells
  > means an outer thunk's "value" is observed as the inner Bindings,
  > even though the outer's eventual value is something else (the //
  > of the inner with other contributions).

So: cross-thunk propagation is NOT a valid Path B move.

## Synthetic does NOT reproduce

A minimal `fix (extends overlay (final: {...with final; pkgA...}))`
synthetic works on ALL paths (TW, default v3, NO_THUNK_ALL +
CELL_EVERYWHERE).  The bug requires structural complexity beyond
2-overlay lib.fix + with-self-lookup.

## 2026-05-17 — TW-side instrumentation lands the hypothesis kill

Added `TW_DBG_WITH_NAME=<symbol>` gate to `src/libexpr/eval.cc`
`lookupVar`: when set, logs every with-lookup of the named symbol
with the with-source's current type-tag.  Counterpart to v3's
existing `V3_DBG_WITH_CYCLE`.

Measurement on `(import <nixpkgs>{}).hello.name` AND on
`builtins.isAttrs (import <nixpkgs>{})`:

  - TW: **zero** with-lookups for `libsForQt5`.  The look-up that
    cycles v3 never fires under TW at all.
  - v3 (NO_THUNK_ALL): single look-up firing inside `res` thunk's
    `super`-executing closure, with-source is a Black slot.

**Hypothesis killed**: "TW also fires the same `with self;
libsForQt5` look-up and handles it gracefully via some mechanism
v3 lacks."  FALSE.  TW never reaches the look-up.  v3 is forcing
something eagerly that TW defers.

Per LESSONS_LEARNED §1.3 ("Eager-vs-lazy asymmetry is the dominant
bug class"): the Path B fix is to find the v3-side eager force and
make it lazy in `lower.cc`, matching TW.

## Where to look — narrowed targets

The cycle stack:
  [93] thunk `res` codeOff=454 EXEC=super
  [92] thunk `conflictingAttrs` codeOff=513
  [91] anon `<thunk>` codeOff=529
  [90] call `msg` codeOff=148
  [89] call `super` codeOff=398

Frame 93 IS inside res's body (with EXEC=super, meaning the closure
currently dispatched is named "super").  The with-stack at fire is
a single source — the `with pkgs;` from `all-packages.nix:29`.

`with pkgs;` at the top of `all-packages.nix` is the look-up
scope: any unresolved name in res's body falls through to pkgs
(=fixpoint).  Under TW, res's outer attrset construction does not
fire the look-up; under v3, something DOES.

Most likely v3 culprits:
  (a) An attrset entry in res that v3 emits non-thunked but TW
      defers via `maybeThunk`.  Audit `lowerAttrs` -> `thunkifyForAttr`
      -> `isTrivialForLazy` for any AST shape that's currently
      classified as "trivial" but contains a `fromWith` reference
      transitively.
  (b) An `inherit` clause without `(from ...)` where the inherited
      name has `fromWith=true` but v3 emits an eager `OP_GET_LOCAL`
      via a path that bypasses `thunkifyForAttr`.
  (c) A let-binding in res's body that v3 forces eagerly (e.g.
      pure-call recognized by isTrivialForLazy(forArg=true)) but
      whose body resolves `with pkgs;`.

The next concrete step: enable `V3_DBG_FORCE_TRACE` filtered to
position `stage.nix:150` or to res's CU, capture the EXACT op that
fires the look-up, work backward from there to the lower.cc
emission site.



How does TW resolve `with self; libsForQt5` while self is mid-
construction in `lib.fix (extends ... allPackages)`?

Candidate hypotheses:

  1. **TW publishes x's value earlier than v3.**  When `f`'s body
     terminates, TW writes the result to x's Value cell.  If this
     happens BEFORE the `with self; libsForQt5` look-up fires, TW
     observes x as Evaluated.
  2. **TW's `with` is bind-at-push, not lookup-at-deref.**  At
     `with self;` push time, TW snapshots self's bindings; lookups
     hit the snapshot without re-forcing self.
  3. **TW publishes intermediate `//` results via the lvalue chain.**
     TW's `mkAttrs` writes to `*v`, the parent's slot.  Each `//`
     in `prev // overlay final prev` might publish progressively.
  4. **The lookup never fires under TW** because TW's lazy eval
     defers `pkgs.libsForQt5` access until after the fixpoint has
     settled.

Without confirming WHICH, Path B can't pick a correct architectural
move.  The next concrete action: instrument TW with a counterpart
to V3_DBG_WITH_CYCLE, run hello.name, observe whether the same
look-up fires and what TW does that v3 doesn't.

## What this rules out

- **Hypothesis killed**: "Turning on NIX_V3_CELL_EVERYWHERE
  closes the libsForQt5 cycle alone."  FALSE — shapeCell never gets
  populated for the relevant outer thunk.
- **Hypothesis killed**: "Cross-thunk shapeCell propagation is the
  Path B fix."  FALSE — already tried and reverted (Phase 1.5b);
  semantically wrong.
- **Hypothesis killed**: "Synthetic 2-overlay lib.fix + with-self-
  lookup reproduces the bug."  FALSE — synthetic works everywhere;
  the bug requires structural complexity beyond it.

## What remains open

Path B's real fix depends on the open question above.  Until then,
the action plan's "Path B" name is not yet wired to a concrete
implementation.

The narrow positive: the cycle source is well-localized (single
`with self; libsForQt5` at stage.nix:150 during allPackages res
construction).  The Path B work needs to either:

  (a) Eliminate the eager force that makes the look-up fire mid-
      construction (CELL_UPDATE_EVERYWHERE plan §"Why not the
      cross-thunk hack?": "ensure no thunk is forced while it's
      mid-construction"), OR
  (b) Add a TW-style mechanism v3 doesn't have today (per the open
      question's resolution).

## Repro state

`test/repro-hello-name.nix` + `test/run-hello-name-repro.sh`
encode the gate-bisection.  This doc adds the cycle's exact
location + the architectural blockers.  Phase 2 next session:
investigate TW's behavior on the same `with self;` lookup.
