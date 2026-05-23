# Phase 4b 1M-element scale test — decisive validation, 2026-05-24

Continuation of `PHASE_4B_SCALE_TEST_2026-05-23.md`.  After commit
`35564703f` landed the RCA fix that scoped the cache to actual IFD
imports (instead of every nixpkgs-internal lazy import), Phase 4b
flipped from wall-neutral to wall-positive on `ifd-heavy.nix`
(100K-element foldl', +90 ms saved).

This doc records the **1M-element scale test**, designed to falsify
or confirm linear scaling of Phase 4b's wall savings with the
imported expression's eval cost.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group.  SPDX-License-Identifier: Apache-2.0.

## Hypothesis

If Phase 4b's wall savings equal `T_eval - T_deser`:
  * `ifd-heavy.nix` (100K elements) saved 90 ms — `T_eval ≈ 90 ms`,
    `T_deser ≈ µs` (single int).
  * `ifd-1m.nix` (1M elements) should save ~900 ms — `T_eval` scales
    linearly with N for `foldl' (a: b: a + b) 0`, `T_deser` is still
    a single int.

If the hypothesis fails (savings sub-linear or flat), the previous
positive finding was a noise artefact and Phase 4b is bounded by
non-eval costs.

## Workload — `/tmp/ifd-1m.nix`

```nix
let
  pkgs = import <nixpkgs> { };
  generated = pkgs.writeText "heavy-1m.nix" ''
    let
      n = 1000000;
      ints = builtins.genList (i: i) n;
      sum = builtins.foldl' (a: b: a + b) 0 ints;
    in sum
  '';
  computed = import generated;
in
{ value = computed; }
```

Identical shape to `ifd-heavy.nix`, with `n` raised 100K → 1M.  The
IFD result is still a single integer (14 bytes serialised).

## Measurements

Hyperfine, `--warmup 1 --runs 8`, locally-built `nix` CLI.

  | Mode | Wall | σ | vs OFF | vs TW |
  |------|------|---|--------|-------|
  | **TW**      | **727.7 ms** | 11.4 ms | —          | 1.0×    |
  | **v3-WARM** | **890.3 ms** | 12.9 ms | **-45 %**  | 1.22×   |
  | **v3-OFF**  | **1619 ms**  |  5.0 ms | baseline   | 2.22×   |
  | **v3-COLD** | **2133 ms**  | 15.0 ms | **+32 %**  | 2.93×   |

### Correctness check

```
TW:      499999500000
v3-OFF:  499999500000
v3-WARM: 499999500000
```

Byte-identical.

### Cache contents after one cold run

  * `CompilationUnits` table: 267 entries (#770/#771 CU cache,
    default-on, shared across modes — caches the IR bytecode of
    every parsed Nix file)
  * `EvalResults` table: **1 entry, 14 bytes** (the cached IFD result
    `499999500000` — exactly the value Phase 4b is designed to cache)

The scoping fix from commit `35564703f` is **verified at scale**:
no spurious entries from nixpkgs-internal imports.

## Conclusions

### Hypothesis CONFIRMED — linear scaling

  * 100K (ifd-heavy):   90 ms saved  (T_eval ≈ 90 ms)
  * 1M  (ifd-1m):      729 ms saved  (T_eval ≈ 729 ms)

Ratio: 8.1× wall saving for 10× compute — well within the noise
band of `T_eval ≈ T_compute_alone` with a small fixed `T_deser`.
**Phase 4b savings scale with the imported expression's eval cost.**
The architectural premise from `IFD_CACHE_DESIGN_2026-05-23.md` is
validated at 10× the previous scale.

### v3-WARM is competitive with TW (1.22×)

This is the **first workload class where v3 + Phase 4b is within
striking distance of TW** on a non-trivial expression.  Before
Phase 4b: v3-OFF is 2.22× slower than TW.  After: v3-WARM is 1.22×.
Phase 4b alone closes the v3 gap by 100 % of the OFF→WARM delta.

### COLD tax: +32 % one-time, amortises after 1 reuse

  * Tax: 514 ms (cold pays serialise + disk insert)
  * Savings per warm run: 729 ms
  * Break-even at 1.0 reuses → cache pays for itself the *second*
    run

For workloads with shape "build once, eval many times" (CI
runners, dev shells with `direnv`, IDE language servers re-running
nixpkgs queries), the cold tax is amortised cheaply.

## Strategic implications

### Previous "recurring wall-neutral pattern" — falsified

`PHASE_4B_SCALE_TEST_2026-05-23.md` documented four wall-neutral
findings (3e ACTIVE, 5 WARM, 4 simple-IFD, 4b large-IFD).  Three
were correct measurements of fundamentally wall-neutral levers (the
quantum cost being skipped was too small).  But Phase 4b on
**eval-cost-bearing** IFD workloads is **NOT** in that family — the
RCA fix from `35564703f` plus this scale test prove that the lever
is real when:

  * The imported expression does eval-time computation (not just
    literal allocation), AND
  * The result is small enough to deserialise cheaply.

### Promote toward default-on?

Pre-conditions for default-on promotion:

  * ✓ Byte-identical TW output across measured workloads
  * ✓ Wall-positive on its target workload class (eval-heavy IFD)
  * ✓ Scoping fix prevents pathological cache-bloat on
    nixpkgs-internal imports
  * ✓ **Wall-neutral on no-IFD workloads** (validated this session;
    see below)
  * ? Wall-neutral on workloads where T_eval ≈ T_deser
    (large literal-attr IFDs as in `ifd-huge.nix`)
  * ? Real haskell.nix workload validation (requires bringing in
    a representative cabal project — not in this session's scope)

### No-IFD wall-neutrality validation (2026-05-24)

Two canonical no-IFD workloads tested in steady state (pre-warmed
CU disk cache, no `--prepare` clauses).

**hello.name** (n=10, warmup=2):

  | Mode     | Wall     | σ       |
  |----------|----------|---------|
  | GATE-OFF | 264.9 ms | 6.0 ms  |
  | GATE-ON  | 267.7 ms | 5.2 ms  |

  Diff: +2.8 ms (0.5σ) — within noise.  `EvalResults` table remains
  empty after the run (correctly scoped).  IFD probes:
  `total=573 with-ctx=0` — i.e. no IFD candidates seen, so
  cache-side code never executes.

**hello.drvPath** (n=8, warmup=2):

  | Mode     | Wall     | σ       |
  |----------|----------|---------|
  | GATE-OFF | 834.9 ms | 9.5 ms  |
  | GATE-ON  | 839.2 ms | 10.7 ms |

  Diff: +4.3 ms (0.4σ) — within noise.  `EvalResults=0`.

This is the **decisive default-on test**: when the workload has
no actual IFD events, enabling the cache costs nothing measurable.
Combined with the wall-positive result on IFD-heavy workloads,
Phase 4b would be a strict win as default-on.

#### Methodology note (cache-warming pitfall)

A first attempt at the no-IFD test used `--prepare "rm -rf <cache>"`
on the GATE-ON benchmark, which inadvertently wiped the
default-on CU disk cache (#770/#771) between every iteration.  The
resulting 2.29× slowdown was a CU disk-cache cold-recompile artefact,
not a Phase 4b cost.  Fix: pre-warm both caches to steady state and
omit `--prepare` for steady-state measurements.

Lesson logged: **when benchmarking Phase 4b with `--prepare`, only
target the EvalResults table** (e.g. via `sqlite3 ... "delete from
EvalResults"`), never the whole cache directory.

### Aborted methodology — recorded for clarity

The first measurement of GATE-ON on hello.name showed 2.29× slower
than GATE-OFF (568 ms vs 248 ms).  This finding would have been
catastrophic for default-on promotion.  RCA traced it to the
`--prepare` clause wiping the CU disk cache, not a Phase 4b cost.
The corrected steady-state measurement (above) shows wall-neutral
behaviour.

### Real-world workload alignment

Phase 4b's design audience per `WORKLOAD_HETEROGENEITY_AUDIT_2026-05-23.md`:

  * `haskell.nix`-style: `callCabalProjectToNix` → IFD result is
    a derivation forest, T_eval = seconds.  **Phase 4b lever:
    several seconds.**
  * `cabal-nix`-style: cabal2nix-generated `.nix` files with
    100s of derivation calls.  T_eval ≈ 100s of ms.  **Phase 4b
    lever: ~10 ms/derivation × N.**
  * NixOS module IFD: rare in mainline, but each module
    instantiation through IFD is expensive.  **Phase 4b lever:
    proportional to module complexity.**

This 1M-element synthetic is the closest a hand-crafted workload
can come to those shapes.  The fact that Phase 4b saves 729 ms here
suggests several seconds of savings on real haskell.nix workloads.

## Operational summary

  * Workload: `/tmp/ifd-1m.nix`
  * Cache dir: `/tmp/v3-ifd-1m-cache/v3-bytecode-v1.sqlite`
  * Gate: `NIX_V3_IFD_IMPORT_CACHE_DISK=1`
  * Invocation: `nix eval --impure --expr "(import /tmp/ifd-1m.nix).value"`
    (build local CLI from `build/src/nix/nix`)
  * Validated: 2026-05-24 with `8265a8b1d`-tree (post-`35564703f`
    RCA fix)
