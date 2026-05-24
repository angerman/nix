# Profiling improvements — concrete next-investment plan

**Date:** 2026-05-24
**Author:** session synthesis
**Status:** active — concrete TODOs with effort estimates, falsifiers, and priority
**Triggering doc:** [`PROFILING_AUDIT_2026-05-24.md`](PROFILING_AUDIT_2026-05-24.md) — captures the gaps this plan closes

---

## 1. Position (TL;DR)

The profiling audit (`PROFILING_AUDIT_2026-05-24.md`) identified concrete gaps and two methodology-error patterns. This doc operationalises that audit into **3 tiers** of profiling improvements, each item with:

- A specific gap or methodology failure it closes
- Effort estimate
- Pre-committed falsifier / success criterion
- Composition with existing infrastructure

**Top recommendation: T1.1 (per-call-site instrumentation framework, 1-2 days)** unblocks the Phase 3e/5 scope audit AND prevents recurrence of the Phase 4b-class scope bug. Highest ROI single investment.

**Tier 1 (this session's priority) totals ~5 days** for the items that unblock currently-open strategic questions or close the two methodology blind spots. Tiers 2-3 are sequenced behind Tier 1.

---

## 2. Prioritisation principle

Three filters applied to rank improvements:

1. **Does this close a methodology blind spot that has CAUSED a false structural conclusion?** (highest weight — the Phase 4b RCA and CU-disk-cache RCA showed the audit's §4 problem is real)
2. **Does this unblock a currently-open strategic question?** (e.g., Phase 3e/5 scope audit; warm-eval execute-only residue; .name-class optimization path)
3. **What's the effort-vs-payoff ratio?** (≤2 days small, 2-5 days medium, >5 days large; deep infrastructure deferred unless top-1-or-2 priority)

Items that score high on (1) AND (2) AND are small effort go in **Tier 1**. Items high on (2) only go in **Tier 2**. Items high on (1) without immediate gating question OR high effort go in **Tier 3**.

---

## 3. Tier 1 — methodology-blind-spot fixes + strategically-gating gaps (~5 days total)

### T1.1 — Generic per-call-site cache-hook instrumentation (1-2 days)

**Gap:** profiling reports "cache fired 1000 times, 33 % hit rate" but cannot tell you WHICH call sites those fires came from. The Phase 4b scope bug existed because the cache fired on every `primImport` invocation including nixpkgs-internal paths; profiling registered "cache fires happening" but not "fires happening at wrong call sites."

**Closes:** §4.1 methodology blind spot AND unblocks Phase 3e/5 scope audit (the deferred follow-up from `35564703f` commit body lessons §3).

**Design:**

```c++
// In each cache-hook call site
struct CacheHookCallSite {
    const char * site_name;      // "primImport-ifd-disk"
    const char * source_pos;     // __FILE__ ":" __LINE__
    uint64_t  fires;
    uint64_t  hits;
    uint64_t  misses;
    uint64_t  inserts;
    uint64_t  bytes_written;
    uint64_t  ns_in_hook;        // exclusive; only when env-gate is on
};

// global registry
static std::unordered_map<std::string, CacheHookCallSite> g_cacheHookSites;

#define CACHE_HOOK_PROBE(name) \
    static CacheHookCallSite & __site = registerCacheHook(name, __FILE__ ":" __LINE__); \
    ScopedNsCounter __ns(__site.ns_in_hook); \
    ++__site.fires;
```

Dump under `NIX_VM_STATS` or `NIX_VM_CACHE_SITES=1`:

```
cache-hook call sites (NIX_VM_CACHE_SITES=1):
  primImport-ifd-disk @ primops.cc:7530  fires=5  hits=5  miss=0  ins=0  bytes=0  ns=2.1K
  primImport-cu-disk  @ primops.cc:7501  fires=267  hits=267  miss=0  ins=0  bytes=0  ns=145K
  primDrvHash-mid     @ primops.cc:8910  fires=785  hits=260  miss=525  ins=525  bytes=29K  ns=23.5M
```

**Falsifier / success criterion:**
- Re-run Phase 4b on `/tmp/ifd-large.nix` with T1.1 active. The probe MUST show:
  - `primImport-ifd-disk` site firing only on the IFD path (1 fire)
  - NO firing on `<nixpkgs>/lib/strings.nix` etc.
- If the probe shows nixpkgs-internal fires, the scope-bug pattern would have been detected; falsify by replicating the original `35564703f` pre-fix state and confirming the probe surfaces the bug.

**Effort:** 1-2 days. ~30 LoC for the macro + ~10 call sites instrumented.

**Composition:**
- Reuses the [`MEASURE_TWICE_CUT_ONCE_2026-05-23.md`](MEASURE_TWICE_CUT_ONCE_2026-05-23.md) discipline (pre-commit threshold)
- Extends the per-primop wall instrumentation pattern (#788) to cache-hook sites
- Becomes permanent gated-zero-cost infrastructure (like #788's `primOpCounter.nanos`)

### T1.2 — `elsewhere` RSS bucket decomposition (1 day)

**Gap:** `NIX_VM_STATS` reports `elsewhere = peak_rss − boehm_heap − v3_arena`. This bucket was 1071 MB at hello.drvPath's peak before #751/#752 inlined `attrPosTable`. The remaining ~76 MB elsewhere is not header-decomposed; we don't know whether it's CompilationUnit bytecode + lambdas, vector slack (`std::vector::capacity − size`), fiber stack, SQLite cache page allocations, Boehm metadata, or something else.

**Closes:** unblocks `MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md` §3.3 ("elsewhere census") AND informs whether further mem-reduction work has headroom.

**Design:**

Add `Census` struct totalling:

```c++
struct ElsewhereCensus {
    uint64_t cu_bytecode_bytes;       // sum over CompilationUnits of code.size()
    uint64_t cu_lambdas_bytes;        // sum over lambdas of LambdaCore + cold-fields
    uint64_t cu_other_bytes;          // strings/symbols/etc.
    uint64_t vector_slack_bytes;      // sum over instrumented std::vectors of (capacity - size) * sizeof(T)
    uint64_t fiber_stack_bytes;       // ucontext stack allocs
    uint64_t sqlite_cache_bytes;      // PRAGMA cache_size * page_size
    uint64_t boehm_metadata_bytes;    // GC_get_total_bytes() - GC_get_heap_size()  (approximation)
    uint64_t unaccounted_bytes;       // elsewhere - sum_above
};
```

Dump under `NIX_VM_STATS`:

```
elsewhere RSS decomposition (76 MB total post-#752):
  CU bytecode:      32 MB  42%
  CU lambdas:        8 MB  11%
  vector slack:     12 MB  16%
  fiber stack:       2 MB   3%
  SQLite cache:     16 MB  21%
  Boehm metadata:    4 MB   5%
  unaccounted:       2 MB   3%
```

**Falsifier / success criterion:**
- Sum of categorised bytes must be within ±10 % of `elsewhere` total
- If `unaccounted` is > 20 % of total, the decomposition is incomplete; add more categories before declaring victory

**Effort:** 1 day. Instrumentation in ~5 places (CU writer/reader, primary `std::vector` containers, fiber stack, SQLite config, Boehm).

**Composition:**
- Closes `MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md` §3.3 explicitly
- Reuses the `NIX_VM_STATS` reporting infrastructure
- Permanent always-on cost minimal (~7 size_t fields + sum on stats dump)

### T1.3 — Per-alloc-site instrumentation for Thunks / Closures / ListVecs (1-2 days)

**Gap:** `NIX_V3_BINDINGS_ATTR=1` (#746) gave per-alloc-site Bindings rollup that surfaced the 94 %-from-one-line and 100 %-slack findings. The same pattern would surface similar concentrations for Thunks, Closures, ListVecs, Strings — but the instrumentation only exists for Bindings.

**Closes:** mem-reduction work that's currently blind to non-Bindings hot spots. Per `MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md`'s Tier B/C items, this is a prerequisite for several specific optimisations.

**Design:**

Extend `bindingsOriginTable` pattern to a generic `allocOriginTable<T>`:

```c++
template <typename Tag>
struct AllocOriginRecord {
    const char * site_name;
    const char * source_pos;
    uint64_t alloc_count;
    uint64_t alloc_bytes;
    uint64_t alloc_bytes_max;   // dump-time max retained
    uint64_t slack_bytes;       // for over-allocated structures
};

extern std::unordered_map<std::string, AllocOriginRecord<ThunkTag>> g_thunkOrigins;
extern std::unordered_map<std::string, AllocOriginRecord<ClosureTag>> g_closureOrigins;
extern std::unordered_map<std::string, AllocOriginRecord<ListVecTag>> g_listVecOrigins;
extern std::unordered_map<std::string, AllocOriginRecord<StringTag>> g_stringOrigins;
```

Macro wrapper:

```c++
#define ALLOC_ORIGIN_PROBE(tag, name) \
    static AllocOriginRecord<tag> & __rec = registerAllocOrigin<tag>(name, __FILE__ ":" __LINE__); \
    ++__rec.alloc_count;
```

Dump under `NIX_V3_THUNK_ATTR=1` / `NIX_V3_CLOSURE_ATTR=1` / etc.

**Falsifier / success criterion:**
- Apply T1.3 to hello.drvPath
- Identify top-3 Thunk allocation sites accounting for ≥ 50 % of Thunk bytes
- Confirm the call-site information is actionable (i.e., the top sites lead to concrete optimisation candidates the team can prioritise)
- If top-3 sites account for < 30 % of Thunk bytes, the Thunk allocation is too spread out for per-site optimisation; revert to category-level work

**Effort:** 1-2 days. Templated extension of #746's pattern. ~20 call sites total across the 4 allocation types.

**Composition:**
- Direct extension of #746's success pattern
- Pairs with T1.2 for full memory attribution coverage
- Permanent gated-zero-cost when env-gate is off

---

## 4. Tier 2 — strategically-gating gaps without methodology evidence (~5 days total)

### T2.1 — Per-collection GC pause distribution (1 day)

**Gap:** Boehm scan rate (#702: 38 MB/s on v3, 181 MB/s on TW) is the only GC metric. No per-collection latency distribution. Under nursery default-on (post-Phase E), GC pauses move from a few large Boehm collections to many small nursery scavenges; the distribution shape matters for interactive UX and parallel-eval planning.

**Closes:** gating question for `GC_VS_TW_ANALYSIS_2026-05-23.md` ("is GC scan time becoming a bottleneck?")

**Design:**
- Hook `GC_set_start_callback` to record collection start
- Sample `clock_gettime(MONOTONIC)` at start + end
- Histogram into 10 latency buckets (1µs/10µs/100µs/1ms/10ms/100ms/1s/10s)
- Dump under `NIX_VM_STATS`

**Falsifier / success criterion:** post-spike, can answer: "of the N collections during hello.drvPath, what fraction were < 10 ms?" If the answer is "all of them", GC isn't a UX blocker; if "1-2 were > 100 ms", that informs nursery-tuning urgency.

**Effort:** 1 day.

### T2.2 — Per-IR-pass cost in optimizer pipeline (2-3 days)

**Gap:** the opt_*.cc passes (#680/#682/#690/#694/#742/#774 etc.) are individually unattributed. `V3_TIMING=1` reports total lower-time but not per-pass split. Some passes are O(n) graph walks; some are O(n²) join-point analyses; we don't know which costs what.

**Closes:** strategic question "which opt_*.cc passes are worth investing more in vs which are overhead?" Currently the team adds passes by intuition; per-pass data would direct attention.

**Design:**
- Wrap each `opt_*.cc` entry point in `ScopedNsCounter` writing to a per-pass record
- Dump under `V3_TIMING=1` extension or new `NIX_VM_OPT_TIMING=1`

```
opt_*.cc per-pass timing (hello.drvPath compile):
  opt_const_fold      :  1.2 ms (8.4 %)
  opt_strict_call     :  0.3 ms (2.1 %)
  opt_occur           :  2.4 ms (16.8 %)
  opt_strictness_v2   :  4.1 ms (28.7 %)
  opt_cross_block_cse :  0.6 ms (4.2 %)
  ... etc ...
  total compile time  : 14.3 ms
```

**Falsifier / success criterion:** post-spike, identify the top-3 most expensive opt passes. If they're load-bearing (per #680/#690/#742 commit bodies), keep; if they're cheap-win passes that take more time than they save (need composing with eval-time data), consider deferring or fast-pathing.

**Effort:** 2-3 days. ~10 passes to instrument; need careful nesting (some passes call sub-passes).

### T2.3 — Methodology lint for benchmark harnesses (1 day)

**Gap:** the CU-disk-cache cold-tax artifact (audit §4.2) happened because `hyperfine --prepare "rm -rf <cache>"` was idiomatic-looking but wiped more than intended. No lint exists to catch this class of error.

**Closes:** §4 methodology blind spot category #2.

**Design:**
- Add a `test/lint-benchmark-prepare.sh` that scans `*.sh` files in `bench/` and `test/` for:
  - `hyperfine --prepare "rm -rf .*cache.*"` patterns
  - `rm -rf` on `~/.cache/nix/v3-bytecode-v1.sqlite*` outside explicit "wipe everything" tests
  - `--prepare` that names paths the harness itself doesn't restore afterward
- Run as part of CI lint suite

**Falsifier / success criterion:** must catch the pre-fix `297f900971` ifd-large benchmark script that produced the false cold-tax reading. If it doesn't, add the specific anti-pattern.

**Effort:** 1 day. Lint script + CI integration.

**Composition:**
- Pairs with `test/lint-no-inline-getenv.sh` precedent
- Becomes part of measure-twice-cut-once's "audit methodology" meta-rule

---

## 5. Tier 3 — strategic measurement spikes (currently deferred per other docs)

### T3.1 — `cu` / `capturedWiths` null-fraction counters (0.5 day)

**Gap:** `DATA_STRUCTURE_AUDIT_2026-05-21.md` B1/B2 claimed ~90 % null fractions on `Closure::cu` and `Closure::capturedWiths`, but these were never directly verified. The audit explicitly flagged these as "unverified, need measurement spike."

**Closes:** `DATA_STRUCTURE_AUDIT_2026-05-21.md` B1/B2 verification AND informs whether side-table extraction (move null-frequent fields to separate sparse map) is worth pursuing.

**Effort:** 0.5 day. Two counter increments at known points.

### T3.2 — Common-value tracer (top-N strings / ints / paths / SHAs / shapes) (1 day)

**Gap:** repeated allocation of the same string ("system" / "x86_64-linux" / known store paths / common SHAs) is invisible without a tracer. `MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md` §3.4 codified this as a Tier A spike.

**Closes:** gating question for string-pool / value-intern memory optimisations.

**Effort:** 1 day. Histogram by value identity at allocation time; dump top-N.

### T3.3 — Phase E nursery mortality on real workloads (1 day)

**Gap:** `PERF_AUDIT_2026-05-23.md` T3.3 codified this. Phase E v0.2 mortality has been measured on synthetic tests (42-57 %) but not on real production workloads like hello.drvPath / firefox / cardano-node.

**Closes:** gating decision for `GC_VS_TW_ANALYSIS_2026-05-23.md` "flip Phase E default-on" question.

**Effort:** 1 day. Existing Phase E counters; just need workload runs + analysis.

### T3.4 — ExprAttrs / ExprLet dedup re-measurement (1 day)

**Gap:** Stage 9 killed at thunk-body granularity (#772, 1.17×/1.03× dedup). The `STAGE_9_KILLED_2026-05-22.md` revival trigger requires measuring dedup at coarser granularity (whole `ExprAttrs` / `ExprLet` bindings). Spike not yet run.

**Closes:** Stage 9 revival decision per killed-stage triggers.

**Effort:** 1 day. Modify `dedup_survey.cc` to walk ExprAttrs/ExprLet level.

---

## 6. Tier 4 — deeper infrastructure (deferred unless strategic priority shifts)

### T4.1 — Per-LambdaCore aggregate evaluation time (2-3 days)

Would enable "which functions are hot" attribution. Currently can identify hot opcodes (NIX_VM_OPCYCLES) but not hot functions. Useful for Stage 4 v4 / let-floating prioritisation.

### T4.2 — Per-Thunk lifetime tracking (3-5 days)

Allocation-time + dealloc-time pairs would surface long-lived Thunks (potential leak candidates) and short-lived Thunks (nursery-mortality candidates). Significant infrastructure; defer until simpler aggregate metrics (T3.3) prove insufficient.

### T4.3 — Flame-graph / stack-based attribution (3-5 days)

`samply` / `perf` integration with v3-symbol mapping. Useful for understanding cross-cutting hot paths. External tools already work in unintegrated form (`bench/perf-trace.py` uses them); deeper integration is large effort, partial payoff.

---

## 7. Sequencing recommendation

Given Tier 1's ~5 days and direct gating on currently-open strategic questions, recommend:

**Week 1 (immediate):**
- Day 1-2: T1.1 (per-call-site cache-hook instrumentation) — unblocks Phase 3e/5 scope audit
- Day 3: T1.2 (elsewhere RSS decomposition) — closes MEMORY_REDUCTION §3.3
- Day 4-5: T1.3 (per-alloc-site for Thunks/Closures/ListVecs) — unblocks Tier B/C mem-reduction items

**Week 2 (if Tier 1 confirms value):**
- Day 6: T2.3 (methodology lint) — paired with measure-twice-cut-once codification
- Day 7-8: T2.2 (per-IR-pass timing) — informs opt_*.cc pipeline investment
- Day 9: T2.1 (GC pause distribution) — gates Phase E default-on decision

**Week 3 (measurement-spike batch):**
- Day 10: T3.1 (cu/capturedWiths null-fractions)
- Day 11: T3.2 (common-value tracer)
- Day 12: T3.3 (Phase E mortality on real workloads)
- Day 13: T3.4 (ExprAttrs/ExprLet dedup re-measurement)

**Deferred indefinitely:** T4.1 / T4.2 / T4.3 unless strategic priorities shift.

Total Tier 1+2+3 effort: ~13 days (split over 3 weeks at realistic pace). Tier 1 alone is ~5 days and delivers the highest-impact subset.

---

## 8. Composition with existing infrastructure

Each Tier 1+2 item extends infrastructure that already exists:

| Item | Extends | Pattern |
|---|---|---|
| T1.1 cache-hook instrumentation | #788 primop wall-clock | Per-call-site counter + ns tracker, gated env var |
| T1.2 elsewhere decomposition | #702 NIX_VM_STATS RSS bucketing | Add named sub-categories to existing decomposition |
| T1.3 per-alloc-site (non-Bindings) | #746 BINDINGS_ATTR | Template the per-alloc-site rollup pattern across 4 allocation types |
| T2.1 GC pause distribution | Boehm `GC_set_start_callback` | Histogram into NIX_VM_STATS |
| T2.2 per-IR-pass timing | V3_TIMING per-import | Per-pass split inside compile phase |
| T2.3 methodology lint | test/lint-no-inline-getenv.sh | New lint script with same CI integration pattern |
| T3.1-T3.4 | per-spike infrastructure | One-shot probes per audit doc |

**No new architectural patterns required.** All items reuse infrastructure the team has already validated.

---

## 9. Honest limits

- Effort estimates assume the team's recent pace. Same-day RCAs have shown the team can move fast on well-scoped tasks; these estimates should be revised if Tier 1 items take longer than expected.
- The "Tier 1 unblocks Phase 3e/5 scope audit" claim assumes the scope-audit follow-up will find what Phase 4b's did. If Phase 3e/5 turn out to have no similar scope issue, T1.1 still adds value for future cache-hook work but doesn't immediately unlock the wall lever the team is hoping for.
- T1.2 elsewhere decomposition may surface that "unaccounted" is 30-40 % even with all named categories; that would be its own measurement task to close.
- T1.3 per-alloc-site for non-Bindings assumes the #746 pattern generalises. If Thunk/Closure allocation is more spread out than Bindings, the per-site attribution might not surface hot spots — in which case category-level work suffices.
- Tier 4 items are deferred but not falsified; if a strategic priority shift makes per-function attribution urgent (e.g., a parallel-eval push or Stage 4 v5 priority), T4.1 moves up.
- The "methodology lint" T2.3 catches the specific class of error from §4.2 but won't catch every future methodology bug. It's a partial solution, not a complete one.

---

## 10. Pre-committed falsifier for the whole plan

If after Tier 1 (~5 days) is complete:
- Phase 3e/5 scope audit fires AND shows no similar scope bug → T1.1's primary motivation didn't deliver; reassess T1.1's standalone value
- `elsewhere` decomposition shows >30 % unaccounted → T1.2 incomplete; either add more categories or accept the bound
- Per-alloc-site Thunk/Closure rollup shows no concentration >30 % → T1.3 didn't generalise the #746 pattern; defer T1.3-like work for other allocation types

If all three Tier 1 falsifiers PASS, proceed to Tier 2 immediately. If any FAIL, document in a follow-on `PROFILING_RESULTS_<date>.md` and decide whether to continue.

---

## 11. Cross-references

- [`PROFILING_AUDIT_2026-05-24.md`](PROFILING_AUDIT_2026-05-24.md) — the gap audit this doc operationalises
- [`MEASURE_TWICE_CUT_ONCE_2026-05-23.md`](MEASURE_TWICE_CUT_ONCE_2026-05-23.md) — methodology rule each Tier 1 item respects (pre-committed falsifier)
- [`MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md`](MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md) §3 — Tier A measurement spikes; T3.x in this doc maps to these
- [`PERF_AUDIT_2026-05-23.md`](PERF_AUDIT_2026-05-23.md) — sibling audit; T3.3 nursery mortality maps from there
- [`DATA_STRUCTURE_AUDIT_2026-05-21.md`](DATA_STRUCTURE_AUDIT_2026-05-21.md) B1/B2 — T3.1 cu/capturedWiths null fractions
- [`EVAL_CACHE_ARCHITECTURE_2026-05-23.md`](EVAL_CACHE_ARCHITECTURE_2026-05-23.md) §13 — T1.1 is the followup specifically needed to audit Phase 3e/5 scope
- [`PERF_TRACE_TOOL_DESIGN_2026-05-20.md`](PERF_TRACE_TOOL_DESIGN_2026-05-20.md) — existing time-series profiler; T2.1/T2.2 extend this category
- [`STAGE_9_KILLED_2026-05-22.md`](STAGE_9_KILLED_2026-05-22.md) — T3.4 ExprAttrs/ExprLet dedup re-measurement maps to revival trigger
- `LESSONS_LEARNED_2026-05-15.md` §4.9 — original 10-mechanism debug story; T1.1 closes #4 (regression tests) at the cache-hook layer

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
