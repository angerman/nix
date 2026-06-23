# Mid-eval reuse SEGV — RCA log (2026-06-23)

Tracking the root-cause work for the `NIX_V3_MIDEVAL_REUSE` SEGV (task #130),
per the goal: proper RCA with instruments/profiles, no guessing.

## Context

`NIX_V3_MIDEVAL_GC=1` (non-moving mid-eval tenured mark-sweep) + sweep + bin is
correct + `--brute` 22/22. Enabling reuse (`NIX_V3_MIDEVAL_REUSE=1`) → SEGV.

## Layer 1 — stale free-list bins (FIXED, ccec2bd01)

RCA tool: `NIX_V3_MIDEVAL_POISON` (sentinel-on-bin + double-bin detector +
nearest-cell-start dump).  Finding: bins were never cleared; across sweeps with
changing cell-start configs a region binned big in sweep N stayed in the bin while
sweep N+1 sub-divided it → oversized stale entry → popped → overlapped a live
string (`type=Bindings`, content a live store-path string, `belowType=Chars` at
`p-48`).  FIX: `clearFreeListBins()` per sweep + keep dead cells' start bits under
mid-eval + interior-bit clear on alloc.  Poison stopped firing; NO_REUSE byte-id.

## Layer 2 — residual missed root (OPEN, this doc)

After Layer-1 fix, reuse still SEGVs.  lldb: `EXC_BAD_ACCESS` reading
`Value::tag()` on a WILD `Value*` `0x68bf24000` (not an arena/nursery address) —
a live cell got reused and a stale reference now reads a garbage `Value*`.  The
poison can't catch it (read-after-reuse, not write-while-binned).  ⇒ a genuine
mark-completeness gap: the resident-nursery mid-eval mark misses a live cell.

### Candidate gap (to CONFIRM with an instrument, not assume)

`walkAllV3Roots` (precise_root.cc) does NOT walk `dirtyContainers` (the inter-gen
remembered set: tenured cells holding nursery pointers).  The post-scavenge
AUDITOR (gc.cc:1502) DOES walk it.  For the SCAVENGER the dirty list is a genuine
root source (it finds tenured→nursery edges WITHOUT a full tenured-heap walk).
The mid-eval mark walks transitively, so in theory it reaches those cells anyway —
but that must be MEASURED, not assumed.

### Instrument

`NIX_V3_MIDEVAL_AUDIT`: after the mid-eval mark completes (marker has all bits),
walk the dirty list (+ any other auditor-only root) and COUNT how many arena cells
it marks that the mark MISSED, dumping their type.  A non-zero count proves the
gap + names the cell type; zero means the gap is elsewhere (keep digging).

### RESULT — CONFIRMED + FIXED (Layer 2)

`NIX_V3_MIDEVAL_AUDIT` instrument: the dirty-list walk marked **4434, then 9529
NEW cells** the mid-eval mark had missed (`remembered set IS a needed root`).
CONFIRMED: the inter-gen remembered set is a genuine root the non-moving mid-eval
mark must walk.  FIX: walk `dirtyContainers()` as roots in `runMajorMarkSweep`
under the mid-eval gate (mark_sweep.cc).  RESULT: hello/git byte-id at all
thresholds; poison clean; **free-list hits 0 → 5697** (reuse now reclaims); SEGV
gone for hello/git.

## Layer 3 — attrSelect IC sole-references (CONFIRMED + FIXED)

After Layer 2, `--brute` (1 MB nursery) with reuse still failed 2 suites
(brute-audit, apply-overrides-1.7).  Profile: apply-overrides diverged
`makeOverridable v3=[] tw=[7]`, `overrideAttrs.pname v3=[] tw=["hi"]` — a live
override Bindings became EMPTY (size→0 = zeroed by reuse).  poison CLEAN (0 hits)
⇒ the cell was binned + popped + zeroed without an intervening WRITE = a
read-only-live missed root.  Control: no-reuse passes 9/0 ⇒ reuse-specific.

RCA (read, not guessed): the SCAVENGER walks a live closure/thunk's
`cu->attrSelectCache` (gc.cc:641/693/725) — the IC pins `Bindings*` that can be
the SOLE reference to a transient attrset (built, attr-selected, otherwise
unreferenced).  But (a) `MarkVisitor::walkClosure/walkThunk` did NOT walk the IC,
and (b) the mid-eval block CLEARED the IC before marking (copied from the MOVING
gen-major, where the IC's raw pointers go stale post-forwarding).  Under the
NON-MOVING mid-eval mark the IC pointers stay VALID, so clearing DROPPED a live
root → the override Bindings was swept + reused + zeroed → empty.

FIX: (1) `MarkVisitor::walkCuIC()` walks `cu->attrSelectCache` from walkClosure /
walkThunk (mirrors the scavenger; mid-eval-gated, deduped per-CU); (2) STOP
clearing attrSelectCache in the mid-eval block (the mark now keeps its cells alive
— non-moving, pointers valid).  recSlotCache / materialize-memo / env-intern /
capWiths stay cleared: their cells are reachable via OTHER marked roots (rec
Value / caller / closure upvalEnv / closure capturedWiths), so clearing only
invalidates stale entries after reclaim — safe.  RESULT: apply-overrides 9/0 under
brute+reuse.  (full --brute result below)
