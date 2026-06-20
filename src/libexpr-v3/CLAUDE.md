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
| [`lode/SESSION_ARC_2026-05-27.md`](lode/SESSION_ARC_2026-05-27.md) | **READ FIRST for any GC / memory work**. 17-commit session arc index: foundation (Stages 1+3+5 MVP), Stage 6 SPIKE SHIP-GREEN verdict (239 MB hello / 797 MB HNE freeable), 4 Rule-0 falsifications (Boehm wall, §6.2 tuning, periodic GC, Chain Phase C ×3), HNE bucket decomp + cache attribution.  Recommended next-step order with effort + yield + blocker per task | When the user asks about GC, memory reduction, "ditch Boehm", precise roots, peak RSS, Stages 4-7, what's been tried, what's falsified, or what to do next |
| [`lode/GC_PRECISE_ROOT_FOUNDATION_2026-05-27.md`](lode/GC_PRECISE_ROOT_FOUNDATION_2026-05-27.md) | The 7-stage roadmap: Stage 1 (tagIsPointer ✓), Stage 3 (walkAllV3Roots ✓), Stage 5 MVP (GcRoot RAII ✓), Stage 6 SPIKE SHIP-GREEN, Stages 4 + 6 production + 7 pending.  Updated with the SHIP-GREEN result in §"Stage 6" | When the user asks about Stage 1-7 details, precise-root walker, tagIsPointer, GcRoot, RootVisitor |
| [`lode/LIVE_FRACTION_SPIKE_2026-05-27.md`](lode/LIVE_FRACTION_SPIKE_2026-05-27.md) | Stage 6 SPIKE record: live-fraction tracer (`NIX_V3_LIVE_TRACE=1`) → 239 MB freeable on hello.drvPath, 797 MB on HNE.  End-of-run measurement is sound LOWER BOUND on what mid-eval precise GC could reclaim | When the user asks about Stage 6 ROI, live-fraction trace, what fraction of arena is garbage, the SHIP-GREEN verdict |
| [`lode/HNE_BUCKET_DECOMP_2026-05-27.md`](lode/HNE_BUCKET_DECOMP_2026-05-27.md) | HNE memory bucket decomp: 2987 MB peak = 1594 arena + 403 boehm + 990 elsewhere.  990 MB elsewhere attributed to ImportCache (~700 MB) + SQLite (~270 MB) via differential measurement.  `NIX_V3_NO_DISK_CACHE=1` workaround saves 500-950 MB peak RSS today | When the user asks about HNE memory, where the 3 GB goes, ImportCache, content cache, disk cache, cache LRU, Phase 4b |
| [`lode/BOEHM_TUNING_FALSIFIED_2026-05-27.md`](lode/BOEHM_TUNING_FALSIFIED_2026-05-27.md) | FALSIFIED on macOS aarch64: `free_space_divisor` + `gcollect_and_unmap` + `force_unmap_on_gcollect` + periodic GC all leave `boehm_unmapped=0`.  Periodic GC + 9× wall regression rules it out as a default.  Identifies arena registration as the root cause (587 MB per-collect scan) and points at arena dereg as the fix | When the user asks about Boehm tuning, free_space_divisor, gcollect_and_unmap, periodic GC, why peak Boehm heap is stuck |
| [`lode/ARENA_DEREGISTRATION_DESIGN_2026-05-27.md`](lode/ARENA_DEREGISTRATION_DESIGN_2026-05-27.md) | 2-3 day spike design.  Replace arena's `GC_add_roots` with a bridge-source side-table.  Pre-committed SHIP threshold: ≥ 100 MB peak_rss on HNE.  Critical prereq: audit Tag::External + String/Path pointer ownership.  Includes implementation outline + pitfalls + per-config measurement plan | When the user asks about arena dereg, Boehm root scan cost, bridge thunks, side-tables for `nix::Value *` |
| [`lode/STAGE_6_PRECISE_GC_DESIGN_2026-05-27.md`](lode/STAGE_6_PRECISE_GC_DESIGN_2026-05-27.md) | 2-3 week impl design.  Cheney semispace (Option A) recommended over mark-sweep / per-type arenas.  Per-workload SHIP gates: ≥ 200 MB hello.drvPath, ≥ 600 MB HNE.  Prereq chain: Phase E v0.2 default-on + arena dereg + Stage 5 MVP + this doc | When the user asks about Stage 6 production GC, mark-sweep vs compacting, the semispace approach, when major GC fires, safe-points |
| [`lode/STAGE_6_IMPLEMENTATION_GUIDE_2026-05-27.md`](lode/STAGE_6_IMPLEMENTATION_GUIDE_2026-05-27.md) | **Read after the design doc**.  Day-by-day execution playbook for the 2-3 week Stage 6 implementation: 5-day Week 1 foundation (dual-region Arena + MoveGCVisitor + major-scavenge driver + special-case fields + validation harness), 5-day Week 2 tuning (trigger cadence + long-tail sweep + stress mode), 5-day Week 3 ship.  Includes pseudocode, file:line modification map, regression-test checklist, risk mitigations cross-referenced to this session arc's 6 falsifications | When the user asks about implementing Stage 6 day-by-day, dual-region Arena, MoveGCVisitor, what to change in alloc.hh / vm.cc, the Week 1 foundation work, OR "ok let's start Stage 6" |
| [`lode/PHASE_E_V02_STRESS_DESIGN_2026-05-27.md`](lode/PHASE_E_V02_STRESS_DESIGN_2026-05-27.md) | 1-3 day handoff for "architecture alignment".  Resolves the Phase E v0.2 stress-mode missed-root concern AR7 → unblocks nursery default-on → obviates the 144 MB fakeClo dead-pool lever AND aligns the codebase with Stage 6's semi-space pattern.  Three-day plan: (1) real-workload stress validation `NIX_V3_GC_STRESS=1000` on hello/firefox/HNE; (2) mortality measurement per GC_VS_TW_ANALYSIS §4.1; (3) flip default-on (mirrors Phase D Step 11 `c0911aee6`).  Pre-committed SHIP gates: --quick 6/6 + --core 15/15 + --brute 11/11 + nixpkgs byte-equality + wall ≤ 5% regression + mortality ≥ 50% | When the user asks about nursery default-on, Phase E v0.2, stress-mode missed-root, AR7, the 144 MB fakeClo lever's architectural path, or "architecture alignment" |
| [`lode/GC_DESIGN_POST_CHENEY_2026-05-28.md`](lode/GC_DESIGN_POST_CHENEY_2026-05-28.md) | **§4 flat-MS recommendation SUPERSEDED 2026-05-29 by `GC_DECISION_2026-05-29.md` (PIVOT-IMMIX). §5 falsifier framework + §6.7 Plan B (Immix) STILL VALID.** Original: 3-agent synthesis + Cheney 2× peak math + four pre-commit falsifiers (line-occupancy / sweep-cost / post-GC peak / BiBOP). Result: F1 ≥30% measured at 46-50%, F2 fail at 40-54% wall, F3 fail at peak. Net: Immix path. **Read GC_DECISION_2026-05-29 FIRST for current Stage 6 path.** | When the user asks about Cheney falsification math, the falsifier framework, the original flat-MS-vs-Immix analysis, or scope-reality (what GC alone can't solve) |
| [`lode/STRING_DEDUP_AUDIT_2026-05-28.md`](lode/STRING_DEDUP_AUDIT_2026-05-28.md) | Measurement-spike proposal (~0.5-1 d). v3 dedupes attribute keys (`globalSymbolTable`) + PosIdx; runtime string VALUES (`allocChars` at 20+ sites) are NOT deduped; compile-time literals sit in an append-only `stringPool` (half-measure). Plausibility hypothesis (drv hashes / store paths / concat results / outPath materialization duplicate heavily on real evals) is UNMEASURED — per-Tag live-fraction data does NOT separately bucket strings. **Proposed `NIX_V3_STRINGS_ATTR=1` spike** mirrors T1.3 / #746 pattern: per-site attribution + duplication-rate estimate. Pre-committed thresholds: <50 MB no-lever / 50-100 MB marginal / 100-200 MB moderate / >200 MB significant. **Spike, not implementation commitment.** | When the user asks about string interning, string dedup, intern table, drv-hash duplication, store-path repetition, `allocChars` cost, or compile-time literal pool |
| [`lode/L_MEASUREMENT_GAP_2026-05-28.md`](lode/L_MEASUREMENT_GAP_2026-05-28.md) | **METHODOLOGY AUDIT.** Every "v3 has structurally high L" claim rests on ONE end-of-eval data point (`LIVE_FRACTION_SPIKE`) + ONE mid-eval inference (back-calc'd from Cheney +486 MB regression). NO time-series L(t), NO L_distribution, NO L_trough. §5.7-class methodology hole = "moment-vs-distribution conflation"; third instance of pattern ([[same-host-bisect]] + [[head-5-counter-trap]]). Mark-sweep recommendation SURVIVES (MS is L-insensitive for peak; robust to variance) but argument shifts from "L is high" to "MS is robust to unknown L distribution." Trigger-policy variant: could low-L moments save Cheney? Unknown. **Proposed `NIX_V3_LIVE_TRACE_PERIODIC=K` spike** (~1 d, ~150 LoC) extends live_trace.cc. Pre-committed acceptance: ≥10 samples per workload; hello + HNE + M5; cross-validate vs L_end ±5%. Parallel with MS Phase 4 SHIP-gate. **CLOSED 2026-05-29** by `L_TIME_SERIES_DATA_2026-05-29.md`. | When the user asks about live fraction, L measurement, "is non-moving right", trigger policy, time-series GC measurement, or methodology audit of GC claims |
| [`lode/PHASE_4_PRELIM_FALSIFIED_2026-05-29.md`](lode/PHASE_4_PRELIM_FALSIFIED_2026-05-29.md) | **PHASE 4 SHIP-GATE PRELIM FALSIFIED.** σ-envelope verdict on 120-eval noise-floor matrix (3 workloads × 4 configs × N=10). hello.drvPath gate-ON shows +21.88 MB regression vs ≥200 MB reduction required; HNE Δpeak within 2σ noise vs ≥500 MB required. Three of four §9 criteria FAIL by ≥2× threshold. PRELIM not FINAL — Steps 11-13 rescue path anticipated (now superseded by PIVOT-IMMIX). σ on small workloads is 0.1 MB (tight); HNE σ=65 MB lives in `elsewhere` bucket not arena. | When the user asks about flat MS SHIP gate, why current MS doesn't ship, noise floor on HNE, or how the σ envelope verdict works |
| [`lode/L_TIME_SERIES_DATA_2026-05-29.md`](lode/L_TIME_SERIES_DATA_2026-05-29.md) | **L(t) DATA + ANALYSIS.** Closes L_MEASUREMENT_GAP methodology hole. NIX_V3_LIVE_TRACE_PERIODIC=K spike output: HNE L is **non-monotonic** 0.48→0.59→0.41 (range 0.19); hello monotone 0.54→0.69. L_min observed = 0.41 ≫ Cheney 0.3 threshold → non-moving GC stays right. "Trigger-policy variant of Cheney" closed without revival. Opportunistic MS trigger bookmarked for Phase 6+ (~200 MB potential ROI). | When the user asks about L_distribution, L_trough, time-series live fraction, or trigger-policy variants |
| [`lode/BIBOP_LITE_PROJECTION_2026-05-29.md`](lode/BIBOP_LITE_PROJECTION_2026-05-29.md) | **F4 PROJECTION (JUDGMENT CALL).** Pre-committed F4 BiBOP-lite spike using projection (no carcass): 5-20% of dead Bindings clustered into returnable pages = 30-158 MB on HNE / 7-30 MB on hello. Neither clearly clears ≥15% threshold nor falls below 5%. Treatment: ADD-ON in Step 13′ of PIVOT-IMMIX path, NOT gate-blocking. Empirical confirmation deferred to Phase 13+ if Immix doesn't capture the full Bindings win. | When the user asks about BiBOP-lite, Bindings page segregation, F4 verdict, or "should we segregate Bindings into dedicated pages" |
| [`lode/IMMIX_LINE_OCCUPANCY_2026-05-29.md`](lode/IMMIX_LINE_OCCUPANCY_2026-05-29.md) | **F1 EMPIRICAL — IMMIX PASS.** Per-block 128 B line-occupancy probe via existing `live_trace.cc::reportLinesAtSize`. hello.drvPath 46.5%, HNE 50.5% fully-dead lines (vs ≥30% threshold) — both PASS by wide margin. All 8 measurements (2 wl × 4 line sizes 64/128/256/512 B) clear by 11-23 points. Bytes recoverable: 265 MB hello / 796 MB HNE (vs ≥200/500 MB SHIP). **This finding triggered the PIVOT-IMMIX decision.** | When the user asks about Immix viability, line-occupancy, fully-dead lines, F1, or "is Immix worth the +2 KLoC vs flat MS" |
| [`lode/STAGE_6_FALSIFIERS_RESULT_2026-05-29.md`](lode/STAGE_6_FALSIFIERS_RESULT_2026-05-29.md) | **4-FALSIFIER SYNTHESIS.** Single-page decision-input integrating F1+F2+F3+F4 + Phase 4 SHIP gate verdict. Net: 1 PASS (Immix viable), 2 FAIL (flat MS sweep cost + post-GC peak), 1 JUDGMENT (BiBOP). Per `GC_DESIGN §5.5` flowchart, F1 ≥30% terminates at "Implement Immix path." Recommendation to Step 10: PIVOT-IMMIX. | When the user asks about the combined falsifier verdict, GC family decision basis, or "why Immix not flat MS" |
| [`lode/GC_DECISION_2026-05-29.md`](lode/GC_DECISION_2026-05-29.md) | **BINDING DECISION — PIVOT-IMMIX.** Step 10 commits v3's Stage 6 GC to mark-region (Immix), not flat mark-sweep. `freeListBins_` per-exact-size allocator (~150-200 LoC) retires when Immix lands. Mark phase + bridge_root_registry + conservative C-stack scan + whole-block-free SURVIVE as scaffolding. Effort: ~4 KLoC / 4-6 wk. Decision-blocking contract per §7: re-introduction of flat MS as default requires new empirical data + pre-committed threshold + same-host-bisect verification. **THIS DOC SUPERSEDES the flat-MS recommendation in `GC_DESIGN_POST_CHENEY §4`.** | When the user asks about v3 Stage 6 GC family, "should we use Immix or flat MS", the production GC roadmap, or how to extend the GC | 
| [`lode/STRINGS_ATTR_SPIKE_2026-05-29.md`](lode/STRINGS_ATTR_SPIKE_2026-05-29.md) | **STEP 18 — STRING DEDUP NO LEVER.** NIX_V3_STRINGS_ATTR=1 per-allocChars site attribution: total raw allocChars 7 MB hello / 36 MB HNE. Upper-bound dedup savings ≤25 MB on HNE — below 50 MB no-lever threshold. Strings are 1.2% of arena vs Bindings 84%; Immix attacks the right bucket. Spike infrastructure preserved as future measurement tool. **Falsifies the string-dedup hypothesis.** | When the user asks about string dedup, allocChars footprint, intern table, or "should we dedup runtime strings" |
| [`lode/EXIT_GC_SPIRAL_PLAN_2026-05-29.md`](lode/EXIT_GC_SPIRAL_PLAN_2026-05-29.md) | **4-WEEK TACTICAL EXIT FROM GC SPIRAL.** After 6 GC falsifications + 0 MB shipped + a 7th (Immix) projecting below SHIP gate, pause GC track; spend Week 0 quantifying orthogonal levers (cache eviction, per-site fixes); Weeks 1-2 execute highest-yield; Week 3 second; Week 4 integrate + M5 + GC re-eval. Pre-committed thresholds at each decision point; plan's own Rule 0 escalates back to Immix if orthogonal levers all falsify. | When the user asks about "what's next", "M5 watchdog", cache eviction, per-site fixes, or the GC spiral exit |
| [`lode/GC_PAUSE_2026-05-29.md`](lode/GC_PAUSE_2026-05-29.md) | **GC TRACK PAUSED (Rule-0 hypothesis kill).** Hypothesis: "GC is v3's primary RSS lever, addressable by sequencing GC variants until one ships" — KILLED by 6 falsifications + Immix projecting below SHIP. Pauses Immix continuation (Steps 14′-17′); existing gated code (Steps 11′-13′) stays opt-in, non-disruptive; methodology continues; Week 0-3 focuses on orthogonal levers per EXIT_GC_SPIRAL_PLAN. **Reversible:** if Day 5 decision matrix routes back to GC, work resumes from Step 14′. | When the user asks about "should we keep doing Immix", "why pause GC", or the GC sunk-cost question |

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

## Pre-merge gate — RUN THE FULL `--brute` (not a subset)

**Before committing/merging ANY v3 change, run the full 22-suite battery:**

```bash
nix develop -c bash src/libexpr-v3/test/all-v3-tests.sh --brute
```

This runs every suite under the aggressive moving-GC stress (1 MB nursery +
`V3_DBG_NURSERY_AUDIT=1 V3_DBG_NURSERY_BRUTE=1`) — it catches BOTH missed-root /
use-after-free regressions AND TW-divergence (drv-parity, chain-parity,
brute-audit, 583 App-cache, lang 143, etc.). **Expect `22/22 ALL GREEN`.**

Running only SUBSETS (lang / drv-parity / chain-parity / v3-smoke / darwin-smoke)
is NOT sufficient and has shipped regressions: the 583 POS-3 valueEqual/mapAttrs
forcing divergence (2026-06-20) passed lang+drv-parity+chain-parity but failed
only under the full `--brute`'s `brute-audit` suite.  Subsets are fine for a fast
inner loop; the full `--brute` is the gate.

CPU/RSS perf is a SEPARATE gate and MUST be measured on the quiet host darwin-4
(`aarch64-darwin-4.lan`) — the laptop's ~5-10% noise floor exceeds the keep-bar
(see `bench/baselines/darwin4-rows.tsv`).  Correctness (`--brute` + byte-identity)
can run anywhere; perf cannot.

**STAMP every benchmark with the commit it was measured at.** Numbers drift the
moment the hot path changes, so an untagged number is worthless.  Convention:
(1) record in `bench/baselines/darwin4-rows.tsv` (it has a `commit` + `date`
column per row — a row whose `commit` is an ancestor of HEAD is STALE, re-measure);
(2) attach the full block to the tested commit as a git note
(`git notes add -F - <commit>` / read with `git notes show <commit>`).  Getting
the source onto darwin-4 when the github push key fails: `rsync -az
--exclude='*.o' --exclude='*.dylib' src/libexpr-v3/ aarch64-darwin-4.lan:Projects/iohk/nix/src/libexpr-v3/`
then remote `nix develop -c ninja -C build src/libexpr-v3/v3-eval src/nix/nix`.

### Checkpoint profiling — `make profile` (re-runnable; ALWAYS git-note it)
"Where does the v3 VM spend CPU at scale" is NOT a one-off.  Re-run the SAME
methodology at every checkpoint and diff:

```
ssh aarch64-darwin-4.lan 'cd ~/Projects/iohk/nix/src/libexpr-v3 && \
  COMMIT=<canonical-HEAD> bench/profile-at-scale.sh'    # firefox M5 HNE
# or, on the canonical checkout:  make profile / make profile-note
```

`bench/profile-at-scale.sh` (the committed form of the methodology in
`lode/PROFILE_AT_SCALE_2026-06-21.md`) emits, per workload, the on-CPU phase
breakdown (ALLOC / BINDINGS / PARSE / DISPATCH / LOWER / HASH / STRING / TLS /
GC / FORCE / CALL — idle/wait excluded) via macOS `sample`, plus the
DETERMINISTIC dynamics (opcode histogram, thunk churn = allocated-vs-forced,
nursery hit-rate).  The deterministic counters are exact + host-independent; the
CPU-category % carries single-sample variance, so trust the counters for fine
deltas and the categories for direction.  **darwin-4 caveat:** its source
checkout lags its rsync'd binary, so `git rev-parse` there is the WRONG commit —
pass `COMMIT=<laptop HEAD>` and attach the git note from the canonical checkout.
Like the CPU/RSS rows above, **every checkpoint profile MUST be git-noted to the
commit measured** (`bench/profile-at-scale.sh --git-note`, or capture stdout and
`git notes append <HEAD> -F -` on the laptop).  A ledger
(`bench/samples/profile-ledger.tsv`) also accrues a commit-stamped one-liner per
run as a quick drift index.

## Critical constraints (hard rules; load-bearing)

0. **The generational nursery + Phase-D write barriers + gen-major collection are SHIPPED and DEFAULT-ON** (flip `e863f127d`; opt-out RETIRED `3c17abb08`). `barrier.cc` hardcodes `g_phaseDActive = true`, the nursery/scavenge/gen-major gates are hard constants, and `NIX_V3_NURSERY` / `_SCAVENGE` / `GEN_MAJOR` are now NO-OPS. **You MUST assume nursery + moving-GC semantics in all v3 code**: any tenured object holding a nursery payload needs a Phase-D barrier + a scavenger walker, or it's a missed-root UAF (the PhD-6 class). The legacy per-op major GC (`alloc.hh g_majorGcEnabled`) stays hard-`false` — re-enabling it alongside the always-on nursery is the M-3 UAF trap; `NIX_V3_EVAC` (which requires it) is therefore an unrevived experimental path. Boehm is still the underlying page allocator, but the generational layer above it is active. Environment-sharing (`Closure/Thunk` upvalues in a shared `Env`) + Env interning are ALSO default-on (opt-out `NIX_V3_NO_ENV_SHARING` / `NO_ENV_INTERN`). Stress every force-path / GC change with the full `--brute` (see the pre-merge gate above). Background: LESSONS §1.6, `NURSERY_PHASE_D_DESIGN_2026-05-18.md`, the project memory's Phase-D / FP-4 / flip entries.

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
