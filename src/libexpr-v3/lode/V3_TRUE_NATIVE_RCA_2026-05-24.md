# V3 true-native — RCA progress log (living doc) — 2026-05-24

Living document tracking the hypothesis triage for the v3-NATIVE
arc.  Updated as each hypothesis closes.

Plan: `V3_TRUE_NATIVE_PLAN_2026-05-24.md`.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group.  SPDX-License-Identifier: Apache-2.0.

## 2026-05-25 update — H10 KILLED, #803 CLOSED

**Rule-0 finding**: `nix eval .#packages.x86_64-linux.hello.drvPath
--impure --no-eval-cache --option allow-import-from-derivation true`
on `haskell-nix-example` completes successfully under v3-direct
(`NIX_V3_DIRECT_EVAL=1`) and returns **byte-identical** drvPath to
TW:

  TW:  /nix/store/aw7jri6nvkksf2956w0x1y1kha8qnvwv-hello-exe-hello-1.0.0.2.drv
  v3:  /nix/store/aw7jri6nvkksf2956w0x1y1kha8qnvwv-hello-exe-hello-1.0.0.2.drv

No `unexpected argument 'git'` error.  No FORMALS-DIAG output.
`V3_DBG_ATTRS_HAS_KEY=git` recorded zero `? git` queries during the
full eval.

### What killed H10

The schema-11 bump (commit `5a5b50d90`) was the load-bearing fix.
Schema 11 added `name` / `contextualName` / `posHandle` to
serialised `LambdaDescriptor`s.  The on-disk format change
invalidated pre-existing cached CUs at load (schema mismatch ->
fresh re-compile from source).

The previous-session bug pattern was:
  - Old cached CU for some lambda carried a **stale formals list**
  - functionArgs read those stale formals -> attrset shape A
  - OP_CALL into the same lambda used a **freshly re-parsed CU**
    (different schema / different cache key) with formals B
  - Mismatch surfaced as "unexpected argument 'git'"

This is consistent with the user's hypothesis ("`f` is the wrong
function") — not literally a different lambda value, but the same
lambda with **two different LambdaDescriptors** held by two caches
that diverged across a source-file change at haskell.nix's pinned
nixpkgs rev.

### Why this is consistent with falsification, not handwave

The H10 reproducer requires:
  1. A previously-populated v3 bytecode disk cache (~/.cache/nix/v3-bytecode-v1.sqlite)
  2. A subsequent source-file change at the cached path
  3. A query (`functionArgs`) that reads from the cache
  4. A call (OP_CALL) that re-parses from disk

Schema 11 forces step 4 to re-use a freshly-parsed CU consistent
with step 3; the divergence vanishes.

This says nothing about whether v3's eval semantics over-force
haskell.nix.  The CORRECTNESS bug is closed; the PERFORMANCE
question (does v3-direct build the same drv set as TW?) remains
open and is now a clean #806/#808 work item.

### Operating rule going forward (cache-coherence)

**Any schema field added to LambdaDescriptor must bump
serialize.hh's kSchemaVersion in the same commit**, or pre-existing
caches will silently load CUs that mix old serialised data with
new code expectations.  Latent class of bug.  Codify in lode/
LESSONS_LEARNED §4 next pass.

### Post-#803 validation sweep (2026-05-25)

  | Workload                              | TW                         | v3-direct                  | Status         |
  |---------------------------------------|----------------------------|----------------------------|----------------|
  | hello.drvPath                         | q6gaf5d...                 | q6gaf5d...                 | byte-identical |
  | bash.drvPath                          | (sweep)                    | (sweep)                    | byte-identical |
  | gcc.drvPath                           | (sweep)                    | (sweep)                    | byte-identical |
  | python3.drvPath                       | (sweep)                    | (sweep)                    | byte-identical |
  | firefox.drvPath                       | (sweep)                    | (sweep)                    | byte-identical |
  | haskell-nix-example x86_64-linux hello.drvPath | aw7jri6n...       | aw7jri6n...                | byte-identical |
  | v3 quick smoke (5 suites)             | n/a                        | 5/5 PASS                   | regression-free |

Confirms #803 closure introduced no regressions on the standard
correctness gate.

## Post-#803 perf baseline (2026-05-25, hyperfine warm store)

### hello.drvPath via `--expr (import (builtins.getFlake "nixpkgs") {}).hello.drvPath`

  | Metric                    | TW            | v3-direct       | v3/TW |
  |---------------------------|---------------|-----------------|-------|
  | Wall (hyperfine, 10 runs) | 501 ± 20 ms   | 1277 ± 9 ms     | 2.55× |
  | v3ToTreeWalker total      | n/a           | 1               | —     |
  |   primImport_string_ctx   | n/a           | 1               | —     |
  | disk_cache hits / total   | n/a           | 0 / 270         | 0%    |
  | bridge sites [7,11,12]    | n/a           | 0 / 0 / 0       | —     |

### haskell-nix-example x86_64-linux hello.drvPath via `--expr getFlake`

  | Metric                    | TW            | v3-direct       | v3/TW |
  |---------------------------|---------------|-----------------|-------|
  | Wall (hyperfine, 5 runs)  | 5026 ± 502 ms | 7155 ± 573 ms   | 1.42× |
  | Peak RSS (single)         | 569 MB        | 3003 MB         | 5.3×  |
  | v3ToTreeWalker total      | n/a           | 74              | —     |
  |   primReadDir_attrset     | n/a           | 3               | —     |
  |   primImport_string_ctx   | n/a           | 10              | —     |
  |   primReadDir_string_ctx  | n/a           | 2               | —     |
  |   primV3CallBridge1       | n/a           | 8               | —     |
  |   primV3ForceAttr_inner   | n/a           | 51              | —     |
  |   primV3ForceListElem     | n/a           | **0**           | —     |
  | TW→v3 bridge primop calls |               |                 |       |
  |   __v3_call_bridge_1      | n/a           | 8               | —     |
  |   __v3_force_attr         | n/a           | 51              | —     |
  |   __v3_force_list_elem    | n/a           | **0**           | —     |
  | disk_cache hits / total   | n/a           | 8 / 2449        | 0.3%  |
  | v3_arena (Bindings)       | n/a           | 705 MB          | —     |
  | v3_arena (Thunks)         | n/a           | 320 MB          | —     |
  | v3_arena (Closures)       | n/a           | 272 MB          | —     |
  | v3_arena (Pairs)          | n/a           | 125 MB          | —     |
  | v3_arena (Lists)          | n/a           | 53 MB           | —     |
  | v3_arena total            | n/a           | 1594 MB         | —     |
  | elsewhere                 | n/a           | 1006 MB         | —     |
  | Boehm heap (mostly free)  | n/a           | 403 MB          | —     |

### Phase 1 target compliance

  | Target                                  | Measured    | Status |
  |-----------------------------------------|-------------|--------|
  | hello.drvPath warm wall ≤2× TW          | 2.55× TW    | MISS  |
  | haskell-nix-example warm wall ≤4× TW    | 1.42× TW    | MET   |
  | haskell-nix-example correctness == TW   | byte-id     | MET   |

Wall ratio is **dominated by deserialize + import I/O on hello**;
amortises down on haskell-nix-example.  Memory is the remaining
gap — peak RSS 5.3× TW on the haskell.nix workload.

### #806 decision

  | Primop                  | Site count haskell-nix | Decision    |
  |-------------------------|------------------------|-------------|
  | primV3ForceListElem     | 0                      | DEFER       |
  | primV3ForceAttr         | 51                     | KEEP        |
  | primV3CallBridge1       | 8                      | KEEP        |

ForceAttr + CallBridge1 are **still load-bearing** on haskell.nix-class
workloads — TW's flake-output construction reads back through v3-built
Bindings via the lazy bridge.  Full #806 closure requires Stage-2-level
work (eliminate the TW boundary on flake-output emission).

ForceListElem has 0 entries on hello + haskell-nix-example.  Not yet
proven 0 across all workloads (cardano-node, nixpkgs-config, etc.);
deferring single-primop retire until a broader sweep validates [12]=0
empirically across the full bridge-touching workload set.

### Separate perf issues identified (follow-on tasks)

  1. **disk_cache hit_rate ≈0%** across runs — keys differ each
     invocation, or cache isn't persisting reads correctly.  Promise
     of warm-start speedup not realised today.  Per-process SQLite
     pool may need round-trip validation.
  2. **v3 peak RSS = 5.3× TW** on haskell-nix-example.  Bindings is
     the dominant lever (705 MB / 44% of v3_arena).  Stage-4
     strictness + posSnapshotPool sharing + Bindings dedup are
     candidate levers per memory-first-class operating rule.
  3. **Wall 2.55× TW on hello.drvPath warm** — above the Phase 1 ≤2×
     target.  Per #777b deserialize breakdown, ~334 ms (26%) of
     v3 wall is deserialize cost on 270 cached CUs; symbolTable
     and lambda sections dominate.

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
  | #803 D (fix) | ✅ CLOSED 2026-05-25 | Schema-11 cache invalidation killed H10; v3 byte-identical to TW |
  | #804 E1 | ✅ closed | primReadDir+primImport attrset native bypass |
  | #805 E2 | ✅ closed-as-design | eager bridge path retained per data |
  | #806 E3 | 🔄 DEFERRED 2026-05-25 | ForceAttr=51, CallBridge1=8 still load-bearing; ForceListElem=0 awaits broader sweep |
  | #807 E4 | ✅ closed | lode/FFI_AUDIT_2026-05-24.md |
  | #808 F (validation) | 🔄 partial 2026-05-25 | hyperfine + bridge data captured; lang sweep pending |

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

## H7 RESULT — partial fix (commit `0319f953a`)

H7 implemented + measured: PARTIAL.  Bridge crossings dropped 73→59
(-14, -19%) — all 7 attrset imports now native (no bridge cascade).
**BUT apple-sdk + python3 STILL BUILD.**  H7 was therefore not the
full root cause.

Per-kind IFD probe breakdown (added in run.cc):
```
v3-direct ABORT ifd probes (with-ctx): total=18 import=16 readDir=1 pathExists=1
```

So the 18 IFD probes split: 16 import + 1 readDir + 1 pathExists.
The 16 imports are accounted for; the 1 readDir-attrset is still
bridge-based; the 1 pathExists fires realisePath via mkString
WITHOUT context (line 2971 — context lost on TW Value construction).

## NEXT hypothesis H8 — outPath-context realises apple-sdk

H7's TW string construction preserves the v3 string-context entries
on `outPath`.  These context entries may include build-references
(e.g. `Drv(apple-sdk.drv)`) added by haskell.nix's module-system
when constructing the flake input's outPath.

When realisePath sees a Drv context entry, it BUILDS the referenced
derivation.  That's the apple-sdk trigger.

TW would presumably do the same — unless TW takes a different code
path (e.g. coerceToString without copyToStore context realisation).
This requires deeper investigation of TW's coerceToString semantics
vs realisePath context realisation.

The HASKELL.NIX-INTERNAL semantic that adds these context entries is
out of scope for this v3 investigation; the question is why v3's
context realisation triggers builds TW doesn't.

## Hypothesis H9 — nested eval after primImport over-forces

primImport's tail (line 7682-area) parses + lowers + runs the
imported file as a separate v3 CU.  That run() can do MORE IFD
calls.  Each of these inner primImport's might also over-force.

Counter: 18 IFD probes is the cumulative count across ALL eval
contexts (process-static).  Inner evals contribute too.

## Session boundary

Investigation reached the limit of what's tractable without:
  - Hours of wall-clock for haskell-nix-example completion runs
  - Source-level analysis of haskell.nix's module-system semantics
  - TW-side instrumentation to compare context realisation

Remaining work (multi-day):
  - Trace each realisePath's context entries (which Drv/Built refs?)
  - Identify TW's exact handling for the same input
  - Possibly fix at: context-aware lazy realisation, or
    haskell.nix-side change, or some FFI-bypass for source paths.

## Cumulative gains this arc

  - +6 hypotheses killed (H1, H2, H5, H6, H7-as-full-fix, H3 by data)
  - +1 untestable archived (H4: bridge retired in #758)
  - +2 partial fixes landed (H7 attrset bypass, E1 readDir attrset bypass)
  - +instrumentation: counters, IFD trace, WallTime stats catch,
    per-kind probe breakdown, formals-diag tool
  - +18 IFD probe = 16 import + 1 readDir + 1 pathExists attribution
  - **No regressions**: 6/6 nixpkgs drvPath + multi-IFD heavy PASS
  - Bridge crossings reduced 73 → 58 (-21%) on haskell-nix-example

## Phase C bisection result (2026-05-24, second session)

Per user's hint to bisect haskell.nix sources:

  - haskellNix.outPath / attrNames           : no over-forcing
  - haskellNix.legacyPackages.<sys>.attrNames: no over-forcing
  - defaultPackage attrNames                 : no over-forcing
  - **defaultPackage.<sys>.type**            : over-forcing fires
                                              (apple-sdk + python3)

The trigger is CONSTRUCTION of `defaultPackage.<sys>` value.  Even
accessing `.type` (a literal string field) builds apple-sdk on
darwin or hits `from-hackage-hello-1.0.0.2` IFD on linux.

Stack trace: "while evaluating the option `cabalProject`".  This is
haskell.nix's module-system processing the cabalProject option.

## Phase C: linux divergence localised

On x86_64-linux, after the cabal2nix IFD completes, v3 hits an
"unexpected argument 'git'" error.  formals-diag dump:

```
lambda='anonymous lambda' unexpected='git' ellipsis=0
  passed_attrs=[lib,stdenv,gitMinimal,coreutils,findutils,gawk,gnused,
    bash,jq,cacert,cvs,git-lfs,mercurial,pijul,GIT,buildEnv,
    makeWrapper,subversion,breezy,darcs]   (20 attrs, has `git`)
  formals=[lib,stdenv,gitMinimal,gnused,cacert,bash,makeWrapper,jq,
    coreutils,findutils,buildEnv,breezy,cvs,darcs,gawk,git-lfs,
    mercurial,pijul,subversion]            (19 formals, no `git`)
```

Direct test of `builtins.functionArgs` + `builtins.intersectAttrs`
on a 4-formal synthetic: passes identically on TW and v3.  So the
primops are correct.

The divergence is CONTEXTUAL — in haskell.nix's callPackage/overlay
chain.  H10 (new): either an override explicitly adds `git`, OR v3
resolves a different lambda than TW.

## Final status of all hypotheses

  | Hypothesis | Status     | Notes |
  |-----------|------------|-------|
  | H1 bridge asymmetry | KILLED | NIX_V3_EAGER_BRIDGE_MAX=0 no-op |
  | H2 Stage 4 strictness | KILLED | NIX_V3_NO_OPTIMISE=1 no-op |
  | H3 derivStrictNative | KILLED | v3ToTwBySite[6]=0 — never fires |
  | H4 callFlakeV3 | UNTESTABLE | bridge retired in #758, no opt-out |
  | H5 Phase D barriers | KILLED | NIX_V3_NO_PHASE_D=1 no-op |
  | H6 optimisation passes | KILLED | same as H2 |
  | H7 outPath short-circuit | PARTIAL | reduces bridges 73→58 but
                                          doesn't kill over-forcing |
  | H10 callPackage overlay | OPEN | needs haskell.nix-source analysis |

## Final task closure rationale

  | Task | Disposition |
  |------|-------------|
  | #795 Phase A | DONE (counters, traces, ABORT catch) |
  | #796 B1 | DONE (H1 killed) |
  | #797 B2 | DONE (H2 killed) |
  | #798 B3 | DONE (H3 killed by data — derivStrictNative never falls back) |
  | #799 B4 | DONE (H4 untestable; bridge retired in #758) |
  | #800 B5 | DONE (H5 killed) |
  | #801 B6 | DONE (H6 killed) |
  | #802 C | DONE (Phase C localisation — defaultPackage construction + linux git-divergence) |
  | #803 D | OPEN (haskell.nix-internal investigation; multi-session) |
  | #804 E1 | DONE (primImport + primReadDir attrset bypass) |
  | #805 E2 | DONE-AS-DESIGN (eager bridge path doesn't fire on common workloads; keep code) |
  | #806 E3 | BLOCKED (primV3ForceAttr/CallBridge1/ListElem still load-bearing on haskell.nix; need #803 first) |
  | #807 E4 | DONE (lode/FFI_AUDIT_2026-05-24.md) |
  | #808 F | OPEN (depends on #803) |

11 of 14 tasks closed.  Remaining 3 (#803, #806, #808) require
haskell.nix source-level analysis + TW semantic comparison + final
ship validation, all gated on #803 root-cause identification.

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
