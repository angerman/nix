# Stage 6 production precise GC — 2-3 wk session handoff design

**Status**: Ready-to-execute design for a focused multi-week effort.
**Estimated effort**: 2-3 weeks end-to-end (design final + impl +
  validate + measure).
**Prerequisite chain**:
  1. Stage 1 (`tagIsPointer`) ✓ commit `6f854fa2c`
  2. Stage 3 (`walkAllV3Roots`) ✓ commits `02c95eba0` + `e7639f837`
  3. Stage 5 MVP (`GcRoot` RAII) ✓ commit `173481af3`
  4. **Arena deregistration spike** (per
     [`ARENA_DEREGISTRATION_DESIGN_2026-05-27.md`](ARENA_DEREGISTRATION_DESIGN_2026-05-27.md))
  5. Stage 5 bulk apply to ~150 primop C++ helper sites
  6. This document
**Validated by**: Stage 6 SPIKE (commit `f3491859f`) — 239 MB freeable
  on hello.drvPath, 797 MB freeable on HNE (4× SHIP threshold).

## 1. The goal

Replace the Arena's bump-only allocator with a precise GC that can
reclaim dead cells DURING eval, not just at process exit.

Target memory delta (per Stage 6 SPIKE):
* hello.drvPath: 239 MB peak RSS reduction (SHIP-GREEN)
* HNE:           797 MB peak RSS reduction (SHIP-GREEN, 4× threshold)

Constraint: peak_rss measurement was end-of-run residual — a LOWER
BOUND on what mid-eval GC could reclaim.  Production GC firing mid-
eval will reclaim at least these amounts; potentially more.

## 2. Design space — three viable allocator/GC shapes

The fundamental design question: how does sweep KNOW where the cells
are and how big each is?

### Option A: Cheney semispace (moving / copying)

Two arenas (T1, T2).  Allocate from T1.  On major GC: walk
reachability via `walkAllV3Roots`, copy each live cell from T1 to T2,
update all pointers to forwarded addresses, free T1.  Swap T1 ↔ T2.

**Pros**:
* No per-cell type tags required — visit + copy via known field types
* Automatic compaction — no fragmentation
* The Stage 5 RootVisitor pattern's `& slot` references already
  support pointer rewriting (`v.payload.bindings = fwdBindings(...)`)
* gc.cc's existing Scavenger DOES THIS for the nursery — patterns
  are validated

**Cons**:
* 2× peak memory transiently during copy phase (T1 + T2 both live)
* String/Path `const char*` payloads need separate handling
  (they're not v3 cells, but pointers point INTO arena chars)
* `Closure::cu` external pointers (CompilationUnit*) point INTO
  ImportCache deque (libc) — must NOT be rewritten
* `Slot` pointers (Value*) point INTO cells — must be rewritten
  to the cell's NEW address after copy

**Architecturally compatible**: Stage 7 (compacting GC) was already
in the roadmap as a separate ~3-4 mo project.  Option A makes
Stage 6 LOOK LIKE Stage 7 lite.

### Option B: Mark-sweep with per-block free-list

Each cell carries a per-cell header (type tag + size).  Mark phase
walks reachability + sets mark bits.  Sweep phase walks each block
linearly, frees unmarked cells onto a per-size-class free-list.

**Pros**:
* Cells don't move — no pointer rewriting
* C++ stack roots without GC_ROOT are safe (pointers stay valid)
* No 2× peak memory transient

**Cons**:
* +4-8 B header per cell (~25-50% size overhead on the 16 B Value!)
* Fragmentation — free-list slots may not match new allocation sizes
* Sweep cost ∝ arena size (not live size) — expensive for sparse heaps

### Option C: Per-type segregated arenas with per-block mark bits

Separate arena per type (`closureArena`, `thunkArena`, `bindingsArena`,
...).  Each type has uniform cell size (modulo FAM tail).  Per-block
mark bitmap.

**Pros**:
* No per-cell header — type known from arena ownership
* Mark-sweep per type without size tags
* Bindings still need variable size handling (FAM) — size-class
  buckets or per-allocation size

**Cons**:
* Major refactor of `allocClosure` / `allocThunk` / `allocBindings`
* Cells of different types can no longer be adjacent (locality cost)
* FAM size variation in Bindings/Closures/Thunks complicates bucket
  decisions

### Recommended: Option A (Cheney semispace)

Rationale:
1. **Pattern already validated** in gc.cc's nursery scavenger.
2. **No per-cell overhead** — keeps Value tight at 16 B.
3. **Automatic compaction** — better cache behaviour post-GC.
4. **Forward-compatible with Stage 7** — semispace IS the moving GC.
5. **Pointer rewriting infrastructure already present** in the
   RootVisitor pattern (`& slot` parameter is rewritable).

The 2× transient memory concern is real but bounded: the worst-case
spike is brief (during the copy phase), and the spike SHRINKS to
live-size after the swap.  Net peak across the eval is `max(pre-GC,
live + new-T2)` which is approximately `live + (current allocation
rate × time-between-GCs)`.

## 3. Implementation plan (~2 weeks)

### Week 1: extend nursery scavenger to "major"

The current Phase D/E nursery scavenger already walks from roots +
forwards into a survivor pool.  Generalize to a "major" mode that
walks the FULL tenured arena.

Tasks:
1. **Refactor Arena to dual-region** (T1, T2):
   - Add `Arena::activeRegion` + `Arena::backupRegion`
   - `alloc()` uses activeRegion
   - `swapRegions()` for the swap post-copy
   - Each region is a vector of blocks like today, just one per region
2. **Generalize fwd-functions** in gc.cc:
   - `fwdClosureMajor()` copies from T1 to T2 (not nursery → tenured)
   - Maintain a forwarding table (`unordered_map<old_addr, new_addr>`)
   - On copy: install a sentinel in the OLD cell (`payload.raw = new_addr`
     with a NEW Tag like Tag::Forwarded) so future walks find new addr
3. **Pointer rewriting in walkAllV3Roots**:
   - The RootVisitor's `& slot` parameter is already rewritable
   - Add a `MoveGCVisitor` subclass that rewrites every visit to
     point at the copied cell
4. **Special-case fields that are NOT v3-arena pointers**:
   - `Closure::desc` (LambdaDescriptor*) — libc, no rewrite
   - `Closure::cu` (CompilationUnit*) — libc, no rewrite
   - String/Path `const char *` — see §3.1 below
   - External `void *` — see §3.2 below

### Week 2: validation + tuning

1. **`--core` 15/15 PASS** under the gate (e.g., `NIX_V3_MAJOR_GC=1`)
2. **HNE + cardano-node M5 byte-identical** to TW
3. **Measure**: peak_rss delta on hello.drvPath / HNE — meet the
   pre-committed thresholds (§5 below).
4. **Tune trigger**: when does major GC fire?  Candidates:
   - Per N allocations (e.g., every 1M cells)
   - At threshold (e.g., when T1 reaches 256 MB)
   - At safe-points (between bytecode opcodes; primop bodies excluded)
5. **Validate stress mode**: run with aggressive GC (`MAJOR_GC_STRESS=1`)
   on hello.drvPath + see no crashes / no test regressions

### Week 3: ship + retirement criteria

1. Default-on after validation: `NIX_V3_NO_MAJOR_GC=1` becomes the
   opt-out (mirrors Phase D's gate retirement pattern).
2. Update SESSION_ARC + roadmap docs.
3. Retire the env-var gate when default-on has soaked for 2 weeks
   of nixpkgs eval + a clean cardano-node M5 measurement.

## 3.1 String / Path pointer handling

`Tag::String` / `Tag::Path` Values hold `const char *`.  Three
source categories:

1. **Arena chars** (`Alloc::allocChars`) — POINTS INTO the moving arena.
   Must be tracked + rewritten OR moved to a non-moving region.
2. **globalSymbolTable / libexpr symbols** — libc malloc, immortal.
   No action.
3. **TW-bridged strings** — Boehm-managed.  No action (Boehm sees
   them via the bridge-root registry from arena dereg).

Decision: **Move arena chars to a separate non-moving region**.
Chars are typically immortal (string literals + interned symbols)
and don't need GC at all — they can live in a separate bump-pointer
arena that's never reclaimed.  Removes them from the moving-GC
problem space.

Implementation: add `Arena::charsArena` (separate region).  `allocChars`
allocates from charsArena.  No Boehm registration needed (no Boehm-
managed pointers stored).

## 3.2 External tag handling

Tag::External Values store `void *` pointing to TW-owned external
values (per the arena-dereg audit §4.2).  Like bridge sources, these
are Boehm-managed and need separate registration.

Decision: ban Tag::External in v3 cells.  If TW bridges an External,
materialize it (force to concrete Tag) or bridge-thunk-wrap it.
This may require touching the TW-to-v3 bridge in `treeWalkerToV3()`.

Alternative: register External payloads in an externals-registry
side-table parallel to the bridge-source registry.

## 4. Safe-point model

The current Phase D/E scavenger fires at `exitDepth == 0` — the
dispatch loop's "between bytecode opcodes" state.  Stage 6 major GC
SHOULD use the same safe-point model:

* Fire only between opcodes
* Skip when inside a primop body (helper functions hold non-GC_ROOT
  Value*s)

This matches the gc.cc Scavenger's existing constraint.  Stage 5's
GC_ROOT infrastructure is forward-looking for a future model that
fires mid-primop, but Stage 6 doesn't need that yet.

## 5. Pre-committed thresholds (per [[measure-twice-cut-once]])

### Per-workload SHIP gate

| Workload      | SHIP threshold      | TUNE        | FALSIFY    |
|---------------|---------------------|-------------|------------|
| hello.drvPath | ≥ 200 MB ↓ peak_rss | 100-200 MB  | < 100 MB   |
| HNE           | ≥ 600 MB ↓ peak_rss | 300-600 MB  | < 300 MB   |
| cardano-node M5 | ≥ 500 MB ↓ peak_rss | 200-500 MB | < 200 MB |

### Wall regression budget

≤ 5 % wall regression on every workload.  Major GC pauses must be
bounded — long pauses (> 50 ms) are a UX problem even at low
per-pause frequency.

### Correctness gate

* `--quick` 6/6 PASS
* `--core` 15/15 PASS
* `--full` PASS (the longer suite that includes brute audit)
* HNE + hello.drvPath + hello.name + firefox.name byte-identical
  to TW
* cardano-node M5 byte-identical to last-known-good baseline

If ANY of these fails, FALSIFY (commit revert + measurement doc).

## 6. Pitfalls to avoid

### Per this session's prior-art findings

1. **Don't repeat ChainBindings Phase C's mistake** — the 4-condition
   revival prerequisite applies broadly: get a minimal repro of any
   failure mode BEFORE adjusting policy.  If Stage 6 fails on a
   nixpkgs eval, build a minimal repro first.

2. **Don't assume the Stage 5 GC_ROOT bulk-application is needed**
   if the safe-point model excludes primop bodies.  Validate the
   safe-point model first (§4); add GC_ROOTs only if measurements
   show primop-internal allocations triggering GC.

3. **Don't ship without the External + String audits** (per arena-
   dereg design §4).  These audits are SHARED PREREQUISITES — finish
   them in the arena-dereg session, then Stage 6 inherits the
   audit conclusions.

### Specific Stage 6 hazards

4. **Forwarding-pointer Tag must be unique** — adding Tag::Forwarded
   requires updating every `tagIsPointer` consumer + Stage 1's
   static_asserts.  Or use a sentinel value in the OLD cell that
   distinguishes "this is a forwarding pointer" without a new Tag.

5. **The 2× transient memory hazard** — for big workloads on
   memory-pressured systems, major GC's copy phase could trigger
   OOM.  Mitigations:
   - Trigger major GC at lower watermarks
   - Stream the copy (per-block, not whole-arena)
   - Fall back to mark-sweep if T2 allocation fails

6. **Pointer-stability for foreign references** — `Closure::cu`
   points to a CompilationUnit in ImportCache::cus.  If a Closure
   moves but its cu pointer stays valid, that's fine.  But if a
   future change moves CUs (e.g., LRU eviction per Phase 4b),
   this assumption breaks.  Document the contract.

## 7. Cross-references

* `LIVE_FRACTION_SPIKE_2026-05-27.md` — Stage 6 ROI validation
* `HNE_BUCKET_DECOMP_2026-05-27.md` — per-workload baseline
* `ARENA_DEREGISTRATION_DESIGN_2026-05-27.md` — prerequisite
* `GC_PRECISE_ROOT_FOUNDATION_2026-05-27.md` — 7-stage roadmap
* `CHENEY_NURSERY_DESIGN.md` — existing semispace pattern in nursery
* `BOEHM_DEPENDENCY_2026-05-21.md` — Layer-1 status
* `IDEAL_GC_DESIGN_2026-05-26.md` §3.5, §6 — original Stage 6 framing
* `[[memory-first-class]]` — SHIP gate origin
* `[[measure-twice-cut-once]]` — methodology
* `[[falsification-rule]]` — Rule 0

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
  Input Output Group.
SPDX-License-Identifier: Apache-2.0
