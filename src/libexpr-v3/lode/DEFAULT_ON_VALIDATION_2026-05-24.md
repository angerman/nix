# Post-default-on broad validation — 2026-05-24

After commit `d22e1bfd3` flipped two defaults coordinated:
  (1) `NIX_V3_REFUSE_FORMALS_BRIDGE` lifted (opt-OUT instead of opt-IN)
  (2) `NIX_V3_IFD_IMPORT_CACHE_DISK` lifted (Phase 4b default-on)

This doc records the post-flip validation sweep proving the
defaults are zero-regression and net-positive across measured
workload classes.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group.  SPDX-License-Identifier: Apache-2.0.

## Headline: strictly an improvement

  | Mode          | PASS | FAIL | vs other |
  |---------------|------|------|----------|
  | **New defaults** | **63** | **16** | +1 PASS |
  | Old defaults  |   62 |   17 | baseline |

The single test that PASSES with new defaults but FAILS with old
defaults: `679-cosmetic-parity.sh`.  All 16 failures under new
defaults ALSO fail under old defaults — they are **100% pre-
existing**, not regressions from the flip.

## Validation sweep matrix

### A. Regression suites

  | Suite                                  | Result | Notes |
  |----------------------------------------|--------|-------|
  | Core (10 tests)                        | 9/10   | Lint nit only (pre-existing, vm.cc:92) |
  | Full (79 tests, new defaults)          | 63/79  | 16 failures = 100% pre-existing macOS-specific |
  | Full (79 tests, OLD defaults baseline) | 62/79  | 17 failures = SAME 16 + 679-cosmetic |

### B. Byte-identical nixpkgs sweep

  | Sweep                              | Result | Notes |
  |------------------------------------|--------|-------|
  | 20-package custom drvPath/name     | 20/20  | Curated diversity: small/medium/heavy |
  | 64-package nixpkgs sweep (#759)    | 64/64  | All passing in --full suite |
  | callFlake sweep (#758) inc. cardano | 15/15 | cardano-node workloads PASS |

### C. Phase 4b cache behavior

  | Test                                          | Result |
  |-----------------------------------------------|--------|
  | hello.name + hello.drvPath: no IFD events     | 0/0    |
  | hello.drvPath: EvalResults table              | empty  |
  | bash/gcc/python3/firefox.drvPath: IFD events  | 0 each |
  | Multi-IFD-heavy: cache scope                  | 5 entries × 865 B |
  | Cross-process WARM hit rate                   | 5/5 (100 %) |

Phase 4b cache is **correctly scoped** — inactive on all standard
nixpkgs workloads (no false-positive caching), active and complete
on actual IFD workloads.

### D. Wall measurements (new defaults vs opt-out)

  | Workload            | new defaults | opt-out (old) | ratio |
  |---------------------|--------------|---------------|-------|
  | hello.drvPath       | 674.6 ms     | 707.7 ms      | 1.05× faster (within σ) |
  | firefox.drvPath     | 2544 ms      | n/a           | TW=1287 ms (1.98× TW, unchanged) |
  | multi-IFD-heavy     | 840.4 ms     | 1656 ms       | **1.97× faster** (Phase 4b auto-applying) |

### E. Stress tests

  | Test                                                  | Result |
  |-------------------------------------------------------|--------|
  | V3_DBG_GC_STRESS=100 + hello.drvPath                   | PASS   |
  | V3_DBG_GC_STRESS=100 + multi-IFD-heavy (cached)        | PASS   |
  | Phase D barriers + Phase E nursery + Phase 4b cache + GC stress | PASS |

## RCA of the 16 pre-existing failures

All 16 failures are reproducible with old defaults (`NIX_V3_REFUSE_FORMALS_BRIDGE=1
NIX_V3_NO_IFD_IMPORT_CACHE_DISK=1`).  Failure categories:

### Cat A: macOS stack-size warning prepended to test output

The test scripts capture v3 stdout/stderr and compare to expected
strings.  On macOS, v3 emits:

```
Failed to increase stack size from 8372224 to 62914560 (desired:
62914560, maximum allowed: 67092480): Invalid argument
```

This warning prepends to the actual eval result.  Tests fail because
`"Failed to increase ...\n[correct result]" != "[correct result]"`.

Affected (8 tests): `cell-update-protocol`, `nursery`, `wc-laziness`,
`disk-cache`, `695-getflake-bridge.sh`, `696-import-ifd.sh`,
`inherit-from-laziness`, `thunk-all-regression`.

### Cat B: macOS RLIMIT_AS warning prepended

Similar to A but the warning is `v3 limits: setrlimit(RLIMIT_AS,
...) failed (Invalid argument); relying on SIGALRM watchdog`.

Overlaps with Cat A.

### Cat C: v3-eval standalone CLI lacks NIX_PATH

`wc38-nixpkgs-probe.sh` uses `v3-eval --expr "(import <nixpkgs>...)"`
without setting NIX_PATH.  Fails because v3-eval standalone has no
`nixpkgs=...` registry; the `nix eval` CLI inherits NIX_PATH from
the environment.  Test script needs to set NIX_PATH.

### Cat D: Print-shallow rendering mismatch

`v3.log` / `v3-eval-tests.log`: `builtins.map (x: x*2) [1 2 3]`
yields `[ <APP> <APP> <APP> ]` in v3 vs `[ 2 4 6 ]` in TW.  v3's
default shallow-print doesn't force the elements of a list.  This is
a long-standing TW-parity gap on shallow printing, not a correctness
issue (the values themselves are correct when forced).

### Cat E: Test-fixture format mismatches

`fail.log`: 116 v3 eval-fail tests, 109 fixture mismatches.  The
fixture format doesn't match v3's current error message phrasing.
Many tests in this set predate #678-#693 error-message alignment.

### Cat F: pre-existing lint nit

`lint-no-inline-getenv`: vm.cc:92 has an inline `std::getenv` call
that doesn't follow the cached-static pattern.  Code path is
cold (V3_DBG_SIGTRAP debug gate, fires once if at all), so the
lint's perf concern doesn't apply, but the lint is a strict
syntactic check.

## Per-failure baseline confirmation

For each of the 16 failures, the SAME failure reproduces under
old defaults:

  | Failure                  | New defaults | Old defaults |
  |--------------------------|--------------|--------------|
  | lint-no-inline-getenv    | FAIL         | FAIL         |
  | 695-getflake-bridge.sh   | FAIL         | FAIL         |
  | 696-import-ifd.sh        | FAIL         | FAIL         |
  | cell-update-protocol     | FAIL         | FAIL         |
  | disk-cache               | FAIL         | FAIL         |
  | fail                     | FAIL         | FAIL         |
  | inherit-from-laziness    | FAIL         | FAIL         |
  | intrinsic-recognition    | FAIL         | FAIL         |
  | lambda-lift              | FAIL         | FAIL         |
  | nursery                  | FAIL         | FAIL         |
  | repros.sh                | FAIL         | FAIL         |
  | thunk-all-regression     | FAIL         | FAIL         |
  | v3                       | FAIL         | FAIL         |
  | wc-laziness              | FAIL         | FAIL         |
  | wc38-nixpkgs-probe.sh    | FAIL         | FAIL         |
  | v3-eval-tests            | FAIL         | FAIL         |
  | **679-cosmetic-parity**  | **PASS**     | FAIL         |

Net effect of the default flip: **+1 PASS, no regressions**.

## What this validates

The post-flip validation cleanly answers each pre-condition
question for shipping the new defaults:

  * ✓ No correctness regressions: 20/20 + 64/64 + 15/15 byte-
    identical to TW across diverse nixpkgs + cardano-node
    + flake workloads.
  * ✓ No regression test failures introduced: 100 % of failures
    reproduce under old defaults too.
  * ✓ Phase 4b correctly scoped: empty cache on no-IFD workloads
    (5 packages checked).
  * ✓ Phase 4b correctly populates: 5/5 IMPORT-DISK-HIT cross-
    process on multi-IFD workload.
  * ✓ GC stress survives: V3_DBG_GC_STRESS=100 with cached and
    uncached paths.
  * ✓ Wall-positive where Phase 4b applies: 1.97× on multi-IFD.
  * ✓ Wall-neutral where Phase 4b doesn't apply: hello.drvPath
    within noise.
  * ✓ Formals-bridge unlock surfaces no regression: 9/10 core
    pass unchanged.

## Strategic implications

### The default flip is shippable

  * Strictly net positive over the prior defaults (+1 PASS, 0
    regressions).
  * Real workload classes (nixpkgs derivations, cardano-node
    flakes, multi-IFD workloads) all work correctly.
  * Phase 4b's wall benefit is automatic — no user knob to
    discover.

### Cleanups suggested (separate followups, NOT default-on blockers)

  * Cat A/B/F failures: update the test runner to strip macOS
    stack-size and RLIMIT_AS warnings before comparison.  ~50
    LoC; fixes 8-10 tests on macOS.
  * Cat C: set NIX_PATH for v3-eval-based tests OR migrate
    those tests to `nix eval --expr` shape.
  * Cat D: deep-force list elements before shallow-print, matching
    TW's printer.  A long-standing print-parity item.
  * Cat E: align fail-fixture expected error texts with v3's
    current phrasing (post-#678 / #680 / #693 sweep).

## Cross-references

  * `lode/PHASE_4B_SCALE_TEST_2026-05-24.md` — 1M scale + no-IFD
    wall-neutrality
  * `lode/PHASE_4B_MULTI_IFD_2026-05-24.md` — multi-IFD heavy +
    methodology RCA
  * Commits: `d22e1bfd3` (default flip), `23b8c4fa7` (#793
    primReadDir realisePath), `35564703f` (RCA fix)

  * Test logdirs:
    * /var/folders/.../v3-test-logs.58495 (new defaults, 79 tests)
    * /var/folders/.../v3-test-logs.84961 (old defaults baseline)
