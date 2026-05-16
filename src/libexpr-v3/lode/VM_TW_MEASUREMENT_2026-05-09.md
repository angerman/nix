# v3 / TW Time Measurement (2026-05-09)

The pure-v3 inversion goal needs a way to validate that "TW only for
leaf store/IO callbacks" is *actually* the case during a real
evaluation — not just on paper.  Without a percentage breakdown,
"v3-direct on nixpkgs" could be 95 % v3 or 60 % with a hidden hot
bridge cycle and we wouldn't know.

This memo:
1. Catalogues progress since `REVIEW_2026-05-08.md`.
2. Documents the existing telemetry surface.
3. Identifies three gaps for "what % of eval was in v3 vs TW?".
4. Proposes four cheap improvements, ranked.

## 1. Progress since 2026-05-08

The same-day update at the top of `REVIEW_2026-05-08.md` flagged
the `OP_WITH_LOOKUP nix-update` blocker as the next gating bug
under `NIX_V3_DIRECT_EVAL=1`.  That has now closed, plus several
adjacent items:

| Item | Commits | Status |
|---|---|---|
| #529 — thunkify inherit-from from-exprs whose head is `fromWith` Var/Call | `c8362c2f9` | Closed |
| #530 — lexical-with chain: static IR materialisation of `capturedWiths` | `7c648facf`, `af8715433`, `4a54c850d`, `298870af2`, `873696188` | **Closed.**  This is the with-scope blocker that gated nixpkgs under v3-direct. |
| Three KNOWN-FAIL assertions promoted to positive checks | `b4d7f78ba` | Tests previously marked broken now pass |
| `bench-eval-only.sh` measurement harness | `c755e6752` | New tool — VM-vs-FFI breakdown |
| `BENCH-2026-05-08-POST-530.md` perf report | `c755e6752` | First v3-direct reality check |
| `opt_inline rewriteVar` LetRec lexicalWiths fix | `298870af2` | Bug-fix follow-on from #530 |

In 24 hours: the second and third v3-direct blockers closed.
INVERSION_PLAN Phase 1 is now substantively complete for `nix eval
--expr`/`--file` on the synthetic suite.  Phase 2 (flake
installables) is the next concrete step.

## 2. Existing telemetry — what's already there

Two env-var combinations cover most of the question.

### 2.1 `V3_TIMING=1` + `NIX_VM_STATS=1`

Records four nanosecond counters per hook entry, dumped at process
exit:

```
v3 hook timing (ms): lower=L compile=C run=R bridge=B
v3 hook stats:  evalEntries=N cacheHits=H cacheMisses=M
v3 force stats: forceEntries=N forceHits=H forceMisses=M
```

Defined in `v3_hook.cc:1156-1213` (`V3HookStats` struct).
Timestamps captured at `v3_hook.cc:1527, 2207, 2208, 2279, 2465`.
Output gated by either flag at `v3_hook.cc:1604, 1696`.

**Field semantics:**
- `lowerNs` — AST → IR translation time.
- `compileNs` — IR → bytecode time.
- `runNs` — v3 dispatch-loop wall time.
- `bridgeNs` — wall time spent inside TW callbacks invoked from v3.
- `evalEntries` — count of top-level v3 eval-hook entries.
- `forceEntries` — count of TW → v3 re-entries (i.e., TW called
  back into v3 mid-eval).

### 2.2 The percentage formula

The "v3 vs TW during evaluation" percentage is:

```
v3_pct = runNs / (runNs + bridgeNs)
TW_pct = bridgeNs / (runNs + bridgeNs)
```

`lowerNs + compileNs` is parse-and-build cost — separate question
("amortisable via disk cache on production workloads").  Subtract
it explicitly when comparing eval-only ratios.

### 2.3 `NIX_V3_BRIDGE_TIMING=1`

Per-direction bridge totals (v3→TW vs TW→v3), adding ~10-30 ns
overhead per bridge call.  Useful for finer attribution when
`bridgeNs` alone doesn't explain a slowdown — e.g., is the time
spent in v3-calling-TW (a primop body) or TW-calling-back-into-v3
(a callback like a `filter` argument)?

### 2.4 `NIX_V3_PRIMOP_DUMP=1`

Per-primop call counts (not times), plus the three TW→v3 bridge
primops (`__v3_call_bridge_1`, `__v3_force_attr`,
`__v3_force_list_elem`).

## 3. What `BENCH-2026-05-08-POST-530.md` actually showed

For every synthetic workload (fib, ackermann, path-deep, letrec-fix,
list-build, fold-add, with-deep, `lib.foldl'` over 1..1000) under
`NIX_V3_DIRECT_EVAL=1`:

```
evalEntries=1  forceEntries=0  bridge=0.000ms
```

**0 % FFI on the synthetic suite.**  v3 owns the whole evaluation;
no TW callbacks fire.  The 2.4× v3.run/TW.eval ratio on fib is
**pure dispatch-loop overhead, not bridge cost** — exactly what
mechanical wins (computed-goto, typed numeric opcodes) attack.

This is a useful diagnostic finding by itself: the inversion is
working, the FFI surface is at zero on these workloads, and the
perf gap is fully attributable to `vm.cc dispatchLoop`.

## 4. Three gaps in the current telemetry

For the pure-v3 goal — validating "TW only for leaves" on real
workloads — the existing telemetry has three gaps:

### 4.1 Aggregate, not per-primop

`bridgeNs` is one counter.  If `nixpkgs#hello.drvPath` spends 200 ms
in `derivationStrict` and 50 ms in `import`'s parse step, the two
are summed.  `NIX_V3_PRIMOP_DUMP=1` gives counts per primop but not
times.  Without per-primop attribution we can't tell which TW
callback dominates on a given workload, so we can't prioritise
which leaf to optimise / port to v3-native next.

### 4.2 Process-aggregate, not within-eval

Counters cover a whole process run, summed across every eval
invocation.  There's no "during this single call to `forceValue`,
what fraction was in TW" — useful when chasing a specific cycle or
deciding whether a given suspected hot bridge is actually hot.

### 4.3 Real-world workloads not in the bench

`bench-eval-only.sh` runs synthetic shapes that don't touch FFI.
To measure the actual percentage on nixpkgs (where
`derivationStrict`, `builtins.path`, `import` parse, fetchers all
hit TW), the harness needs a flake or `(import <nixpkgs> {}).…`-
style target.  `BENCH-REAL-WORLD-2026-05-04.md` did this for the
hook-mode path; the post-530 bench did not for v3-direct.

## 5. Proposed improvements (ranked)

### A. Per-primop FFI time (~50 LOC, ½ day)

Add a `bridgeNsByPrimop[primop_index]` map, accumulate on entry/exit
of each bridged primop body.  Report as:

```
v3 FFI breakdown (ms):
  derivationStrict count=12 time=234.5
  builtins.path    count= 3 time= 18.7
  import (parse)   count= 1 time= 42.1
  fetchurl         count= 0 time=  0.0
  ...
```

Identifies which TW callback dominates on a given workload.
Critical for prioritising "which leaf to port next."

Implementation: extend `V3HookStats` with a per-primop ns map keyed
on `PrimOp::name`; `bridgeBuiltin<N>` wrapper records entry/exit
timestamps; dump alongside the existing primop-count dump.

### B. Extend `bench-eval-only.sh` to nixpkgs targets (½ day)

Add the same shapes `BENCH-REAL-WORLD-2026-05-04.md` covered, but
under `NIX_V3_DIRECT_EVAL=1`:
- `(import <nixpkgs> {}).hello.name`
- `(import <nixpkgs> {}).hello.drvPath`
- `builtins.length (builtins.attrNames (import <nixpkgs> {}))`
- `(import <nixpkgs> {}).haskellPackages` attrNames-deep

Plus a flake target once Phase 2 lands.  Without this the
percentage question stays theoretical — the synthetic suite says
"0 %" and we can't validate that on real code.

### C. Single-line summary at every v3 eval end (~30 LOC, ½ day)

A `NIX_V3_SUMMARY=1` flag that prints one line at process exit
without the verbose `NIX_VM_STATS` / `V3_TIMING` dumps:

```
v3-eval-summary: total=523ms (lower=12 compile=43 run=441 bridge=27)
                 v3=84.3% TW=5.2% setup=10.5% (forceEntries=14)
```

The percentage in a form a human can paste into a commit message or
a benchmark log without grep+arithmetic.  Trivially derived from
existing counters; just adds the formatting + division.

### D. Sampling profiler with v3/TW labels (~200 LOC, deferred)

Heavier work: a sampling thread that introspects `activeV3VM()`
(landed in STG-7, `7abe1b433`) to attribute each sample to v3 or
TW based on whether a v3 VM is on the stack.  Wall-clock
attribution at *function* granularity, not just bridge granularity.

Useful when (A) shows that `derivationStrict` is hot but doesn't
say *where in `derivationStrict`* — a sampling profiler does.
Defer until A's per-primop view points at a primop big enough to
need finer-grained analysis.

## 6. Recommendation

**A + C in one half-day** (~80 LOC, additive, no architectural
risk).  Both extend existing telemetry without changing semantics.

**B in another half-day** for representative workloads.  Without
this the percentage measurement is meaningless — it'll always be
"0 %" on a workload that doesn't hit TW.

**D deferred** until A surfaces a primop hot enough to warrant
sub-primop attribution.

After A+B+C: a one-line answer to "what fraction of `nix eval
nixpkgs#hello.drvPath` ran in v3 vs TW?" — the metric the pure-v3
goal needs to measure progress.

## 7. Why this matters now

Without per-workload measurement, "drop reliance on TW as early as
possible" is a directional goal with no scalar.  We can't tell
whether `NIX_V3_DIRECT_EVAL` on a nixpkgs build is genuinely 95 %
v3 or 60 % with a hot bridge cycle hiding in plain sight.

The synthetic suite says 0 % — encouraging, but not predictive of
real workloads.  Phase 2 of the inversion plan (flake installables)
will introduce a one-shot TW→v3 bridge for resolution; that bridge
is the kind of cost that goes invisible without per-primop
attribution.

The infrastructure for the answer is mostly already in tree.  ~80
LOC + half a day of bench plumbing closes the gap.

---

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
