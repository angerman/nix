<!--
Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
-->
# GOAL: close the v3 list-iteration gap (map / foldl' / filter)

**How to use this file:** read it top to bottom, then implement the tasks **in
wave order, one at a time**. Each task has a *pre-committed kill-criterion* — set
the threshold, build the A/B, measure on darwin-4, and **keep the change only if it
clears the keep-bar; otherwise REVERT** (with a commit recording the data +
threshold + why). Do **not** batch tasks or skip the measurement. This is the
operational form of the project's measure-twice + Rule-0 discipline.

---

## 0. Read first (context you need beyond this file)

| Doc / tool | Why |
|---|---|
| `lode/LIST_ITERATION_PERF_2026-06-08.md` | The RCA this plan implements — full evidence, file:line, and *why* each fix. **Read it; the tasks below are its conclusions.** |
| `lode/MEASUREMENT_GATE_2026-06-07.md` (esp. §8 darwin-4 recipe, §9 method) | How to measure honestly. darwin-4 is the **only** valid timing host; the laptop debug build cannot time these. |
| `bench/` — `v3-vs-tw-table.sh`, `scaling-check.sh`, `v3-vs-tw-gate.sh`, `Makefile` | The validation harnesses. `make -C bench selftest` first; then the `measure`/`scaling` targets. |
| `lode/MEMORY_REPRESENTATION_2026-06-07.md` | **Lever 2 = pointer-tag `Value` 16→8B.** Tasks **T3 and T6 intersect it** — see the coordination note below. |
| `CLAUDE.md` (this dir) | Rule 0 (every commit kills a hypothesis), V3-NATIVE, the env-gate retirement rule, `lint-no-inline-getenv`. |

### The gap being closed (measured: idle darwin-4, post-fix 2.35.0, byte-IDENTICAL, same binary both arms)
- **map.foldl** `foldl' (a:x:a+x) 0 (map (x:x+1) (genList (i:i) 2M))` → **3.93× CPU / 1.71× RSS** (1.14s/556MB vs 0.29s/324MB).
- **filter** `length (filter (x:x>1) (genList (i:i) 2M))` → **4.77× CPU / 2.48× RSS** (1.05s/515MB vs 0.22s/207MB).

### The one-paragraph diagnosis
`genList` + spine-force is at **parity** — it is **not** list construction. The whole
gap is the **per-element operation**: v3 dispatches **13 bytecode insns/element** and,
per `sample`, the eval-thread self-time is led by **TLS access `_tlv_get_addr`
(1124) > `dispatchLoop` (851)**, then GC-marking, then the per-element call. Memory
is dominated by the **64-byte `ValuePair`** allocated per lazy element (384MB =
86% of allocation), where TW uses a 16-byte cell on an 8-byte backbone.

---

## Rules that apply to EVERY task (do not skip)

1. **Pre-commit the kill-criterion** (the keep-bar and the revert-bar) *before* writing code. They are given per task below; tighten if you wish, never loosen post-hoc.
2. **Gate the change** behind one env var (`NIX_V3_<NAME>=1` opt-in, or `NIX_V3_NO_<NAME>=1` if you make it default-on) **with an inline retirement criterion in the comment at the first `getenv` site** (CLAUDE.md rule 4). No bare `getenv` (`lint-no-inline-getenv`).
3. **Measure A/B on darwin-4** (single-tenant — reap any stray eval first, never run two loads at once). Build per `MEASUREMENT_GATE §8`.
4. **Correctness is non-negotiable:** results must stay **byte-identical** (verify via `bench/v3-vs-tw-gate.sh`), the **scaling guard** (`make -C bench scaling`) must stay green (no new complexity regression), and the **core + lang suites** must pass.
5. **Rule-0 commit message:** state what hypothesis the change kills/confirms + the measured A/B delta + the threshold it cleared (or, on revert, why it failed).
6. **V3-NATIVE:** the fix lives in the v3 VM. Never route list ops through the tree-walker.

### Measurement gotchas (these bit the RCA — heed them)
- **`NIX_VM_STATS` dumps multiple times; read the FINAL/cumulative dump (`tail`, not `head`).** Early dumps lie (`insns=2`, `peak_rss=34MB`). Always reconcile internal `peak_rss` against `/usr/bin/time -l` OS RSS.
- **Trust measurement over code-reasoning for ranking.** In the RCA, the per-force `std::vector` *looked* like the #1 CPU cost in source but measured ~1%; the real #1 (TLS) was invisible in source. Profile before you believe.

### ⚠ Coordination note — `Value` 16→8B (blocks T3 & T6)
`MEMORY_REPRESENTATION_2026-06-07.md` Lever 2 shrinks `Value` 16→8B via pointer
tagging. If/when it lands it **automatically** delivers T6 (8-byte backbone) and
**half** of T3 (a 4-field `ValuePair` becomes 32B for free). **Do not fork the core
`Value`/`ValuePair` representation.** Before starting T3/T6, check the status of that
lever with its owner and either (a) fold T3/T6 into it, or (b) confirm it is not in
flight and proceed. T1/T2/T4/T5/T7 are independent of it.

---

## T0 — Measurement scaffold (PREREQ; blocks all waves) — ✅ DONE (commit de79407ff)

**Already implemented:** `make -C src/libexpr-v3/bench measure-list-iter` exists
(`bench/list-iter-measure.sh`) and the baseline is pinned + reproduced (genList
1.00×, mapfoldl pairs=6000002/insns=26000033/peakRSS=555M, filter
lists=2000002/peakRSS=515M). **Start at Wave 1 (T1).** Use this target as the
yardstick for every task's keep/revert measurement. Original spec retained below.

**Do:** add a `make -C src/libexpr-v3/bench measure-list-iter` target that, on a given
`NIX_BIN`, runs **on darwin-4**:
- per-pass user-CPU **v3 vs TW** for `genList`, `map`, `foldl`, `mapfoldl`, `filter`
  (best-of-N, the decomposition from the RCA — genList must read ~1.00×);
- the **final-dump** `NIX_VM_STATS` alloc counts (`pairs`, `lists`, `insns`) and
  `peak_rss` for `mapfoldl` and `filter` (the RCA's batch-2 measurement);
- emits a one-line-per-metric table.

(The workloads + measurement already exist ad hoc in the RCA and in
`bench/v3-vs-tw-table.sh` — wrap them so every task below measures identically.)

**Pin the baseline** by running it once on the current HEAD binary and recording the
numbers in the commit. **Definition of done:** `make -C bench measure-list-iter`
reproduces the baseline above (genList ≈1.0×, mapfoldl pairs≈6M/insns≈26M,
peak_rss≈556MB; filter lists≈2M/peak≈515MB).

---

## Wave 1 — safe, measured-waste wins (parallelizable after T0)

### T1 — Saturated `callClosureN` *(START HERE — highest yield × confidence ÷ risk)*
- **Do:** add a saturated multi-arg call (`callClosureN`/`callClosure2`) that enters an
  arity-`n` closure body once with all args in slots (the `OP_CALL_N` fast-path logic
  already exists, `vm.cc:6256`). Route `primFoldl` (`primops.cc:1314-1315`) and other
  multi-arg-callback C++ primops through it, eliminating the curried
  `callClosure(op,acc)` that allocates a throwaway partial-application `ValuePair`
  per fold step.
- **Why:** measured — map.foldl allocates **6M pairs = 2M genList + 2M map + 2M curry
  PAPs**; the third 2M (128MB) is pure waste, plus a second `callClosure` dispatch
  per element on the **dominant** pass.
- **Gate:** `NIX_V3_SATURATED_CALL=1` (or default-on with `NIX_V3_NO_SATURATED_CALL=1`).
- **Kill-criterion:** **KEEP if** foldl-pass CPU **−≥15%** *or* `pairs` drops by ~2M
  (6M→4M) on map.foldl; **REVERT if** <5% *and* no pair drop.
- **Risk:** low (calling convention, not representation). Only 1-arg `callClosure`
  exists today (`vm.cc:13077`).

### T2 — Strip per-element gated debug/trace checks
- **Do:** hoist/eliminate the per-element `dbgLogForce*` and `periodicLiveTraceEnabled`
  checks from the force/dispatch hot path (they fire — and read thread-locals — on
  every element even when the features are OFF).
- **Gate:** none needed if it's a pure hoist preserving behavior when enabled; otherwise `NIX_V3_HOTLOOP_TRACE=1` to restore.
- **Kill-criterion:** **KEEP if** foldl-pass CPU **−≥2%**; **REVERT if** 0 (it's pure overhead, so any measurable drop wins).
- **Risk:** trivial.

### T7 — Inline `OP_LESS` int-int fast path
- **Do:** give `<`/`>` an inline int comparison opcode mirroring `OP_EQ`, instead of the
  indirect `primLessThan` function-pointer call (`vm.cc:10746`).
- **Gate:** `NIX_V3_INLINE_LESS=1`.
- **Kill-criterion:** **KEEP if** filter CPU **−≥3%**; **REVERT if** <1%.
- **Risk:** low (filter-specific).

---

## Wave 2 — re-measure on the T1 binary first, then:

### T3 — Shrink `ValuePair` 64→32B for plain 2-arg Apps  ⚠ coordinate (see note)
- **Do:** stop carrying `evaluated`+`third` (the App-memo + App3 slots,
  `value.hh:228`) on plain 2-arg `App`/`PrimOpApp` entries. The `deepForce` writeback
  already overwrites the resolved element slot, so the memo is redundant for the
  one-shot `map`/`genList` entries this workload produces.
- **Why:** measured — `ValuePair` is **64B × 6M = 384MB = 86%** of map.foldl's
  allocation; ~half is the dead-weight slots.
- **Gate:** `NIX_V3_SLIM_APP=1`.
- **Kill-criterion:** **KEEP if** map.foldl peak RSS **−≥120MB** *and* byte-identical
  *and* suites green; **REVERT if** <50MB *or* any divergence.
- **Risk:** medium (core representation + memoization semantics). **Coordinate with the
  `Value` 16→8B lever before starting.**

### T4 — filter fusion: `concatLists ∘ map (λ. [x] | [])` → in-place builder
- **Do:** recognize the bytecode-filter shape (`bytecode_primops.cc:513`) and lower it
  to a single builder that appends kept elements directly — eliminating the **2M
  singleton `[x]` ListVecs** — while preserving the laziness the bytecode form exists
  for (the lazy-`throw` correctness case at `bytecode_primops.cc:315-320`).
- **Why:** measured — filter allocates `lists=2,000,002` (144MB of singletons); this is
  why filter (2.48×) is worse than map.foldl (1.71×).
- **Gate:** `NIX_V3_FILTER_FUSE=1`.
- **Kill-criterion:** **KEEP if** filter peak RSS **−≥120MB** *and* filter CPU **−≥20%**
  *and* the lazy-throw test still passes *and* byte-identical; **REVERT if** <50MB.
- **Risk:** medium.

---

## Wave 3 — biggest levers, highest risk (re-quantify residual first)

### T5 — Cut per-element TLS / dispatch re-entry  *(biggest CPU lever)*
- **Do:** stop paying full `dispatchLoop` re-entry per element for trivial lambda
  bodies. Either (a) hoist the `VMScope` active-VM management + `getNixEvalState` +
  flag reads (`vm.cc:2650-2681`) out of leaf calls, or (b) add a fast-path that
  executes a trivial body without re-entering a full loop. Goal: collapse the
  per-element `_tlv_get_addr` traffic (the measured #1 self-time).
- **IMPORTANT:** **re-measure after T1+T2 land** — the RCA notes this cost partly
  dissolves as dispatch entries drop. Quantify the residual TLS/dispatch self-time on
  the post-T1/T2 binary *before* committing to this surgery.
- **Gate:** `NIX_V3_LEAFCALL_FAST=1`.
- **Kill-criterion:** **KEEP if** foldl-pass CPU **−≥25%** *and* all suites green;
  **REVERT if** <10%.
- **Risk:** **high** (core VM dispatch). Largest single CPU opportunity but the most
  delicate.

### T6 — Narrow `ListVec` backbone 16→8B  ⚠ likely subsumed by `Value` 16→8B
- **Do:** store `Value*` (or a tagged 8B slot) in `ListVec.elems[]` instead of inline
  16B `Value` (`alloc.hh:102-107`). **Most likely delivered for free by the `Value`
  16→8B lever** — do this as *part of* that work, not separately.
- **Gate:** (folds into the `Value` lever's gate).
- **Kill-criterion:** **KEEP if** map.foldl peak RSS **−≥30MB**; **REVERT if** <15MB.
- **Risk:** medium-high (touches every list op). Lower priority — smaller yield than T3.

---

## Suggested ownership / sequencing
- **Wave 1 in parallel** (T1 = the headline; T2, T7 = quick independent wins).
- **Re-pin** the baseline on the T1 binary, then **Wave 2** (T3 ⚠coordinate, T4 — independent of each other).
- **Wave 3** after re-quantifying (T5 high-risk; T6 with the `Value` lever).

## Definition of done for the whole goal
map.foldl and filter measurably closer to TW on **both** axes, each fix landed behind a
gate with its A/B delta in the commit, the **scaling guard green** (no complexity
regression), results **byte-identical**, and `lode/LIST_ITERATION_PERF_2026-06-08.md`
updated with a short "what shipped / what reverted + measured deltas" addendum. Reverted
tasks are a *success* of the discipline, recorded with their data — not failures.
