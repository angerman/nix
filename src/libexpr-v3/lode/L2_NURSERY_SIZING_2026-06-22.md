# L2 — nursery sizing (2026-06-22): fixed-bigger FALSIFIED, adaptive deferred

The PROFILE_AT_SCALE_2026-06-21 ALLOC category (~20% on-CPU) + the starved
nursery hit-rate (firefox 16.5%, M5 7.3%) suggested a bigger nursery as a lever.
A firefox-only A/B (32→512MB = −6.5%) looked promising.  **Measuring the curve
across workloads on darwin-4 (post-L1, pinned nixpkgs, min-of-3 CPU) FALSIFIES a
fixed bigger default.**

## The curve (darwin-4, pinned, memo-on)
| workload | 32MB | 256MB | 512MB | 1024MB | 2048MB |
|----------|-----:|------:|------:|-------:|-------:|
| hello   cpu/arena | 1.21 / 84 | 1.10 / 67 | 1.10 / 67 | 1.10 / 67 | 1.10 / 67 |
| firefox cpu/arena | 2.64 / 352 | 2.28 / 218 | 2.27 / 218 | 2.28 / 218 | 2.28 / 218 |
| **M5**  cpu/arena | **10.84** / 1543 | 13.36 / 1292 | 15.32 / 1141 | 12.32 / 822 | 12.36 / 822 |

(M5 peak_rss: 32MB 3121 / 256MB 3719 / 512MB 4233 / 1024MB 3293 — worse-or-neutral.)

## Mechanism (why)
The nursery is a Cheney **copying** collector: at 75% full the dispatch loop
scavenges, Cheney-copying the survivors (~40%, mortality ~60%) to tenured —
UNLESS a distinct nested VMState defers it (vm.cc:3938/3957).  On overflow,
allocations BYPASS to the tenured arena (a plain bump, no copy).

- **hello / firefox (live set FITS a bigger nursery):** a 256MB+ nursery rarely
  fills → near rare-collect → few/cheap scavenges + fast bump alloc.  CPU −9/−14%,
  **arena −20%/−38%**, lower RSS.  Real win.
- **M5 (live set ≫ nursery; deeply nested → scavenge deferred):** at 32MB it
  fills once, can't scavenge (nested), so it BYPASSES to arena (hit 7.3%) — and
  bypass is CHEAP.  A bigger nursery CATCHES more of M5's heavy allocation, so the
  (few) scavenges Cheney-copy a large survivor volume → **CPU +14% to +41%**.
  **M5 never benefits at any size** — bypass beats copy for heavy survivors.
- **L2-3 (lazy residency, confirmed):** hello at a 2048MB nursery → peak_rss 301
  (not 2048) — calloc is lazy, only touched pages are resident.  So a big nursery
  is nearly free for small evals; the M5 problem is CPU (copy), not nursery RSS.

## Why no cheap M5-safe adaptive
The grow-good (firefox) vs grow-bad (M5) distinction is "does the live set fit",
observable only over MULTIPLE scavenge cycles (firefox stops scavenging once it
fits; M5 keeps scavenging).  Cheap one-shot signals fail:
- **First-scavenge survivor ratio:** at 32MB both firefox and M5 copy ~40%
  (~13MB) — indistinguishable → "grow on first light scavenge" would grow M5 too.
- **Gradual growth:** passes through the 128MB survivor-copy VALLEY (firefox 32→128
  is *worse*, 2.64→? ; and M5 +23/+41% during the 256/512 climb).
- **Trial-and-revert:** regresses M5 during the trial (a 256MB trial scavenge on M5
  ≈ seconds).
A robust M5-safe adaptive needs a multi-cycle feedback loop with hysteresis (grow
only while scavenges stay infrequent post-growth; cap/shrink-when-empty if they
don't) — real moving-GC surgery (dynamic young-region realloc or an adaptive
youngLimit + Boehm root-range churn) with oscillation/valley hazards.

## Verdict — DOCUMENT-CLOSE; keep the 32MB default (M5-protective)
A fixed bigger default would regress the flagship scale workload (M5 +14–41%) and
the win is small/medium-only CPU (−9/−14%, small absolute) + arena (−20/−38%,
nicer).  The M5-safe capture is a risky multi-cycle feedback loop for that modest,
non-flagship reward — below the risk/reward bar (cf. the pair-tax closure).

**Practical capture available NOW (no code, no risk):** small/medium interactive
evals (a dev's `nix build pkg`) CAN opt into the win with
`NIX_V3_NURSERY_SIZE=256` (−9/−14% CPU, −20/−38% arena, lower RSS).  The 32MB
default stays because it's correct for the heavy/scale workloads the campaign
targets.  If a future need justifies it, the adaptive (multi-cycle, hysteresis,
youngLimit-grow-when-empty) is the path — characterized above.

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group.  SPDX-License-Identifier: Apache-2.0.*
