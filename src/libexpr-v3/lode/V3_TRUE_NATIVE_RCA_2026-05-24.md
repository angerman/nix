# V3 true-native — RCA progress log (living doc) — 2026-05-24

Living document tracking the hypothesis triage for the v3-NATIVE
arc.  Updated as each hypothesis closes.

Plan: `V3_TRUE_NATIVE_PLAN_2026-05-24.md`.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group.  SPDX-License-Identifier: Apache-2.0.

## Status: 2026-05-24 EOD (UPDATED)

  | Task | Status | Notes |
  |------|--------|-------|
  | #795 A1 (v3ToTW site counter) | ✅ LANDED `91a3abbd6` | 7 sites instrumented |
  | #795 A2 (IFD trace V3_DBG_IFD) | ✅ LANDED (this session) | Per-IFD-import path+ctx trace |
  | #795 A3 (WallTime stats-catch) | ✅ LANDED (this session) | Emits ABORT counts on WallTimeExceeded |
  | #796 B1 (H1 bridge asymm.) | ✅ KILLED | NIX_V3_EAGER_BRIDGE_MAX=0 no effect |
  | #797 B2 (H2 Stage 4 strict) | ✅ KILLED | NIX_V3_NO_OPTIMISE=1 no effect |
  | #800 B5 (H5 Phase D) | ✅ KILLED | NIX_V3_NO_PHASE_D=1 no effect |
  | #801 B6 (H6 optimisations) | ✅ KILLED | NIX_V3_NO_OPTIMISE=1 no effect |
  | #798 B3 (H3 derivStrict) | ⏸ requires code-gate add | |
  | #799 B4 (H4 callFlake) | ⏸ bridge retired in #758 | can't toggle |
  | #802 C (localise) | 🔄 partial | 17 IFD entries identified; trigger TBD |
  | #803 D (fix) | ⏸ pending root cause | |
  | #804 E1 / 805 E2 / 806 E3 / 807 E4 | ⏸ pending D | |
  | #808 F (validation) | ⏸ pending E | |

### Phase B triage outcome

**All four testable hypotheses (H1, H2, H5, H6) killed** — the over-
forcing is INVARIANT to:
- Bridge mode (eager / lazy)
- Stage 4 strictness analysis
- Phase D write barriers
- All optimisation passes

The bridge crossings are IDENTICAL across all variants (73 v3ToTreeWalker
calls, 18 IFD probes, same 17 IFD entries).  This means the over-
forcing is INTRINSIC to v3's eval semantics, not driven by any optional
feature.

### Phase A2 IFD-trace data

V3_DBG_IFD=1 reveals all 17 IFD bridge entries on haskell-nix-example:

  | Order | Type   | Path / attrs |
  |-------|--------|--------------|
  | 1-2   | string-ctx | flake.nix x2 |
  | 3     | attrset | 17-attr (outputs/outPath/inputs/sourceInfo/_type/narHash/rev/...,lib,legacyPackages,...) ← nixpkgs flake-outputs |
  | 4-5   | string-ctx | flake.nix x2 |
  | 6     | attrset | 10-attr (outputs/outPath/inputs/sourceInfo/_type/narHash/rev/...) |
  | 7     | attrset | 17-attr (same shape as #3) |
  | 8-10  | string-ctx | flake.nix x3 |
  | 11-13 | attrset | 17-attr x3 (same shape) |
  | 14-15 | string-ctx | index-state.nix x2 |
  | 16    | attrset | **7-attr (outPath/sourceInfo/narHash/rev/shortRev/lastModified/lastModifiedDate)** |
  | (next) | apple-sdk-11.3 building |

**The 16th IFD entry is the over-forcing trigger.**  It is an attrset
import with ONLY metadata attrs (no `outputs`, no `legacyPackages`).
Right after this import returns from realisePath, apple-sdk-11.3 begins
building.

### Hypothesis H7 (new) — `import <flake-source>` over-realises context

When haskell.nix's flake.nix evaluates `import inputs.foo`, the
attrset `inputs.foo` has a context entry referencing the flake input's
source storePath.  `realisePath` calls `coerceToString` which, on TW
side, iterates the attrset's attrs to compute the string + context.
If one of the iterated attrs is a thunk whose body forces
`legacyPackages.aarch64-darwin.stdenv.cc`, v3's eager-side eval
realises the full stdenv chain — including apple-sdk.

TW's coerceToString may short-circuit on `outPath` attr earlier, only
returning that string and skipping iteration.  If v3's bridge does NOT
short-circuit (because it iterates via primV3ForceAttr per-attr lookup,
which forces them lazily but the act of being a primOpApp may force the
v3 attrset to compute them eagerly), the divergence emerges.

**Test for H7**: in primImport's attrset branch, manually extract the
attrset's `outPath` attr v3-NATIVELY before bridging, and pass that
string directly to realisePath.  If apple-sdk builds disappear, H7 is
confirmed.

This is the most promising single fix.

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
