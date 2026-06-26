# BiBOP per-type block-pool allocator — design (S2.2 RSS foundation)

**Date:** 2026-06-26  **Branch:** angerman/2.35-eval-profiling-v2
**Goal:** make the (correct, S2.1b-validated) mid-eval moving compactor PROFITABLE by
giving it density variance to exploit — segregate tenured cells by CellType so
high-mortality types (Closure 81% / Bindings 75% / List 73% dead at firefox peak) cluster
into SPARSE blocks that compaction can empty + munmap. Grounded in three subsystem maps
(arena/alloc/immix, 2026-06-26).

## 0. Why (the measured premise)

`lode/S2_2_DENSIFY_MORTALITY_2026-06-26.md`: per-type mortality spans 17→81% at firefox
peak, but blocks are uniformly ~50% live because every block mixes high- and low-mortality
types. `lode/S2_2_BLOCK_FREE_RCA` + the PCT-sweep git-note (e8c8f8a58): the current evac is
RSS-NEGATIVE at every PCT because it bump-allocates dest into FRESH blocks (churn), and
firefox has ~no sparse blocks to recycle into. BiBOP manufactures the sparsity; per-type
Immix recycling realizes it.

## 1. Substrate (from the maps — what we build on)

- **16 MB blocks, kAlign=16, kLineBytes=128, 131072 lines/block.** NO in-block header;
  per-block metadata lives in parallel side-table vectors in `Region` indexed by block:
  `cellStarts` (128KB/blk), `cellTypes` nibble-packed (512KB/blk), `lineMarks` (16KB/blk),
  `freeSpans` (variable). Helpers: cellTypeUnpack/Pack, isCellStart, cellTypeAt,
  findContainingCellStart, markLinesForCell, anyMarkInRange (alloc.hh / mark_sweep.cc).
- **Single active block, bump allocator.** `Arena::alloc(size, CellType)`: huge cutoff
  (>4MB → hugeBlocks) → Immix span (gated `g_immixAllocEnabled`, off) → free-list bin
  (gated, off) → **bump (default)**. Stamps cell-start bit + CellType nibble iff
  `cellMetaEnabled()` (= majorGcEnabled||midEvalGc).
- **refill()**: `majorGcEnabled() ? mmap : calloc`. majorGcEnabled is HARD FALSE → blocks
  are **calloc'd** by default. `freeWholeBlock()` munmaps (assumes mmap'd) + erases the
  block's parallel metadata. **⇒ munmap-under-mid-eval is the open prerequisite** (S2.1
  reported freedRSS=16.8MB under mid-eval — VERIFY how, since calloc'd blocks can't munmap;
  likely the evac dest blocks are special or freedRSS is mis-attributed).
- **Immix recycling EXISTS but is gated off**: `rebuildFreeSpansFromLineMarks` (post-sweep)
  stores per-block free spans; `g_immixAllocEnabled` would serve allocs from them.
  `NIX_V3_IMMIX_RECYCLE_PCT` skips low-dead blocks. The evac calls `forceFreshBlock()`
  (mark_sweep.cc:1828) which NULLS the Immix cursor → dest always bump-fresh (the churn).
- **Alloc entry points**: allocClosure / allocThunkSuspended(+Shared) / allocBindings(+Chain)
  / allocList / allocPair / allocValue / allocChars / allocEnv. nurseryOrArena: Closure,
  Thunk, List. Tenured-only: Bindings, Pair, Chars, Value, Env.
- **★ PREREQUISITE BUG (found by the map)**: the Cheney scavenger's `fwdClosure/fwdThunk/
  fwdBindings/fwdList/fwdPair` (gc.cc) call `threadArena().alloc(bytes)` **without the
  CellType arg** → promoted tenured cells are stamped `CellType::None`. So a large fraction
  of tenured cells are UNTYPED, which (a) blocks per-type routing and (b) already forces the
  mid-eval mark into conservative byte-scans for them. Fixing this is prerequisite + an
  independent correctness/perf win.

## 2. Architecture decision: single Arena, per-type LANES (not separate Arenas)

Add a per-CellType "lane" = {active block + bump cursor + Immix span cursor + the set of
blocks owned by this type} INSIDE the existing single `Region`. Rejected: separate `Arena`
instances per type (duplicates all side-table machinery, block registration, huge path,
the conservative-scan integration — huge blast radius). The lane approach reuses ALL
existing per-block metadata (cellStarts/cellTypes/lineMarks/freeSpans stay per-block; a
block just belongs to one lane) and the conservative-scan/Boehm registration unchanged.

**Which types get a lane (the high-mortality, movable, high-volume ones):** Thunk,
Bindings, List, Closure, Pair. **Shared "cold" lane:** Chars (variable-size, 38% mort,
reloc-broken — keep pinned for now), Env (17% mort, mostly live), Value (rare). Huge cells
(>4MB) stay on the separate hugeBlocks path. A lane is chosen by `CellType` at alloc.

**Metadata shrink (optional, B1.3):** if a block is single-type (BiBOP guarantees it), the
512KB/block `cellTypes` nibble array collapses to ONE per-block type tag → frees ~tens of
MB of metadata on firefox/M5-class arenas. Defer until correctness lands.

## 3. Per-type Immix recycling (the profit mechanism)

Post-sweep, rebuild free spans PER LANE. The evac, when relocating a sparse block of type
T, allocates dest into lane-T's NON-CANDIDATE partial blocks' free spans (NOT fresh, NOT
the block being evacuated). Candidate (being-evacuated) blocks are excluded from the span
pool so dest never lands in a block we're about to free. Then sparse source blocks empty →
freeWholeBlock → munmap. This requires removing `forceFreshBlock()` for the per-lane path
and activating the (existing) Immix span allocator scoped to the lane.

## 4. Risks / open questions (must resolve in B0)

1. **munmap-under-mid-eval**: blocks are calloc'd (majorGcEnabled false) → freeWholeBlock's
   munmap is invalid. Must mmap lane blocks (decouple block-alloc-method from majorGcEnabled)
   — B0.2. Verify the S2.1 16.8MB-freed mechanism first.
2. **Realizable ceiling**: per-type mortality ≠ guaranteed per-block sparsity if a type's
   deaths are time-uniform (each lane-T block ~T%-dead uniformly → still needs 2:1 compaction,
   not free-outright). B0.3 must PROJECT the realizable net RSS + pre-commit a ship threshold
   before building the full allocator (Rule 0 go/no-go).
3. **evacChars** (S2.1a corruption) still broken — Chars lane stays pinned (acceptable: 38%
   mort, ~1.2% arena); revisit only if Chars-block-free is needed.
4. **CPU cost**: N active blocks (one per lane) = N partially-filled tail blocks
   (fragmentation ~N×8MB worst case) + per-alloc lane-routing branch. Must measure (B3.2).
5. **Scavenger three-hop** (young→survivor→tenured): the CellType must survive all hops
   (B0.1).

## 5. Phased task plan (added to the todo list as B0.1–B3.3)

**Phase B0 — prerequisites + go/no-go (do NOT build the allocator until B0.3 passes):**
- **B0.1** Fix the scavenger CellType-stamp bug: `fwdClosure/fwdThunk/fwdBindings/fwdList/
  fwdPair` pass the correct CellType to the tenured alloc. Validate byte-id + --brute;
  measure the mid-eval conservative-byte-scan reduction (independent win).
- **B0.2** Decouple block-alloc-method from `majorGcEnabled` → mmap tenured blocks under
  mid-eval so `freeWholeBlock` can munmap. Verify the current S2.1 munmap path first. Gate
  `NIX_V3_BIBOP` (default off); byte-id + --brute.
- **B0.3** GO/NO-GO projection: instrument a static "if cells were type-segregated, how many
  blocks per type go sparse (<25%/<50% live) → realizable munmap MB" on firefox + M5
  (darwin-4). PRE-COMMIT a ship threshold (e.g. ≥80 MB net firefox peak-RSS). If below, STOP
  (document; the lever is below the bar) — do not build B1+.

**Phase B1 — per-type allocation lanes (gated NIX_V3_BIBOP):**
- **B1.1** Add per-CellType lane struct to `Region` (active block + bump + immix cursor +
  owned-block set); type→lane routing in `Arena::alloc`. Lanes for Thunk/Bindings/List/
  Closure/Pair; shared cold lane for Chars/Env/Value.
- **B1.2** Route all tenured allocators + the (B0.1-fixed) scavenger promotions + nursery-
  overflow-to-tenured through the right lane. byte-id + --brute.
- **B1.3** (optional) Single-type-block tag → collapse the 512KB/blk cellTypes array to one
  per-block tag; measure metadata RSS saved. byte-id + --brute.
- **B1.4** Validate B1: byte-id (hello/git/gcc/firefox) + --brute 22/22 + measure arena
  layout (blocks-per-lane, per-lane density histograms — confirm the variance appeared).

**Phase B2 — per-type Immix recycling evac (make it profitable):**
- **B2.1** Per-lane `rebuildFreeSpansFromLineMarks` (scope spans to the lane's blocks).
- **B2.2** Evac dest → recycle into lane-T's non-candidate partial blocks (remove
  forceFreshBlock for the lane path; activate lane-scoped Immix span alloc; exclude candidate
  blocks). byte-id + --brute under 1MB-nursery stress (re-uses the S2.1b blackhole-pin +
  NO_CONSERV_SCAN + the deterministic nix-develop A/B harness).
- **B2.3** Per-lane candidate selection (evacuate each lane's sparse blocks into its denser
  partials). Tune the per-lane PCT.
- **B2.4** Validate B2: --brute 22/22 + the firefox full-compaction A/B (byte-id, bhThunks>0
  handled) + measure blocksFreed/freedRSS per lane (must be NET-POSITIVE now).

**Phase B3 — measure + ship:**
- **B3.1** darwin-4 net peak-RSS: firefox + M5 + HNE, BiBOP+recycling vs default-v3 vs TW.
  SHIP-GATE: beat default-v3 peak RSS by ≥ the B0.3 threshold, byte-id.
- **B3.2** CPU cost: BiBOP lanes + recycling must not regress eval CPU materially (darwin-4
  warm + cold; the N-active-blocks fragmentation + lane-routing overhead).
- **B3.3** Default-flip decision + full nixpkgs byte-id soak if it ships; else document the
  measured ceiling + leave gated.

**Honest scope:** B0 is ~days (the scavenger fix + mmap + projection); B1+B2 are the
multi-week foundational allocator + recycling-evac build; B3 is the darwin-4 ship gate.
B0.3 is the Rule-0 gate — if the projected ceiling is below the bar, we STOP at B0 with a
measured kill rather than build a multi-week allocator for a sub-threshold win.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
