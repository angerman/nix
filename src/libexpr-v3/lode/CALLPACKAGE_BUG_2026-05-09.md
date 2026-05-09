# v3-direct callPackage with-scope bug — investigation notes 2026-05-09

## Symptom

```
$ NIX_V3_DIRECT_EVAL=1 nix eval --impure \
    --expr '(import nixpkgs {}).hello.name'
error: v3 OP_WITH_LOOKUP: name 'callPackage' not found in with-scope
```

Affects ANY full nixpkgs eval — `.hello.name`, `.path`, `.lib.id`,
`._type`, `builtins.attrNames` — even `builtins.typeOf` of the
imported set fails.  The failure happens during the construction of
`pkgs` itself, before any user-side attribute access.

This is what task #516 ("STG-14b: close remaining v3-internal cycle
on nixpkgs hello.name", in_progress) was tracking; this doc captures
2026-05-09 findings.

## V3_DBG_WITH=1 + V3_DBG_WITH_PUSH_PREV=1 trace

```
v3 OP_WITH_LOOKUP miss: name='callPackage' (sid=2128) base=1 top=2
  with[1] tag=16 thunk_ptr=0x0
    -> SLOT(0x88cdf79b0)=tag=10 (Thunk evaluated)
    -> tag=7 attrs size=1 {prev}     <-- WRONG: only `prev` field
```

The with-stack at the failing OP_WITH_LOOKUP contains a Tag::Slot
pointing to a Tag::Thunk whose evaluated form is an attrset of size
1 with the single attribute `prev`.  This is `lib.extends`'s
let-binding `let prev = f final; in prev // overlay final prev` —
the LET-REC's bindings attrset has been pushed onto the with-stack
where `pkgs` should have been.

V3_DBG_WITH_PUSH_PREV traces the OP_WITH_PUSH that pushed `{prev}`:

```
v3 OP_WITH_PUSH {prev}: cu=0x... ip=141 frames=88
  containing lambdas[14]: codeOffset=140 nUp=4 nLocals=5177 name=super
  containing lambda body [140..150):
    [140] OP_GET_UPVALUE         operand=3
    [141] OP_WITH_PUSH           operand=0
    [142] OP_GET_UPVALUE         operand=3
    [143] OP_MAKE_THUNK          operand=15  data=[0,1]
    ...
```

Lambda 14 (named `super`) is **all-packages.nix's innermost lambda**
— the body of `res: pkgs: super: with pkgs; { ... }`.  The
parameter named `super` is the innermost; `pkgs` is the next outer.
`OP_GET_UPVALUE 3` retrieves what should be `pkgs`.  This lambda's
nLocals=5177 — consistent with the thousands of attribute bindings
in all-packages.nix.

Closure 14 was constructed at offset 132-136:

```
  [132] OP_GET_UPVALUE 0   <- enclosing lambda 13's upvalue 0
  [133] OP_GET_UPVALUE 1   <- enclosing lambda 13's upvalue 1
  [134] OP_GET_UPVALUE 2   <- enclosing lambda 13's upvalue 2
  [135] OP_GET_LOCAL 0     <- enclosing lambda 13's LOCAL 0 = pkgs param
  [136] OP_MAKE_CLOSURE    operand=14  data=[4,0]
```

So lambda 14's upvalue 3 is statically tied to lambda 13's
parameter `pkgs`.  At runtime, lambda 13 must have been called with
`pkgs = {prev}` for upvalue 3 to be `{prev}`.

## Root cause hypothesis

Call chain for the failure (working backwards):

1. `(import nixpkgs {}).hello.name` triggers fixed-point eval.
2. `lib.fix toFix` invokes the chained extends fold.
3. Each `lib.extends`-applied stage is `final: let prev = f final; in prev // overlay final prev`.
4. For `allPackages` (the all-packages.nix overlay), the call is
   `overlay final prev` = `allPackages final prev` = `(self: super: ...) final prev`.
5. Inside `allPackages`, `let res = import all-packages.nix {} res self super`.
6. The full call: `(allpkgs-fn {}) res self super`.  
   - Lambda 13 = `pkgs:` (the middle of the curried `res: pkgs: super:`).  
   - Lambda 13 is called with `self` (the `allPackages` outer arg, = `final` from extends).
   - `pkgs` = `self` = `final` SHOULD = the full fixed-point pkgs.

But at runtime, `pkgs` evaluates to the let-rec slot `{prev}` from
`lib.extends` — NOT the fixed-point.  Either:

- (A) The OP_CALL at site 5 passes the wrong argument (some
  miscompilation of curried application), or
- (B) The call sites for `lib.extends`'s `overlay final prev` swap
  args somehow, or
- (C) v3's lowering for `final` (the `final:` lambda parameter of
  lib.extends) captures the let-rec recAttrs slot for `prev`
  instead of capturing the lambda parameter, and that wrong-Slot
  propagates as `pkgs`.

(C) is the most consistent with what the trace shows: the value
on the with-stack is a Tag::Slot referencing a Tag::Thunk that
evaluated to `{prev}` — that's the let-rec's recAttrs storage, not
a regular function-argument value.  A correct `final` would be a
deeply-evaluated attrset value, not a Tag::Slot to a 1-entry
let-rec.

## What works (sanity)

Synthetic / lib-only repros that mirror parts of the structure all
pass under v3-direct:

- `let f = res: pkgs: super: with pkgs; { foo = callPackage; }; pkgs = ...; in (f null pkgs null).foo` — works.
- `lib.fix (lib.extends overlay base)` with one stage — works.
- `lib.foldl' (lib.flip lib.extends) base [stage0 stage1 ...]` chain
  with allPackages-shape stage — works.
- Even adding multiple stages BEFORE and AFTER the allPackages-
  shape stage doesn't reproduce.

So the bug requires SOMETHING that's missing from minimal repros.
The actual nixpkgs has:

- 9+ overlays composed via foldl' (flip extends) (vs. 3 in repros).
- Big formals-set on outer all-packages function.
- Mutual references between stages (each stage's body reads attrs
  defined by other stages via `final` / `super`).
- `__splicedPackages` sub-attrset returned by booter.
- Cross-CU calls (lib code in lib/fixed-points.nix, all-packages.nix
  in pkgs/top-level/).
- Disk-cache hit/miss interactions.

## Negative results (narrow the search)

The following diagnostic kill switches all leave the symptom
unchanged (still `OP_WITH_LOOKUP: name 'callPackage' not found`):

| env var                            | what it disables                  | result    |
|------------------------------------|-----------------------------------|-----------|
| `NIX_V3_NO_INTRINSIC_RECOGNISE=1`  | Fix/Extends/Compose recognition   | unchanged |
| `NIX_V3_NO_OPTIMISE=1`             | the IR optimiser pipeline         | unchanged |
| `NIX_V3_NO_REC_SLOT_CAPTURE=1`     | rec-slot capture (Tag::Slot path) | unchanged |
| `NIX_V3_NO_INLINE_REC_SLOT=1`      | inline RecBindingSlotRef          | unchanged |
| `NIX_V3_NO_INVERT_EVAL=1`          | invert-eval direct path           | unchanged |
| `NIX_V3_NO_LIFT_LAMBDA=1`          | lambda hoisting                   | unchanged |
| `NIX_V3_NO_LAMBDA_SKIP=1`          | lambda-skip pass                  | unchanged |
| `NIX_V3_THUNKIFY=0`                | broader-thunkify                  | unchanged |

So the bug lives in core lowering / emit / runtime — NOT in the
optional optimisation paths these gates control.

## What to do next session

1. **Bisect for triggering complexity**: keep adding nixpkgs-shape
   features to the minimal repro until v3-direct also fails.  Most
   likely candidate: the recursive `let res = ... res self super`
   pattern in stage.nix's allPackages, where `res` is inside its
   own definition.
2. **Instrument OP_CALL on lambda 13**: log the runtime value of
   the arg (`pkgs`) at every call site that targets that lambda.
   Expected: `final` (a deeply-evaluated attrset).  Observed:
   `Tag::Slot -> {prev}`.  Find the call site that passes
   `Tag::Slot`.
3. **Audit `final` resolution in lib.extends**: lower.cc's
   resolveVar for `final` (the lambda param at level 0, displ 0)
   should produce a direct local — not a Tag::Slot to a let-rec
   entry.  Check whether some nested-lambda freeVar capture is
   conflating `final` with the LET-REC's recSlotVar.
4. **Look at #498 + #516**: both tasks are in_progress and might
   share root cause.  #498 = "always-thunkify regresses callPackage
   in nixpkgs"; #516 = "v3-internal cycle on nixpkgs hello.name".
   This `OP_WITH_LOOKUP miss` symptom is consistent with both.

## Workaround for bench

Until this is fixed, the bench harness's nixpkgs full-pkgs workloads
(`hello-name`, `attrnames-pkgs`, `hello-drvPath`, `git-name`,
`firefox-drvPath`) stay tagged `skip-by-default` in
`bench/workloads.toml`.  The `lib`-only workloads (everything tagged
`real-world` minus `skip-by-default`) all work in v3-direct and form
the load-bearing real-world signal.

cardano-node would face the same wall: it's `import nixpkgs {} //
import ./pkgs.nix` shape, which goes through the same all-packages
+ extends path.
