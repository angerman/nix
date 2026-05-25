## Option 4 hybrid complete — 2026-05-18

This doc closes the action plan's Phase 1 ("Close the A-series with
iterative forceValue").  Phase 1 exit criterion was:

> "(import <nixpkgs> {}).hello.name evaluates to a string under
> NIX_V3_DIRECT_EVAL=1 without C-stack overflow."

**STATUS: MET.**

```
TW:        hello.name → "hello-2.12.3"   0.47s
v3-direct: hello.name → "hello-2.12.3"   0.58s   (+23%)
```

7 other attr queries match TW within parity:
```
.name           v3 0.58s   TW 0.47s
.pname          v3 0.58s   TW 0.47s
.version        v3 0.59s   TW 0.47s
.meta.description v3 0.58s TW 0.46s
.outputs        v3 0.58s   TW 0.46s
.system         v3 0.58s   TW 0.46s
.type           v3 0.58s   TW 0.47s
```

## What got built

### 1. Option 4 hybrid wrapper for `derivation` / `derivationStrict`

The user's strategic note proposed four implementations.  Option 4 is
the hybrid: bytecode wrapper does phases 1-3 (attr iteration, force,
coerce-to-string), C FFI leaf does phases 4-7 (libnixstore boundary).

Concretely:
- `derivationStrict` → bytecode wrapper at `bytecode_primops.cc` →
  `__derivationFromPreprocessed` (new C primop) → shared helper
  `buildAndWriteDrvNative` in `primops.cc`.
- `derivation` → bytecode wrapper that calls `builtins.derivationStrict`
  + builds the user-facing attrset (drvPath / outPath / per-output sub-
  derivations / etc.).

The wrapper iterates `args` attrs entirely at bytecode level, calls
`builtins.toString` on each non-flag non-special-env value, builds a
preprocessed attrset, and hands it to the FFI leaf.  Per-attr work
runs as bytecode opcodes (no C-stack per iteration) — the original
C-stack overflow source on nixpkgs stdenv chains is eliminated.

### 2. Supporting infrastructure landed in this session

- **`CFF_FORCE_WB_PTR_KEEP`** (vm.cc:1300, include/v3/vm.hh:50): new
  flag for op_force_slow's writeback that KEEPS the forced value on
  the stack while ALSO memoizing into the source slot.  Used by
  OP_ATTRS_SELECT_IC (both IC HIT + IC INSTALL paths) and
  OP_ATTRS_SELECT_DYN to convert their previously C-recursive
  `forceValue(slot)` to iterative force.

- **dispatchLoop body-level try/catch** (vm.cc:1627-end): centralizes
  `clearBlackMarksOnException` cleanup so `callClosure` /
  `runOnExistingVm` can drop their try/catch and become plain
  tail-position dispatchLoop calls.  Enables compiler TCO (when the
  signature mismatch allows).

- **`builtins.seq` lower.cc fast-path** (lower.cc:1413): `seq x y`
  emits as `lowerExpr(x); ir::Force; lowerExpr(y)` directly, skipping
  the OP_CALL_PRIMOP arg-prep that would C-recursively force x.

- **`isTrueValue` defensive Tag::Slot chase** (vm.cc:539): up to 32
  Slot hops before the bool check.  Required after CFF_FORCE_WB_PTR_KEEP
  writes a Slot back into OP_NOT/OP_ASSERT's stack neighborhood under
  some retry shapes.

- **`primRemoveAttrs` WHNF element force** (primops.cc:1340): the
  names list's elements get forced before the isString check.
  Mirrors `primAttrNames` / `primCatAttrs` pattern.

- **20-case `derivation-parity.sh`** (test/derivation-parity.sh):
  byte-for-byte drvPath comparison v3-direct vs TW.  Covers
  minimal / with-args / multi-output / mixed-env / drv-cross-ref /
  buildInputs / 3-level chain / ignoreNulls / fixed-output /
  with-passthru / outpath-ref / mixed-list / 10-level chain +
  direct `builtins.derivationStrict` variants of all of the above.

## What did NOT close in this session

`(import <nixpkgs>{}).hello.drvPath` (and `.outPath`) take >30s to
evaluate vs TW's 0.64s.  This is **not** a C-stack overflow (the
architectural problem this session solved) — it's a wall-time
issue specific to the access patterns that require materializing
the full transitive derivation graph.

Diagnostics (`V3_DBG_FORCES=1 V3_DBG_FORCE_STRIDE=50000`) show:
- 200,000 forces in 10s → ~20K forces/s (TW: ~400K forces/s in
  empirical measurement)
- The single hot thunk is at `lib/customisation.nix:409:20`
  (`extendDerivation`'s `outputsList = map (outputName: ...)`)
- That single thunk is forced 64,693 times by the time 200,000
  forces total have happened — 32% of all forces are on it
- Memory grows to >1 GB
- ratio (forces / allocations) = 0.26 — allocating 4x faster than
  forcing

What this means: the bytecode `map` produces lazy entries (Tag::App)
that aren't memoized when accessed through the `extendDerivation`
overlay-and-`//`-merge pattern.  Each access re-applies the lambda.
TW caches this at the attrset-entry level; v3's bytecode iteration
doesn't — that's a *separate* (post-Phase-1) optimization.

Tried as quick fixes (none helped):
- `NIX_V3_NO_BC_MAP=1` (disable bytecode map) — WORSE (43s)
- `NIX_V3_NO_BYTECODE_PRIMOPS=1` — WORSE (21s, doesn't complete)
- `NIX_V3_NO_BC_DERIVATION_HYBRID=1` — WORSE (24s, doesn't complete)

So the hybrid wrapper IS helping; the remaining 50x gap is a deeper
memoization issue in mapAttrs-style App-entry caching.

## Rule 0 — hypothesis kills

  H1 "primDerivation*'s C-recursion is fundamentally unsolvable
  without fibers or worklist restructuring."  **FALSIFIED** via
  Option 4: the bytecode wrapper replaces every per-attr C-frame
  with a vm.frames push.

  H2 "hello.name times out at 60s+ — v3 is stuck in a busy loop."
  **FALSIFIED**: hello.name completes in 0.58s.  The earlier
  apparent slowness was a correctness bug (Tag::Slot reaching
  OP_NOT throwing "expected bool") masked as a timeout because the
  driver's output capture didn't show the error.

  H3 (NEW, OPEN): "hello.drvPath's 50x wall-time gap vs TW is in
  the mapAttrs / extendDerivation memoization protocol — bytecode
  Tag::App entries re-evaluate per-access rather than caching."
  To be verified by next investigation.

## Action plan status

- [x] Phase 0 — Hygiene (MET 2026-05-15 per ACTION_PLAN Appendix A; env-var inventory at `ENV_VAR_INVENTORY_2026-05-15.md` categorizes 167 gates KEEP=17 / RETIRE-NOW=3 / RETIRE-AFTER-X=147; the "deferred" note refers to the 3 RETIRE-NOW gates marked for post-Phase-0 follow-up, of which eager-bridge TLS was retired in commit `7c8cc6e2c`)
- [x] **Phase 1 — Iterative forceValue** (CLOSED via Option 4)
- [ ] Phase 1.5 — Workload measurement spike (no change this session)
- [ ] Phase 2 — Cycle-handling architectural decision (next phase)
- [ ] Phase 3 — Closure-pool reckoning
- [ ] Phase 4 — Decomposition + hygiene consolidation

Phase 2 candidates now include the H3 memoization investigation —
likely the highest-leverage post-Phase-1 win.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0
