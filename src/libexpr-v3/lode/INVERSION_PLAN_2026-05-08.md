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
