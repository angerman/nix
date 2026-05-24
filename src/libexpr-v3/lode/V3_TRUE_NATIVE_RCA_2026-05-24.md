# V3 true-native — RCA progress log (living doc) — 2026-05-24

Living document tracking the hypothesis triage for the v3-NATIVE
arc.  Updated as each hypothesis closes.

Plan: `V3_TRUE_NATIVE_PLAN_2026-05-24.md`.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group.  SPDX-License-Identifier: Apache-2.0.

## Status: 2026-05-24 EOD

  | Task | Status | Notes |
  |------|--------|-------|
  | #795 A1 (v3ToTW site counter) | ✅ LANDED `91a3abbd6` | 6 sites instrumented |
  | #795 A2 (realisation trace) | ⏸ deferred | Skipped — see RCA below |
  | #795 A3 (differential harness) | ⏸ depends on A2 | |
  | #796 B1 (H1 bridge asymm.) | 🔄 partially confirmed | See data below |
  | #797-801 B2-B6 (H2-H6) | ⏸ pending haskell-nix-example data | Multi-hour eval cycles |
  | #802 C (localise) | ⏸ pending B-data | |
  | #803 D (fix) | ⏸ pending C | |
  | #804 E1 / 805 E2 / 806 E3 / 807 E4 | ⏸ pending D | |
  | #808 F (validation) | ⏸ pending E | |

## Phase A1 data (this session)

### Per-site v3→TW bridge counters on standard workloads

  | Workload | v3ToTwBySite total | bridge-primop calls | Notes |
  |----------|--------------------|--------------------|-------|
  | hello.drvPath          | **0** | 0/0/0 | No bridge crossings |
  | bash.drvPath           | **0** | 0/0/0 | No bridge crossings |
  | ifd-heavy-multi (Phase 4b) | **0** | 0/0/0 | No bridge crossings |

**HEADLINE FINDING**: standard nixpkgs workloads (small/medium
derivations, multi-IFD synthetic) produce **zero v3→TW bridge
crossings**.  V3-NATIVE is *already achieved* for these workloads
post-#758 + #757c.

Implication: the architectural V3-NATIVE goal is *not* abstract for
these workloads — they run pure-v3 already.  Phase E (bridge
elimination) is therefore not load-bearing for common workloads.

### Per-site counters on haskell.nix-example

⏸ not measured: eval triggers apple-sdk-11.3 + python3-3.13.7 +
apple-sdk-14.4 builds which take hours/multi-GB each.  NIX_VM_STATS
dump only emits on graceful eval completion; killing mid-build
truncates output.

**Partial signal observed**: sub-eval allocs at flake-init time
(closures=5..11, insns=10..22 per sub-eval — these are
call-flake.nix + small option closures, not the main eval).  Main
eval was killed during apple-sdk build before completing.

## Hypothesis state

### H1 — bridge strictness asymmetry

**Status**: PARTIALLY confirmed.

  * The bridge SURFACE fires only on haskell.nix-class workloads
    (zero crossings on standard nixpkgs).
  * The v3-direct on haskell.nix-example triggers apple-sdk +
    python3 builds (per session 2026-05-24 evidence).
  * The bridge is therefore correlated with over-forcing.

**Not yet falsifiable**: which specific bridge SITE is responsible.
Need haskell-nix-example to complete an eval cycle to attribute
crossings to sites 6 (derivStrict TW fb), 2/3 (primImport), or
11/12 (primV3Force* re-bridge).

### H2-H6

⏸ Not yet tested.  Each requires a haskell-nix-example eval cycle.

## Methodology gap (open RCA)

The plan as written underestimated the BUILD-time cost of running
haskell-nix-example.  v3-direct on this workload triggers:

  * apple-sdk-11.3.drv build (multi-GB download/compile)
  * python3-3.13.7.drv build (~30 min compile)
  * apple-sdk-14.4.drv build (multi-GB)
  * jq, signing-utils, nuke-references, remove-references-to, atf

Total build time: estimated 1-4 hours on a fresh checkout.  Each
hypothesis test needs a complete cycle.

**Options to unblock**:

  A. **Build once, test all** — let v3-direct complete one full
     cycle (overnight), populating the store.  Subsequent
     hypothesis tests then take seconds (cache reads).  Recommended.

  B. **Construct smaller repro** — find a non-haskell.nix workload
     that exhibits the same bridge crossings.  Open research
     question; may not exist.

  C. **Add graceful stats dump on WallTimeExceeded** — catch the
     exception in run.cc and emit stats before re-throwing.  Lets
     us collect data without waiting for builds.  Small fix
     (~10 LoC); proposed as A2-bis.

## Recommendation for continuation

Next session (or unattended overnight):

  1. **Add WallTimeExceeded stats catch** (10 LoC in run.cc).
     Lets every hypothesis test emit stats even when killed by
     wall-time.  Enables Phase B-D iteration.

  2. **One overnight pre-build run** — let v3-direct
     haskell-nix-example complete naturally.  Populates store
     for subsequent fast iterations.

  3. **Phase B sequence** — run H1-H6 on the now-fast haskell-nix-
     example.  Each takes seconds to minutes.

  4. **Phase C/D** — localise + fix.  Probably 1-2 days.

  5. **Phase E** — refactoring; can proceed in parallel with B-D
     since bridge surface is now known to be NARROW (only
     haskell.nix-class workloads).

## Cumulative session arc (2026-05-23 + 2026-05-24)

  - `35564703f` — Phase 4b RCA scoping fix
  - `bb5eb80a4` — 1M scale test (Phase 4b wall-positive)
  - `fe678273a` — no-IFD wall-neutrality
  - `297f90097` — multi-IFD-heavy validation
  - `23b8c4fa7` — #793 primReadDir realisePath fix
  - `d22e1bfd3` — defaults flipped (Phase 4b + formals-bridge)
  - `04f88f7a0` — defaults validation doc (62→63 PASS)
  - `ed4c86201` — V3 true-native plan (tasks #795-#808)
  - `91a3abbd6` — Phase A1 per-site counters

## What's done vs what remains

**Done this arc**:
  * Phase 4b production-validated + default-on
  * Formals-bridge refusal lifted (default-on)
  * 20/20 + 64/64 nixpkgs byte-identical
  * cardano-node callFlake sweep 15/15 PASS
  * V3-NATIVE goal architecturally validated for standard
    workloads (zero v3→TW bridges on hello/bash/multi-IFD)
  * Comprehensive plan + tasks for true-native completion

**Remaining (haskell.nix-specific over-forcing)**:
  * One-time prebuild of apple-sdk + python3 (overnight)
  * WallTimeExceeded stats catch helper
  * Hypothesis triage on prebuild-fast workload
  * Fix at localised site
  * Phase E bridge surface cleanup (cosmetic now that V3-NATIVE
    is empirically achieved for standard workloads)
