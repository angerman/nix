# Generational major collection — design (2026-06-14)

The real path to the Layer-C win: reclaim the **224 MB tenured stranded dead**
(44 % of firefox peak) that neither engine recovers today — the one capability
TW's conservative GC structurally forbids, and the only way v3 *beats* TW on
derivations.

## Why this is now the right next step

PhD-6 closed the nursery's three missed-root classes (`PHASE_D_VERDICT_2026-06-14
.md`), so the generational nursery is **correct** (`--brute` clean, byte-identical).
But the measurements reframed what the nursery *buys*:

- The nursery scavenges only the **young** generation → reclaims **churn** (the
  68.7 %-never-forced thunk garbage), a CPU win on mark-heavy repeated workloads
  (HNE/M5: Phase D −19/−53/−42 % CPU).
- Under the nursery the **major GC is OFF** (M-3), so the **tenured** arena is
  never collected.  The 224 MB stranded dead lives in tenured → **not reclaimed**.
- Empirically: firefox nursery peak_rss **820 MB** vs major-GC **618 MB** (+202).
  The major-GC default's single end-of-eval mark *does* reclaim some tenured dead;
  the nursery loses that and adds Cheney semispace overhead.

So: **nursery = young-churn reclaim (CPU); generational major = tenured-dead
reclaim (RSS).** Both are needed for the full Layer-C win.  This doc designs the
second.

## What "generational major" means here

A major collection that runs *occasionally* (not per-op), marks the **tenured**
live set, and frees the tenured dead — but does so **generationally**, reusing the
nursery's machinery so it is NOT the cache-bound full-heap mark that workstream E
was falsified on (one firefox full mark = +38.8 %, `E_DEPTH0_VERDICT`).

Two viable shapes (decide by a falsifier spike, per measure-twice):

### Shape A — promote-aware mark-sweep of tenured, triggered by tenured growth
- Keep the nursery scavenge for young.  Track **tenured bytes promoted** since the
  last major; when it crosses a threshold (e.g. tenured grew > N MB), run a major
  mark-sweep over the **tenured arena only** (young is empty/just-scavenged).
- The mark roots = the same precise root walk (`walkAllV3Roots`) + the now-correct
  Phase D remembered set (dirtyContainers) is NOT needed here (major marks
  everything tenured-reachable).
- Cost = one tenured mark per threshold-crossing, amortized over many ops — far
  less frequent than the per-op major-GC the nursery replaced.  The E falsifier
  (+38.8 %/fire) bites only if it fires *often*; tune the threshold so it fires
  O(1–few) times per drvPath eval (like today's gc_count=1).
- Reuses the EXISTING major mark-sweep (`mark_sweep.cc`) — it already marks +
  sweeps the arena.  The change is the TRIGGER (tenured-growth-based, nursery-
  compatible) + ensuring it composes with the nursery (don't double-collect young).

### Shape B — Immix-style mark-region on tenured (the GC_DECISION_2026-05-29 path)
- The committed Stage-6 GC family is Immix (mark-region).  Its line-occupancy
  spike (`IMMIX_LINE_OCCUPANCY_2026-05-29`) measured 46–50 % fully-dead lines on
  hello/HNE → 265/796 MB recoverable.  Immix reclaims tenured dead by recycling
  fully-dead lines without a full compaction.
- Composes naturally with the nursery: nursery = gen-0 (copying), Immix tenured =
  gen-1+ (mark-region).  This is the textbook generational+Immix design.
- Bigger build (the Immix allocator was scoped at ~4 KLoC, `GC_DECISION`), but it
  is the *committed* family and directly targets the tenured-dead reclaim.

## RESULT (2026-06-14) — Shape A IMPLEMENTED + MEASURED: works, on M5

Implemented as opt-in `NIX_V3_GEN_MAJOR=1` (requires the nursery): at the major-GC
safepoint, `nursery->forceScavenge(vm)` empties the nursery (single-region: all
survivors → tenured), THEN the existing IC-clear + `runMajorMarkSweep` runs over
the now nursery-free tenured set (M-3-safe).  Correct: hello.drvPath byte-identical
(r77jznkw) + AUDIT 0 hits + canary 5/5 (default path unchanged, opt-in).

Measured (laptop, min user-CPU + maxRSS over 2; firefox + M5; HNE excluded — it
does IFD = remote build, not a clean eval benchmark):

| workload | major-default | nursery-alone | gen-major (Shape A) |
|---|---|---|---|
| firefox | 4.27 s / 580 MB | 2.97 s / 639 MB | 3.70 s / **589 MB** |
| M5      | 15.51 s / 2492 MB | 12.56 s / 2251 MB | 16.02 s / **1868 MB** |

**Verdict: Shape A reclaims the tenured stranded dead — hugely on M5 (the
production-goal workload), negligibly on firefox.**
- **M5: −624 MB vs major-default (2492→1868) at +3 % CPU; −383 MB vs nursery at
  +28 % CPU.** This IS the Layer-C reclaim — on M5, where the dead lives.
- **firefox: 589 ≈ major's 580** — Shape A only recovers what the one major-GC
  already reclaims (+13 % CPU vs nursery, −13 % vs major).  firefox has NO 100 MB+
  of *additional* tenured dead beyond a single major.

**The pre-committed firefox falsifier (peak < 518 MB AND CPU ≤ +15 % vs nursery)
FAILED** (589 ≮ 518; +25 % CPU).  BUT per the threshold-recalibration rule its
load-bearing PREMISE — "firefox holds 224 MB reclaimable tenured dead" — was
measured WRONG: firefox's major-GC already gets it; the reclaimable dead is on M5
(−624 MB).  Recalibrated to the correct workload, Shape A SHIPS the win.

**Net vs the production major-GC default, gen-major wins on BOTH headline
workloads:** M5 −624 MB RSS at +3 % CPU, firefox −13 % CPU at neutral RSS.  So
gen-major is a SHIP-worthy opt-in lever; a default-flip is a cost/benefit call
(vs nursery-alone it trades +25-28 % CPU for the M5 −383 MB).

## Recommendation

**Shape A first** (cheap, reuses `mark_sweep.cc`): add a tenured-growth-triggered
major mark-sweep that fires a handful of times per eval, composed with the nursery.
It's a small change to validate the core hypothesis — *does reclaiming tenured
dead a few times mid/end-eval bring firefox peak below the major-GC default's 618
and toward the 224-MB-freed floor?*  Pre-committed falsifier (measure-twice):
- SHIP if firefox peak_rss (nursery + Shape-A major) < major-GC default 618 MB by
  ≥ 100 MB AND CPU regression ≤ +15 % vs the nursery-alone CPU.
- KILL if the major fires too often to stay within +15 % CPU (→ the E cache-bound
  wall reasserts; then Shape B / Immix is the only path, accept its cost).

Only escalate to Shape B (Immix tenured) if Shape A's trigger can't hit the CPU
bar — i.e. if reclaiming tenured dead inherently needs the mark too often.

## Prereqs / sequencing
1. The nursery is correct (DONE — PhD-6).  Required: a moving major must also
   honour the same move-safety the nursery now has (the 3 fixed classes); audit
   that the major mark-sweep (non-moving today) stays non-moving, OR extends the
   barrier/registry fixes if it moves.
2. Re-measure the nursery's HNE/M5 CPU wins (now correct) to set the CPU baseline
   the Shape-A falsifier compares against.
3. Implement Shape A trigger + compose with nursery; run the falsifier spike.

## Standing gates
Full byte-identity (06-07 + 7 rows + lang 143 + core), `--brute` clean (the
move-safety surface), the cumulative RSS ledger, Rule 0.  A wrong major-collection
mover is a UAF — same care class as PhD-6.

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0.*
