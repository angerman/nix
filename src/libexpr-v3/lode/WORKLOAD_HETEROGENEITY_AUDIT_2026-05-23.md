# Workload heterogeneity audit — 2026-05-23

Cross-workload measurement of v3 wall structure across 9 candidate
workloads.  Generated using `bench/workload-heterogeneity.sh`
(committed).

**Goal**: identify whether the optimization levers exposed by
hello.drvPath generalise to other workloads, or whether some
workload has a different wall structure exposing new levers.

**Result**: drvPath workload class is HOMOGENEOUS (5 packages all
show the same structure as hello).  `.name` workloads are a
STRUCTURALLY DIFFERENT class (parsing/lowering-dominated, not
derivation-dominated).  firefox.drvPath is hello.drvPath at 4-5×
scale.  Phase 5 wall hypothesis FALSIFIED cross-workload.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group.  SPDX-License-Identifier: Apache-2.0.

---

## Cross-workload data table

| Workload          | Peak RSS | Insns | Attrsets | Bindings | Drv calls | Drv ms | Top primops |
|-------------------|----------|-------|----------|----------|-----------|--------|-------------|
| hello.name        |  195 MB  | 2.3 M |  33 682  |  73 MB   |       0   |    0   | __findFile, import, removeAttrs |
| hello.drvPath     |  666 MB  | 15 M  | 200 533  | 406 MB   | 16 226    | 10 220 | __derivationFromPreprocessed, __derivCoerce, __derivationStrictRaw |
| hello.outPath     |  666 MB  | 15 M  | 200 533  | 406 MB   | 16 226    |  8 798 | same |
| bash.drvPath      |  669 MB  | 15.3M | 203 631  | 406 MB   | 16 493    |  9 197 | same |
| coreutils.drvPath |  636 MB  | 12.2M | 168 245  | 399 MB   | 11 903    |  7 551 | same |
| gcc.drvPath       |  671 MB  | 15.4M | 205 282  | 407 MB   | 16 874    |  7 177 | same |
| python3.drvPath   |  673 MB  | 15.6M | 208 511  | 407 MB   | 17 143    |  9 310 | same |
| firefox.name      |  197 MB  | 2.3 M |  34 873  |  74 MB   |       0   |    0   | __findFile, import, removeAttrs |
| firefox.drvPath   | 1432 MB  | 60 M  | 757 691  | 687 MB   | 77 185    | 39 475 | same |

## Findings

### Pattern 1 — `.name` queries are a different workload class

hello.name and firefox.name both:
  * Have **0 derivation primop calls**
  * Use only ~33-35 K attrsets (vs ~200K for drvPath)
  * Peak at ~195 MB (vs ~666 MB)
  * Run only ~2.3 M instructions (vs ~15 M)
  * Top primops are `__findFile, import, removeAttrs` — *not*
    derivation primops

These are parsing / lowering / nixpkgs-attribute-traversal
dominated.  They never reach the derivation construction code
path because `.name` is reachable from `mkDerivation`'s direct
attrs without forcing the rest of the build inputs.

Implication: hello.drvPath optimization levers (#741 cache,
derivation primop work) are IRRELEVANT to `.name` workloads.  If
`.name` performance matters, different levers are needed
(parser, lowerer, primop call overhead).

### Pattern 2 — All `.drvPath` workloads are structurally identical

5 different packages (hello, bash, coreutils, gcc, python3) all
show:
  * 12-17 K derivation primop calls (variance: 1.4×)
  * Drv primops top-3 by inclusive wall
  * ~400 MB Bindings allocation (variance: 2%)
  * ~666 MB peak RSS (variance: 6%)
  * 12-15 M instructions

Per-call drv cost varies (gcc 425 µs / call vs hello 630 µs /
call) but the SHAPE is identical: drv primops dominate, Bindings
dominate memory, all-primop wall ≈ drv wall.

Implication: hello.drvPath's wall structure GENERALISES to the
whole drvPath workload class.  The #741 findings apply uniformly.

### Pattern 3 — firefox.drvPath is hello.drvPath at 4-5× scale

  * Drv calls: 4.8× (77 185 vs 16 226)
  * Peak RSS: 2.2× (1432 MB vs 666 MB)
  * Insns: 4× (60 M vs 15 M)
  * Drv ms: 3.9× (39 475 vs 10 220)

Same shape, more of everything.  Per-call drv cost slightly LOWER
on firefox (511 µs vs 630 µs).

**Tested Phase 5 ACTIVE+DISK on firefox.drvPath**:
  * Cold cache: 3499 lookups, 2040 hits (**58.3 % intra-process**
    duplicate rate vs hello's 34 %), 1459 misses
  * Warm cache: 3499 / 3499 / 100 % hit rate
  * Wall WARM vs OFF: **1.02 × slower** (within noise).  Same
    result as hello.drvPath — the libstore-tail target remains
    too small relative to cache I/O even at 4-5× scale.

### Pattern 4 — hello.drvPath vs hello.outPath

Same instructions (15 M), same attrsets, same memory.  drvPath
shows MORE drv ms (10 220 vs 8 798 — +16 %).

Hypothesis: drvPath forces extra ATerm serialisation /
computeStorePath / hashDerivationModulo work that outPath skips.
Worth a follow-up audit if it matters.  ~1.4 s wall delta is too
small for hello-scale eval to matter.

## Strategic implications

### #741 wall lever — falsified cross-workload

Phase 5 ACTIVE+DISK was tested on:
  * hello.drvPath: 1.00× ± 0.02 (in-noise)
  * firefox.drvPath: 1.02× slower (in-noise)

Across the **whole drvPath workload class** (homogeneous per
Pattern 2), in-process caching is wall-neutral-or-negative.  The
libstore tail (~30-50 µs / call) is too small relative to cache
I/O (~15 µs lookup + deserialise).  Earlier-in-body hooking
would help but re-enters the shapeCell-pollution constraint.

**#741 is now CLOSED at "correct, no wall benefit"** for the
drvPath workload class.

### Levers identified from this audit

  1. **`.name` workloads are a separate optimization class**.
     If `.name` perf matters (e.g. flake exploration, IDE
     hover, attribute-set enumeration), the relevant levers are
     parser / lowerer / __findFile-import-removeAttrs path —
     none of which #741 touches.

  2. **firefox.drvPath as the new performance benchmark**: at
     1.4 GB peak RSS and 60 M instructions, it surfaces issues
     hello.drvPath doesn't.  For future memory or instruction-
     count optimizations, firefox is a better signal than hello.

  3. **gcc.drvPath has CHEAPER per-call drv cost** (425 µs vs
     hello's 630 µs).  Worth investigating whether gcc-style
     derivations could inform a faster code path applicable to
     hello.  Likely lower priority — the absolute drv ms is
     small either way.

  4. **`.outPath` query has +16 % drv ms over `.drvPath`** on
     hello.  Surprising — outPath should be cheaper (it doesn't
     compute the drv hash).  Maybe drvPath caches state outPath
     re-derives.  Investigate if needed.

### Untested workloads (potential future audit subjects)

  * **nixpkgs sweep** (the existing 64-package test): structurally
    similar to firefox-on-steroids.
  * **cardano-node M5**: heavier still; would test if v3:TW ratio
    stays in current ~2× band.
  * **haskell.nix hello-world**: would expose actual IFD firing
    (vs current OP_IFD_PROBE count which is mostly nullary
    findFile / non-derivation imports).  This is the Phase 4
    motivation workload.
  * **nixos-toplevel**: known slow; might surface different
    bottlenecks if it completes.

## Concrete next-session candidates (post-audit)

Ordered by leverage:

  1. **Phase 4 — Class B IFD primops** (haskell.nix workload):
     test whether REAL IFD primops (where build cost is seconds,
     not microseconds) make cache wall savings show up.  This
     is the original S4 scope per IFD_DEEP_DIVE §11.

  2. **`.name`-class optimization**: separate workload class with
     its own bottleneck.  Audit the __findFile / import /
     removeAttrs path; identify levers.  Could be a 1-session
     spike.

  3. **#776 Let-floating** (Stage 4 v4 followup): the audit
     doesn't disprove Stage 4 work being useful.  Just confirms
     the drv-primop-cache lever is small.

  4. **Memory work on firefox.drvPath**: 1.4 GB peak RSS could
     have new levers different from hello.drvPath (where #748/
     #750/#752 took peak from 4.3 GB → 1.08 GB).

## Methodology notes

  * The script ran each workload ONCE with `NIX_VM_STATS=1` +
    `NIX_VM_PRIMOP_TIME=1`.  Single-run measurement; not
    statistically rigorous on wall.  Use hyperfine for wall.
  * Initial bug: `head -1` grabbed sub-eval stats; fixed to
    `tail -1` for main eval.  All numbers above are from the
    main eval's stats dump.
  * Wall shown at whole-second resolution because the script
    times via shell `date` — coarse but adequate to show the
    coreutils-fastest / firefox-3× pattern.

## Falsifier outcome

The hypothesis tested by this audit: "Workload heterogeneity
exposes new wall-clock levers different from hello.drvPath."

**PARTIALLY CONFIRMED**: yes, `.name`-class workloads are a
different class.  **MOSTLY FALSIFIED**: the drvPath workload class
is homogeneous — 5 packages all show the same hello.drvPath
structure.  Phase 5 wall hypothesis re-falsified at 4-5× scale on
firefox.

## Cross-references

  * `bench/workload-heterogeneity.sh` — the audit script
  * `lode/ROADMAP_ALIGNMENT_POST_741_2026-05-23.md` — proposed
    this audit as Tier 4 #8
  * `lode/IFD_CACHE_DESIGN_2026-05-23.md` — #741 design
  * `[[741-arc-complete-2026-05-23]]` — #741 arc memory
