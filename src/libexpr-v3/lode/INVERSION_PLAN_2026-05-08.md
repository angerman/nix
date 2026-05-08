# v3 Inversion Plan — TW as Leaf, v3 as Host

Date: 2026-05-08
Status: PLAN (not yet implemented)
Related: #454 Phase E (partial inversion via hooks), #523 (this audit)

## Why

Today the integrated `nix` CLI runs TW as the host and v3 as a hook
inside TW.  When v3 produces a value (e.g. an attrset with closure
attributes), it must be bridged back into a TW Value so TW's caller can
continue.  For most types this is trivial; for closures with formals
(`{a, b ? c, ...}: body`) it is not — TW's `autoCallFunction` is
dispatched on `tLambda` shape, and `tPrimOpApp(__v3_call_bridge_1, h)`
loses that.  The current code refuses such bridges with `BlackholeError`,
catches at the `forceAttr` site, and re-evaluates the OUTER Expr via
TW.  Under nixpkgs's `lib/default.nix` (5929 instructions, 525 lambdas,
many formals attrs) the re-evaluation hits v3's mid-construction state
and surfaces as `v3 forceValue: infinite recursion (blackhole)`.

The bridge exists because v3 is integrated as a hook; the result must
fit `nix::Value &`.  Removing that contract removes the bridge.

## What "inversion" means

Concretely: when `NIX_USE_V3=1` (or a successor flag), the CLI calls
v3's pipeline directly.  v3 produces a v3 `Value`.  Output is rendered
by v3's own printer.  TW is invoked **only** when v3 hits a primop or
operation it does not implement (e.g. derivationStrict's full strict
merge, store-side path operations, certain externals).  At those leaf
points, v3 calls TW with concrete TW Values — no bridge, just an FFI.

Compare to today: `state.eval(expr, env, v)` enters TW, TW invokes the
v3 eval hook, hook runs v3, hook bridges the v3 result into `v`.
After: `v3RunRootExpr(state, expr) → v3::Value`.  No `state.eval`, no
hook, no bridge.

## What v3 already has

* `parseExprFromString` / `bindVars` (TW; reused — there's no v3 parser).
* `lowerNixExpr(e, symbols, positions) → ir::Module`.
* `compile(module) → CompilationUnit`.
* `run(cu) → v3::Value` with `runOnExistingVm` STG-10 sharing.
* `forceValue(vm, v)` / `forceDeep` (in `cli/v3-eval.cc`, easy to lift).
* `printNixValue(out, v, symTab)` and `toJsonValue` in `cli/v3-eval.cc`.
* Native primop coverage (#453 Phase D).

The standalone `v3-eval` binary uses exactly this pipeline and passes
142/142 lang tests.

## What needs to move

### Phase 1 — minimum viable inversion, scoped to `nix eval --expr/--file`

1. Lift `forceDeep` and `printNixValue` from `cli/v3-eval.cc` into a
   public header (e.g. `v3/print.hh`, `v3/runRoot.hh`) so the CLI can
   reuse them.
2. Add `Value runRootExpr(EvalState & state, Expr * e)` that wraps the
   v3 pipeline (lower → compile → run + setNixEvalState).
3. In `src/nix/eval.cc CmdEval::run`, gate on `NIX_V3_DIRECT_EVAL=1`
   (or similar): if set AND we have an `--expr` / `--file` installable,
   do NOT call `installable->toValue`.  Instead:
   * Re-parse / re-evaluate the user's expression via `runRootExpr`.
   * Apply `-A attrPath` by descending v3 attrs (`forceValue` + lookup).
   * For `--apply`, lower+compile+run the apply expr separately, then
     `callClosure` the result on the descended value.
   * Print:
     * default: `printNixValue` (deep-forces).
     * `--json`: `toJsonValue`.
     * `--raw`: coerce v3 string + `writeFull`.
4. Verify on `(import <nixpkgs> {}).hello.name` and `.lib.fix`.  Both
   should pass without hook re-entry / bridge cycles.
5. Run the v3-eval lang/parity suites — they're already v3-direct, so
   no regression expected, but verify nothing breaks via build path.

### Phase 2 — handle flake-based installables

Flake resolution is TW machinery (Installable::toValue).  Two options:

* **2a** Keep flake resolution in TW.  The TW path produces a TW
  Value for the resolved attrset; convert to v3 Value once via a
  one-shot bridge (TW → v3, no return trip).  Then run v3-direct from
  there onward.  Simpler; keeps the "inversion" scope focused.
* **2b** Reimplement flake resolution in v3.  Larger; touches lots of
  store / fetcher code paths.

Recommend 2a as default; 2b as a follow-up.

### Phase 3 — the other CLI commands

`nix build`, `nix run`, `nix-instantiate`, etc. each have their own
eval entry.  Iterate them one at a time, mirroring Phase 1.

### Phase 4 — leaf TW callbacks

For primops v3 doesn't implement, v3 today calls TW via Bridge thunks.
That works for forward calls (v3 → TW → return TW value → v3 wraps as
Bridge thunk).  This direction stays.  What goes away is the inverse
(v3 result bridged back into a `nix::Value &` for a TW-host caller).

## What goes wrong if we do this

* CLI commands not yet ported still go via TW eval hook.  Until Phase 3
  is complete, partial coverage.  Mitigation: gate Phase 1 behind a
  separate flag from `NIX_USE_V3=1` so users opt in.
* v3 may hit a primop / construct it doesn't implement.  Today the
  call hook handles this; under v3-primary, we need an explicit
  "TW leaf call" mechanism.  The infrastructure exists (Bridge thunks
  for TW values), so this is plumbing, not new design.
* Error formatting differs.  v3's printer may format errors / values
  slightly differently from TW's `ValuePrinter`.  Cutover-parity tests
  guard 140/142 of these; remaining 2 are pre-existing diffs (eval-okay
  -inherit-from / eval-okay-print).

## Why this fixes the lib.fix cycle

The cycle is: v3 produces a formals-closure → bridge refuses → forceAttr
catches → re-runs outer Expr in TW → TW's re-eval hits v3's still-Black
thunks → BlackholeError.

Under v3-direct: the result of `lib/default.nix` stays as a v3 attrset
of v3 closures.  `.fix` accessor returns a v3 Tag::Closure.  v3's
printer prints `«lambda fix @ ...»`.  No bridge.  No fallback retry.
No cycle.

## Phase 1 result — landed 2026-05-08

Phase 1 ships in 4 commits:

1. `bcd041fc9` — lift v3 printer + forceDeep + JSON renderer to public header.
2. `eb7b385fd` — add `runRootExpr` helper.
3. `cd490979f` — wire `NIX_V3_DIRECT_EVAL` gate in `CmdEval::run`.
4. `10189b368` — regression test (`run-direct-eval-tests.sh`, 26/26 pass).

The v3-direct path is wired and works for the 26 representative shapes in
the regression test (scalars, lists, attrs, let/let-rec, lambdas with
formals, conditionals, primops, JSON output).  Existing TW path
unaffected; lang-tests stay 142/142, cutover-parity stays 140/142.

What v3-direct revealed: **the lib.fix cycle was masking an underlying
v3 evaluator bug**, not a bridge issue.

Under `NIX_V3_DIRECT_EVAL=1`:
- `(import <nixpkgs> {}).hello.name` fails with `OP_ATTRS_SELECT:
  attribute not found` — `self` (lib's fix-point parameter) chases
  to a Bindings of size=1 with key `callLibs` (the inner let-rec's
  bindings) instead of the outer fix-point.
- The standalone `v3-eval` binary fails on the SAME expression with
  the SAME error.  v3-direct's behavior matches v3-eval — the
  inversion correctly decoupled from the bridge cycle.

The bug is documented at lower.cc:1546-1568 as a known
"wrong-upvalue-capture bug in the thunkify path under deep nesting"
— `inherit (self.X) Y` inside `let X = ...; in {...}` where the
let-rec bumps the level offset.  Default `NIX_V3_SELF_DOT_MAX_LEVEL=0`
only thunkifies at the immediately-enclosing lambda; nixpkgs lib
hits level=1 (across the inner `let callLibs = ...`).

Setting `NIX_V3_SELF_DOT_MAX_LEVEL=1` swaps this for a different bug
(`OP_WITH_LOOKUP: name 'nix-update' not found in with-scope`) — a
separate upvalue-capture issue under the broader thunkify path.

**Phase 1 success criteria met.**  Bridge bug architecturally bypassed
on the v3-direct path; remaining failures are pre-existing v3 bugs
that hook-mode previously masked via TW fallback.

## Phase 2+ — remaining work

The v3-direct path now exposes the actual bugs to fix:

a. **#528 [LANDED 917b4fc5a]: self-dot inherit-from default level
   0 → 4.**  The earlier `s_maxLevel=0` default left
   `inherit (lambda_param.X) Y` eager whenever wrapped in a `let`
   (the canonical lib.fix shape).  Bumped default to 4; thunkify
   itself was correct (it defers `self.X` until `Y` is forced,
   matching TW).  9/9 self-dot regression suite, all 13+ other v3
   suites still green.

b. **#529 [LANDED c8362c2f9]: thunkify `inherit (X) Y` where X is a
   fromWith Var or fromWith-Var Call.**  Closes the `nix-update` and
   `callPackages` with-lookup misses by extending the
   `isComplexFromExpr` gate to also catch:
     - bare `ExprVar` with `fromWith=true`
     - `ExprCall` whose head is a fromWith ExprVar
   Both cases now thunkify so the with-lookup is deferred to
   force-time of the inherited Y-attribute.  11/11 self-dot
   regression suite (was 9/9), all other suites still green.

c. **next failure under v3-direct: `OP_WITH_LOOKUP callPackage not
   found`.**  Different shape — captured-withs propagation through
   deeply-nested thunks inside the stdenv booter.  The diagnostic
   trace shows a thunk frame with `withBase=1` whose captured-with
   slot resolves to `attrs size=1 {prev}` — the inner extends-
   chain's let-rec bindings, NOT the outer `with pkgs;`.  Hypothesis:
   the failing thunk was captured inside a `with X;` where X (a
   `prev` lambda parameter) is mid-construction, and the OUTER `with
   pkgs;` is not in this thunk's captured-withs because module
   imports break the dynamic-with-stack chain at file boundaries.
   Tracked as #529 follow-up.

c. **flake installables** — `nix eval nixpkgs#hello.name` (option 2a:
   keep flake resolution in TW, one-shot bridge to v3 once resolved).

c. **autoArgs** (`--arg` / `--argstr`) on the v3-direct path.

d. **`--write-to`** recursive directory emission.

e. **other CLI commands** — `nix build`, `nix run`,
   `nix-instantiate` — each has its own eval entry that needs the
   same `NIX_V3_DIRECT_EVAL` gate.

## First-week scope

A focused first sprint can land Phase 1 in 4 commits:

1. Lift `forceDeep` + `printNixValue` + `toJsonValue` into a public
   `v3/print.hh` with public API.
2. Add `runRootExpr(state, e)` helper to `v3/run.hh`.
3. Wire `NIX_V3_DIRECT_EVAL=1` path in `CmdEval::run` for `--expr`/
   `--file`.  Smoke test: `nix eval --impure --expr '1 + 2'` and
   `(import <nixpkgs> {}).hello.name`.
4. Add a regression test under `test/run-direct-eval-tests.sh`.

After that, the lib.fix cycle should be unreachable on the v3-direct
path, even with the existing hook-mode bridge bugs untouched.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
