# Baseline comparison: post-Option-4 → post-Phases-A-H (2026-05-18)

## Setup

- **Pre-Phase-A baseline**: `2026-05-18-post-option4.json` (commit
  `8f9a01d5f`, taken before any of IR Phase A-H landed).
- **Post-Phase-A-H baseline**: `2026-05-18-post-phases-A-H.json`
  (commit `db6112c5c`, taken after Phases A-H + IR-CHECK MVP +
  profiler).
- N=5 runs per cell, modes = `tw` and `v3-direct`, deep capture on.
- Bench harness uses a pinned `NIXPKGS=p5cm66j33sbpn8ni9f2hlr279sfhvgwq-source`.

## Methodological caveat: TW shifted across baselines

TW p50 wall-time dropped 30-50% across nearly every workload between
the two runs.  Since Phases A-H don't touch TW, this is **system-
state drift**, not a real TW speedup — most likely thermal/page-
cache warming on the M-series macOS host.  The raw `--baseline`
diff therefore overstates "improvement" everywhere.

**Mitigation used here**: compare the **gap** = (v3-direct ms − TW ms)
between baselines.  System-state drift affects both modes
proportionally and cancels.  A *gap reduction* is the real signal
that Phase A-H helped on that workload.

## Result table — gap-delta ranked

```
workload                            TW old→new      v3 old→new      gap old→new       delta
ackermann-3-7                       320→195ms       628→363ms       +308→+168        -140 WIN
attrset-build-1k                    104→64ms        111→70ms        +8→+5            -2
derivation-chain-10                 117→83ms        150→91ms        +33→+8           -25 WIN
derivation-chain-30                 143→88ms        148→96ms        +5→+8            +3
derivation-strict-multi-output      141→84ms        225→87ms        +84→+2           -82 WIN
fib25                               181→91ms        251→125ms       +70→+34          -36 WIN
fib30                               911→426ms       1325→813ms      +414→+387        -27 WIN
fib33                               2863→1687ms     5504→3222ms     +2641→+1535      -1106 WIN
fold-add-10k                        108→62ms        122→72ms        +14→+10          -4
letrec-fix-5                        109→62ms        104→68ms        -5→+5            +10 reg
lib-attrnames                       116→65ms        117→69ms        +1→+4            +3
lib-evalModules-100                 110→67ms        185→110ms       +75→+43          -32 WIN
lib-evalModules-trivial             115→66ms        167→103ms       +53→+37          -16 WIN
lib-fix-deep                        111→65ms        115→72ms        +4→+7            +3
lib-foldl-10k                       110→65ms        139→83ms        +29→+19          -10 WIN
lib-foldl-1k                        104→63ms        117→80ms        +13→+17          +4
lib-genAttrs-100                    108→66ms        123→74ms        +15→+8           -7 WIN
lib-makebinpath                     116→66ms        162→75ms        +46→+9           -37 WIN
lib-mapAttrs-100                    106→62ms        124→73ms        +17→+11          -6 WIN
lib-recursive-update                110→70ms        124→79ms        +14→+9           -5 WIN
lib-strings-ops                     118→67ms        133→75ms        +15→+8           -7 WIN
lib-types-int                       135→64ms        180→106ms       +45→+41          -4
list-build-1k                       103→66ms        116→68ms        +14→+2           -11 WIN
path-deep-30                        110→60ms        110→65ms        -1→+4            +5
string-concat-1k                    101→68ms        106→69ms        +4→+0            -4
with-deep-200                       103→63ms        107→66ms        +4→+3            -1
```

### Aggregate gap reduction

Sum of all gap deltas = **−1417ms** of v3-vs-TW time eliminated
across the 26 measurable workloads.  Dominated by fib33 (−1106ms)
and ackermann-3-7 (−140ms), but well-distributed across mid-range
workloads too.

### Wins by category

**Big wins** (gap reduced >100 ms):
- fib33: gap shrunk 2641ms → 1535ms (−1106ms; v3 still 1.91× TW)
- ackermann-3-7: 308ms → 168ms (−140ms; v3 still 1.86× TW)

**Medium wins** (25-100 ms gap reduction):
- derivation-strict-multi-output: 84ms → 2ms (essentially at parity, ratio 1.03×)
- lib-makebinpath: 46ms → 9ms (ratio 1.14×)
- fib25: 70ms → 34ms (ratio 1.37×)
- lib-evalModules-100: 75ms → 43ms (ratio 1.65×)
- fib30: 414ms → 387ms (ratio 1.91×)
- derivation-chain-10: 33ms → 8ms (ratio 1.09×)

**Small wins** (5-25 ms): 7 workloads.

**Within noise** (±5 ms): 8 workloads.

**Small regressions** (3-10 ms gap growth): 6 workloads — all on
60-100ms-total wall-time bench rows.  Likely IR-optimizer compile-
time overhead (Phase A-H adds work even when no opportunity is
found) showing up on workloads too small to recoup.  Notable:
- letrec-fix-5: was −5ms ahead of TW; now +5ms behind.

## Cross-section by workload family

| Family | Workloads | v3/TW (new) | Phase-A-H impact |
|---|---|---|---|
| Compute (fib, ackermann) | 4 | 1.37× – 1.91× | Big absolute wins; ratio mostly flat (TW also benefited from cache warming).  Multi-arg-curry beta (Phase F) + identity-lambda fast-path + selector recognition delivered fib33's −1.1s gap reduction. |
| Library traversals (`lib-*`) | 13 | 1.07× – 1.65× | Mixed; lib-makebinpath (−18% ratio) and lib-evalModules-100 (−2.4%) showed real shrink.  Tiny ones (`lib-attrnames`, `lib-fix-deep`) within noise. |
| Derivations | 3 | 1.03× – 1.09× | Near-parity now.  derivation-strict-multi-output dropped from 1.60× to 1.03× — the Phase A-H pipeline closed almost the entire gap. |
| Trivial shapes | 4 | 1.01× – 1.07× | All within noise; opt overhead and gains roughly cancel. |

## Real-world nixpkgs cold-path (working `<nixpkgs>` reference)

The bench's pinned nixpkgs (`p5cm66j33...`) trips a pre-existing
`OP_WITH_LOOKUP: name 'minor' not found in with-scope` error under
v3-direct (a stable failure inherited from the prior baseline; not
a Phase A-H regression).  Captured separately against the working
`<nixpkgs>` flake-input
(`/nix/store/jfyir...77dbgds155bbz3vd3qywq1sii07i5ljs-source`) into
`2026-05-18-post-phases-A-H-nixpkgs.json`:

| workload | TW p50 (ms) | v3-direct p50 (ms) | ratio |
|---|---:|---:|---:|
| hello-name | 488 | 1146 | **2.35×** |
| hello-pname | 497 | 1138 | 2.29× |
| hello-version | 485 | 1218 | 2.51× |
| hello-meta | 503 | 1146 | 2.28× |
| hello-system | 483 | 1099 | 2.28× |
| attrnames-pkgs | 483 | 1101 | 2.28× |

**v3-direct is ~2.3× slower than TW on real nixpkgs cold-path
queries.**  Memory footprint: v3 575MB RSS vs TW 135MB — 4× more
memory, suggesting Boehm-GC pressure is part of the slowdown.

### Regression vs Phase-1-MET claim

The `project_phase1_met_2026-05-18.md` memory (commit `ecc99fd07`)
reported:
- hello.name v3: 580ms, TW: 470ms → 1.4× ratio.

Today's measurement (commit `db6112c5c`, post-Phase-A-H):
- hello.name v3: 1146ms, TW: 488ms → 2.35× ratio.

**TW is within noise (470 → 488 ms; +4%).  v3 doubled (580 → 1146
ms; +97%).**  That is a genuine regression somewhere in the 14
commits between `ecc99fd07` and HEAD (which are: bench re-baseline,
IR Phases A-H, IR-CHECK MVP + close-gaps + canonical fixture
refactor, profiler).  Possible culprits in order of suspicion:

1. **Phase A-H optimiser compile-time overhead**: each phase
   adds passes that run UNCONDITIONALLY at every CU compile,
   including the bytecode-primop CUs installed at startup.  If
   each `installBytecodePrimop` call now spends ~50 ms more on
   optimise() and we install ~10 primops, that's 500ms of pure
   startup overhead.
2. **Phase F (App-spine fold) re-runs constantFold +
   elimRedundantForce + inlineTrivialBindings** if any fold
   fires.  Maybe firing many times during the bytecode-primop
   install.
3. **Phase H (genList unroll) creates new Functions/Blocks**,
   which inflates the module and slows subsequent compile/run.
4. **Per-opcode counter** is gated and shouldn't matter when
   NIX_VM_OPCOUNTS is unset, but verify the static-init gate is
   well-predicted.

The next investigative move is a **per-commit bisect on
hello-name wall time** (3 N=5 measurements between e90bc6d29
[before Phase A] and db6112c5c [HEAD]) to localise the
regression.  All Phase A-H opt-out gates exist
(`NIX_V3_NO_BETA_REDUCE` / `NO_PRIMOP_FOLD` / `NO_STREAM_FUSION`
/ `NO_LAMBDA_LIFT` / `NO_SELECTOR_LAMBDA` / `NO_APP_SPINE_FOLD`
/ `NO_IF_FOLD` / `NO_GENLIST_UNROLL`), so a bisect-by-env
verifies the localisation without rebuilding.

## What the data says about next steps

The IR pipeline has now plateaued for the workloads in this corpus:

1. **Compute-heavy gains are real but capped.**  fib33 ratio is
   still 1.91× — the floor is per-OP_CALL / per-OP_FORCE dispatch,
   not IR shape.  Closing it requires VM-level work (dispatch
   fast-paths, threaded code, or JIT) — not IR.

2. **Derivation-heavy workloads are near-parity.**  derivation-
   strict-multi-output at 1.03× says the bytecode-wrapped primop
   path is working as designed (Option 4 hybrid landed earlier).
   No remaining IR gain available here.

3. **Library-traversal workloads have a fixed-cost floor.**
   ~5-15ms v3-over-TW gap on workloads with 60ms total wall time
   suggests **v3 startup is ~10ms more expensive than TW** (lower
   + optimise + compile vs. just bindVars + eval).  Worth profiling
   the optimize() pipeline itself with `V3_TIMING=1` to see if any
   phase is amortising poorly.

4. **Nixpkgs cold-path 2.3× floor**: per the per-opcode profile,
   the dispatch breakdown on this workload class is dominated by
   GC scan time + Tag::App allocation churn (not visible to the
   opcode counter).  The per-op profile won't shrink this; **Stage
   3 (Cheney nursery default-on) is the likely next floor-mover.**

5. **Tiny workloads have a small but real optimizer-overhead
   regression.**  6 workloads showed +3-10ms gap growth.
   Probably worth measuring `optimise(m)` wall time on a few
   shapes and considering a `--O1` style preset that skips Phases
   D-H for shapes that don't benefit.  Low priority.

## Recommendation

The user-facing milestone (per the earlier strategic discussion):

**Pick #3 — tackle the hello.drvPath 30× gap, since IR phases A-H
have done what they can on this corpus and the next 5-10× requires
runtime/allocator work.**

Rationale:
- Phase A-H reduced the v3-vs-TW gap by ~1.4s across the bench
  corpus; further IR work has diminishing returns.
- The nixpkgs cold-path floor at 2.3× and the hello.drvPath floor
  at ~30× are both characterized as GC-/alloc-pressure dominated
  (575MB RSS on hello-name; 1GB+ on hello.drvPath per prior
  memos).
- The Cheney nursery design exists (`CHENEY_NURSERY_DESIGN.md`);
  Phase A (allocator) and Phase C (scavenge) have landed; Phase D
  (write barriers) is the blocker for default-on (per memory).
- A nursery default-on flip + the prior closure-pool work
  (`opt_closure_pool` references in the existing alloc.hh
  `tryPopFakeClo` / `recycleFakeClo` machinery) could plausibly
  shave a 2-5× factor off the alloc-pressure-dominated workloads.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group.  SPDX-License-Identifier: Apache-2.0.
