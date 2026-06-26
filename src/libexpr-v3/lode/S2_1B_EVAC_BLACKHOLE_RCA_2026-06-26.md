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

## RCA progress (2026-06-26, instruments NIX_V3_WB_TRACE) — narrowed, 3 hypotheses killed

Built three gated diagnostics (mark_sweep.cc EvacVisitor::visitSlot stale-leave logs;
precise_root.cc forceWriteTarget reloc trace; primops.cc backtrace at the tag=14 abort).
Ran firefox under EVAC+PRECISE_ONLY+PCT=1.0. Findings (VERIFIED, not assumed):

1. **forceWriteTarget is NOT the cause — FALSIFIED.** visitSlot STALE-unmovable=0,
   STALE-owner-null=0 (it never leaves a force-writeback slot stale), and frame.
   forceWriteTarget walks are RARE (3 total, 1 relocated cleanly). The armedWritebackValue()
   stale-key theory is also moot (g_sharedWbDetect default-off; not consulted in cellWrite).

2. **The leaked value is `Value::vBlackhole`** (rawword=0x7fff000000000000) at a STACK
   address — the result of `forceValue` in `toStringCoerceCtx:965`, reached via
   primDerivationFromPreprocessed → primDerivCoerce → toStringCoerceCtx (an env-var coercion).

3. **It originates at vm.cc:8033 `retVal = Value::vBlackhole`** — the OP_RETURN SELF-CYCLE
   defer. The detector (vm.cc:7958-7967): chase `retVal = retVal.asThunk()->evaluated`
   through Evaluated indirections, then `if (retVal.asThunk() == fr.thunk)` → self-cycle →
   vBlackhole. Under the moving evac this pointer-identity check MISFIRES (a non-cyclic
   env-var thunk is spuriously detected as a self-cycle) → vBlackhole leaks into toString.
   firefox WITHOUT evac never hits this (byte-id), so the evac INTRODUCES the false cycle.

4. **The evac DOES rewrite the `evaluated` field** (mark_sweep.cc:1506 `case Evaluated:
   visitValue(t->evaluated)`) and `fr.thunk` (precise_root.cc:58 visitThunk) — so the naive
   "indirection not relocated" theory is also INSUFFICIENT. The misfire is a subtler
   relocation/identity interaction (candidates: forward-map aliasing collapsing two thunks
   to one; a thunk reachable only via a C-stack local — retVal mid-chase — not relocated
   while fr.thunk is; or the chase reading a moved-from cell whose stale `evaluated` aliases
   fr.thunk's new address). NOT YET pinned — needs V3_DBG_RETURN_SELF correlated with evac
   relocation generations.

## RCA RESOLVED-mechanism + FIX (sound) + repro LOST to nondeterminism (2026-06-26, later)

**Mechanism VERIFIED.** vBlackhole sources 8033 (OP_RETURN self-cycle) and 14238
(forceValue cross-stack) ruled out — their V3_DBG loggers showed 0 events while tag=14
still fired. The remaining force-path vBlackhole sources are the `onMyFrames` pointer-
identity checks (vm.cc:8742 OP_FORCE / 14204 forceValue: `frames[i].thunk == t`).
DECISIVE TEST: `NIX_V3_NO_BLACKHOLE_AS_VALUE=1` (disables both onMyFrames defer paths)
FLIPS the failure from `tag=14` to **"infinite recursion encountered"** — proving an
in-force (Blackhole-state) thunk is being force-RE-ENTERED under the evac, with
blackhole-as-value merely masking it as vBlackhole. ROOT CAUSE: the moving evac
relocates an in-force blackholed thunk while a stale, un-rooted C-stack reference to it
survives (no-scan) → re-forcing via the stale ref re-enters the blackhole → onMyFrames
identity mismatch → vBlackhole/infinite-recursion.

**FIX (sound, mark_sweep.cc fwdRaw): pin Blackhole-state thunks during evac** (don't
relocate a mid-force thunk; opt-out NIX_V3_EVAC_MOVE_BLACKHOLE=1). Blackhole thunks are
few (bounded by live C-stack force depth) → negligible compaction loss. With it, firefox
byte-id + compacts (5.4M cells moved, blocksFreed, munmap).

**⚠ FIX IS NOT e2e-VALIDATED — the repro is NONDETERMINISTIC (store-state-dependent).**
The corruption reproduced ~7/7 on the FIRST firefox.drvPath evals (COLD store →
copyPathToStore in toStringCoerceCtx's Path case is allocation-heavy → an evac fires in
a window that catches an in-force thunk). After the store warmed (paths already copied →
no copy → different allocation pattern), it VANISHED: the EXACT committed b2f621eb7
binary (no fix, no instruments) is now 5/5 byte-id, AND 3/3 even at evac-every-1MB
(NIX_V3_MIDEVAL_GC_THRESHOLD_MB=1 GROWTH=1.0). So fix-on and fix-off are BOTH byte-id now
— the A/B cannot distinguish them. The fix targets the VERIFIED mechanism and is sound,
but "fix makes firefox byte-id" is UNCONFIRMED because the window closed.

**To validate (next):** a DETERMINISTIC repro — either (a) evict firefox's source paths
from /nix/store to force the cold copyPathToStore path again, or (b) a synthetic that
forces a thunk whose body does a re-entrant store/IO op (mimicking copyPathToStore) while
an evac fires mid-force. Then A/B the blackhole-pin. Until then the fix ships gated
(NIX_V3_EVAC default-off, experimental) as a sound RCA-motivated measure, NOT a validated
one. LESSON: evac correctness repros that touch copyPathToStore are store-state-sensitive
— pin the store state (or use a synthetic) before A/B-ing a fix.

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
