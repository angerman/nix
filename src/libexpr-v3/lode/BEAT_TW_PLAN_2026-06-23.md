# Plan: beat the tree-walker (TW) on CPU AND memory — 2026-06-23

## Where we stand — CORRECTED baseline (darwin-4, cache-off, MEDIAN-of-5, beat-tw-compare.sh)

⚠️ The min-of-5 numbers I first used were ARTIFACTS (min catches fast outliers +
cross-session drift).  P0.1 A/B vs pre-mid-eval (d141896aa) confirmed NO default
regression, so the TRUE stable gap (median, tight <1% within-run spread) is:

| workload | TW | v3 default | gap | v3 mid-eval+reuse |
|---|---|---|---|---|
| firefox.drvPath | 0.73 s / 358 MB | **2.69 s / 676 MB** | **3.68× CPU, 1.89× RSS** | 3.73 s / 612 MB |
| M5 cardano-node.name | 3.60 s / 982 MB | **11.10 s / 2984 MB** | **3.08× CPU, 3.04× RSS** | 16.82 s / 2577 MB |

So v3 is ~3× TW on CPU and ~1.9–3× on RSS — WORSE than the earlier (artifact)
1.8–2.5× / 1.6–2.25×.  **Mid-eval reuse honest verdict (median): −9.5% RSS firefox /
−13.6% M5, at +39% / +52% CPU** — a real-but-modest RSS lever at a steep CPU cost
(both the earlier "−40%" [NIX_VM_STATS teardown artifact] and "~0" [min-statistic
artifact] were wrong).  CPU is the bigger gap, and mid-eval makes it worse, so
mid-eval stays opt-in/off.

LESSON (now baked into beat-tw-compare.sh): use MEDIAN not min; measure TW+v3
back-to-back same-session; ratios vs the stable TW anchor; never NIX_VM_STATS for
peak-RSS (its teardown GC perturbs).

To BEAT TW we need v3 < TW on both axes — today we are ~2× behind on both. This is
a multi-week program. The plan is prioritized cheap-RCA-first / highest-leverage,
and EVERY lever is measure-first per Rule 0 (the mid-eval "−40%" artifact this
session is the cautionary tale: never trust a single noisy number).

## What v3's RSS is made of (the target to shrink, M5)

peak RSS 2215 MB ≈ arena ~1023–1543 MB + Boehm ~402 MB + "elsewhere" ~1200 MB
(ImportCache + SQLite disk-cache + flake-fetch transients).  TW carries NONE of the
v3 "elsewhere" disk-cache memory and runs a leaner heap → 982 MB.  **So the single
biggest v3-vs-TW RSS chunk is "elsewhere", not the arena** — the prior campaign
optimized the arena; the untouched lever is the caches.

## Phase 0 — MEASUREMENT FOUNDATION (do first; everything depends on it)

The whole mid-eval RSS confusion was a measurement failure (darwin-4 same-config
RSS swung 1803–3032, CPU 6.5–12; NIX_VM_STATS perturbed peak RSS).  We cannot grade
any lever until measurement is trustworthy.

- **P0.1 — reliable TW-vs-v3 CPU+RSS harness.** Verify darwin-4 truly idle (or
  quantify noise floor); median-of-N (not min, not single); correlate fire-count +
  arena + peak-RSS + CPU in ONE run (a committed `bench/` script).  Re-establish the
  stable baseline table above with error bars.  Output: a re-runnable
  `bench/beat-tw-compare.sh` + a noise-characterization note.  GATE: same-config
  RSS variance < 5%.

- **P0.2 — WARM (cache-on) head-to-head.** Production uses the warm path (parse+lower
  amortized).  Re-measure TW vs v3 warm CPU+RSS cleanly (the cache-off gap overstates
  the steady-state).  This tells us the REAL production gap to close.

## Phase 1 — MEMORY (the bigger gap: 1.6–2.25× → <1×)

- **M1 — decompose "elsewhere" (~1200 MB M5).** RCA, not guess: instrument the
  ImportCache footprint (entry count × Value-subgraph size), the SQLite/disk-cache
  in-memory size, and the flake-fetch transient peak; A/B with NIX_V3_NO_DISK_CACHE.
  Identify the dominant component.  (Prior HNE decomp: ImportCache ~700 MB + SQLite
  ~270 MB — VERIFY on M5.)  This is the highest-potential RSS lever (>arena).

- **M2 — attack the dominant "elsewhere" component** (per M1; likely ImportCache).
  RCA the cache's retention (what keeps imported nixpkgs module subgraphs alive),
  then an LRU/size-cap eviction or a lighter representation.  Measure RSS delta +
  the warm-CPU cost of eviction (cache misses re-import).  GATE: byte-id + --brute.

- **M3 — make the arena reclaim visible to the OS.** Mid-eval reuse already reclaims
  the arena bump (1543→1023) but it does NOT lower peak RSS (calloc'd blocks, no
  munmap; peak set by transients).  RCA: confirm the bump-vs-peak timing (is the peak
  set by an early transient before the arena dominates?).  If the reclaim CAN matter:
  enable mmap'd arena blocks + whole-block-free under mid-eval (the deferred
  consistency change — alloc/free method must flip together for regular+huge blocks)
  so freed pages return to the OS.  Measure the real RSS delta.  GATE: --brute 22/22
  with reuse.  If the peak is transient-bound, DROP mid-eval as an RSS lever (honest
  kill) and focus on M1/M2.

- **M4 — thunk over-allocation (shared with C1). ✗ KILLED 2026-06-23
  (lode/M4_THUNK_AVOIDANCE_RCA).** Measured: only **0.7%** of v3's thunks are the
  trivial var/const forms TW's maybeThunk avoids (99.3% real deferred work — the
  opt passes already remove the trivial ones at compile time).  The v3-vs-TW count
  excess (~730K) is ≤17MB arena, negligible.  No maybeThunk gap.  The real thunk
  lever is CHURN (62-67% unforced) = a CPU lever needing strictness/eager-eval
  (byte-id risk, L3-hard, deferred), NOT maybeThunk.  **The real arena RSS lever is
  the LIVE SET → #134 ImportCache eviction (promoted to #1 RSS lever).**

## Phase 2 — CPU (1.8–2.5× → <1×)

- **C1 — thunk avoidance (= M4).** The ALLOC category is ~20% of on-CPU; fewer
  thunks cuts it directly.  Highest-leverage non-JIT CPU lever.

- **C2 — JIT (the structural lever; multi-week dedicated project).** The uniform
  per-op interpreter gap only closes with native codegen.  Foundations proven
  (J0 platform / J1 encoder / J2 byte-id codegen / J3 GC-safepoint contract — all
  validated standalone, lode/JIT_DESIGN_2026-06-19.md).  Remaining = J3 real-
  scavenger integration (spill live v3 ptrs at allocation safepoints; the UAF crux)
  + value-stack ABI trampoline + VM compile-trigger on hot callCount + bail +
  darwin-4 grade.  Ship only without gaming (must cover allocating bodies).
  RCA-first: confirm the hot-body coverage on M5/firefox (what fraction of CPU is in
  JIT-able bodies) BEFORE committing the multi-week build.

- **C3 — GC mark cost (conditional on M3).** If a non-moving sweep becomes a default
  RSS lever, the mark dominates its CPU (markMs up to 1 s/sweep on M5).  RCA + reduce
  (incremental mark / fewer sweeps / drop redundant conservative scans now that
  precise nursery traversal exists).

## Sequencing + kill criteria

1. P0.1 ✓, P0.2 (measurement) — FIRST, cheap, unblocks all.
2. M1 ✓ (decompose RSS) — arena 53% #1, MALLOC_SMALL 24% #2.
3. **M4/C1 (thunk avoidance) ✗ KILLED 2026-06-23** — measure-first falsified it
   (0.7% avoidable; opt passes already cover it).
4. **M2 (#134 ImportCache eviction) ✗ RE-FALSIFIED 2026-06-23** — was already
   killed 2026-05-30; re-confirmed on the current binary (firefox 675→890 MB as
   eviction tightens).  Root cause = **the arena never releases pages to the OS**
   (the SAME pin that killed mid-eval reuse this session).  Reclaim-based RSS
   levers are DEAD.  (lode/M2_IMPORTCACHE_FALSIFIED_2026-06-23.md.)

### REFRAME (after two RSS-lever KILLs): the arena-no-release wall + honest ceiling

Reclaim (eviction, reuse) cannot lower peak RSS — pages aren't munmap'd and reclaim
triggers fresh allocation.  Only two RSS paths survive, and **neither beats TW
alone**:
- **Allocate fewer cells** — FP-2/FP-3 mostly closed; only thunk CHURN left
  (L3-hard, byte-id risk).
- **Arena page-release (#136/M3)** — HARD (needs whole-block-free; sweep frees
  scattered cells), LOW ceiling: firefox→~480 MB (still 1.34× TW), M5 live arena
  ~1023 MB ALONE ≈ TW's whole 982 MB.
- **NEW lever surfaced — MALLOC_SMALL CU-shrink (#139)**: the 690 MB #2 bucket is
  libc-malloc (per-CU bytecode/IR/ICs/PosTables), NOT arena-pinned — free IR after
  lowering / shrink CU structures / fragmentation.  The only un-explored RSS lever.
- **Boehm FFI 402 MB** — fewer TW-Value crossings (V3-NATIVE-ward).

Honest: beating TW on RSS is a BROAD foundational program (M3 + #139 + Boehm +
cell representation), not a single lever.  Next RSS step = #139 (un-explored) then
M3 (with the low-ceiling caveat).

5. C2 (JIT) — last, multi-week, only after RCA confirms hot-body coverage; the
   structural CPU lever now that thunk-count avoidance is dead.

Each step: RCA/profile (no guess) → implement gated → validate (byte-id + --brute) →
measure on the P0 harness → git-note + lode note.  Beating TW likely requires M2
(caches) + C1 (thunks) + C2 (JIT) to all land; M3 may be a kill.  Honest bar: if a
lever's clean measured delta is below the noise floor, kill it (don't ship noise).

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0*
