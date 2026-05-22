# v3 evaluator — Claude session instructions

When working on the v3 bytecode VM (`src/libexpr-v3/`), read this file first. It overrides any older guidance in `USAGE.md` or `lode/` snapshots.

## Authoritative strategic doc set (2026-05-15)

These four documents are the current canonical reference. Older docs are point-in-time artifacts; trust the four below over anything in `lode/REVIEW_*.md`, `lode/RCA_*.md`, or `lode/*_PLAN.md` unless cross-referenced.

| Doc | Purpose | When to read |
|---|---|---|
| [`lode/ACTION_PLAN_2026-05-15.md`](lode/ACTION_PLAN_2026-05-15.md) | Active 8-week phased plan (Phases 0-4 + 1.5 measurement spike) with TODO checklists, exit criteria, and kill criteria | **First. Always.** This drives day-to-day work. |
| [`lode/LESSONS_LEARNED_2026-05-15.md`](lode/LESSONS_LEARNED_2026-05-15.md) | Distilled from ~1380 commits + lode/ archive. Part 0 names the main issue; §1-3 codify constraints, what worked, what didn't; §4 has Nix-domain knowledge + bisection methodology + the 10-item debug story | Before opening any RCA, adding any gate, claiming any perf win |
| [`lode/ALIGNMENT_SCORECARD_2026-05-15.md`](lode/ALIGNMENT_SCORECARD_2026-05-15.md) | Vision-vs-reality scorecard for 12 components + 6 drift items + orphan gaps | Quarterly health check |
| [`lode/ROADMAP_TO_VISION_2026-05-15.md`](lode/ROADMAP_TO_VISION_2026-05-15.md) | Long-horizon Stages 1-9 (+ candidates 10-12 pending measurement) | After ACTION_PLAN's Phase 0-4 close |
| [`lode/LINKING_DESIGN_2026-05-17.md`](lode/LINKING_DESIGN_2026-05-17.md) | Concrete linking design — thunk-body content-addressed cells + manifest split | When working on Stage 9 / module imports / disk cache |
| [`lode/PERF_STRATEGY_2026-05-17.md`](lode/PERF_STRATEGY_2026-05-17.md) | Candidate Stages 10 (salsa), 11 (HAMT), 12 (JIT decision) — **NOT committed**, pending Phase 1.5 measurement | When the user asks about incremental eval, warm-eval optimization, persistent attrsets, JIT, or workload-mode strategy |
| [`lode/PARALLEL_EVAL_CAPABILITIES_2026-05-18.md`](lode/PARALLEL_EVAL_CAPABILITIES_2026-05-18.md) | Candidate Stage 13 (multi-core capabilities, sparks, parallel GC) with embedded self-correction — **NOT committed**, pending parallel-potential trace measurement | When the user asks about parallel/multi-core eval, capabilities, sparks, work-stealing, parallel GC, or GHC-RTS-style threading |
| [`lode/IR_OPTIMIZATION_PLAN_2026-05-18.md`](lode/IR_OPTIMIZATION_PLAN_2026-05-18.md) | 8-phase IR optimization plan (A-H) — beta reduction, primop constant fold, stream fusion, lambda lift, selector recog, App spine, If fold, genList unroll. ACTIVE work as of 2026-05-18 per action plan Phase 2(R) | When the user asks about IR optimization phases A-H, opt_*.cc pipeline, stream fusion, beta reduction, or per-op dispatch reduction |
| [`lode/EXTEND_DERIVATION_INVESTIGATION_2026-05-18.md`](lode/EXTEND_DERIVATION_INVESTIGATION_2026-05-18.md) | Investigation of hello.drvPath 30× perf gap; decomposes the 200× per-force gap into 4 factors | When the user asks about hello.drvPath / outPath / force-rate / extendDerivation perf |
| [`lode/NURSERY_PHASE_D_DESIGN_2026-05-18.md`](lode/NURSERY_PHASE_D_DESIGN_2026-05-18.md) | Phase D write-barrier deep dive — three viable shapes (a/b/c) with critical review + self-critique; identifies Tag::App memoization as under-recognized hazard; audit-first recommendation | When the user asks about Phase D, write barriers, nursery default-on prerequisites, intergenerational pointers, or remembered sets |
| [`lode/LODE_REVIEW_2026-05-18.md`](lode/LODE_REVIEW_2026-05-18.md) | Critical review of lode/ folder + IR serializability + optimizer pipeline + FileCheck recommendation. **Key finding**: FileCheck-style infrastructure already exists in ir_dump.cc — under-used | When the user asks about lode/ cleanup, IR serialization, optimizer pipeline documentation, IR testing infrastructure, or FileCheck |
| [`lode/IR_CHECK_INFRASTRUCTURE_PLAN_2026-05-18.md`](lode/IR_CHECK_INFRASTRUCTURE_PLAN_2026-05-18.md) | Step-by-step breakdown of 5-step IR-CHECK infrastructure plan. Includes MVP path (5 days), risks, decisions, sub-tasks, self-critique. Original 9-day estimate revised upward to 10-15 days realistic | When the user asks about implementing FileCheck-style IR testing, the v3-opt driver, per-pass entry points, sidecar .expected convention, or fixture authoring |
| [`lode/MAX_HEAP_LIMIT_DESIGN_2026-05-18.md`](lode/MAX_HEAP_LIMIT_DESIGN_2026-05-18.md) | Design for `NIX_V3_MAX_HEAP=2G` in-process heap cap via Boehm GC_set_max_heap_size + OOM hook. Throws typed OutOfMemoryError; cross-platform; graceful; diagnostic. ~3 days; recommended before Stage 3 nursery default-on | When the user asks about memory limits, RSS caps, heap budgets, OOM handling, or GC tuning under pressure |
| [`lode/ERROR_UX_DESIGN_2026-05-20.md`](lode/ERROR_UX_DESIGN_2026-05-20.md) | Error-message UX design synthesizing rustc/Elm/Roc/GHC/TS/Python/Tvix/Lix prior art. 5 ranked techniques, 3 before/after examples, 3-phase implementation (Diagnostic struct + two-span errors + Levenshtein + error codes + trace summarisation). Natural extension of #677-#681 TW-parity work. | When the user asks about error messages, diagnostics, UX improvements, "did you mean" suggestions, source spans, trace summarisation, or `--explain` |
| [`lode/FFI_AUDIT_2026-05-20.md`](lode/FFI_AUDIT_2026-05-20.md) | FFI / TW fallback inventory. ~1015 LoC FFI infra + 104 TW-cross sites + 109 primop wrappers. 6 TW dependency mechanisms classified. 4-tier migration plan (Tier 0 system-info-as-constants in 1-2 days; Tier 1 Stage 2/3/9 architectural; Tier 2 bytecode-install callback primops; Tier 3 opcode-ify pure ops). Includes V3_DBG_TW_CROSS measurement-spike proposal. | When the user asks about FFI surface, TW fallback, bridge plumbing, primop migration, or "what can move into the VM" |
| [`lode/PERF_TRACE_TOOL_DESIGN_2026-05-20.md`](lode/PERF_TRACE_TOOL_DESIGN_2026-05-20.md) | Design for `perf-trace.py` — time-series CPU% / RSS / Boehm-heap sampler for TW vs v3-direct with SVG overlay. `psutil` sidecar + in-process `GC_get_heap_size()` probe gated by `NIX_V3_HEAP_TRACE`. Closes LESSONS §4.9 Item 5 ("documented CPU-profile workflow"); supplies the instrument Phase 1.5's drvPath force-rate decomposition needs (factors b/c are otherwise unobservable). ~3 days; first measurement is the Rule 0 falsifier for "factor-2 GC-scan dominance" on hello.drvPath. | When the user asks about CPU/RSS/heap profiling over time, comparing TW vs v3 visually, the bench/samples/ output, `samply` integration, or "how do we see what v3 is doing during eval" |

## Rule 0 — the falsification rule

**Every commit body must answer: "what hypothesis does this kill?" If it kills none, it doesn't merge.**

A commit may exit an investigation by falsifying a model (delete code + gate), confirming one (delete alternative + its gate), or renaming the investigation to a fresh top-level issue. A commit may NOT exit by:
- Adding an opt-in gate so "both can coexist for now"
- Adding a `V3_DBG_*` / `NIX_V3_*` with no kill criterion
- Reverting + reapplying without a measurement between
- Producing a `*_findings.md` doc without code change

This is the upstream rule. All others in ACTION_PLAN Part 1 are specializations.

## Running v3 probes safely (operational essentials)

When invoking `v3-eval` or `nix eval --impure --expr ...` against any non-trivial workload (anything that touches nixpkgs), **always** combine these:

```bash
NIX_V3_MAX_WALL_TIME=30s   # or 60s for known-long evals
NIX_V3_MAX_HEAP=2G          # tune per workload (Boehm grows past 1 GB on hello.drvPath)
NIX_V3_MAX_CPU_TIME=60s     # CPU budget; useful when WALL_TIME may be too loose
NIX_V3_DIRECT_EVAL=1
```

The limits are real (`limits.cc` / `initLimits()`, called from `runRootExpr`).
They throw typed errors (`WallTimeExceededError` / `CpuTimeExceededError` /
`OutOfMemoryError`) with allocation stats — far better than a SIGKILL.
USAGE.md §"Resource limits" documents the units (K/M/G for heap; s/m/h for time).

**Verified 2026-05-19**: `NIX_V3_MAX_WALL_TIME=2s v3-eval --expr 'let f = x: f x; in f 0'` throws
`v3 WallTimeExceededError: NIX_V3_MAX_WALL_TIME=2.00s exceeded after 2.00s
 (alloc: closures=12 thunks=1 lists=1 attrsets=1 rss=29.73 MB boehm_heap=384.25 MB)`.

**Do NOT rely on shell-level `timeout`** when probing v3:
- macOS `ulimit -v` is a no-op for virtual memory.
- SIGTERM from `timeout` skips v3's clean unwind and drops the alloc stats.
- The v3-internal cap survives across re-entrant `runRootExpr` calls (bytecode-primop install path).

### Skipping TW pre-eval is the v3-direct default (no env var needed)

For `nix eval --impure --expr ...`, `NIX_V3_DIRECT_EVAL=1` makes
the CLI hand TW a `mkThunk(...)` only — TW parses but never
pre-evaluates.  v3-direct then owns evaluation entirely.  This
**skip-pre-eval** behavior is the permanent goal (more compute in
v3, less in TW) and the default whenever `NIX_V3_DIRECT_EVAL=1`.

The previous `NIX_V3_SKIP_INSTALLABLE_PREEVAL` env var that gated
this behaviour was retired in **#760 (commit `3af813638`,
2026-05-22)** and its remnants scrubbed in **#764**.  If you find
the old name in scripts, drop it — `NIX_V3_DIRECT_EVAL=1` alone
is the gate now.

For the `v3-eval` binary directly, there is no preeval at all —
it's been v3-only from day one.

## Critical constraints (hard rules; load-bearing)

0. **Today's allocator is Boehm conservative GC**, inherited from cppnix. The Cheney nursery design (`CHENEY_NURSERY_DESIGN.md`) exists; Phase A (allocator) and Phase C (scavenge) have landed but are gated `NIX_V3_NURSERY=1` opt-in (default-OFF). Phase D (write barriers) is unresolved. **Do not assume nursery semantics in v3 code**. Empirical consequence: Boehm heap grows past 1 GB on `hello.drvPath` runs and stays there. See LESSONS §1.6 and `EXTEND_DERIVATION_INVESTIGATION_2026-05-18.md`.

1. **V3-NATIVE**: v3 owns evaluation. TW is permitted ONLY at FFI leaves (store, paths, derivations, file I/O, eval-state parse). Routing v3 thunks/cycles through TW is forbidden. The commit retracting TW-routing is literally titled "architectural mistake" (`cf12c1880`). See `LESSONS_LEARNED_2026-05-15.md` §1.1.

2. **v3-native primops are CORRECT** (not drift). Pure-data primops (`map`, `filter`, `foldl'`, `attrNames`, `attrValues`, etc.) stay v3-native. Marshalling v3 Values ↔ Boehm-managed TW Values is per-call expensive AND crosses GC ownership. The FFI is for system boundaries (store, paths, I/O), not for replacing pure data ops. **Do not shrink `primops.cc` by replacing v3-native primops with FFI calls.** See `LESSONS_LEARNED_2026-05-15.md` §1.2.

3. **No new RCA letter on an open one.** A1-A12 must close before A13. STG-15 must wait for STG-14b. If a workload has an open letter, the next divergence on it is a sub-letter on the same root-cause track. See ACTION_PLAN Part 1 rule 1.

4. **No new env-var gate without an inline retirement criterion** in the comment at the first `getenv()` read site. PRs that violate are reverted, not amended. See ACTION_PLAN Part 1 rule 2.

5. **No `getenv("X")` / `getenv("Y")` / shell-prototype gates.** `test/lint-no-inline-getenv.sh` should fail CI; if it doesn't, fix it before adding the next gate.

6. **IR-CHECK fixtures use `--file %s`, never `--expr`.** Every fixture in `test/ir-fixtures/` is one self-contained `.nix` file: the Nix expression at the top is the test source; `%s` (substituted by the runner with the fixture's path) is what `v3-eval --file` parses. The canonical RUN: line is:

   ```
   # RUN: v3-eval --file %s --emit-ir | v3-check %s
   ```

   Reasons codified from `IR_CHECK_INFRASTRUCTURE_PLAN_2026-05-18.md` §501-525:
   - Matches LLVM's `.ll`-file pattern exactly (one file, one logical scenario, multi-RUN under `--check-prefix=` for mode variations).
   - Keeps the expression authorable like real Nix code (multi-line, indented, comments).
   - The Nix parser skips `#` lines as comments, so RUN: + CHECK: metadata can coexist with the source.

   For TWO logical scenarios (different Nix expressions / different shapes), **create two fixture files** — don't multiplex via `--expr` in multiple RUN: lines. See the `ifFold-true-pos.nix` / `ifFold-false-pos.nix` / `genListUnroll-{n4,n1,n16}-pos.nix` / `appSpineFold-{n2,n3,n4,impure-arg}-pos.nix` families as canonical examples. Multi-RUN with `--check-prefix=` is reserved for SAME source under different optimisation modes (e.g. `betaReduce-composition-pos.nix` runs `--emit-ir-raw` vs `--emit-ir`).

   Authoring guide and per-fixture index: `test/ir-fixtures/README.md`.

## When the user reports a v3 failure on nixpkgs

Default workflow (from LESSONS_LEARNED §4.8):
1. **Bisect nixpkgs itself.** Strip overlays, config; try non-forcing queries (`builtins.functionArgs (import <nixpkgs>)`); walk down to the smallest attribute that triggers; bisect env-gates one at a time.
2. **Capture the minimal repro** as `test/repro-<issue>-<shape>.nix` + `test/run-<issue>-tests.sh` driver.
3. **Keep the repro forever** — even after the bug closes — as a positive regression guardrail.

Existing examples: `test/repro-455.nix`, `test/repro-495-broader-thunkify-bug.nix`, `test/wc38-bisect-harness.sh`.

## The debug story (LESSONS_LEARNED §4.9)

Ten mechanisms. When you cannot answer "how would I debug this if it failed silently" with one of these, **build the missing one before the next investigation**:

1. Bisect nixpkgs (§4.8)
2. Differential testing vs TW oracle (`run-cutover-parity-tests.sh`, `bench-v3-vs-tw.sh`; gap: arbitrary-input front door)
3. Trace evaluation (`NIX_TRACE_EVAL`, `V3_DBG_HOT_FORCE`, `V3_DBG_ALLOC_DUMP`, `V3_DBG_HOT_CALLEE`)
4. Regression tests (every bug fix; bisected repros)
5. Profiling (`V3_TIMING`, `allocStats`, `bench.py`; gap: documented CPU-profile workflow)
6. Differential fuzzing — random Nix expressions, parity assert (not yet built)
7. Property tests for VM invariants (force idempotence, sharing equivalence, cycle-detection totality; not yet built)
8. Deterministic stress (`V3_DBG_GC_STRESS`, `V3_DBG_RECYCLE_STRESS`, `V3_DBG_ALLOC_SEED`; Stage 3 prerequisite)
9. Crash artifacts on abort (frame stack + source + recent ops; not yet built)
10. Issue → fixture manifest (`test/REPROS.md`; not yet built)

## What's superseded (do not trust as current state)

- **`USAGE.md`**: written through May 2026-05-05. Performance claims ("v3 is at parity with the tree-walker") are inaccurate on real workloads; v3-direct currently does not complete `hello.name` on real nixpkgs. NIX_USE_V3 cutover hook described there was deleted in `e8d7c3885`. Useful for: build commands, supported AST shapes, lang-test status.
- **`lode/REVIEW_*.md`, `lode/RCA_*.md`, `lode/*_PLAN.md`**: point-in-time artifacts. Many describe mechanisms that have since been retired (`partialBindingsRegistry`, `v3_hook.cc`, `CFF_TAINTED`, etc.).
- **`lode/OPTIMIZATION_PLAN.md`** (177 KB): chronological log; forward-looking sections superseded by ROADMAP_TO_VISION.

## Memory references

The user's persistent memory at `~/.claude-io/projects/-Users-angerman-Projects-iohk-nix/memory/` contains complementary references:
- `feedback_v3_native_constraint.md` — the V3-NATIVE rule + Boehm-GC marshalling reasoning
- `feedback_falsification_rule.md` — Rule 0
- `feedback_nixpkgs_bisection.md` — bisection methodology
- `project_strategic_docs_2026-05-15.md` — index of the four strategic docs
- `feedback_always_add_tests.md` — every bug fix needs positive + negative + regression tests

## Copyright

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.
