# R2.4 evacuation — implementation design (the load-bearing moving piece)

**Date:** 2026-06-02
**Status:** DESIGN — the concrete plan for v3 Nofl-Immix's evacuation phase,
the ONLY mechanism that returns RSS for v3's interleaved allocation
(R0/R1/R2.1 established this). Measure-twice doc before the riskiest code
in the GC (moving store-path-critical cells; a missed root → munmap'd page
→ wrong store path / SIGSEGV).

Builds on: R2.0 (`55745a112`, mmap/munmap blocks), R2.1 (`753866da6`,
density histogram). Companions: [`IMMIX_NOFL_DESIGN_2026-06-02.md`](IMMIX_NOFL_DESIGN_2026-06-02.md),
[`R1_PAGE_RELEASE_RESULT_2026-06-02.md`](R1_PAGE_RELEASE_RESULT_2026-06-02.md),
[`POST_F4_MEMORY_PROFILE_2026-06-02.md`](POST_F4_MEMORY_PROFILE_2026-06-02.md).

---

## 1. Why evacuation is mandatory (recap of the data)

- R1: on macOS only `munmap` returns RSS; `std::free`=0%, `madvise`=0%.
  ⇒ RSS only returns by freeing WHOLE blocks.
- R2.0+sweep: at mid-eval GC triggers, `blocksFreed=0` — live cells are
  scattered across every block, so NO block is fully dead. Whole-block-free
  alone reclaims nothing.
- R2.1: but many blocks are SPARSE. HNE late cycle: 26 blocks <25% live →
  **436 MB evacuable for 88.7 MB copy cost (4.9:1)**. (M5: see §6.)
- ⇒ Evacuation = relocate the few live cells out of sparse blocks into
  dense blocks → sparse blocks become fully dead → `munmap`. This is the
  Immix "opportunistic evacuation" and the only path to the watchdog.

## 2. The machinery already exists — adapt the nursery Scavenger

`gc.cc::Scavenger` is a working, brute-validated Cheney copying collector
for the nursery. Tenured evacuation is the SAME machinery retargeted:

| | nursery scavenge (exists) | tenured evacuation (R2.4) |
|---|---|---|
| source | the whole nursery (young) | only SPARSE tenured blocks (<25% live) |
| which cells copied | all live nursery cells | only live cells *in candidate blocks* |
| destination | tenured region (S/T) | holes in DENSE tenured blocks / fresh dest block |
| forwarding | side-table `unordered_map<old,new>` | same |
| worklist | graylist drain (`forwardClosure/Thunk/List`) | same |
| slot rewrite | `RootVisitor` `T*&` callbacks | same |
| missed-root check | `liveTenuredRanges` + brute scan | same |
| trigger | nursery full | end of major-mark-sweep, when sparseBlocks>0 |

Reused substrate: `RootVisitor` (rewritable `T*&` slots, already moving-GC
shaped — `visitString/visitPath` forward buffers), `walkAllV3Roots` +
transitive `MarkVisitor` walk pattern, `freeWholeBlock`→`munmap` (R2.0),
the density histogram (R2.1) for candidate selection, the brute audit.

## 3. Algorithm (runs at the END of runMajorMarkSweep, after sweep)

```
evacuate():
  1. candidateBlocks = { b : density(b) < EVAC_PCT (default 25%) }
     (computed from the sweep's per-block liveBytes; skip if empty)
  2. forward.clear()                       // side-table old->new
  3. For each ROOT slot (walkAllV3Roots with an EvacVisitor):
       EvacVisitor.visit<T>(T*& slot):
         if slot in candidateBlock:
            slot = forwardCell(slot)        // copy+register+enqueue, rewrite
         // ALWAYS recurse into *slot's contents (graylist) to find
         // deeper pointers into candidate blocks — even for non-moved cells.
  4. Drain graylist: for each enqueued (new) cell, walk its outgoing
     pointers with the same EvacVisitor (rewrite any that point into
     candidate blocks).
  5. forwardCell(old):
        if forward.count(old): return forward[old]
        new = allocInto(dense-hole or fresh dest block, sizeof(*old))
        memcpy(new, old, size); forward[old]=new; enqueue(new); return new
  6. After drain: every candidate block has zero live cells (all forwarded)
     -> freeWholeBlock(b) -> munmap -> RSS returns.
```

Destination allocation (step 5 `allocInto`): use the existing Immix
free-span / bump allocator into NON-candidate blocks (the dense ones have
holes from swept dead cells; or bump a fresh dest block). Must NOT allocate
into a candidate block (would re-pin it).

## 4. The correctness knife-edge (why this is the riskiest code)

Every pointer to a cell in a candidate block MUST be found and rewritten.
A missed slot → after `munmap`, that slot dangles → deref → SIGSEGV, or
(worse) the page is reused → silent wrong store path. Mitigations:
- **Precise roots must be COMPLETE** (Stages 1/3/5). Post-F4 roots are
  cleaner (no bridge tables). `walkAllV3Roots` is the single source.
- **C-stack hazard**: like the nursery, evacuate ONLY at `exitDepth==0`
  safepoints (no mid-primop C-locals holding un-walkable cell pointers).
  This is why the major-GC trigger already gates on the dispatch safepoint.
- **Brute audit**: after evacuation, scan all surviving tenured ranges for
  any pointer still landing in a (now-munmap'd) candidate range = a missed
  root. `run-brute-audit.sh` + `NIX_V3_GC_STRESS=N` are the gates.
- **Conservative C-stack scan** (`walkCStackConservative`, mark_sweep.cc):
  any cell reachable only via a C-local is PINNED (its block excluded from
  candidates) rather than moved — safety over completeness.
- **String/Path buffers** (`allocChars`): `visitString/visitPath` must
  forward char buffers too (with string-context side-table re-keying), or
  exclude string-bearing blocks from candidates in the first cut.

## 4a. R2.4a RESULT — evacuation is SAFE (precise walk reaches ~100%)

Implemented (commit `703cff5ed`): the sweep now reports precise-reachable
vs conservative-only marked cells. **HNE: conservativeOnly = 19-213 of
3-6 MILLION precise (0.0% pinned).** ⇒ the precise-root walk reaches
essentially every live cell, so evacuation can move ~all sparse-block live
cells and rewrite their references; only a handful of C-stack-pinned cells
exist (their blocks excluded from candidates). The Stage 1/3/5 precise-root
foundation is effectively COMPLETE post-F4. **Green-light for the mover.**

## 5. Phasing (each step gated NIX_V3_MAJOR_GC + own sub-gate, validated)

- **R2.4a DONE** (`703cff5ed`): evac-movability measure → 0.0% pinned, safe.

### R2.4b mover — refined plan (the next build, gated `NIX_V3_EVAC=1` OFF)
Requires NIX_V3_MAJOR_GC. Default-OFF so it's isolated from default/GC/CI
paths until validated (goal §5 "default-OFF gate during bring-up"; retire
the gate when the SHIP gate clears). Concrete decisions:
1. **Candidate blocks**: sparse (<EVAC_PCT, default 25% live) blocks,
   recorded as [start,end) ranges after sweep. EXCLUDE any block with a
   conservative-only mark (R2.4a says ~0, but exclude for safety).
2. **Mover = a tenured `EvacScavenger`** structurally mirroring the proven
   `gc.cc::Scavenger` (forward map old→new, graylist, per-type fwd+walk,
   drain), with the source predicate `n.contains(p)` replaced by
   `inCandidateBlock(p)`.
3. **Destination**: copy live candidate cells into FRESH dest blocks (mmap'd,
   appended non-candidate, own bump pointer) — dense by construction. (A
   later refinement can bump into dense-block free-list holes to avoid new
   blocks; fresh-dest is simpler + correct first.)
4. **VERIFY-BEFORE-FREE**: after the rewrite walk, re-scan each candidate
   block; munmap ONLY if it has zero remaining marks/live cells. A block
   that isn't fully evacuated (unexpected un-forwarded cell) is PINNED, not
   freed — makes a missed-pointer a leak (safe) not a dangle (corruption).
5. **Safepoint**: evacuate only at exitDepth==0 (already where major-GC
   fires) so the C-stack is shallow (minimal conservative pins).
6. Validation BEFORE any default-on: hello+HNE drvPath byte-equal under
   `NIX_V3_EVAC=1`; brute audit clean; `NIX_V3_GC_STRESS=1000`; RSS drops
   (HNE peak below the 2434 baseline). First cut may restrict candidates to
   blocks containing only fixed-size types (Pair/List) if the all-type fwd
   proves intricate; widen in R2.4c.
- **R2.4b** Real copy+forward+rewrite for the SIMPLEST cell types first
  (ValuePair, ListVec — fixed-size, no char buffers), candidate blocks
  restricted to those containing only such cells. munmap emptied blocks.
  Gate: hello+HNE byte-equal + brute clean + GC_STRESS=1000 + RSS drops.
- **R2.4c** Extend to Bindings/Closure/Thunk + string buffers. Full
  candidate set. Same gates.
- **R2.4d** Tune EVAC_PCT + trigger cadence for M5 < 4096 (the SHIP gate).

SHIP gate (per the goal): M5 peak < 4096; hello/HNE below post-F4 baseline;
wall ≤10%/15%; drvPath byte-equal on all three; --quick 6/6 + --core 19/19
+ brute under stress.

## 5b. CRITICAL FINDINGS (2026-06-03) — what shapes the mover

Reading the nursery `Scavenger` + cell structs surfaced two hard
constraints that reshape R2.4b (and explain why the design doc's "mirror
the Scavenger" was incomplete):

**Finding A — v3 cells are NOT self-describing.** There is no per-cell type
header. A cell's type is known ONLY from the `Tag` of the `Value` that
points to it (Bindings has just a Sorted/Chain `kind`, not a cell-type
tag). ⇒ given an arbitrary cell-start address you cannot know its type, so
you cannot content-walk (forward its pointer fields) without a typed
pointer to it.

**Finding B — interior pointers pin Bindings/Pairs.** `Tag::Slot` and
`Thunk::cell` point INTO `Bindings::entries[]` (let-rec slots, `with`
resolution, upvalue capture). The nursery `Scavenger` `std::abort()`s
rather than move a Bindings/Pair precisely because a cell-start-keyed
forward map can't fix up interior pointers. Bindings are ~84% of the
arena, so a cell-start-only mover would pin ~all of the interesting bytes.

**Consequences for the mover:**
- Moving a Bindings needs BOTH (i) its TYPED `Tag::Attrs` pointer (to
  content-walk with known type) AND (ii) interior-pointer rewrite to
  `forward[owner] + (p − owner)` for every `Tag::Slot`/`cell` into it.
  Owner lookup uses the existing `findContainingCellStart` (cellStarts
  bitmap). Cell COPY size is type-agnostic (span to next cell-start, as
  the sweep computes), so copying needs no type; only content-walk does.
- A cell reached ONLY via an interior pointer (never via a typed pointer)
  gets copied but not content-walked → its pointees may not be forwarded.

**THE LINCHPIN — verify-before-free via re-mark makes this SAFE regardless.**
After the rewrite walk, re-mark from roots (rewritten pointers now point
to dest). For each candidate block, `munmap` ONLY if it has zero marks.
Any un-rewritten / dangling-into pointer re-marks the old block → it stays
mapped (a safe leak), never a dangle. So the mover is SAFE even when
content-walk is incomplete; incompleteness only costs YIELD (pinned
blocks), never correctness. This is the key that lets R2.4b ship safely
before the full Nofl per-granule metadata byte (which would add cell-type
self-description and remove the typed-pointer dependency — a later R2.1′).

**Revised R2.4b mover:** (1) candidate = sparse blocks; (2) walk roots
transitively with EvacVisitor — typed pointers → fwd(copy+content-walk)+
rewrite; interior Slot/cell → fwd owner (copy, span size)+rewrite to
owner'+offset; (3) re-mark verify; (4) munmap only zero-mark candidates.
Gated `NIX_V3_EVAC=1` OFF. Validate byte-equal + brute + GC_STRESS + RSS
drop. First measurement: realized yield (fully-clean candidate blocks) vs
the R2.1 opportunity — tells us if metadata-byte (R2.1′) is needed for
M5<4096 or if the verify-clean subset already clears it.

## 6. Evacuation opportunity data (R2.1 histogram)

| workload | cycle | sparseBlocks(<25%) | evacuable_RSS | copy_cost | ratio |
|---|---|---|---|---|---|
| HNE | late (98 blk) | 26 | 436 MB | 88.7 MB | 4.9:1 |
| M5 | early (128 blk) | 31 | 520 MB | 39.8 MB | 13:1 |
| M5 | late (424 blk) | **257** | **4311 MB** | **611 MB** | **7:1** |

M5's watchdog gap is E ≈ 590 MB; the late cycle offers **4311 MB**
evacuable — ~37 of the 257 sparse blocks clears the watchdog, leaving
enormous headroom. The early M5 cycle also already shows `blocksFreed=4`
(67 MB) naturally-fully-dead → **R2.0's munmap fires in-arena on the
gate-critical workload without crashing** (de-risks the page-release wiring).
NB: M5's mark cost on 424 blocks is heavy (full eval under GC+trace
exceeded 270 s wall) — GC trigger cadence + mark cost is an R2.4d tuning item.

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
