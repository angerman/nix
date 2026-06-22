# Mid-eval non-moving tenured mark-sweep — design (2026-06-22)

## Problem (measured, project_v3_vs_tw_rss_rootcause_2026-06-22)

v3 peak RSS = 1.7-1.9× TW (firefox 712 vs 375 MB). Root cause: **ALL v3 GC is
gated to `exitDepth == 0`** (nursery scavenge nursery.hh:306; gen-major
vm.cc:4047), but deep nixpkgs eval is ~always at `exitDepth > 0` (higher-order
primop callbacks nest the dispatch loop). So the GC never fires mid-eval: the
32 MB nursery fills once, ~78% of allocs bypass to the tenured arena, and tenured
is never collected → arena grows monotonically to its high-water (firefox 352 MB,
~157 MB live → ~195 MB dead-unreclaimable). TW's Boehm is stop-the-world on heap
pressure (nesting-independent) → plateaus near live.

## Why the gate exists

The collector MOVES objects (nursery Cheney scavenge; gen-major's `forceScavenge`).
A moving collection can only forward the OUTERMOST dispatch loop's locals; nested
dispatch loops (callClosure2 per-element re-entry, primop callbacks) hold C-stack-
local pointers the mover can't find/forward → dangle. Hence `exitDepth == 0`.

## Design — a NON-MOVING tenured mark-sweep, fireable at `exitDepth > 0`

The major mark-sweep (`runMajorMarkSweep`, mark_sweep.cc) is ALREADY non-moving
(it replaced the Cheney semispace). The `exitDepth==0` gate wraps it only because
gen-major calls the moving `forceScavenge` FIRST. Run the mark-sweep WITHOUT
forceScavenge, with the nursery RESIDENT, and it is safe at any depth:

  1. **No forceScavenge** — the nursery stays resident (young cells alive).
  2. **Mark** = the existing pipeline, PLUS one addition:
     - `walkAllV3Roots(vm, visitor)` — precise walk of all active VMStates'
       frames/value-stacks/with-stacks. Marks tenured cells reachable from roots.
       (It SKIPS nursery cells: `tryMark` returns false for any pointer not
       `arena.inActive` — mark_sweep.cc:111.)
     - **NEW `walkNurseryConservative`** — for each active VMState's nursery, scan
       the used young region `[base, next)` (+ active survivor region if Phase E)
       word-by-word; any word that is `arena.inActive` → `markConservative`. This
       covers tenured cells reachable THROUGH nursery cells — which the precise
       walk skips. Mirrors `walkCStackConservative` exactly. No-op when the nursery
       is empty (so it is also safe/free on the gen-major post-forceScavenge path).
     - `walkCStackConservative` (existing) — C-locals of ALL nested frames (current
       SP → stack base), spilling callee-saved regs via setjmp.
     - `drainConservative` — transitive byte-walk of conservatively-marked cells.
  3. **Sweep** = unchanged. Iterates `arena.blockRanges()` = TENURED arena blocks
     ONLY (the nursery is a separate region: nursery.hh base/next/end ≠ arena
     blocks) → nursery cells are never swept. Dead tenured cells → free-list bins
     (gated `g_freeListReuseEnabled`) + clear cell-start bit; fully-dead blocks →
     freed to libc.
  4. **Reuse** — re-enable the free-list pop in `Arena::alloc` (alloc.hh:1451),
     decoupled from the hard-false `majorGcEnabled()`. Subsequent tenured allocs
     pop swept cells instead of bumping → the arena PLATEAUS near the live set.

## Safety argument (a missed root = the PhD-6 UAF class)

- **Non-moving** → no pointer is forwarded → every conservatively-found pointer
  (nested C-locals, nursery contents) stays VALID after the collection.
- **Completeness of marking** (no live tenured cell is swept):
  - reachable from VMState roots → precise `walkAllV3Roots`.
  - reachable through the nursery → conservative `walkNurseryConservative` (scans
    ALL used nursery bytes → over-approximate → never misses).
  - reachable only via a C-local (primop body temporary) → conservative C-stack
    scan (caller spills caller-saved regs at the call boundary; setjmp spills
    callee-saved).
  - reachable only via a transient cache (IC / materialize-memo / env-intern /
    capWiths) → CLEARED before the mark (same as gen-major vm.cc:4094-4113); they
    repopulate on next use.
- **Conservatism is one-directional**: false positives keep dead cells alive
  (minor over-retention, cleaned at the next real scavenge) — never frees a live
  cell.
- **Validation**: full `--brute` (1 MB nursery + AUDIT + BRUTE) with the trigger
  forced frequent, on hello/git/firefox + byte-identity. A missed root surfaces as
  an AUDIT hit or a divergent drv.

## Gating + retirement

- `NIX_V3_MIDEVAL_GC=1` — enables the mid-eval trigger (fire `runMajorMarkSweep`
  without forceScavenge at `exitDepth > 0` on arena-byte pressure) AND implies
  free-list reuse (build + pop). Default-OFF.
- Retirement criterion (Rule 0): delete the gate + flip default-on once darwin-4
  shows firefox/M5 RSS drops materially toward TW AND `--brute` is clean across a
  full nixpkgs sweep; OR delete entirely if Immix (GC_DECISION_2026-05-29) lands
  and subsumes it. This is the lower-risk subset of Immix (no evacuation/moving).

## STATUS (2026-06-22) — IMPLEMENTED, default-safe, KNOWN missed-root bug

Implemented behind `NIX_V3_MIDEVAL_GC` (default-OFF): the trigger (vm.cc), the
nursery used-range accessor (nursery.hh `forEachUsedRange`), the conservative
nursery scan + a shared de-boxing `conservativeMarkWord` (mark_sweep.cc), the
free-list build+reuse re-enable (mark_sweep.cc sweep + alloc.hh pop), and the
tunables (vm.cc).

**DEFAULT PROVEN UNCHANGED**: every new path is gated on `g_midEvalGcEnabled`
(incl. the de-box, which falls back to the exact prior raw scan when off).
Verified: hello.drvPath default == TW byte-identical; `--core` 21/21 GREEN.

**MID-EVAL PATH (opt-in) HAS A KNOWN MISSED-ROOT BUG.** When the sweep actually
fires (hello at `NIX_V3_MIDEVAL_GC_THRESHOLD_MB=16`+, 3 fires) the drv hash
DIVERGES → the sweep frees a live tenured cell. Ruled out by bisection: NOT
cache-clearing (same hash on/off), NOT free-list reuse (hits=0 — the corruption
is the sweep itself: whole-block-free / cell-start-bit clear of a missed-root
cell), NOT boxed C-locals nor boxed nursery words nor `drainConservative`'s
untyped byte-scan (all three de-boxed, bug persists).

**ROOT-CAUSE OF THE BUG (the real fix):** the conservative BYTE-SCAN of the
nursery is fundamentally leaky for nested boxed pointers. It marks a nursery-
reachable tenured cell, but transitive coverage of THAT cell's contents depends
on `drainConservative` — which byte-scans only untyped (Value/Env/Chars) cells
and even de-boxed still misses some nested-pointer shape. **The correct fix is
PRECISE nursery traversal**: when the precise mark (`walkAllV3Roots` → MarkVisitor)
hits a nursery cell, instead of skipping it (`tryMark` rejects non-arena ptrs),
WALK it by type (`walkClosure`/`walkThunk`/`walkBindings`/…, which call
`visitValue` → decode the v8nan box correctly), deduping nursery cells in a
separate visited-set (since `tryMark` can't dedup them). This mirrors what the
scavenger does (gc.cc) but without forwarding. It is a larger `MarkVisitor`
change (a `Nursery*` member + a nursery-visited set + a nursery branch in each
`visitX`), gated on `g_midEvalGcEnabled` so the default stays untouched.

**NEXT STEP**: replace the conservative nursery byte-scan with precise nursery
traversal (above); then the missed-root closes. Validate: hello/git/firefox
byte-id across thresholds → full `--brute` (1 MB nursery + AUDIT + BRUTE) →
darwin-4 firefox/M5 peak-RSS measurement (target: arena plateaus near live, RSS
toward TW's 375 MB). A differential-mark audit (compare mid-eval mark vs
gen-major's post-forceScavenge mark; the delta = the missed cell's type) is the
tool to confirm closure.

## STATUS UPDATE 2 (2026-06-22) — MARK CORRECT; RECLAIM blocked on cell-metadata gate

**The hard part is DONE + validated.** The non-moving mid-eval mark-sweep now
marks correctly via PRECISE nursery traversal (commit 357d12859): hello.drvPath
byte-identical at thresholds 8/16/24/32/48/64 MB; git.drvPath byte-identical;
**full `--brute` 22/22 ALL GREEN with `NIX_V3_MIDEVAL_GC=1` forced active** (the
1 MB-nursery + AUDIT + BRUTE missed-root stress). On darwin-4 firefox cache-off it
**fires 5× at exitDepth>0, byte-identical** — the moving-GC-at-exitDepth>0 safety
problem (the novel risk) is SOLVED.

**But it reclaims NOTHING yet** (firefox: free-list hits=0, arena 352 MB unchanged,
RSS 684→672 MB only). RCA: the sweep's `v3 sweep: blocksScanned=0` — the per-block
sweep finds no cells because **the cell-start bitmap is not maintained**. That
maintenance (alloc.hh:1502) — AND the mmap-vs-calloc block-allocation method
(2515), the cell-start-bitmap-per-block allocation (2544), whole-block-free /
munmap (2284), huge-block free policy (1352), and the queries isCellStart (1531) /
cellTypeAt (1565) / setCellStartBitInBlock (1897) / clearCellStartBitFor (2450) /
findContainingCellStart (1995) — are ALL gated on `majorGcEnabled()` (hard-false).
The legacy major-GC reclaim machinery was disabled when the nursery shipped; GC_
DECISION_2026-05-29 deferred its replacement to Immix (paused).

**NEXT STEP (well-scoped, classified): enable the cell-metadata machinery under
the mid-eval gate.** Add `Arena::cellMetaEnabled() = majorGcEnabled() ||
detail::g_midEvalGcEnabled` and switch the cell-metadata sites from
`majorGcEnabled()` to it. This is ~15 INTERLOCKING sites, not a one-liner — that
is why it is its own focused pass (a missed site = silent miss or crash):
 - block ALLOC method (regular 2515, huge 1352): mmap vs calloc;
 - block FREE method (huge freeHugeBlock 2284 munmap-vs-free; regular freeWholeBlock
   munmaps UNCONDITIONALLY but is only reached when metadata is on → consistent);
 - per-block cell-start bitmap alloc (2544);
 - cell-start MAINTENANCE: bump path (1502), setCellStartBitInBlock (1897),
   setCellStartBitFor (~2147, the free-list-reuse re-set), clearCellStartBitFor;
 - cell-TYPE stamping: setCellTypeInBlock (1919), setCellTypeFor (~2460, the
   free-list-reuse re-stamp) — REQUIRED so a reused cell's type is correct, else
   drainConservative byte-scans it instead of typed-walking;
 - QUERIES: isCellStart (1531), cellTypeAt (1565), findContainingCellStart (1995).
 CRITICAL CONSISTENCY: alloc/free method (mmap↔munmap vs calloc↔free) must flip
 together for BOTH regular and huge blocks (mismatch = munmap-on-calloc crash);
 every alloc path that stamps a cell-start bit must also stamp its TYPE.  Leave the
 Immix line-mark sites (1616/1633/1663/1734) on `majorGcEnabled()` (the free-list-
 bin path uses cell-start bits, not line-marks).
 KEY FINDING from this pass: the DEFAULT gen-major sweep is ALSO a no-op today
 (`blocksScanned=0` — cellStarts empty under majorGcEnabled=false); its reclaim is
 the nursery scavenge's young-death only, NOT a tenured sweep.  So enabling
 cell-metadata under mid-eval gives v3 its FIRST real mid-eval tenured reclamation.
 Then: re-run `--brute` with mid-eval forced (validates metadata + reuse UAF-free
 under the moving-nursery stress) → darwin-4 firefox/M5 peak-RSS (free-list reuse
 fires at scale where ~78% of allocs bypass to the tenured Arena::alloc; target:
 arena plateaus near ~157 MB live, RSS toward TW's 375 MB).

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0*
