# Profiling story audit — what we can attribute today

**Date:** 2026-05-24
**Author:** session synthesis
**Status:** strategic — captures the state of profiling/instrumentation infrastructure as of post-Phase 4b session
**Triggering question:** "How good is our profiling story, can we precisely attribute where we spend the time (and memory) on?"

Companion doc: [`PROFILING_IMPROVEMENTS_2026-05-24.md`](PROFILING_IMPROVEMENTS_2026-05-24.md) — concrete next-investment plan to close gaps surfaced here.

---

## 1. Position (TL;DR)

**Stronger than the Nix ecosystem at large; weaker than ideal at cross-cuts.** At the levels the team operates at — per-op cycles, per-primop wall, per-alloc-site Bindings, RSS bucket decomposition, cache state — the story is competitive: Tvix and Lix don't have anything comparable, Determinate is closer. The measurement infrastructure is itself a project differentiator.

At the cross-cuts — per-function aggregate time, per-IR-pass cost, "elsewhere" RSS decomposition, GC pause distribution, per-alloc-site for non-Bindings allocations — there are real gaps.

**The methodology track record is the biggest signal.** Two false-structural conclusions in a single week (Phase 4b cache scope; CU-disk-cache cold-tax artifact) suggest the profiling tells you *what's happening inside instrumented systems* but doesn't always tell you *what the instrumented systems are doing*. The fix isn't more instrumentation alone — it's adding a methodology-audit habit and closing the specific blind spots that produced the false conclusions.

---

## 2. Time attribution — what's instrumented

| Dimension | Mechanism | Precision | Status |
|---|---|---|---|
| Total wall (cold/warm/per-mode) | `hyperfine` + `V3_TIMING` | ±5-15 ms σ at n=8-10 | ✓ Strong; the [[measure-twice-cut-once]] rule enforces n≥10 hyperfine |
| Per-op cycle cost | `NIX_VM_OPCYCLES=1` (#786, #787 RCA, #790 inter-loop fix) | per-opcode CPU cycles | ✓ Strong post-#787 RCA |
| Per-primop wall (ns + count) | `NIX_VM_PRIMOP_TIME=1` (#788) | per-call ns × count | ✓ Strong — surfaced the 99 % primop concentration in 3 derivation primops |
| Bigram dispatch patterns | `NIX_VM_BIGRAMS=1` (#780 spike) | per-(op,op) pair count | ✓ Strong — falsified register-VM rewrite |
| Opcode dispatch breakdown | `NIX_VM_OPCOUNTS=1` (#778) | per-opcode count | ✓ Strong — falsified Stage 5 PIC |
| Per-import phase breakdown | `V3_TIMING=1` (#769) | parse / lower / emit / compile / eval per CU | ✓ Strong on cold path; partial on warm |
| Force-order cross-evaluator trace | `NIX_TRACE_EVAL=1` | TW-vs-v3 byte-by-byte | ✓ Strong; powerful for divergence RCA |
| Time-series CPU% / RSS / heap | `bench/perf-trace.py` (`PERF_TRACE_TOOL_DESIGN_2026-05-20.md`) | over-eval samples + SVG overlay | ✓ Strong but underused |
| Cache hit/miss rates | NIX_VM_STATS reports per-table | aggregate per-cache | ✓ Strong; caught the over-eager scope bug today |
| Per-LambdaCore aggregate time | — | — | ✗ **GAP** — can identify hot ops but not hot functions |
| Per-IR-pass cost in optimizer | partial via V3_TIMING | only top-level | ✗ **GAP** — opt_*.cc pipeline opacity; can't tell which pass costs what |
| GC pause times | partial via Boehm stats | scan-rate aggregate only | ✗ **GAP** — no per-collection latency distribution |
| Per-thunk evaluation time | thunksForced count only | aggregate, not per-Thunk | ✗ **GAP** — can't say "this thunk took 50 ms" |
| Per-call-site cache-hook instrumentation | — | — | ✗ **GAP** — would have caught the Phase 4b scope bug at first measurement |
| Inclusive vs exclusive time at call boundaries | partial (#790 fix) | exclusive only | ✗ Partial — #790 fixed one misattribution class; others may exist |

---

## 3. Memory attribution — what's instrumented

| Dimension | Mechanism | Precision | Status |
|---|---|---|---|
| Total RSS decomposition | `NIX_VM_STATS` (#702) | peak_rss / boehm_heap / boehm_free / v3_arena / elsewhere | ✓ Strong — exposed the 1071 MB "elsewhere" lever resolved by #751/#752 |
| Per-category bytes | `bytesValues` / `bytesBindings` / `bytesThunks` etc. (always-on) | per Tag category | ✓ Strong but unconditional cost (~25-40 MB; V3_RELEASE flag candidate per [[warm-eval-instrumentation-2026-05-23]]) |
| Per-alloc-site Bindings | `NIX_V3_BINDINGS_ATTR=1` (#746) | source-line × alloc bytes + slack% | ✓ Strong — surfaced 94 % from one line, 100 % primIntersectAttrs slack |
| Attrset size histogram | `attrsetSizeBuckets[10]` (always-on) | 10-bucket cardinality | ✓ Strong |
| Cache state (per-table) | `NIX_VM_STATS` reports CompilationUnits + EvalResults entries + bytes | per-SQLite-table | ✓ Strong; caught the over-eager scope bug |
| Boehm heap state | `GC_get_heap_size()`, `boehm_free` | aggregate | ✓ Strong |
| Allocation-burst peaks | `NIX_V3_MAX_HEAP` enforcement | wall-clock RSS sampling | ✓ Strong — defensive layers landed in #753 |
| `cu` / `capturedWiths` null fractions | — | — | ✗ **GAP** — DATA_STRUCTURE_AUDIT B1/B2 still un-measured (Tier A spike) |
| `elsewhere` bucket decomposition | partial | header-level only | ✗ Partial — CompilationUnit bytecode + vector slack + fiber stack + SQLite + Boehm metadata not separated |
| Per-alloc-site for Thunks / Closures / ListVecs / Strings | — | — | ✗ **GAP** — only Bindings has alloc-site attribution today |
| Per-Thunk lifetime / mortality | partial via `Thunk::forces` slot | aggregate only | ✗ **GAP** — no allocation-time + collection-time pairs |
| Common-value tracer (top-N strings/ints/paths/SHAs) | — | — | ✗ **GAP** — `MEMORY_REDUCTION_OPPORTUNITIES §3.4` spike pending |
| Nursery survivor mortality | `NIX_V3_PHASE_E=1` survival counters | aggregate | ✓ Partial; PERF_AUDIT T3.3 spike pending for production-mode mortality |
| String dedup opportunities | — | — | ✗ **GAP** — same string allocated N times invisible without common-value tracer |
| Cache-line / branch-mispredict attribution | external (`samply`, `perf`) | per-symbol | ✓ via external tools but not integrated |

---

## 4. The methodology track record — two false-structural conclusions this week

The profiling story has caused TWO false-structural conclusions in a single week. Worth documenting because the pattern is what motivates the [`PROFILING_IMPROVEMENTS_2026-05-24.md`](PROFILING_IMPROVEMENTS_2026-05-24.md) plan more than any single gap.

### 4.1 Phase 4b cache scope over-eagerness (commit `35564703f`)

**Observed:** Phase 4b shows wall-neutral across hello/gcc/python3/firefox + synthetic IFD workloads. SQLite shows 737 KB blobs being written. Cache hit rate reasonable.

**Reported structural conclusion:** "lever too small at primop-call boundary; need to move to coarser scope (Unison Item 1/2)."

**True cause:** the cache hook ran `forceDeep + serialise + disk-insert` on every `import` call, including nixpkgs-internal lazy imports. Over-eager scope was force-evaluating nixpkgs's lazy attrsets eagerly AND filling cache with nixpkgs-internal blobs. The 737 KB blob was a forceDeep'd `<nixpkgs>/lib/strings.nix`, not the IFD result.

**Detection signal that was MISSED:** the cache wrote blobs from nixpkgs-internal paths, not just the IFD-result path. **No instrumentation existed to surface per-call-site cache-hook invocation patterns.** Profiling told us the cache was working; it didn't tell us it was working on the wrong things.

**Fix that revealed truth:** `isIfdImport` gate scoped to actual IFD imports → 1 entry / 14 bytes (not 737 KB) → wall flipped from neutral to **1.10× faster**.

### 4.2 CU-disk-cache cold-tax artifact (commit `fe678273a` + `297f900971`)

**Observed:** Phase 4b reported COLD +78 % wall tax (1527 ms vs 857 ms OFF on ifd-large). Documented across `b248b0f8d`, `0902a6088`, EVAL_CACHE_ARCHITECTURE §13.

**Reported structural conclusion:** "Phase 4b has a real cold-write tax that scales with cache work."

**True cause:** `hyperfine --prepare "rm -rf <cache>"` wiped the default-on CU disk cache (#770/#771) before every iteration. The cost was CU disk-cache cold-recompile (parse+lower+emit for 267 CUs), NOT Phase 4b. Correct methodology pre-warms both caches and either doesn't prepare-wipe or wipes only `EvalResults` table via `sqlite3 "delete from EvalResults"`.

**Detection signal that was MISSED:** the COLD timing was an outer envelope over BOTH cache layers. **No per-cache-layer cold-tax breakdown existed.** Profiling told us cold was slow; it didn't tell us which cache's cold dominated.

**Fix that revealed truth:** scoped prepare to only `EvalResults` → COLD === OFF within noise. Cold tax is essentially zero at scale.

### 4.3 Common shape of both failures

Both errors have identical shape:

```
Profiling told us A.
Structural conclusion required A AND a hidden B.
B turned out to be where the cost actually lived.
```

In #4.1, A = "cache machinery executes on every import"; B = "scope filter on what counts as IFD." Two layers; profiling instrumented one.

In #4.2, A = "cold timing includes Phase 4b cold work"; B = "cold timing also includes CU-disk-cache cold work which dominates." Two layers; profiling reported the outer envelope.

**The blind spot is at the boundaries between instrumented systems.** Each individual subsystem has good instrumentation; the cross-system interactions don't.

### 4.4 What two same-week errors imply

Empirically: the profiling story has weak spots at compositional boundaries. Not at the level the team operates within an instrumented subsystem (those measurements are solid), but at the level of "is the instrumented subsystem doing the work I assumed?"

The fix has two components:
1. **Per-call-site instrumentation** for cross-system hook points (would have caught #4.1)
2. **Per-layer cold-tax breakdown** for stacked caches (would have caught #4.2)

Both are concrete actions, both ≤2 days, both surface in [`PROFILING_IMPROVEMENTS_2026-05-24.md`](PROFILING_IMPROVEMENTS_2026-05-24.md).

---

## 5. Where gaps hurt the currently-open strategic questions

Cross-referencing instrumentation gaps against the open Q's of the post-Phase-4b session:

| Open question | Profiling gap that blocks it |
|---|---|
| Will Phase 3e / Phase 5 scope audit generalize the Phase 4b RCA? | No per-call-site instrumentation for cache hooks; need to add it (T1.1 in improvements doc) |
| Is the "elsewhere" 1071 MB lever in `MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md` further exploitable? | `elsewhere` bucket still not decomposed past header-level (T1.2) |
| Is GC scan time becoming a bottleneck post nursery default-on? | Per-collection latency distribution not measured (T2.2) |
| Are the opt_*.cc passes individually expensive? | Per-IR-pass cost unattributed; can't tell which pass to optimise (T2.3) |
| Where does the warm-eval execute-only residue live? | V3_TIMING per-phase exists but doesn't reach inside execute portion (T3.x) |
| Should Stage 4 v4 / let-floating get more investment? | No per-function aggregate time; can't say "this lambda is hot" (T4.1) |
| Is string interning vs duplication a meaningful lever? | No common-value tracer (T3.2) |
| Are non-Bindings allocations (Thunks/Closures/ListVecs) hot at specific call sites? | Only Bindings has per-alloc-site attribution today (T1.3) |

---

## 6. Strength assessment by audience

How v3's profiling compares to neighbouring projects (subjective; based on public-doc reading):

| Project | Per-op breakdown | Per-primop wall | Per-alloc-site mem | Time-series profiler | Falsifier discipline |
|---|---|---|---|---|---|
| **v3 (this fork)** | ✓ OPCYCLES + bigrams + opcount | ✓ PRIMOP_TIME | ✓ Bindings only | ✓ perf-trace.py | ✓ measure-twice rule |
| TW (upstream Nix) | ✗ | partial via `NIX_SHOW_STATS` | ✗ | ✗ | ad-hoc |
| Tvix | partial via Rust trace | ✗ | ✗ | ✗ | partial |
| Lix | ✗ | partial | ✗ | ✗ | ad-hoc |
| Determinate | partial (`builtins.parallel` traces) | ✗ | ✗ | partial | ad-hoc |
| Snix | ✗ | ✗ | ✗ | ✗ | partial |

v3 is the only Nix-class evaluator with all five columns positive. **This is itself a project differentiator** — the same instrumentation infrastructure that enabled today's RCAs is what other projects would need to build before reaching similar conclusions.

---

## 7. Cross-references

- [`PROFILING_IMPROVEMENTS_2026-05-24.md`](PROFILING_IMPROVEMENTS_2026-05-24.md) — concrete next-investment plan derived from §4 + §5 gaps
- [`MEASURE_TWICE_CUT_ONCE_2026-05-23.md`](MEASURE_TWICE_CUT_ONCE_2026-05-23.md) — §4 methodology track record evidence for the meta-rule "benchmark methodology audits before structural conclusions"
- [`EVAL_CACHE_ARCHITECTURE_2026-05-23.md`](EVAL_CACHE_ARCHITECTURE_2026-05-23.md) §13.3(d) — partially wrong-cause conclusion that motivated this audit
- [`PERF_TRACE_TOOL_DESIGN_2026-05-20.md`](PERF_TRACE_TOOL_DESIGN_2026-05-20.md) — time-series tool (column ✓ in §6 above)
- [`MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md`](MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md) §3 — Tier A measurement spikes (T3.x in improvements doc map to these)
- [`PERF_AUDIT_2026-05-23.md`](PERF_AUDIT_2026-05-23.md) — sibling perf audit; T3.3 nursery mortality spike still pending
- [`DATA_STRUCTURE_AUDIT_2026-05-21.md`](DATA_STRUCTURE_AUDIT_2026-05-21.md) — B1/B2 null-fraction claims still unverified (T3.1 in improvements doc)
- `LESSONS_LEARNED_2026-05-15.md` §4.9 — original 10-mechanism debug story; §4.9.5 "documented CPU-profile workflow" partially closed by PERF_TRACE_TOOL but with caveats from §4 above

---

## 8. Honest limits

- The "5 columns positive" comparison in §6 is subjective and based on public-documentation reading, not direct measurement of other projects. Tvix in particular may have undocumented internal tooling.
- The methodology-error count (#4.1 + #4.2) is 2-in-one-week. That's a small sample; could be normal noise or could indicate a systemic pattern. The improvements doc treats it as pattern-worth-addressing but a third instance would confirm.
- The "GAP" markings in §2 and §3 don't all need to be closed. Some are deep investments (per-Thunk lifetime tracking) that may never have proportional payoff. The improvements doc prioritises by current-open-question gating, not gap coverage.
- The profiling story being competitive doesn't mean it's *optimal* — it means the team has built more than peers. Optimal would require closing the gaps in §2/§3 that block actual strategic decisions, which is exactly what the improvements doc covers.

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
