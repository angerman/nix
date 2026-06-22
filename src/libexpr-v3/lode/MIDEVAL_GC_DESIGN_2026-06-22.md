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

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0*
