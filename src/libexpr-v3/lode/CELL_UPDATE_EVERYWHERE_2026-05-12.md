# Cell-Update Everywhere — Design Plan for #558 Architectural Fix

**Date:** 2026-05-12
**Status:** Design + Phase 1 prototype
**Goal:** Replace v3's partial-Bindings registry + STG WHNF recovery + chain-peek triad (S2 / S3 / S5 in COMPREHENSIVE_REPORT) with a per-thunk cell-update protocol.

## Background

The 15-commit STG ladder (ff629384b..1214a40b0) closed the libsForQt5 OP_WITH_LOOKUP cycle by accumulating partial Bindings into a per-thunk registry and using chain-peek heuristics (largest-layer-wins, registry-wide search, CFF_TAINTED) to recover from Black thunk forces during fix-point construction.

**The remaining failure** (post-ladder, traced via V3_DBG_ATTRS_HAS_KEY=isFromBootstrapFiles): a size=1 Bindings `{isFromBootstrapFiles = true}` from a bootstrap pkg's passthru is returned by v3's STG WHNF recovery when evaluating `(other-nixpkgs-pkg).passthru.isFromBootstrapFiles or false`. The chain-peek finds the bootstrap pkg's passthru entry in the OUTER pkgs thunk's chain (registered there by `publishToAllThunkFrames`'s scope=all behavior) and returns it as the answer for the unrelated pkg.

This flips `isFromNixpkgs pkg` from TRUE (TW) to FALSE (v3), which flips `lib.all isBuiltByNixpkgsCompiler …` in darwin's `allDeps` from TRUE to FALSE, which fires `lib.deepSeq resultDetails resultDetails`, which cascades through every pkg's `pkg.stdenv.cc.cc` chain → eventually forces libsForQt5 → its `inherit (pkgs) lib` thunk → forces pkgs (= lib.fix's x, BLACK) → OP_ATTRS_SELECT 'qt5' miss.

## The Fix: Cell-Update Everywhere

Replace cross-thunk partial-Bindings publication with per-thunk cell update.

### Existing infrastructure (STG-8, already implemented)

Every Thunk has a `Value * cell` field (closure.hh:129). When OP_ATTRS_REC_SET stores a thunk value into a Bindings entry, it sets `v.payload.thunk->cell = &bindings->entries[i].value` (vm.cc:6998-7020).

When OP_RETURN's CFF_THUNK_RETURN handler fires, it writes `*cell = retVal; cell = nullptr;` (vm.cc:4068-4071). This mirrors TW's in-place `forceValue` update.

### What's missing

The cell is only used at OP_RETURN time — the FINAL value is written. Intermediate state during body execution is NOT visible through the cell.

Consequence: when another thunk forces a Black thunk, the cell still holds the original `Tag::Thunk(t)` (pre-RETURN). The current workaround is the partial-Bindings registry + chain-peek.

### Phase 1: Mid-Body Cell Updates

Make the thunk's body update its own cell as construction progresses.

**At OP_ATTRS_REC_INIT (inside a thunk body):**
1. Allocate the Bindings (already done)
2. Push `Tag::Attrs{bindings}` onto stack (already done)
3. **NEW**: if the current frame is THUNK_RETURN and `fr.thunk->cell` is non-null, write `*cell = Tag::Attrs{bindings}` — the cell now points at the in-progress Bindings.

**At forceValue on a Black thunk:**
1. Check `t->cell` first (BEFORE consulting partial-Bindings registry)
2. If `t->cell != nullptr && *t->cell != Tag::Thunk(t)`, return `*t->cell`
3. Otherwise fall back to STG WHNF recovery (existing code path), which can be retired in Phase 2

The Bindings pointer is heap-stable. Subsequent OP_ATTRS_REC_SET fills entries IN PLACE. Consumers reading through the cell see the partial state via direct deref.

### Phase 2: Retire partial-Bindings infrastructure

After Phase 1 covers all the legitimate use-cases:
- Delete `publishToAllThunkFrames` / `publishToNearestBlackThunkFrame`
- Delete `partialBindingsRegistry`
- Delete `pickLargestLayer` / `lookupInPartialChain` / registry-wide chain peek
- Delete `CFF_TAINTED` and STG WHNF recovery code
- Optionally fold `OP_ATTRS_REC_INIT_TAIL` / `OP_ATTRS_UPDATE_TAIL` back into base opcodes

### Why this fixes the pollution

The pollution mechanism was: `publishToAllThunkFrames` registers `OP_ATTRS_REC_INIT` results with ALL active THUNK_RETURN frames, so one pkg's passthru ends up in another pkg's chain.

With cell-update, each thunk's cell tracks ONLY its own body's progress. Consumers must specifically chase the slot to the cell of the thunk they want to read — there's no global registry to mis-match.

When `pkg.passthru` is forced:
- Today: STG WHNF recovery picks `pickLargestLayer(pkg.passthru.thunk.chain)`. Chain has cross-thunk pollution → wrong Bindings.
- After Phase 1: Read `*pkg.passthru.thunk.cell`. Cell holds the partial Bindings produced by THIS thunk's body, or `Tag::Thunk(t)` if body hasn't started. No cross-thunk source.

## Phase 1 prototype: minimal change

The minimal change to test the approach:

1. **vm.cc OP_ATTRS_REC_INIT handler:** after constructing the Bindings, if the running thunk has a cell, write `*cell = Tag::Attrs{bindings}`.

2. **vm.cc forceValue Black handler:** before falling into STG WHNF recovery, check `t->cell`. If non-null AND `*t->cell` is not `Tag::Thunk(t)` (i.e., the cell has been updated), return `*t->cell`.

Gate the new behavior with `NIX_V3_CELL_EVERYWHERE=1` for bisection. Default-off until validated.

## Tests

**Regression**: must not break:
- `run-558-emit-order-tests.sh` (4 tests)
- `run-inherit-from-laziness-tests.sh` (16 tests)
- `run-fix-inherit-from-self-tests.sh` (5 tests)
- `run-direct-eval-tests.sh` (26 tests)

**Positive (target)**: 
- `(import nixpkgs {}) ? lib` → returns true (TW parity)
- `(import nixpkgs {}).hello.name` → returns "hello-2.12.2" (TW parity)

**Negative**: with cell-everywhere, the chain-peek diagnostic should report ZERO recoveries for the v3-direct nixpkgs flow (compare V3_DBG_BLACKHOLE_AS_VALUE hits count).

## Risk

- **Wrong-shape reads**: a consumer that reads the cell mid-construction sees a partial Bindings. If the consumer enumerates entries (e.g., `builtins.attrNames`), it sees fewer keys than the final. Acceptable for laziness — TW also throws on Black, and lazy attr reads only force specific entries.
- **Cell-aliasing across publications**: must verify that OP_ATTRS_REC_INIT doesn't re-allocate the Bindings* (it doesn't — the Bindings* is fixed at allocation, and OP_ATTRS_REC_SET mutates entries in place).

## Estimate

- Phase 1 prototype: ~1 day (changes to OP_ATTRS_REC_INIT + forceValue Black branch).
- Phase 1 hardening: ~2 days (run all tests, fix edge cases, document).
- Phase 2 deletion: ~1-2 days (mechanical cleanup; harder to validate than to write).
- Phase 3 validation: ~1 day (full nixpkgs + cardano-node v3-fhook bench).

## Phase 1.5 design challenge (2026-05-12 follow-up)

After committing the Phase 1 prototype (commit `421b97069`), we tried Phase 1.5 (pre-allocating cells at MAKE_THUNK so the outer `x` thunk has a cell to read).  We hit a structural conflict:

**The existing STG-8 cell mechanism is dual-purpose:**

1. **In-place parent slot update**: `OP_ATTRS_REC_SET` sets `child.cell = &parent.bindings.entries[i].value` so that at child's `OP_RETURN`, `*cell = retVal` updates the parent's entry IN PLACE.  This is TW's `forceValue(*v)` semantics — consumers reading the parent's entry get the updated value automatically.

2. **"This thunk's value lives at this stable heap location"**: any code that wants to deref the value via a stable pointer can use `Tag::Slot(cell)`.

If we pre-allocate `cell = allocValue()` at MAKE_THUNK, then OP_ATTRS_REC_SET (which only sets cell when nullptr) skips, and the cell stays pointing at a STANDALONE heap Value instead of the parent's entry slot.  At OP_RETURN, `*cell = retVal` updates the standalone Value, but the parent's entry stays at `Tag::Thunk(t)` — consumers reading the parent's entry pay an unnecessary force + chase.  STG-8's in-place update is broken.

**Resolution options:**

A. **Add a separate `shapeCell` field to Thunk** for the in-progress cell.  `cell` stays for STG-8's parent-slot semantics; `shapeCell` is the new pre-allocated heap Value.  forceValue Black reads `shapeCell` (if non-null and updated past the sentinel).  OP_ATTRS_REC_INIT updates `shapeCell` of the innermost THUNK_RETURN frame.  +16 bytes per Thunk, no STG-8 conflict.

B. **Flip OP_ATTRS_REC_SET to override unconditionally** (delete the `cell == nullptr` precondition).  Pre-allocate at MAKE_THUNK; OP_ATTRS_REC_SET still wins.  Pro: same Thunk size.  Con: shared thunks (a literal used in two parent entries) — only the LAST parent's entry would get the cell, breaking the FIRST parent's slot-update.  Currently rare; need to audit.

C. **Tag::Slot wrapper indirection**: don't store thunks directly in entries.  Always wrap as `Tag::Slot(cell)`.  Consumer reads slot → reads *cell → gets Thunk or final value.  More allocations but cleanest semantically — matches the existing STG-7 lambda-parameter slot pattern.

**Recommendation:** Start with **option A** (separate shapeCell field).  Lowest risk, no regression to STG-8, fastest to validate.  Promote to option C in Phase 2 if it cleans up further.

## Next session entry point

Resume by implementing option A:

1. **closure.hh**: add `Value * shapeCell` next to `Value * cell` in `struct Thunk`.
2. **alloc.hh**: `allocThunkSuspended` allocates `shapeCell = allocValue()` and initializes `*shapeCell = Tag::Thunk(t)`.  Gated `NIX_V3_CELL_EVERYWHERE=1` via a static getenv check (or always-on if memory cost is acceptable).
3. **vm.cc OP_ATTRS_REC_INIT**: update logic now writes to `fr.thunk->shapeCell` (not `cell`).
4. **vm.cc forceValue Black branch**: read `t->shapeCell` (not `t->cell`).
5. **vm.cc OP_RETURN**: clear `shapeCell` like `cell` is cleared (read-once).
6. **Disk-cache schema bump** if Thunk size changes affect serialized lambda metadata.

Estimated: ~1 day for option A.  Then validate v3-direct nixpkgs eval.  If cascade closes, proceed to Phase 2 (retire partial-Bindings registry).

---

## Phase 1.5 landed + scope decision (2026-05-12 final)

Phase 1.5 option A implemented as commits:
- `20adafe31`: Thunk::shapeCell field, allocation in allocThunkSuspended.
- `0677e7cd8`: 12-test cell-update protocol semantic suite.
- `ead3f33ae`: STG-correct innermost-only update; dropped the cross-thunk propagation hack from `52eb8f261`.

**Decision (user directive 2026-05-12):** do NOT take the cross-thunk propagation shortcut.  That re-introduces the same shape of pollution as the partial-Bindings registry and is fundamentally non-STG.  STG semantics: a thunk's cell holds only that thunk's own state; cycles throw.

Current state under `NIX_V3_CELL_EVERYWHERE=1`:
- Each thunk has its own shapeCell.
- OP_ATTRS_REC_INIT writes the in-progress Bindings to the INNERMOST THUNK_RETURN frame's shapeCell (and no others).
- forceValue Black reads `*t->shapeCell` before falling through to STG WHNF recovery.
- Per-thunk cell update is STG-correct; no cross-thunk pollution.

Tests pass under both modes.  v3-direct nixpkgs still fails the same way as before — shapeCell recovery doesn't fire for the outer x thunk because x's body doesn't directly fire OP_ATTRS_REC_INIT (its body just calls f), so x's shapeCell stays at the sentinel.  Recovery falls through to legacy STG WHNF (which still has the cross-thunk pollution).  This is correct STG behavior — the residual failure now demonstrates that the cascade IS a real cycle from STG's perspective, and the right fix is to eliminate the eager forcing that produces the cycle, not to paper over with cross-thunk publication.

## Proper architectural plan (post Phase 1.5)

| Phase | Work | Goal |
|---|---|---|
| 1.5 ✅ | Thunk::shapeCell + innermost-only update | STG-correct per-thunk cell update foundation |
| 2 | Make inherit-from unconditionally lazy in lower.cc (match TW's `from->maybeThunk`) | Eliminate the eager-force cycles that the partial-Bindings registry currently masks.  Investigate and fix the perf-hang of NIX_V3_INHERIT_FROM_THUNK_ALL=1. |
| 3 | Retire partial-Bindings infrastructure | After Phase 2 closes the legitimate-cycle cases, delete `publishToAllThunkFrames`, `publishToNearestBlackThunkFrame`, `partialBindingsRegistry`, chain peek, `pickLargestLayer`, `lookupInPartialChain`, `CFF_TAINTED`, STG WHNF recovery.  Mechanical deletion + test pass. |
| 4 | Validation | Full nixpkgs eval, cardano-node v3-fhook bench. |

Phase 2 is the critical path.  The hang with `NIX_V3_INHERIT_FROM_THUNK_ALL=1` is the obstacle; investigating that is the next concrete action.  Once inherit-from is properly lazy, the cascade disappears.

## Why not the cross-thunk hack?

We tested cross-thunk shapeCell propagation (Phase 1.5b, since reverted).  It DID change the cascade's error symptom (eliminated the bootstrap-passthru pollution) but introduced a new "OP_ATTRS_SELECT: not an attrset" failure.  The root cause: writing the same inner Bindings to all outer thunks' shapeCells means an outer thunk's "value" is observed as the inner Bindings, even though the outer's eventual value is something else (the // of the inner with other contributions).

This is the same shape of mistake as the partial-Bindings registry.  It can sometimes succeed because the inner Bindings happens to contain enough state, but it's semantically wrong — any consumer that reads a non-existing key, or that reads while the inner Bindings is being mutated, gets corrupt data.

The proper fix is to ensure no thunk is forced while it's mid-construction (no real cycle in TW-equivalent eval).  That requires laziness — which Phase 2 addresses.

---

## Phase 2 perf investigation (2026-05-12)

`NIX_V3_INHERIT_FROM_THUNK_ALL=1` IS functionally correct.  10 v3 regression suites pass under it (see `run-thunk-all-regression-tests.sh`).  The blocker is per-force perf cost at nixpkgs scale.

### Measured baselines

| Workload | TW | v3 default | v3 + THUNK_ALL |
|---|---|---|---|
| `1+2` | 0.05s | 0.05s | 0.05s |
| `import nixpkgs/lib` (462 attrs) | 0.06s | 0.05s | 0.05s |
| `lib.systems.parse.mkSystemFromString "x86_64-linux"` | 0.07s | 0.08s | 0.08s |
| `foldl' 10K ints` | 0.05s | 0.04s | 0.04s |
| `(import nixpkgs {}) ? lib` | 0.5s | 0.6s (error) | **>120s (timeout)** |
| `(import nixpkgs {}).hello.name` | 0.7s | 0.6s (error) | **>120s (timeout)** |

THUNK_ALL is COMPETITIVE with TW on isolated workloads.  The 100x+ gap only manifests on full nixpkgs bootstrap.

### Allocation profile under THUNK_ALL (30s of nixpkgs eval)

```
110000 forces, 416278 thunks allocated (ratio 0.264)
arena 224MB
hot descriptor: <thunk> at lib/systems/parse.nix:64:44
  = wrap-thunk for `{inherit name;} // value` (function-arg in setType)
  88858 forces (80% of all forces)
```

Key observation: the hot thunk is NOT a THUNK_ALL-induced wrap-thunk.  It's the regular function-argument thunkification that exists in BOTH modes.  THUNK_ALL just lets the eval RUN LONGER because the cycle is eliminated; default mode fails fast.

So the perf gap isn't "THUNK_ALL adds extra thunks for inherit-from".  It's "v3's per-thunk-force cost is X, and nixpkgs has a LOT of thunks to force during stage iteration".

### Optimization candidates (ranked)

1. **Memoization audit**: ratio 0.264 = 4x more thunks allocated than forced.  3x of allocated thunks are NEVER forced.  Are we creating wrap-thunks for entries that get short-circuited?  E.g., `inherit (X) a b c d e` creates 5 entry-thunks; if consumer only accesses `a`, 4 entry-thunks are wasted.  Reduce allocation by hoisting shared decisions to lower-time.

2. **Inline arg-thunkify for trivial bodies**: the hot wrap-thunk `{inherit name;} // value` body is 7 ops.  Each force pays dispatchLoop frame push/pop overhead (~50-200ns).  Replace MkThunk with a "lazy cell" that has the body bytecode inlined into the caller's bytecode at force point.  Saves ~half the per-force overhead.

3. **Thunk-pool / nursery**: v3's nursery is already Cheney-style but Thunk allocations may not be in the nursery hot path.  Verify via gc.cc which allocations go through nursery vs threadArena.

4. **OP_FORCE fast path**: when forceValue receives Tag::Thunk(t) where t is already Evaluated, return t->evaluated immediately.  Current code goes through full chase loop.  Add inlined fast-path check at OP_FORCE.

5. **Skip OP_RETURN cell-update writes** when both cell and shapeCell are nullptr.  Two conditional branches saved per OP_RETURN.

6. **Inline single-entry attrset construction**: `{inherit name;}` builds a Bindings(1) every time.  Pool small Bindings or use stack-allocated lookup.

### Phase 2 plan (revised, multi-session)

| Sub-phase | Work | Acceptance |
|---|---|---|
| 2a | Run `run-thunk-all-regression-tests.sh` in CI to lock functional correctness | All 10 suites pass |
| 2b | Pick optimization #1 (memoization audit) — find where wasted thunks come from | Allocation count down by 30%+ |
| 2c | Pick optimization #2 (inline trivial arg thunks) | Per-force cost down by 50%+ |
| 2d | Re-time `pkgs ? lib` under THUNK_ALL | Within 5x of TW |
| 2e | Iterate until within 1.5x | Within 1.5x of TW |
| 2f | Flip THUNK_ALL default-on; retire partial-Bindings | Cascade closes |
| 2g | Run full nixpkgs `hello.name` end-to-end | Returns `"hello-2.12.2"` |

Each sub-phase commits independently with regression gate.  No flag flips until 2f.
