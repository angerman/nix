# S2.1b — mid-eval moving compactor: end-to-end RUNS, residual Blackhole corruption (RCA in progress)

**Date:** 2026-06-26  **Branch:** angerman/2.35-eval-profiling-v2
**Reproducer:** `bench/s2-evac-compaction.sh` (firefox.drvPath, full-compaction mode).

## Context

The S1.2 pivot (commit abd17037d) proved the conservative C-stack scan is removable
for the *non-moving* mid-eval sweep, and the follow-up (git-note on abd17037d) showed
the *only* RSS path is a MOVING compactor (non-moving + no-scan leaves blocks 25-75%
live, blocksFreed=0). The subsystem map (Explore 2026-06-26) found the machinery
already exists: the evacuator (`runEvacuation`/`EvacVisitor`, mark_sweep.cc) is
independent of the dangerous legacy `g_majorGcEnabled`, runs from the mid-eval path
(`NIX_V3_EVAC` + `NIX_V3_MIDEVAL_GC`), has a `PRECISE_ONLY` mode (no conservative
block-exclusion), and its OWN block-free path (`freeWholeBlock`, not gated on
`majorGcEnabled`).

## What works (firefox.drvPath, NIX_V3_EVAC=1 PRECISE_ONLY=1 PCT=1.0)

The moving compactor runs end-to-end:
```
v3 evac: candidates=13 pins=583 pinnedBlocks=0 consClosureBlocks=0
         movedCells=1181405 movedBytes=92.1MB blocksFreed=1 freedRSS=16.8MB
         [move=2180ms verify=465ms munmap=264ms]
v3 evac-brute TYPED: MARKED(live-dangle)=0  UNMARKED(missing-root/dead)=0
```
→ moves 1.18M cells (92MB), frees a whole block, **munmaps 16.8MB**, and the typed
BRUTE audit finds **zero structural dangling** (precise roots cover the heap graph).
This is the RSS reclaim the campaign said was structurally blocked.

## The residual bug (VERIFIED facts — no speculation)

The eval then ABORTS: `v3 toString: cannot stringify type tag=14` (tag 14 = `Tag::Blackhole`
— a thunk mid-force / cycle-detection sentinel leaking into a value position).

Bisect (firefox, PRECISE_ONLY PCT=1.0):
- scan-OFF, move+free      → tag=14
- scan-OFF, move-only (NO_FREE) → tag=14  ⇒ it is the **MOVE**, not the munmap.
- scan-ON,  move+free      → tag=14  ⇒ the conservative mark-phase scan does NOT fix it
                                       (PRECISE_ONLY ignores the C-stack pins anyway).
- CELL-PIN mode (the #174 brute-22/22 mode), PCT=1.0 → tag=14 too.

So: the corruption is a Blackhole leak caused by RELOCATION (the move), affects BOTH
evac modes, and is specific to firefox PCT=1.0 full compaction — a configuration #174's
SYNTHETIC brute battery never exercised (the prior validation was nursery-scavenge +
synthetic folds, where tenured cells aren't moved).

## Leading hypothesis (UNVERIFIED — to test next)

The force-writeback machinery: a force in progress arms `f.forceWriteTarget` (a `Value*`)
= where the forced result is stored. The comment at vm.cc:3290 notes it "can point into
a Bindings entry (OP_ATTRS_SELECT_DYN / IC path) or into a stack slot."  `EvacVisitor::
visitSlot` (mark_sweep.cc:1212) DOES relocate an interior `forceWriteTarget` whose owner
Bindings is a candidate (line 1223: `p = np + off`), so the naive "interior pointer not
relocated" theory is INSUFFICIENT. The subtler suspect: `armedWritebackValue()` (a map
populated at vm.cc:3401/9885/10120/10307/10638) is **keyed by the `forceWriteTarget`
pointer value**. Relocation changes that pointer (visitSlot rewrites it), so the map
entry is left under the STALE key → the stale-KEEP / WS-A writeback protocol mis-looks-up
after a mid-force relocation → the result is not written / the Blackhole sentinel is not
disarmed → a permanent Blackhole leaks. NEEDS VERIFICATION (instrument the relocation of
an armed forceWriteTarget + the subsequent map lookup) before any fix — do not assume.

Other candidates to rule out: (1) a force frame whose `forceWriteTarget` owner is a
candidate but `fwdRaw` returns null (unmovable) → pinnedCells, left stale (visitSlot:1226);
(2) `Value::vBlackhole` pushed onto the value stack (vm.cc:8749/11799) during a relocation
window; (3) the `evacChars` default-skip interacting with a Chars cell that backs a
context string. The verified bisect already excludes the munmap and the mark-phase scan.

## Next steps (S2.1b)

1. VERIFY the armed-writeback-key hypothesis: instrument `armedWritebackValue()` rekeying
   across an evac that relocates an armed `forceWriteTarget`; confirm the lookup-miss →
   Blackhole-leak chain (or falsify and move to the next candidate).
2. Fix: make the armed-writeback provenance relocation-aware (rekey on relocation, or
   store container+offset instead of a raw pointer, or re-derive post-safepoint), mirroring
   how `visitSlot` already rewrites `f.forceWriteTarget`.
3. Re-run the reproducer → byte-id + blocksFreed>0 + freedRSS>0 + evac-brute dangle=0.
4. Full --brute with EVAC+PRECISE_ONLY+MIDEVAL injected (the 1MB-nursery stress oracle).
5. Then S2.2 (block-free trigger policy) → S2.3 (darwin-4 peak-RSS vs the ~128MB ceiling).

**Honest status:** the moving compactor RUNS, munmaps, and is structurally sound (zero
typed dangling); the remaining work is a precise correctness RCA+fix on the force-writeback/
relocation interaction — genuine multi-day core-VM work, as the multi-week framing predicted.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
