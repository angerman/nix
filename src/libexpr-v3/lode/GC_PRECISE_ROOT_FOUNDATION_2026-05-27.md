# Precise-root foundation — multi-phase scope — 2026-05-27

**Date:** 2026-05-27 (morning, post user clarification "GC = memory not wall")
**Status:** **Stage 1 LANDED this turn**; Stages 2-5 scoped, not implemented
**Companion docs:** [`IDEAL_GC_DESIGN_2026-05-26.md`](IDEAL_GC_DESIGN_2026-05-26.md) §3.5 + §6.1, [`GC_DITCH_BOEHM_FALSIFIED_2026-05-27.md`](GC_DITCH_BOEHM_FALSIFIED_2026-05-27.md) (wall-perf falsifier + memory-focus reframing)

## Context

Per user choice (AskUserQuestion 2026-05-27 morning): pursue precise-root foundation as the bounded multi-session work. Goal is **memory** improvement, not wall. The 400 MB Boehm-reserved heap is the headline memory tax to recover; precise roots are the foundation enabling future moving/compacting GC that would close that gap.

## Stage 1 — `tagIsPointer` canonical predicate (LANDED this turn)

**Problem**: Tag-dispatch tables identifying which Tags carry v3-heap pointers were duplicated across `gc.cc::Scavenger::visitValue`, `postScavengeAudit`, `postScavengeBruteScan`, and `ir_dump.cc`. Disagreement between these tables historically caused missed-root bugs (Phase D / Phase E v0.1 / v0.2 / v0.3 all traced to walker-vs-allocator disagreement).

**Fix**: `include/v3/value.hh` now defines `constexpr bool tagIsPointer(Tag)` as the **single source of truth**. The function enumerates all 17 Tags explicitly:
- Pointer-bearing (7): Closure, Thunk, Attrs, List, App, PrimOpApp, Slot
- Scalar / external (10): Uninitialized, Int, Float, Bool, Null, String, Path, PrimOp, Blackhole, External

Plus convenience `valueHoldsPointer(const Value &)`.

**Why this matters**: any new walker (future GC, AOT distribution validator, snapshot serializer) gets the right classification by including value.hh; no risk of forking the table.

**Validation**: 14/14 v3 suite PASS. Build clean. No code change to existing walkers (they still use their inlined switches); migration to `tagIsPointer` is Stage 2 work.

## Stage 2 — Walker migration (~1-2 d)

**Goal**: Centralise all existing Tag dispatch on `tagIsPointer`. Eliminates the duplication risk.

**Sites to migrate**:
- `gc.cc::Scavenger::visitValue` (the nursery walker; lines 567-615)
- `gc.cc::postScavengeAudit` (post-walk validator)
- `gc.cc::postScavengeBruteScan` (BRUTE diagnostic)
- `ir_dump.cc` value-printing dispatch
- Any future walker

**Implementation**: each site keeps its dispatch (to read the appropriate `payload.*` field for forwarding), but the BRANCH CONDITION uses `tagIsPointer(t)` instead of an open-coded switch. Saves ~20 lines per site + eliminates the duplication.

Test: build + all-v3-tests PASS + brute-audit PASS post-migration (the existing parity gates already exercise the walkers).

## Stage 3 — Complete root-source enumeration (~3-5 d)

**Goal**: A reusable `walkAllV3Roots(Visitor)` function that visits EVERY live v3-heap pointer reachable from any root. Today's nursery scavenger walks only `vm.frames` + a few global roots; a full GC needs everything.

**Root sources to enumerate**:

1. **`VMState::frames`** — active call stack (already walked by scavenger)
2. **`VMState::valueStack`** — operand + local-variable stack (already walked)
3. **`VMState::withStack`** — `with` expression scope chain
4. **`standaloneCellRoots()`** — manually-registered Values (already walked)
5. **`cellOwnerTable()`** — Value-cell → owning-Thunk map (registry pointers)
6. **`v3BridgeLists()` / `v3BridgeAttrsets()`** — FFI handle tables that TW indexes into
7. **CompilationUnit constants** — `cu.constants` Value array per CU (mostly Tag::String/Path; mostly tenured)
8. **`drvHashCacheMap()`** — in-memory derivation eval result cache
9. **`primops.cc` static singletons** — `Value::vTrue/vFalse/vNull/vEmptyList/vEmptyAttrs` (immutable)
10. **C++-side transient roots** — primop bodies hold transient `Value*` on the C++ stack between forces; today these are kept-alive via Boehm conservative scan

The Visitor pattern: `walkAllV3Roots([](Value & v) { /* visit */ })`. Composes with Stage 2's `tagIsPointer` to apply the right operation per Tag.

**Falsifier check** (per `[[falsification-rule]]`): instrument the visitor to count visited pointers and cross-check against a "Boehm-saw-it" set under V3_DBG_ROOT_PARITY=1. Parity passes if Boehm doesn't find any v3-heap pointer the precise walker missed. Run on M5 + hello + HNE.

## Stage 4 — Stack maps at GC-safe points (~3-5 d)

**Goal**: At every potential GC-trigger point (allocation site, primop call boundary), know the EXACT shape of the `valueStack` — which slots hold Value structs vs which slots are uninitialized.

**Implementation**: emit.cc generates per-opcode metadata describing `valueStack` height at the start of each opcode. The Phase D barrier infrastructure already does similar tracking for `dirtyContainers`; this extends it to call boundaries.

Cost: bytecode size grows by ~1% (per IDEAL_GC_DESIGN §6.1 estimate). Adds a side-table per CompilationUnit.

**Falsifier check**: instrument the VM to validate `valueStack` height matches the emit-time map at every OP boundary under V3_DBG_STACKMAP_CHECK=1. Mismatch = bug.

## Stage 5 — GC_ROOT macros for C++ helpers (~2-3 d)

**Goal**: Replace conservative scan of C++ stack frames (Boehm's current job) with explicit registration of Value* held on the C++ stack between forces.

**Pattern**:
```cpp
void primFoo(Value * args, Value & out) {
    GC_ROOT(args[0]);  // protects args[0] across forceValue
    GC_ROOT(args[1]);
    forceValue(args[0]);
    // ...
}
```

`GC_ROOT` adds the pointer to a per-thread root set; the macro's dtor pops it (RAII).

Cost: per-call macro overhead is ~10 ns. Applied to ~100 primop bodies + ~50 helper functions = ~150 sites.

**Falsifier check**: V3_DBG_NO_BOEHM=1 disables Boehm conservative scan; tests must still pass. If any test fails, a Value* held on the C++ stack isn't registered via GC_ROOT.

## Stage 6 — Boehm-free precise GC of v3 arena (~1-2 wk)

**Goal**: With Stages 2-5 in place, replace Boehm's conservative scan with v3's precise walker for v3-allocated memory. Boehm continues to manage TW-side `nix::Value*` until the FFI is restructured.

**Expected memory savings**: dropping Boehm's reserved heap to whatever's needed for FFI-boundary values (~10-50 MB on M5 vs 402.9 MB). Net peak RSS reduction: ~350 MB.

**Falsifier check** (pre-committed SHIP gate per [[threshold-recalibration-rule]]):
- SHIP if ≥200 MB peak RSS reduction on M5 + ≤2% wall regression
- TUNE if 100-200 MB reduction
- REVERT if <100 MB reduction

## Stage 7 — Future: moving/compacting GC

Once precise roots are in place, the team can adopt Whippet (per `GC_BUILD_VS_BUY_2026-05-21.md`) or hand-roll a moving collector. That's a separate ~3-4 month project; precise roots are the prerequisite.

## What this turn lands

Stage 1 only: `tagIsPointer` predicate in value.hh. Build clean, all-v3-tests 14/14 PASS. The 8-stage plan above scopes the rest.

## Why this is "no-regret" foundation work

Per IDEAL_GC_DESIGN §3.5: precise roots are needed for:
- Compaction (moving objects to reduce fragmentation)
- Generational GC (Phase D nursery already; tenured-region replacement is next)
- Pointer compression (cuts Value from 16 B to 8 B if 4 GB heap suffices)
- Cross-process snapshot (AOT-mmap-loaded objects need precise lifetime)
- Stage 13 parallel eval (concurrent GC needs precise root tracking per thread)

Even if "ditch Boehm" is never committed, every one of these future moves benefits from a clean tagIsPointer + walkAllV3Roots foundation.

## Cross-references

- [[gc-ditch-boehm-falsified-2026-05-27]] — wall premise falsified; memory premise live
- [[memory-first-class]] — operating rule framing the SHIP gates
- [[falsification-rule]] — Rule 0 applied per-stage
- [[threshold-recalibration-rule]] — derivation methodology for per-stage thresholds
- lode/IDEAL_GC_DESIGN_2026-05-26.md §3.5 + §6.1 (original design)
- lode/BOEHM_DEPENDENCY_2026-05-21.md (dependency audit)
- lode/GC_BUILD_VS_BUY_2026-05-21.md (Whippet vs hand-roll)

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
