# v3::Value canonicalization (#458 step 4) — B.1 audit

> **ARCHIVED-IN-PLACE 2026-05-27**: This doc is historical (pre-strategic-doc-set or one-off RCA). Preserved here because it is still referenced from one or more KEEP docs. See [`LODE_CLEANUP_REVIEW_2026-05-27.md`](LODE_CLEANUP_REVIEW_2026-05-27.md) for the archival rationale.

---


**Date:** 2026-05-06
**Scope:** Audit the call-site footprint of replacing `nix::Value` with
`nix::v3::Value` as the canonical evaluator type, the goal of step 4
in the v3-primary-driver inversion plan.

This is a *research* document.  It enumerates what changes, classifies
the cost, and proposes a multi-stage migration.  It does NOT commit
the codebase to the migration.

## Scope reminder

- `nix::Value` (TW-side, `src/libexpr/include/nix/expr/value.hh`):
  40+ bytes, tagged-union with separate `internalType` field, designed
  for compatibility with the historical eval ABI.
- `nix::v3::Value` (`src/libexpr-v3/include/v3/value.hh`):
  16 bytes, 5-bit tag in low byte of `tag_payload`.  Designed for
  cache-locality and small allocation overhead.

Today they coexist; conversion happens at every cross-evaluator
boundary via `treeWalkerToV3Public` / `v3ToTreeWalkerPublic`.

## Audit numbers

```
$ grep -rn "nix::Value\b" src/libexpr-v3/*.cc src/libexpr-v3/include/v3/*.hh
117 sites across 9 files

$ grep -rn "Value &\|Value \*\|Value\&" src/libexpr/*.cc src/libexpr/primops/*.cc
594 sites in libexpr (the TW-side library)

$ grep -rn "nix::Value\b\|::Value\b" src/nix src/libcmd src/libstore
~2 sites in libcmd; 0 elsewhere
```

| Module | nix::Value count | Notes |
|---|---:|---|
| `src/libexpr/include/nix/expr/eval.hh` | 85 | public eval API |
| `src/libexpr/include/nix/expr/value.hh` | 50 | the type itself |
| `src/libexpr/include/nix/expr/vm.hh` | 28 | TW v2 BC API |
| `src/libexpr/include/nix/expr/nixexpr.hh` | 21 | Expr->eval signatures |
| `src/libexpr/include/nix/expr/bytecode.hh` | 20 | BC operands typed nix::Value |
| `src/libexpr/include/nix/expr/eval-inline.hh` | 19 | hot inlined forceValue |
| `src/libexpr-v3/primops.cc` | 82 | bridge + cross-eval primops |
| `src/libexpr-v3/v3_hook.cc` | 19 | call/force hooks |
| `src/nix`, `src/libstore` | ~0 | external CLI consumers |

**Key observation:** the *external* consumer surface (`src/nix`,
`src/libstore`, `src/libcmd`) barely touches `nix::Value` directly.
Almost all uses are libexpr-internal.  Replacing the canonical type
breaks libexpr internals + the primop-author API, but does NOT cascade
through every CLI/utility crate.

## Primop counts

```
src/libexpr/primops.cc: 92 primops registered
src/libexpr-v3/primops.cc: 207 primops registered
```

Each TW primop has signature roughly:
```cpp
void primFoo(EvalState&, PosIdx, Value** args, Value& out);
```
207 v3 primops use the v3 signature:
```cpp
void primFoo(v3::EvalState&, v3::Value* args, v3::Value& out);
```
The two signatures are similar but not identical; some v3 primops
also exist as TW primops with deliberately-mirrored semantics.

## Migration cost classification

### A. Trivial — accessor mechanical replacement

Most call sites are either:
- `Value& v` parameter that's mostly forwarded
- `Value::mkInt/mkString/mkAttrs` constructors
- `v.type()` / `v.isAttrs()` / `v.attrs()` accessors

These translate one-to-one against `v3::Value`'s existing accessor
surface (`isInt/isString/isAttrs`, `mkInt/mkAttrs`, ...).
**Estimated effort:** scriptable mechanical refactor.  Maybe 1-2 days
if the refactor pass is careful about case fall-throughs.

### B. Hard — tagged-union shape mismatch

`nix::Value` distinguishes 17+ types via `internalType` enum.
`v3::Value` distinguishes 17 tags but with different semantics:
- `nix::Value::nThunk` covers MULTIPLE thunk variants (suspended,
  evaluated cached, blackhole) by interrogating the thunk pointer's
  state field.
- `v3::Value::Tag::Thunk` does the same but the SHAPE of the thunk
  (`Suspended` / `Blackhole` / `Evaluated` / `Native` / `Bridge`)
  is on `v3::Thunk::state`.
- `v3::Value::Tag::App` is a SEPARATE TAG; `nix::Value` uses an
  internal app-pair embedded in its variant.
- `v3::Value::Tag::Slot` has no nix::Value equivalent.

Equivalent for the canonicalization:
**Either** `v3::Value` grows tags to cover everything `nix::Value`
distinguishes (a `nix::Value` with internalType X maps to v3 tag X'),
**or** `v3::Value` adopts the existing TW thunk-state API for the
existing tags.

The right answer is probably the latter — `v3::Value` already does this
for its own thunks.  The work is in the SHIM that converts the
existing TW pre-built thunk structures to the v3 variant.

**Estimated effort:** ~1 week for the shim, plus the test surface to
verify thunk-state semantics on real-world workloads.

### C. Hard — Boehm GC scanning

`nix::Value`'s tagged-union is laid out so Boehm's conservative
scanner finds embedded pointers correctly (every union field starts at
the same offset, the discriminator only narrows usage).
`v3::Value`'s 16-byte layout has the same property — payload is one
8-byte word that Boehm scans uniformly.

Validation needed: every `nix::Value*` storage location (vector,
map-of-Value, embedded-in-struct) needs the same layout guarantee
when re-typed as `v3::Value*`.  Most should — they're already 16-byte
aligned — but the audit must explicitly verify.

**Estimated effort:** probably 2-3 days of focused validation +
hunting subtle invariants like "the vEmptyList singleton must have
this address layout to compare-equal in this hot path."

### D. Very hard — TW eval loop, callFunction, forceValue

`EvalState::callFunction(Value&, args, Value& out, pos)` has been
the canonical eval entry for years.  Every primop ultimately calls
into it (or its inline-fast-path siblings).  Re-typing the argument
to v3::Value means rewriting:
- the call-loop's curry / formals dispatch (eval.cc:1820-2200)
- forceValue's thunk-resolution loop (eval-inline.hh:130-200)
- App-force path (the same loop)
- ExprApp/ExprCall::eval bodies (nixexpr.cc)

Each of these has been tuned over years for hot-path performance.
A naive re-type loses some of those gains until v3-side equivalents
are tuned in turn.

**Estimated effort:** weeks.  Realistically a 3-month migration
window with parallel TW kept around for fallback.

## Recommended phased migration

**Phase 0 (DONE):** Steps 1-3 of the inversion plan.  v3 owns eval
entry / forceValue / callFunction at the hook level; #458 step 2
inverts the dispatch source.  Leaves `nix::Value` as canonical at
the type level.

**Phase 1 — primop signature unification:**
Add a `v3::PrimOp` variant whose `fn` takes `(v3::EvalState&,
v3::Value*, v3::Value&)`.  Existing 207 v3-side primops already use
this.  Leaves the 92 TW primops untouched but adds a one-time wrapper
that bridges `nix::Value` ↔ `v3::Value` at the entry edge.

**Phase 2 — public API alias:**
Define `using nix::CanonicalValue = nix::v3::Value;` (gated by a
`-DV3_VALUE_CANONICAL` build flag).  Public eval headers introduce
typedef so external consumers (REPL, daemon, libcmd) can be migrated
one-by-one.  Build one-and-only-one canonical at a time; the flag
flips when migration is complete.

**Phase 3 — TW eval loop migration:**
Re-type `EvalState::callFunction`, `EvalState::forceValue`,
`Expr::eval` against the canonical type.  Re-tune hot-path inlining.
Run all benchmarks including cardano-node/nixpkgs/hello.name to
verify no regression.

**Phase 4 — primop body migration:**
Convert the 92 TW primops to the canonical signature.  Many are
mechanical; a handful (`builtins.derivationStrict`, `fetchurl`,
`import`) need careful review for context-propagation gaps.

**Phase 5 — TW removal:**
With everything canonicalized on v3::Value, the TW eval path becomes
a thin facade.  Eventually remove or relegate to a fallback for
diagnostic shapes.

## Honest scope statement

Step 4 is realistically a 1-3 month project depending on how much
parallel-running infrastructure is wanted.  It does NOT fit a single
session.  This document is the deliverable of B.1 (audit) — it
informs the multi-month plan but does not commit any production code.

Concrete next actions (none of them gated on this audit):

- B.2 design: the v3::Value primop signature alias header.
- B.3 stub: a `using v3val = nix::v3::Value` in a new include that
  refactors a single primop family (e.g., `builtins.add`/`sub`/`mul`/
  `div`) to validate the migration shape.

Both are tractable as standalone followup tasks and would inform
whether to commit to Phase 1+ migration.

## Copyright

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group.
SPDX-License-Identifier: Apache-2.0
