# Next steps 2026-05-25 — tactical priorities post-#814 / #815 closure

**Date:** 2026-05-25
**Author:** session synthesis
**Status:** active — tactical week-of plan with falsifiers + ordering + contingencies
**Triggering context:** #814 disk-cache schema-13 fix landed (39 % v3 wall reduction; hello.drvPath warm 2.55× → 1.67× TW); #815 closed (cross-workload regression was stale-cache symptom); haskell-nix-example fully unblocked. The team has cleared a major correctness + caching front; the question is "what next."

Companion docs that this plan operationalises rather than duplicates:
- [`PROFILING_AUDIT_2026-05-24.md`](PROFILING_AUDIT_2026-05-24.md) + [`PROFILING_IMPROVEMENTS_2026-05-24.md`](PROFILING_IMPROVEMENTS_2026-05-24.md) (T1.1 / T2.x / T3.x referenced here)
- [`MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md`](MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md) (Tier A spikes, per-alloc-site playbook)
- [`GC_VS_TW_ANALYSIS_2026-05-23.md`](GC_VS_TW_ANALYSIS_2026-05-23.md) (nursery default-on decision rules)
- [`WARM_EVAL_AND_INSTRUMENTATION_2026-05-23.md`](WARM_EVAL_AND_INSTRUMENTATION_2026-05-23.md) §6.1 (V3_RELEASE compile flag)
- [`MEASURE_TWICE_CUT_ONCE_2026-05-23.md`](MEASURE_TWICE_CUT_ONCE_2026-05-23.md) (pre-committed thresholds)
- [`V3_TRUE_NATIVE_RCA_2026-05-24.md`](V3_TRUE_NATIVE_RCA_2026-05-24.md) (living RCA log for #795-#815 arc)

---

## 1. Position (TL;DR)

**Memory > wall as the next binding constraint on real workloads.** haskell-nix-example currently sits at **1.42× TW wall but 5.3× TW RSS** (569 MB → 3 GB). The wall ratio is acceptable; the memory ratio is the scaling-blocker for haskell.nix-class workloads as they grow.

**The methodology blind-spot pattern is at 3 instances in 3 days.** Phase 4b cache scope (`35564703f`), CU-disk-cache cold-tax artifact (`fe678273a`), disk_cache PK collision (`9e09a7e4c`). Each cost real time. Tier 1 profiling infrastructure (T1.1 per-call-site cache-hook instrumentation) should now move from "nice to have" to "do before the next investigation."

**Two cache-coherence operating rules codified this week.** Both should be CI-enforced where mechanically possible.

**Recommended next week (5 days, all parallelisable):**

| Day | Item | Effort | Expected outcome |
|---|---|---|---|
| 1-2 | A1 — haskell-nix-example memory attribution | 1-2 d | Top-3 RSS sites identified; 200 MB-1 GB recoverable surface |
| 1 | A2 — V3_RELEASE compile flag | 1 d | ~3-4 % wall + 25-40 MB memory |
| 2-3 | A3 — T1.1 per-call-site cache-hook instrumentation | 1-2 d | Methodology infrastructure; unblocks B1 |
| 4 | A4 — Cache-coherence CI lint | 1 d | Two rules become enforced, not just documented |
| 4-5 | B1 — Phase 3e/5 scope audit (gated on A3) | 2 d | Either drvPath-class wall flip-positive OR hard "scope is correct" answer |
| 4-5 | B2 — Nursery default-on spike + flip (parallel) | 2 d | RSS multiplier on real workloads if mortality ≥ 50 % |

Tier A + early Tier B = ~7 person-days, fits one week with parallel work; net expected outcome is hello.drvPath wall ~1.5× TW and haskell-nix-example RSS substantially compressed.

---

## 2. Current state snapshot (2026-05-25 morning, post-#814/#815)

### 2.1 Wall ratios

| Workload | TW (ms) | v3 (ms) | v3:TW | Status |
|---|---|---|---|---|
| hello.drvPath warm (via `(getFlake nixpkgs).hello.drvPath`) | 456 ± 18 | 760 ± 29 | **1.67×** | ✓ Phase 1 ≤2× target MET |
| haskell-nix-example .hello.drvPath | 5026 ± 502 | 7155 ± 573 | **1.42×** | ✓ Phase 1 ≤4× target MET |
| IFD-heavy synthetic (1M elements) | 728 ± 11 | 890 ± 13 | **1.22×** | ✓ Best workload class (Phase 4b applies) |
| IFD-heavy multi (5 IFDs × 200K) | 727 ± 8 | 893 ± 10 | **1.23×** | ✓ Phase 4b validated |

### 2.2 Memory ratios

| Workload | TW peak RSS | v3 peak RSS | v3:TW | Notes |
|---|---|---|---|---|
| hello.drvPath | 145 MB | 1083 MB | **7.5×** | Post-#751/#752 inline; pre-Tier B reduction |
| haskell-nix-example .hello.drvPath | 569 MB | **3003 MB** | **5.3×** | ⚠ Scaling-blocker for haskell.nix-class growth |
| cardano-node M5 | (TW baseline) | 919 MB | (~1×) | ✓ Within 4 GB watchdog headroom |

### 2.3 Bridge crossings (v3-NATIVE measurement, post-#795 Phase A1)

| Workload | Total v3→TW crossings | Status |
|---|---|---|
| hello.drvPath | **0** | ✓ Pure V3-NATIVE |
| bash.drvPath | **0** | ✓ Pure V3-NATIVE |
| ifd-heavy-multi | **0** | ✓ Pure V3-NATIVE |
| haskell-nix-example | 74 (51 ForceAttr, 8 CallBridge1, 10 Import-ctx, 3 ReadDir-attr, 2 ReadDir-ctx) | haskell.nix-class only |

### 2.4 Cache hit rates (post-#814)

| Cache | Cold hit | Warm hit |
|---|---|---|
| CU disk cache (#770/#771) | 0 % cold (always cold-compile on first eval) | 100 % warm |
| IFD eval-result (#741 Phase 4b) | 0 % cold | 100 % warm (when isIfdImport gate fires) |
| EvalResults table | 0 % cold | 100 % warm |

### 2.5 Falsifier ledger (cumulative, #741-#815 arc)

17 + 6 hypothesis closures in the post-Stage-5/6/9-kill window:

| Arc | Positive ✓ | Falsified ✗ |
|---|---|---|
| #741 (eval cache) | 7 (round-trip, determinism, SHADOW correctness, Phase 4 arch, scope-fix wall, scale linearity, multi-IFD compose) | 8 (forceDeep, forceDeepReadOnly, in-proc wall, cross-proc wall, batching, drvPath-class wall, Phase 4 audience, Phase 4b synthetic-literal wall) |
| #795-#808 (V3-NATIVE) | 0 bridges on standard workloads (architectural confirm) | 6 hypotheses (H1-H6 + H4 untestable) |
| #803/#814/#815 (cache coherence) | haskell-nix-example unblock; 5/5 nixpkgs byte-id | H10 killed via schema-11; PK collision; stale-cache poisoning |

23 falsifiers total across these arcs; falsification discipline holding.

### 2.6 Operating rules codified this week (new)

| Rule | Where codified | Mechanism |
|---|---|---|
| **Schema bump on LambdaDescriptor field add.** Any field added to LambdaDescriptor MUST bump `serialize.hh::kSchemaVersion` in same commit. | `e364f7695` commit body | manual discipline (could be lint-enforced, see A4) |
| **Schema bump on deserialise-path change.** Changes to fields not derivable from serialised bytes alone MUST bump `kSchemaVersion` to invalidate older entries. | `455995138` commit body + V3_TRUE_NATIVE_RCA addendum | manual discipline (could be lint-enforced, see A4) |
| **Methodology audit before structural conclusion.** When a measurement crosses a falsifier threshold (especially when it would justify abandoning a multi-week direction), audit methodology BEFORE publishing the structural conclusion. | MEASURE_TWICE_CUT_ONCE §5.7 | manual discipline; T2.3 methodology lint catches one specific subset |

---

## 3. Tier A — same-day high-ROI work (~5 days total, parallelisable)

### A1 — haskell-nix-example memory attribution (1-2 days) — PARTIAL LANDED

**Status (2026-05-26):**
- **A1 measurement spike LANDED** (commits `66b1061cd` + `0f24cda9e` from prior session): HNE peak_rss=3039.5 MB; v3_arena=1593.8 MB (Bindings=704.7 MB); mergeBindings=584 MB / 82.9 % of v3-arena Bindings; site 1 `OP_ATTRS_UPDATE_TAIL` = 98.3 % / 546.7 MB.  See `HNE_MEMORY_ATTRIBUTION_2026-05-26.md`.  **Concentrated case** verified (top-1 site = 82.9 %, falsifier exceeded by 33 pp).
- **A1b falsified** (commit `920cda88c`): pointer-keyed merge memo cache hit 0.03 % on HNE.  v3's thunk memoization defeats pointer-recurrence caches.  Reverted; rationale captured in lode doc.
- **A1a Phase A LANDED** (commit `98ca953bb`): ChainBindings discriminator scaffold — `Bindings::Kind` enum + `parent` pointer (8 B → 16 B header) + chain-aware `lookup()` + unit test `testBindingsChainLookup`.  No consumer constructs Chain yet (Phase A is no-functional-change).
- **A1a Phase B/C/D pending (task #825):** Phase B audits ~208 `entries[]` iteration sites; Phase C enables Chain construction in `mergeBindings` under `NIX_V3_CHAIN_BINDINGS=1`; Phase D promotes to default after the ≥200 MB HNE peak_rss falsifier is met.

**Why now:** the 5.3× RSS gap (569 MB TW → 3 GB v3) is the binding constraint on haskell.nix-class scaling. Cardano-node M5 has 919 MB headroom under 4 GB watchdog, but haskell.nix workloads grow faster than cardano-node-class flakes. Per [[memory-first-class]] rule (`feedback_memory_first_class.md`), even wall-neutral memory wins ≥50 MB should ship.

**Concrete steps:**

1. Run `nix eval .#packages.x86_64-linux.hello.drvPath` on haskell-nix-example with:
   ```
   NIX_V3_DIRECT_EVAL=1 NIX_V3_BINDINGS_ATTR=1 NIX_VM_STATS=1 \
   NIX_V3_MAX_HEAP=4G NIX_V3_MAX_WALL_TIME=600s nix eval ...
   ```
2. Capture per-alloc-site Bindings breakdown (top-N alloc sites + slack%)
3. Capture RSS bucket decomposition: peak_rss / boehm_heap / boehm_free / v3_arena / elsewhere
4. Compare against hello.drvPath baseline (1083 MB peak vs 3003 MB; the 2 GB delta is what we're attributing)
5. Identify haskell.nix-specific Bindings hot sites (likely `lib.fix` / `callPackage` chain / overlay merge)
6. Apply existing memory-reduction levers (per `MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md` Tier B/C) if a clean concentration emerges

**Pre-committed falsifier / decision rule (per measure-twice-cut-once):**

- **Concentrated case:** top-3 Bindings alloc sites account for ≥50 % of the 2 GB delta → pursue per-site optimization using #746 → #748/#750/#752 playbook. Expected: 200 MB - 1 GB recoverable on haskell-nix-example.
- **Diffuse case:** top-3 sites account for <30 % of the delta → memory is structural (Boehm overhead OR fiber stacks OR many small allocations). Pivot to B2 (nursery default-on) as the structural lever.
- **`elsewhere` dominated case:** `elsewhere` bucket >40 % of v3 RSS → T1.2 elsewhere decomposition becomes prerequisite; bump T1.2 to A-tier.

**Expected outcome range:** 200 MB - 1 GB recoverable surface, identified concretely; or a clean "memory is structural; nursery is the lever" data point.

**Composition:** pairs strongly with B2 (nursery default-on) — if A1 surfaces diffuse memory, B2 IS the answer; if A1 surfaces concentration, B2 still compounds via reduced tenured promotion.

**What could go wrong:**
- `NIX_V3_BINDINGS_ATTR=1` may not surface haskell.nix-specific patterns if the dominant allocations are Thunks / Closures / ListVecs rather than Bindings → need T1.3 (per-alloc-site for other types) as follow-on
- RSS dominated by `elsewhere` bucket → need T1.2 decomposition first
- haskell-nix-example may not exercise the full bingo of memory patterns; cardano-node M5 follow-on may be needed

**Cross-references:** `MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md` §2.1 (shapeCell #ifdef quick win), §3.1 (per-category byte breakdown), §3.3 (elsewhere census)

---

### A2 — V3_RELEASE compile flag (1 day) — quick mechanical win ✅ LANDED 2026-05-26 (commit `46ce47c8a`)

**Outcome:** synthetic alloc-heavy bench shows **-8.4 % wall** (DEFAULT 160.5 ± 5.3 ms → RELEASE 147.1 ± 2.3 ms, n=10).  Falsifier ≥ 2 % wall criterion exceeded by ~4×.  Build mode: `-Dlibexpr-v3:v3_release=true` plumbs `-DV3_RELEASE=1` through `add_project_arguments`.  Three macros (`V3_STATS_BUMP`, `V3_STATS_INC`, `V3_STATS_BLOCK`) in `alloc.hh` provide the uniform escape hatch; converted ~85 unconditional bump sites across `alloc.hh`/`vm.cc`/`primops.cc`/`v3_call_flake.cc`/`value_serialize.cc`.  Env-gated diagnostic counters (NIX_VM_OPCOUNTS, NIX_VM_OPCYCLES, V3_DBG_*) remain compileable per §8.5 AR1 mitigation.  143/143 lang + 12/12 v3-core + 15/15 brute pass under both modes.

**Why now:** sitting unfunded since 2026-05-23. ~3-4 % wall + 25-40 MB memory recovery at zero algorithmic risk. Pure `#ifdef`-out work. Falsifier already pre-committed in [[warm-eval-instrumentation-2026-05-23]] §6.1.

**Concrete steps:**

1. Add `V3_RELEASE` compile flag to build configuration (`flake.nix` / meson)
2. `#ifdef`-out:
   - Per-category byte counters in `alloc.hh:238-245` (`bytesValues`/`bytesClosures`/.../`bytesChars`)
   - `attrsetSizeBuckets[10]` 10-branch cascade in `bindingsAlloc`
   - Per-category alloc counters (`pairsAllocated` etc.)
   - `bindingsAllocSiteRecord` call entirely (currently early-returns but pays call+return)
   - `Thunk::forces` 4B slot (~6.4 MB on hello.drvPath)
   - `Thunk::shapeCell` 8B slot (~12.8 MB, gated when `g_cellEverywhere=0` default)
   - `LambdaDescriptor` cold fields (~85 B per Lambda, ~4.2 MB)
   - `bigramCounts[256][256]` 512 KB static
   - 95+ `V3_DBG_*` gates that are i-cache footprint when off (~5 KB)
3. Build with `V3_RELEASE` on, run benchmark suite + `all-v3-tests.sh --core`
4. Hyperfine n=10 vs default build on hello.drvPath, firefox.drvPath, haskell-nix-example

**Pre-committed falsifier / decision rule:**

- **Ship if:** ≥2 % wall reduction OR ≥20 MB peak RSS reduction (n=10 hyperfine, σ < 1 %)
- **Revert if:** <2 % wall AND <20 MB memory (with measurement-data commit per [[measure-twice-cut-once]] §3.8 variant)
- **Investigate if:** functional tests fail under `V3_RELEASE` — some counter might be load-bearing in unexpected ways (e.g., a `V3_DBG_*` gate consumed by a non-debug code path)

**Expected outcome range:** +3-4 % wall + 25-40 MB memory (per WARM_EVAL §2 estimate). Higher than threshold; should land.

**Composition:** independent of A1/A3/B*; can be done in parallel by anyone. Lowest-risk Tier A item.

**What could go wrong:**
- Some always-on instrumentation might be load-bearing for cell-everywhere code path or similar — discovery would be in functional test failure
- Wall reduction might be below threshold if compile-time DCE is already removing some instrumentation in release builds (likely partial; not full)

**Cross-references:** [[warm-eval-instrumentation-2026-05-23]] §6.1 + §3

---

### A3 — T1.1 per-call-site cache-hook instrumentation (1-2 days) — close the recurring blind spot

**Why now:** the methodology blind-spot pattern is at 3 instances in 3 days (audit §4.1 + §4.2 + the disk_cache PK collision from yesterday/this morning). Each instance cost hours of investigation. T1.1 (from PROFILING_IMPROVEMENTS) prevents the next one. **The team has independently validated the per-site-counter pattern works** (v3ToTwBySite in #795 Phase A1, `ifdProbeWithCtx[16]` in #2103cdddb). This task generalises the pattern into reusable infrastructure.

**Concrete steps (per PROFILING_IMPROVEMENTS T1.1 design):**

1. Add `CacheHookCallSite` registry struct in `alloc.hh` or new `cache_probe.hh`:
   ```c++
   struct CacheHookCallSite {
       const char * site_name;
       const char * source_pos;
       uint64_t fires;
       uint64_t hits;
       uint64_t misses;
       uint64_t inserts;
       uint64_t bytes_written;
       uint64_t ns_in_hook;
   };
   ```
2. Add `CACHE_HOOK_PROBE(name)` macro (function-static `CacheHookCallSite`, ScopedNsCounter on `ns_in_hook`, `++fires`)
3. Instrument ~10 cache-hook sites:
   - `primops.cc:7530` `primImport` IFD path (`primImport-ifd-disk`)
   - `primops.cc:7501` `primImport` CU path (`primImport-cu-disk`)
   - `primops.cc:8910` `primDerivationStrict*` mid-body (`primDrvHash-mid`)
   - Phase 3e ACTIVE skip-on-hit (`drvHash-skip-on-hit`)
   - Phase 5 EvalResults lookup (`evalResults-lookup`)
   - Phase 5 EvalResults insert (`evalResults-insert`)
   - CU disk-cache load (`cu-disk-load`)
   - CU disk-cache insert (`cu-disk-insert`)
   - bindingsSetEntry barrier (`bindings-barrier`)
   - Closure FastClo path (`closure-fastclo`)
4. Dump under `NIX_VM_CACHE_SITES=1`:
   ```
   cache-hook call sites (NIX_VM_CACHE_SITES=1):
     primImport-ifd-disk @ primops.cc:7530  fires=5  hits=5  miss=0  ins=0  bytes=0  ns=2.1K
     primImport-cu-disk  @ primops.cc:7501  fires=267  hits=267  miss=0  ns=145K
     drvHash-skip-on-hit @ primops.cc:8910  fires=785  hits=260  miss=525  ins=525  bytes=29K  ns=23.5M
   ```

**Pre-committed falsifier / decision rule:**

- **Verify against historical bug:** re-build at pre-`35564703f` commit (Phase 4b scope-bug state) WITH T1.1 active. The probe MUST show `primImport-ifd-disk` firing on nixpkgs-internal paths (e.g., `<nixpkgs>/lib/strings.nix`), not just the IFD path.
- **If T1.1 reproduces the bug signal:** infrastructure validated; ship.
- **If T1.1 does NOT surface the over-scoping:** the probe design is incomplete (e.g., needs to capture path argument or call-stack); refine before shipping.

**Expected outcome:** permanent gated-zero-cost infrastructure. First user is B1 (Phase 3e/5 scope audit). Future cache investigations get this for free.

**Composition:** B1 is gated on A3. Future #741 follow-on work uses A3. Composes with A4 (CI lint) — together they prevent both cache-scope bugs AND cache-coherence bugs at the lint level.

**What could go wrong:**
- Probe overhead might be measurable on hot paths → may need env-gate for activation, NOT always-on. Default-off is fine since these are diagnostic.
- Generic `CacheHookCallSite` design might not fit all 10 sites cleanly → variant types or per-site struct.
- Source-position captures might leak across translation units in unusual ways → static-storage approach is standard but verify.

**Cross-references:** [[profiling-audit-improvements-2026-05-24]] T1.1, [[measure-twice-cut-once]] §5.7

---

### A4 — Cache-coherence CI lint (1 day) — codify the two new operating rules ✅ LANDED 2026-05-25 (commits `521277ac9` + `7d14733c0`)

Implementation summary (matches the original plan + two refinements):

- `src/libexpr-v3/test/lint-cache-coherence.sh` — bash lint scoping
  diffs by @@-context to `LambdaDescriptor` struct body and
  `deserializeCU` body.
- Rule 1: field add/remove/rename/reorder in LambdaDescriptor requires
  `kSchemaVersion` bump in same diff.  Pure comment edits exempt via
  code-portion pair-matching (strip `// ...` then compare).
- Rule 2: any non-comment line change in `deserializeCU` body requires
  schema bump.
- Escape hatch: `// CACHE-COHERENCE-EXEMPT: <reason>` marker on a
  newly-ADDED line allows refactors that don't change byte layout.
  Marker check is line-anchored to avoid self-detection on the lint's
  own doc text.
- Wired into `all-v3-tests.sh` core suite (now 12 suites).

Falsification-verified six-case matrix: (1) empty diff exit=0,
(2) comment-only edit exit=0, (3) rename exit=1, (4) add no bump
exit=1, (5) add+bump exit=0, (6) clean exit=0.

Original plan (kept below as a record of the design):



**Why now:** two cache-coherence operating rules codified this week (§2.6 above), both currently manual discipline. CI enforcement converts them from "burn-in once, hope nobody forgets" to "the linter prevents the next instance." Pattern precedent: `test/lint-no-inline-getenv.sh` enforces the env-var-gate-retirement rule.

**Concrete steps:**

1. Add `test/lint-cache-coherence.sh` that scans staged diff:
   - **Rule 1 enforcement:** if `serialize.cc` / `serialize.hh` modifies `LambdaDescriptor`-related serialisation, the SAME commit MUST modify `kSchemaVersion`. Detect by:
     - `git diff --cached -- 'src/libexpr-v3/serialize.*'` mentions `LambdaDescriptor` field add/remove
     - Same diff must contain `+kSchemaVersion =` or version constant bump
   - **Rule 2 enforcement:** if deserialise-path files modify field interpretation, same commit MUST bump `kSchemaVersion`. Detect by:
     - Diff in `deserialize.cc` / `cu_loader.cc` / similar
     - `kSchemaVersion` constant in same diff
2. Wire into `pre-commit` hook + CI lint suite
3. Test: synthetic commits that should fail (field add without bump) AND that should pass (field add WITH bump)

**Pre-committed falsifier / decision rule:**

- **Must catch:** a synthetic test commit that adds `LambdaDescriptor::dummyField` without bumping `kSchemaVersion`
- **Must NOT block:** a synthetic test commit that adds `dummyField` AND bumps `kSchemaVersion`
- **False positive rate:** verify on last 30 days of commits; <5 % false-positive rate acceptable (would be reviewer-overridable)

**Expected outcome:** the two cache-coherence rules become enforced. Next instance of "schema-12 deserialise bug" caught at PR review, not at post-deploy hello.drvPath regression.

**Composition:** standalone; not gated on other items.

**What could go wrong:**
- LambdaDescriptor field detection might be fragile (depends on file layout); fallback is grep for known field names
- Schema-bump detection across rename / refactor might miss some changes — accept some false negatives, the lint is a tripwire not a proof

**Cross-references:** `test/lint-no-inline-getenv.sh` precedent, V3_TRUE_NATIVE_RCA §"Operating rule added"

---

## 4. Tier B — strategic unblocks (~5 days total, partial parallel with Tier A)

### B1 — Phase 3e / Phase 5 scope audit (2 days, gated on A3)

**Why now:** the deferred follow-up from `35564703f` lessons §3. If the same scope-bug pattern that hid Phase 4b's wall lever exists in Phase 3e + Phase 5, the **drvPath-class wall could flip positive without architectural change**. Highest theoretical wall payoff on the standard workload class.

**Concrete steps:**

1. With A3's T1.1 instrumentation active, run `hello.drvPath` warm with:
   - `NIX_V3_DRV_HASH_CACHE_ACTIVE=1` (Phase 3e ACTIVE)
   - `NIX_V3_DRV_HASH_CACHE_DISK=1` (Phase 5 disk-backed)
   - `NIX_VM_CACHE_SITES=1` (T1.1 probe output)
2. Inspect per-call-site invocation patterns. Check:
   - Is `drvHash-skip-on-hit` firing only on derivation primops, or also on adjacent non-derivation calls?
   - Is `evalResults-lookup` firing on every call or only IFD-marked calls?
   - Is `evalResults-insert` firing in correct hot path or being called redundantly?
3. If over-scoped: apply RCA fix (analogous to `35564703f` `isIfdImport` gate but for whichever surface is over-scoped). Use `isDrvHashCandidate` or similar boolean gate.
4. Re-measure with corrected scope.

**Pre-committed falsifier / decision rule:**

- **Scope-bug case (analogous to Phase 4b):** probe surfaces fires on non-target call sites → apply scope fix → expect ≥5 % wall reduction on hello.drvPath warm. Ship if ≥3 % wall (n=10 hyperfine).
- **Scope-correct case:** probe shows fires only on intended sites → the leaf-primop scope IS structurally too cheap on drvPath class. Document as confirmed; revisit `EVAL_CACHE_ARCHITECTURE_2026-05-23.md` §13.3(d) RETRACTION note (the retraction may need partial un-retraction — the wall-too-small thesis would be confirmed for this scope).
- **Mixed case:** Phase 3e over-scoped but Phase 5 correct (or vice versa) → fix what's fixable; document the other.

**Expected outcome range:**
- ⅓ probability: scope bug exists in either Phase 3e or Phase 5 → flip to +5-10 % wall on hello.drvPath, no architectural change
- ⅔ probability: scopes are correct; the prior "wall-neutral" reading is genuine; the leaf-primop lever truly is structurally too cheap on drvPath class

Even in the ⅔ "scope correct" case, this audit is high value: it confirms the scope claim WITH instrumentation evidence, not by absence-of-instrumentation default. The retracted §13.3(d) framing can be partially restored with rigour.

**Composition:** gated on A3 (uses T1.1). Outcome shapes whether mmap'd L2 spike re-priorities (if scope correct, mmap's reduced lookup cost still doesn't help — total per-call work is too small) OR is moot (if scope bug fixed, Phase 5 SQLite cost might already be acceptable).

**What could go wrong:**
- Phase 3e ACTIVE might already be correctly scoped — no win
- Phase 5 wall behavior is bound by SQLite I/O cost independently of scope; even correct scope might not flip wall positive
- The scope audit might find no clear pattern → mixed signal, harder to act on

**Cross-references:** `EVAL_CACHE_ARCHITECTURE_2026-05-23.md` §13.3 (a)(b)(d), `35564703f` commit body lessons §3

---

### B2 — Nursery default-on spike + flip (~2 days)

**Why now:** per [[gc-vs-tw-analysis-2026-05-23]] T3.3 measurement is the gating spike. Pre-committed decision rules already exist. **Composes strongly with A1** — if A1 surfaces diffuse memory, B2 IS the lever; if A1 surfaces concentration, B2 compounds via reduced tenured promotion. Either way, B2 is high value.

**Concrete steps:**

1. Run Phase E v0.2 (`NIX_V3_PHASE_E=1`) on hello.drvPath, firefox.drvPath, haskell-nix-example
2. Measure mortality (fraction of nursery allocations that die before promotion to tenured) — Phase E v0.2 counters already exist
3. Measure wall delta vs default-off (n=10 hyperfine)
4. Apply pre-committed decision per `GC_VS_TW_ANALYSIS_2026-05-23.md` §4:
   - **≥50 % mortality AND ≤5 % wall regression → flip default-on.** Commit: change `NIX_V3_NURSERY` default to on; opt-out becomes `NIX_V3_NO_NURSERY=1` per existing pattern
   - **30-50 % mortality → tune** (raise survivor pool size, adjust Phase E age threshold)
   - **<30 % mortality → kill for that workload class.** Document and leave opt-in.
5. If flip: verify all-v3-tests core (10/10) + 5/5 nixpkgs byte-identical + haskell-nix-example byte-identical post-flip
6. Document the wall delta and memory delta in commit body

**Pre-committed falsifier / decision rule** (verbatim from GC_VS_TW_ANALYSIS §4):

```
≥50 % mortality + ≤5 % wall regression → flip default-on
30-50 % mortality → tune
<30 % mortality → kill for that workload class
```

**Expected outcome range:**
- High-probability scenario (per literature + Phase E v0.2 synthetic-test mortality 42-57 %): mortality ≥ 50 %, flip → wall neutral or slight positive, memory savings 3-4× working set
- Moderate scenario: mortality 30-50 % on real workloads (worse than synthetic), need tuning before flip
- Low-probability scenario: mortality < 30 % on production workloads → Phase E v0.2 stays opt-in

**Composition:** A1 (memory attribution) data informs which case applies — if A1 surfaces many short-lived Bindings allocations, mortality will be high. Memory wins compound: A1's per-site savings + B2's structural savings combine.

**What could go wrong:**
- Phase E v0.2 has a known stress-mode missed-root (1 MB stress test); under default-on this could fire on real workloads
- haskell-nix-example might have different mortality than hello.drvPath; per-workload decision may be needed
- Some Tier B/C memory-reduction items in MEMORY_REDUCTION_OPPORTUNITIES become moot post-flip (good!) but others become more urgent (bad if not anticipated)

**Cross-references:** [[gc-vs-tw-analysis-2026-05-23]], `PERF_AUDIT_2026-05-23.md` T3.3, [[nursery-phase-d-decision]]

---

### B3 — Per-IR-pass cost in opt_*.cc (2-3 days)

**Why now:** the optimizer pipeline has grown ~10 passes (opt_const_fold / opt_strict_call / opt_occur / opt_strictness_v2 / opt_cross_block_cse / opt_lambda_lift / opt_strict_call_unthunk / opt_if_fold / opt_app_spine_fold / opt_gen_list_unroll). No per-pass attribution exists. **Don't know which passes carry their weight.** Worth doing before adding more passes.

**Concrete steps (per PROFILING_IMPROVEMENTS T2.2 design):**

1. Identify all `opt_*.cc` entry points (~10 passes)
2. Wrap each entry in `ScopedNsCounter` writing to per-pass record
3. Handle nesting (some passes call sub-passes; need exclusive vs inclusive time)
4. Dump under `V3_TIMING=1` extension or new `NIX_VM_OPT_TIMING=1`:
   ```
   opt_*.cc per-pass timing (hello.drvPath compile):
     opt_const_fold      :  1.2 ms (8.4 %)
     opt_strict_call     :  0.3 ms (2.1 %)
     opt_occur           :  2.4 ms (16.8 %)
     opt_strictness_v2   :  4.1 ms (28.7 %)
     opt_cross_block_cse :  0.6 ms (4.2 %)
     opt_lambda_lift     :  1.8 ms (12.6 %)
     opt_if_fold         :  0.4 ms (2.8 %)
     opt_app_spine_fold  :  0.9 ms (6.3 %)
     opt_gen_list_unroll :  0.5 ms (3.5 %)
     opt_strict_call_unthunk : 0.2 ms (1.4 %)
     total compile time  : 14.3 ms
   ```

**Pre-committed falsifier / decision rule:**

- Identify top-3 most expensive opt passes. For each:
  - **If load-bearing** (per #680/#690/#742 commit bodies): keep, document
  - **If cheap-win**: confirm composition with eval-time data — if compile-time > eval-time savings, consider fast-pathing or deferring
- If no single pass is >25 % of compile time: pipeline is balanced, no action needed
- If a single pass dominates >50 %: investigate that pass specifically

**Expected outcome:** data point for future opt-pass investment. Either confirms pipeline balance OR surfaces specific passes worth optimizing.

**Composition:** independent of A/B work; standalone profiling improvement.

**What could go wrong:**
- Nesting concerns (sub-passes inside passes); exclusive vs inclusive timing distinction matters
- Per-pass timing might not surface the right granularity if individual passes are themselves multi-phase
- Compile-time work is already capped by disk cache (#770/#771 default-on) — per-pass cost only matters on cold compile, which is rare

**Cross-references:** [[profiling-audit-improvements-2026-05-24]] T2.2

---

## 5. Tier C — strategic infrastructure (deferred, cross-team scope)

### C1 — AOT distribution spec (1-2 weeks, cross-team)

`nixpkgs-bytecode-cache` + `nixpkgs-eval-result-cache` as cache.nixos.org artifacts. Per [[warm-eval-instrumentation-2026-05-23]] §6.4 + [[eval-cache-architecture-2026-05-23]] §4.4. The v3 side is done (#777 disk cache + #781b sparse symbolTable + #741 Phase 1-5); the missing link is **infrastructure work in the broader Nix ecosystem**, not v3-VM work. Cross-team coordination story is non-trivial.

**Strategic position:** the two artifacts compose multiplicatively against TW (parse residue + primop residue both eliminated). TW has neither layer and cannot easily ship either. **This is where v3 wins decisively over TW** — but the win is gated on Nix-team buy-in, which the v3 team doesn't control unilaterally.

**Defer rationale:** larger scope than one week; coordination dependency. Worth surfacing in cross-team conversation when standard wall is competitive (post-B1 if it lands).

---

### C2 — Stage 4 v4 / let-floating (#776) — orthogonal lever

Status: dormant per memory entries. Currently 0 elisions on real workloads. Cross-function strictness analysis depth is the limiting factor. Would need substantial work to make it deliver. Per [[stage4-v4-2-2026-05-21]]: cloning machinery complete; analysis depth is the gap.

**Defer rationale:** multi-week investment; orthogonal to current binding constraints; no urgent strategic gating.

---

### C3 — haskell-nix-example wall compression beyond 1.42× — Stage-2-level work

Would require boundary elimination for ForceAttr (51 crossings on haskell-nix-example, still load-bearing). Larger architectural conversation; defer until A1 memory work makes the workload tractable. The wall ratio is acceptable per Phase 1 ≤4× target.

**Defer rationale:** memory is the binding constraint, not wall, on this workload. A1 unblocks the workload first; wall compression second.

---

## 6. Tier D — long-running candidates (deferred unless priority shifts)

| Item | Why deferred |
|---|---|
| mmap'd L2 spike | Priority DROPPED — Phase 4b validated SQLite-backed L2 is sufficient when correctly scoped (§13.3(d) retraction) |
| Stage 13 multi-core parallel eval | Months of work; trace-analysis spike per [[parallel-eval-2026-05-18]] not yet run |
| Whippet GC (Stage 16 candidate) | Dormant; fires only if tenured Boehm becomes next bottleneck (NOT today per #702 falsifier) |
| Stage 12 JIT | Deferred-with-data; per [[jit-confidence-2026-05-23]] revival trigger (dispatch > 40 % wall measured) not fired |
| Cross-process bytecode mmap (Stage 8 candidate) | Same family as C1; gated on AOT distribution commitment |
| Tier 4 profiling items (per-LambdaCore time, per-Thunk lifetime, flame-graph integration) | Per PROFILING_IMPROVEMENTS T4; defer until simpler aggregate metrics prove insufficient |
| .name-class workload optimization | Per workload heterogeneity audit, structurally separate optimization path; defer until .name perf matters as user-facing scenario |

---

## 6.5. Tier R — trigger-gated refactors and revivals

Items NOT on the active week's plan but with **explicit pre-committed triggers** that should re-prioritise them when fired. Each item lists what fires it, what it costs, what it unlocks, and what it depends on. This section exists so the team has the trigger map in view when conditions change — preventing "we'll get to it later" drift on real architectural work.

Several items here also surface in `ROADMAP_TO_VISION_2026-05-15.md`'s "Killed-stage revival triggers" table or candidate-future-stages table; this is the tactical mirror. When triggers fire, refer to the canonical strategic doc for full re-measurement procedure.

### R1 — Full de Bruijn IR (Phase L1) — ~1 week / ~300 LoC

**What:** refactor IR (`ir.hh` / `ir.cc`) to use de Bruijn `(level, index)` variable references instead of named `VarId`s. Mechanical rewrite touching every `opt_*.cc` pass; eliminates SymbolId VALUE in IR variable positions; eliminates the symbol-table-remap round-trip in `serialize.cc` for locals (attrset keys remain symbol-keyed).

**Status:** the #815 RCA landed a **Light variant** (canonical alphabetical ordering at `lowerLetRec` + `lowerAttrs` boundaries) which fixes the specific symptom. **Full variant remains deferred.** See `RCA_815_CROSS_WORKLOAD_2026-05-25.md` §"Lessons for the Full variant" + `LINKING_DESIGN_2026-05-17.md` Phase L1.

**Triggers (any one fires → re-prioritise):**
- **T1.a:** a SECOND distinct #815-class bug appears at an emit site OTHER than `lowerLetRec` / `lowerAttrs` within 30 days. Signal that Light variant + A4 lint discipline is insufficient — the class needs structural fix.
- **T1.b:** audit of `lower.cc` emit sites surfaces ≥ 3 other sites that build symbol-keyed containers without canonical ordering (would itself be a trigger for proactive R1 rather than reactive). This is also AR5 below — a near-term proactive audit task.
- **T1.c:** Stage 9 revival trigger B fires (post-ABT IR-level dedup ≥ 2× re-measured). Per ROADMAP Killed-stage table line 1232.
- **T1.d:** Unison Item 3 (hash-keyed eval cache at IR-subtree scope, beyond primop boundaries) becomes an active target. See `UNISON_IDEAS_2026-05-07.md` Item 3.
- **T1.e:** Stage 13 (multi-core parallel eval) becomes an active target. Process-local SymbolIds + threads = same #815-class bug inside one process; R1 is a prerequisite for parallel-safe symbol semantics. See AR10 below.
- **T1.f:** AOT distribution (C1) commits to schema-stability requirements. Per AR8: AOT artifacts shipped via cache.nixos.org must be cache-coherent cross-machine; Light variant's brittleness is harder to defend at distribution scale.
- **T1.g:** symbol-table remap deserialize cost (was 297 ms pre-#781b, ~5 ms post-sparse) regresses to > 30 ms on a new workload class. Direct signal that local-symbol-remap is back on the critical path.

**Unlocks:**
- Stage 9 dedup re-measurement at IR-subtree granularity (trigger B path)
- Unison Item 3 (cache at IR-subtree scope, not just primop)
- Unison Item 4 (effect propagation — cleaner with content-addressed IR)
- Stage 13 parallel eval safety (no process-local symbol semantics in IR)
- AOT distribution artifact stability (cross-machine cache coherent)
- Elimination of Light-variant brittleness across all emit sites
- Plausible 10-30 % off cold-load wall via symbol-table size reduction
- Cache-coherence rule 1 (LambdaDescriptor schema bump) becomes less critical because VarRef no longer encodes process-local SymbolId values

**Prerequisites:**
- None blocking; the team can start whenever triggered
- BUT: should NOT begin mid-investigation arc (1-week refactor cadence is qualitatively different from the team's 3-5-commits/day kill-with-data work)

**Cross-references:** `LINKING_DESIGN_2026-05-17.md` Phase L1 (original spec), `UNISON_IDEAS_2026-05-07.md` Item 2 (ABT motivation), `RCA_815_CROSS_WORKLOAD_2026-05-25.md` §"Lessons for the Full variant" (most recent rationale), `IFD_DEEP_DIVE_2026-05-21.md` (Item 2 NOT-landed flag from 2026-05-21)

### R2 — Stages 5 / 6 (hidden classes + PICs) revival

**Status:** killed `#778` (Stage 5) and `Stage 6` implicitly. Per ROADMAP `Killed-stage revival triggers` table.

**Triggers** (verbatim from ROADMAP §"Killed-stage revival triggers"):
- Stage 5 Trigger A: AttrSelect family ≥ 5 % of dispatch on a representative workload after a denominator-shifting VM change
- Stage 5 Trigger B: user report of slower-than-expected workload AND profile identifies AttrSelect as hot category
- Stage 5 Trigger C: ≥ 80 % of AttrSelect dispatch hits monomorphic call-sites AND wall savings would exceed 3 %
- Stage 6 Trigger: Stage 5 revives (any trigger)

**Re-measurement effort:** 1 day (re-run #778 opcount banner)

**Probability of revival:** Low (per ROADMAP probabilities)

**Cross-references:** `STAGE_5_6_KILLED_2026-05-23.md`, ROADMAP §"Killed-stage revival triggers"

### R3 — Stage 9 (cell-level dedup) revival

**Status:** killed `#772` Phase L0 spike. Per ROADMAP `Killed-stage revival triggers` table.

**Triggers:**
- Trigger A: coarser-granularity re-measurement (whole `ExprAttrs` / `ExprLet` bindings) shows byte-dedup ≥ 2×. Effort: 2 days (modify `dedup_survey.cc` to ExprAttrs level)
- Trigger B: post-ABT IR-level dedup ≥ 2×. **Requires R1 first.** Effort: 1 day (replace bytecode hash with IR hash in `dedup_survey.cc`)

**Probability of revival:** Moderate (Trigger A) / Low (Trigger B)

**Cross-references:** `STAGE_9_KILLED_2026-05-22.md`, ROADMAP §"Killed-stage revival triggers"

### R4 — Stage 12 (JIT) revival

**Status:** deferred-with-data per `JIT_CONFIDENCE_2026-05-23.md`.

**Trigger:** dispatch share of wall > 40 % via OPCYCLES (NOT opcount) on cardano-node M5 measured DIRECTLY **AND** remaining alternatives < 5 % wall to extract.

**Probability of revival:** Low (per #786 OPCYCLES current dispatch ~5 % wall; #788 primop concentration; alternatives substantial)

**Re-measurement effort:** 0 (data already on hand; would need fresh OPCYCLES run on M5)

**Cross-references:** `JIT_CONFIDENCE_2026-05-23.md` §7

### R5 — Stage 13 (multi-core parallel eval)

**Status:** candidate stage per `PARALLEL_EVAL_CAPABILITIES_2026-05-18.md`. NOT committed.

**Triggers** (per PARALLEL_EVAL self-correction):
- Process-level parallelism (`xargs -P`, Hydra jobset-per-process) measured to capture < 30 % of theoretical parallel benefit on a representative workload
- I/O concurrency alone (without full eval parallelism) measured to capture < 50 % of theoretical
- Critical path on cardano-node M5 measured to be < 30 % of total work
- OR a specific user-facing scenario (e.g., NixOS module eval at scale) demands intra-invocation parallelism

**Effort:** 9-15 months (per PARALLEL_EVAL self-correction; original 6-12 estimate revised up)

**Prerequisite: R1 Full de Bruijn IR** — process-local SymbolIds + threads = same class of bug as #815 inside one process. See AR10 below for the architectural argument.

**Cross-references:** `PARALLEL_EVAL_CAPABILITIES_2026-05-18.md`

### R6 — Stage 16 (Whippet GC) replacement of tenured Boehm

**Status:** dormant candidate per `BOEHM_DEPENDENCY_2026-05-21.md`.

**Trigger:** nursery default-on (B2) lands **AND** tenured Boehm scan time > 10 % of eval wall on a representative workload.

**Probability of revival:** Low (per #702 falsifier, Boehm scan is NOT current bottleneck)

**Prerequisite:** B2 nursery default-on must land first (Whippet replaces tenured Boehm; nursery must be primary allocator before tenured replacement makes sense)

**Cross-references:** `BOEHM_DEPENDENCY_2026-05-21.md`, `GC_VS_TW_ANALYSIS_2026-05-23.md`

### R7 — mmap'd L2 cache (EVAL_CACHE_ARCHITECTURE §7 spike)

**Status:** priority DROPPED post-#815 RCA + Phase 4b validation. Per `EVAL_CACHE_ARCHITECTURE_2026-05-23.md` §13.3(d) RETRACTED.

**Triggers:**
- **T7.a:** B1 (Phase 3e/5 scope audit) finds NO scope bug AND drvPath-class wall stays neutral. Signals SQLite-backed L2 is the actual cost; mmap'd L2 reduces lookup ~400× → wall positive.
- **T7.b:** AOT distribution (C1) commits AND mmap is selected as the SQLite-replacement L2 (compositional fit: shipped via cache.nixos.org, mmap'd at v3 startup).

**Effort:** 3-5 days (per EVAL_CACHE §7)

**Cross-references:** `EVAL_CACHE_ARCHITECTURE_2026-05-23.md` §4.3 + §7 + §13

### R8 — AOT distribution (C1 from Tier C)

**Status:** deferred cross-team. Tier C of this doc.

**Triggers** (all four typically required):
- Standard wall ratio competitive (≤ 1.5× TW on hello.drvPath; currently 1.67× — close)
- Schema-bump rate decreased (< 1 schema bump per month) OR versioned-artifact distribution model accepted (cache.nixos.org URL includes schema version)
- Nix-team coordination confirmed
- v3-team has bandwidth (estimated 1-2 weeks v3-side + cross-team)

**Effort:** 1-2 weeks v3-side + indefinite cross-team

**Composes with:** R7 (mmap'd L2 is the natural distribution format)

**Prerequisite consideration:** R1 (Full de Bruijn IR) reduces schema-bump frequency dramatically by eliminating one source of schema churn (LambdaDescriptor cold-field additions). Not strictly required, but recommended before AOT goes live.

**Cross-references:** `WARM_EVAL_AND_INSTRUMENTATION_2026-05-23.md` §6.4, `EVAL_CACHE_ARCHITECTURE_2026-05-23.md` §4.4

### R9 — `.name`-class workload optimization

**Status:** deferred. Structurally separate optimization path (parser/lowerer/module-traversal-dominated).

**Trigger:** `.name` eval becomes user-facing primary scenario:
- Flake exploration (`nix flake show`)
- IDE hover / attr enumeration
- `nix search` workloads
- LSP-style queries

**Effort:** undetermined (separate audit needed)

**Cross-references:** `WORKLOAD_HETEROGENEITY_AUDIT_2026-05-23.md`

### R10 — Stage 4 v4 / let-floating (#776) resume

**Status:** dormant. Cloning machinery complete; 0 elisions on real workloads.

**Triggers:**
- Cross-function strictness analysis depth gap closed (the limiting factor)
- OR: parallel investment yields the necessary depth (e.g., per-LambdaCore aggregate-time profiling from `PROFILING_IMPROVEMENTS_2026-05-24.md` T4.1 surfaces cross-function call patterns)

**Cross-references:** `[[stage4-v4-2-2026-05-21]]`, ROADMAP Stage 4

### R11 — haskell.nix wall compression beyond 1.42× TW

**Status:** deferred. Currently memory (5.3× RSS) is the binding constraint, not wall.

**Triggers:**
- A1 (haskell-nix-example memory attribution) lands AND memory gap closes to ≤ 2× TW
- AND haskell.nix becomes user-facing primary AND 1.42× wall is the new limiter
- Requires Stage-2-level boundary elimination for ForceAttr (51 crossings, still load-bearing)

**Effort:** multi-week Stage-2-class work

**Cross-references:** Tier C §C3, `V3_TRUE_NATIVE_RCA_2026-05-24.md`

### Tier R summary

| R# | Item | Effort | Prerequisite | Priority signal |
|---|---|---|---|---|
| R1 | Full de Bruijn IR | 1 wk / ~300 LoC | none blocking | second #815-class bug OR Stage 9 trig B OR Unison Item 3 active OR Stage 13 active OR AOT committed |
| R2 | Stages 5/6 PIC revival | TBD (1 d re-measure first) | none | AttrSelect ≥ 5 % dispatch on rep workload |
| R3 | Stage 9 dedup revival | 1-2 d re-measure + impl | R1 for Trigger B | coarser-grain dedup ≥ 2× OR post-ABT IR-level ≥ 2× |
| R4 | Stage 12 JIT revival | 0 (data on hand) | none | dispatch > 40 % wall via OPCYCLES on M5 + alts < 5 % |
| R5 | Stage 13 multi-core | 9-15 mo | R1 | process-parallelism < 30 % theoretical |
| R6 | Whippet GC | TBD | B2 (nursery default-on) | tenured Boehm scan > 10 % eval wall |
| R7 | mmap'd L2 cache | 3-5 d | none | B1 finds no scope bug AND wall stays neutral |
| R8 | AOT distribution | 1-2 wk + cross-team | (R1 recommended) | wall ≤ 1.5× TW + schema stability + buy-in |
| R9 | `.name`-class opt | TBD audit | none | `.name` eval becomes user-facing primary |
| R10 | Stage 4 v4 resume | TBD | per-LambdaCore profiling helpful | strictness analysis depth gap closes |
| R11 | haskell.nix wall < 1.42× | multi-week | A1 lands + memory ≤ 2× | wall becomes binding (not memory) |

**Operational rule:** each item's trigger is the falsifier for "we should NOT start this." When a trigger fires, the item moves from Tier R into the active plan; re-prioritise within ≤ 1 week of trigger firing.

---

## 7. Recommended week ordering

**Day 1 — LANDED 2026-05-26:**
- ✅ A1 + A1a Phase A + A2 all shipped in a single mega-session (commits `66b1061cd` → `46ce47c8a`).  See §1 / §3 for outcomes.

**Day 1** (parallel start) — historical/reference:
- A1 morning: kick off haskell-nix-example memory measurement runs (long; let them run in background)
- A2 afternoon: V3_RELEASE compile flag implementation + first benchmark

**Day 2** (Tier A continues + Tier A3 starts):
- A1 afternoon: analysis of measurement output; identify top-3 sites
- A2 afternoon: hyperfine n=10 vs baseline; ship/revert decision
- A3 morning: T1.1 framework design + first 3-4 cache-hook sites instrumented

**Day 3**:
- A3 morning: remaining cache-hook sites + dump format
- A3 afternoon: historical-bug verification (re-run pre-`35564703f` with T1.1; confirm probe surfaces nixpkgs-internal fires)
- A4 morning: cache-coherence CI lint script + first test cases

**Day 4** (Tier B begins, parallel):
- A4 afternoon: lint integration + false-positive verification on recent commits
- B1 morning: Phase 3e/5 scope audit with T1.1 active
- B2 morning: nursery default-on spike kick-off

**Day 5**:
- B1 afternoon: scope audit RCA conclusion + fix if applicable
- B2 afternoon: nursery mortality measurement + decision rule application

**End of week deliverables (expected):**
- haskell-nix-example RSS path identified; ~200 MB-1 GB recoverable surface known
- V3_RELEASE landed; ~3-4 % wall + 25-40 MB memory recovered on all workloads
- T1.1 instrumentation permanent infrastructure
- Cache-coherence lint preventing both new rule classes
- Phase 3e/5 scope question definitively answered (either fix landed OR scope confirmed correct)
- Nursery default-on decision made (flip / tune / kill per workload class)

**If everything lands:**
- hello.drvPath warm: 1.67× TW → ~1.4-1.5× TW
- haskell-nix-example: 1.42× wall / 5.3× RSS → 1.42× wall / 3.5-4× RSS (likely)
- Permanent prevention infrastructure for the recurring methodology pattern
- Path forward on standard-workload wall has either-or-clarity (mmap L2 priority drops further if B1 finds scope correct; rises if B1 finds nothing wrong AND wall stays neutral)

**Ordering caveats** (see §8.5 architectural risks for full analysis):
- **A2 implementation MUST preserve diagnostic-on-demand counters** (T1.1 sites, `ifdProbeWithCtx`, `v3ToTwBySite`, `V3_DBG_DESERIALIZE_VERIFY`). V3_RELEASE strips always-on instrumentation only; counters with their own env-gate must stay compileable. See AR1.
- **A1 measurements may need re-validation post-B2** if nursery default-on flip changes steady-state memory landscape. Either run A1 twice (pre + post flip) OR delay A1 final analysis until B2 decision lands. See AR2.
- **A1 measurements should isolate Phase 4b cache overhead** by running with and without `NIX_V3_NO_IFD_IMPORT_CACHE_DISK=1`. The EvalResults table can grow during eval on IFD-heavy workloads. See AR3.

---

## 8. Contingency branches

The plan needs to handle non-best-case outcomes. Concrete branches:

### 8.1 If A1 finds clean concentration (≥50 % top-3)

- Apply #746 → #748/#750/#752 playbook on identified sites
- Expected savings: 500 MB - 1 GB on haskell-nix-example
- Follow-on next-week: per-site optimization landing commits, like #748/#750/#752 series

### 8.2 If A1 finds diffuse memory (<30 % top-3)

- Memory is structural; B2 IS the lever
- Move B2 from "parallel" to "primary path"
- Expected: nursery default-on delivers 3-4× working-set reduction → haskell-nix-example RSS to ~750 MB - 1 GB range

### 8.3 If A1 finds elsewhere-bucket dominated (>40 %)

- T1.2 (elsewhere decomposition) becomes prerequisite; bump from Tier 1 profiling improvements to A-tier
- A1 splits into A1a (Bindings attribution, partial answer) + A1b (elsewhere census after T1.2 lands)
- Following week pivots to T1.2 + T1.3 (per-alloc-site for non-Bindings) before further memory work

### 8.4 If A2 (V3_RELEASE) misses threshold

- Revert with measurement-data commit per [[measure-twice-cut-once]] §3.8 variant
- Investigate: which counters are load-bearing? May be specific to release-build DCE behaviour
- Probably 1-day total burn; low risk

### 8.5 If A3 (T1.1) doesn't surface historical bug pattern

- Probe design needs refinement (capture path arg, capture call-stack frame)
- Iterate until historical-bug verification passes
- May extend A3 to 2-3 days

### 8.6 If B1 (scope audit) finds no scope bug

- The drvPath-class leaf-primop lever IS structurally too cheap
- `EVAL_CACHE_ARCHITECTURE_2026-05-23.md` §13.3(d) retraction needs partial un-retraction (the "wall too small at primop boundary" thesis confirmed for this scope, having ruled out scope bug)
- Next-week pivots: either accept 1.67× TW on hello.drvPath as the standard ratio, OR pursue Tier 1 optimization-strategies items (IC broadening, TOS caching) for cross-cutting wall improvement
- mmap'd L2 priority drops further

### 8.7 If B2 (nursery) flips default-on

- Compounds with A1 wins
- Some Tier B/C memory items become moot (snapshotCurrentWiths dies in nursery; mergeBindings slack auto-reclaimed)
- Next-week opens slot for C1 (AOT distribution spec) or Stage 4 v4 resumption

### 8.8 If B2 finds <30 % mortality

- Phase E stays opt-in
- Investigate why mortality is low (survivor pool sizing? Object lifetime patterns?)
- Probably a follow-on measurement task, not a week-of blocker

### 8.9 If MULTIPLE Tier A items succeed simultaneously

- haskell-nix-example RSS could drop from 3 GB to 1-1.5 GB range (A1 site fixes + A2 V3_RELEASE + B2 nursery default-on compounding)
- That's a substantial unblock for haskell.nix-class scaling
- Worth a write-up commit acknowledging the compound win

### 8.10 If the team's velocity is higher than estimated (likely)

- Bump Tier B items into the week; potentially start C2 (Stage 4 v4 resume) by day 5
- Don't pad with low-priority work; bank the time for next-week strategic items

### 8.11 If A2 V3_RELEASE strips diagnostic counters (AR1 materialises)

- Probable signal: B1 / A3 / future cache investigations fail to find data because instrumentation is `#ifdef`-removed
- Mitigation: revise A2 implementation to keep diagnostic-on-demand counters compileable. Distinguish:
  - "Always-on instrumentation" → strip under V3_RELEASE (per WARM_EVAL §6.1 intent)
  - "Env-gated diagnostic instrumentation" (T1.1 sites, ifdProbeWithCtx, v3ToTwBySite, V3_DBG_*) → KEEP under V3_RELEASE, only payload-active when env-var is set
- If discovered AFTER A2 lands and merged: small follow-on patch carving out the env-gated subset. Low risk.

### 8.12 If A1 finds memory pattern that nursery default-on would moot

- Concrete examples from MEMORY_REDUCTION_OPPORTUNITIES: `snapshotCurrentWiths` dies in nursery; mergeBindings slack auto-reclaimed; fakeClo memoization moot post-flip
- Mitigation: defer A1's per-site optimisation landing until B2 decision is known. If B2 flips default-on, the A1-identified sites may be already-fixed structurally
- Concrete sequence: A1 measurement → B2 decision → A1 implementation (only on sites that B2 didn't moot)
- Alternative: land both A1 fixes and B2 flip in same week; measure compound impact

### 8.13 If R1 (Full de Bruijn IR) trigger fires mid-week (AR5 audit surfaces other broken sites)

- Probable signal: someone audits lower.cc for non-canonical container construction and finds 3+ unfixed sites
- Mitigation: do NOT start R1 mid-arc. Document the audit findings; complete current week's Tier A/B work first; schedule R1 as dedicated next-week effort
- Operational rule: 1-week refactor cadence is qualitatively different from the team's 3-5-commits/day tactical work — don't mix the modes
- Stopgap if AR5 audit surfaces undefended sites: apply Light-variant canonical-ordering patches to each surfaced site WHILE R1 is being planned; treats it as defence-in-depth

---

## 8.5. Architectural risks and ordering concerns

Systematic review (2026-05-26) of the current architectural state for risks that affect downstream steps. Each risk is named, scoped, mitigated, and tracked. **Risk = a downstream consequence that could surprise us if not anticipated**, NOT a defect.

Risks are organized by horizon: this-week-plan, near-term brittleness, long-term architectural, methodology / measurement, boundary. Severity ordering within each category.

### Category 1: This-week-plan ordering risks

These risks affect the current Tier A + B sequencing. **Must be considered before Day 1.**

#### AR1 — V3_RELEASE (A2) may strip diagnostic-on-demand counters

**Concern:** A2 implementation `#ifdef`s out "always-on" instrumentation (per WARM_EVAL §6.1). Several diagnostic-on-demand counters are currently in the same compilation units:
- T1.1 cache-hook probes (A3, when landed) — `CacheHookCallSite::fires/hits/misses/inserts/bytes/ns`
- `ifdProbeWithCtx[16]` (from #2103cdddb, used for Phase 4 audience measurement)
- `v3ToTwBySite[]` (from #4093696bf, used for V3-NATIVE bridge measurement)
- `V3_DBG_DESERIALIZE_VERIFY` (from #9e1ddf3bb, used for cross-process cache integrity verification)
- 95+ `V3_DBG_*` gates (per WARM_EVAL §3)

If V3_RELEASE strips all of these uniformly, future cache / bridge / deserialise investigations become impossible on release builds — must rebuild non-release to investigate.

**Affects:** A2, A3, B1, IFD-class investigations, any future cache investigation

**Mitigation:** A2 implementation must distinguish two categories:
- "Always-on" instrumentation that pays cost regardless of env-var state (per-category byte counters, `attrsetSizeBuckets[10]`, `Thunk::forces`, `Thunk::shapeCell`, LambdaDescriptor cold fields, `bigramCounts[256][256]`) → STRIP under V3_RELEASE
- "Env-gated diagnostic" counters that pay zero cost when env-var is off → KEEP under V3_RELEASE (only payload-active when env-var is set; cost-when-off is one branch test)

The distinction is detectable by checking: "does this counter increment unconditionally on the hot path, or only inside an `if (getenv(...))` block?" The latter category is safe to keep.

**Tracking:** before A2 lands, list every counter currently in v3 by category. A2 PR review checks each is correctly classified. See PROFILING_AUDIT_2026-05-24 §2 / §3 for the catalogue.

#### AR2 — B2 nursery default-on changes memory landscape; A1 measurements may not reflect post-flip state

**Concern:** B2 flips Phase E v0.2 (or nursery generally) from opt-in to default-on. Per `GC_VS_TW_ANALYSIS_2026-05-23.md` §5 projections: 3-4× smaller working set, 10-50× faster allocation, 10-50× shorter pauses. Several memory patterns die in nursery (snapshotCurrentWiths, mergeBindings slack reclamation, fakeClo memoization moot).

If A1 runs BEFORE B2 (per current week ordering Day 1-2 vs Day 4-5), A1's per-site memory data reflects pre-flip steady-state. Some surfaced sites may be ALREADY-FIXED by the flip; landing per-site patches on those sites becomes wasted work.

**Affects:** A1 implementation phase (not measurement phase)

**Mitigation:** sequence A1 in two phases:
- A1a Day 1-2: MEASUREMENT — identify top-3 sites + slack% + RSS bucket decomposition
- A1b post-B2: IMPLEMENTATION — only land per-site patches on sites that B2 didn't already moot

Alternative: run A1 measurement BOTH pre-flip (current Day 1-2) AND post-flip (Day 5 evening). Compare. Land patches on the delta-positive sites.

**Tracking:** A1 final write-up should explicitly call out which surfaced sites would be expected to die in nursery post-flip.

#### AR3 — Phase 4b default-on grows EvalResults cache during A1 measurement

**Concern:** Phase 4b is now default-on per `d22e1bfd3`. On haskell.nix-class workloads (where IFD audience > 0), `EvalResults` table grows during eval as cache entries land. Per `297f900971` true-COLD measurement: cache populates ~5 entries / 865 bytes on multi-IFD-heavy synthetic.

For A1 measurement on haskell-nix-example, the 3 GB RSS figure includes whatever EvalResults table size accumulated. This may or may not be significant; without measurement, unknown.

**Affects:** A1 RSS measurement accuracy

**Mitigation:** A1 should run TWICE on haskell-nix-example:
- Run 1: `NIX_V3_DIRECT_EVAL=1 NIX_VM_STATS=1 NIX_V3_BINDINGS_ATTR=1` (default behaviour, Phase 4b on)
- Run 2: + `NIX_V3_NO_IFD_IMPORT_CACHE_DISK=1` (Phase 4b opted-out)
- Diff identifies cache contribution to RSS

**Tracking:** A1 write-up reports both measurements explicitly.

#### AR4 — B1 outcome feeds back into EVAL_CACHE §13.3(d) interpretation

**Concern:** B1 (Phase 3e/5 scope audit) has two possible outcomes (per §4 B1): scope-bug-found (~⅓ prob) or scope-confirmed-correct (~⅔ prob). Each outcome shapes:
- The `EVAL_CACHE_ARCHITECTURE_2026-05-23.md` §13.3(d) retraction — needs partial un-retraction if scope is confirmed correct
- mmap'd L2 (R7) priority — rises if scope correct AND wall stays neutral
- Long-term scope of eval-cache work — confirms or refutes the leaf-primop-too-small thesis

**Affects:** post-B1 strategic narrative; multiple downstream docs need amendment

**Mitigation:** budget Day 5 evening for a B1 follow-on commit that updates:
- `EVAL_CACHE_ARCHITECTURE_2026-05-23.md` §13.3(d) with the audit conclusion
- This doc (NEXT_STEPS) Tier R7 trigger status
- ROADMAP Stage 10 partial-subset subsection if outcome affects it

**Tracking:** B1 commit body must explicitly state the §13.3(d) status update.

### Category 2: Near-term brittleness risks

These risks could fire any week and affect subsequent investigations.

#### AR5 — Light canonical-ordering brittleness (other emit sites)

**Concern:** the #815 RCA landed canonical alphabetical ordering at `lowerLetRec` + `lowerAttrs` ONLY. Other emit sites in `lower.cc` that build symbol-keyed containers may have the SAME bug class without canonical ordering. Audit needed to surface these proactively, OR each will fire as a new #815-class incident.

Concrete candidates worth auditing:
- Dynamic attr construction (`ExprAttrs::DynamicAttrDef` lowering)
- With-shadowing scope construction
- Primop arg construction with named keys
- `ExprLet` (separate from ExprLetRec, may have same pattern)
- Any new emit site added since #815

**Affects:** future cross-process cache integrity

**Mitigation:** add to AR audit task: scan `lower.cc` for `for (auto & kv : *)` over symbol-keyed maps; verify each is followed by canonical sort OR uses a `formalCanonIdx[c]`-equivalent ordering. Estimated effort: ~2-3 hours.

If audit surfaces ≥ 3 undefended sites → trigger T1.b of R1 (Full de Bruijn IR) fires; consider scheduling R1.
If audit surfaces 1-2 undefended sites → apply Light-variant canonical-ordering patches.
If audit surfaces 0 undefended sites → Light variant + A4 lint is sufficient discipline.

**Tracking:** the audit itself should be a 1-2 hour task. Worth surfacing as a Tier A item if not on the current plan; for now, recommend slotting into A4 follow-on day (Day 4).

#### AR6 — Test coverage gap for broader cache-coherence patterns

**Concern:** A4 lint catches the schema-bump pattern. The #815 regression test (`e99e2594e`) catches the SPECIFIC scenario. Neither catches:
- New container types added to serialization without canonical order
- Cache key collisions between same-content-different-path sources (the OTHER #815 fix)
- Future cache-key compositional bugs
- Cross-process Value-graph integrity beyond bytecode

**Affects:** future cache-coherence bug class detection

**Mitigation:** add a **property-test for cross-process cache integrity**:
- Generate random AST (or use a corpus of known-good Nix files)
- Serialize CU in process A, deserialize in process B
- Compare: bytecode bytes, post-remap SymbolId-resolved strings, force result
- Run as part of CI; fails on any divergence
- This is a generalisation of `V3_DBG_DESERIALIZE_VERIFY` from per-call diagnostic to CI-enforced suite

**Effort:** 2-3 days (infrastructure + corpus selection + CI integration)

**Tracking:** worth adding as a Tier B item once Tier A is done. Or as A5 if AR5 audit surfaces multiple sites needing this.

#### AR7 — Phase E v0.2 known stress-mode missed-root

**Concern:** Phase E v0.2 has a documented missed-root in stress-mode (1 MB stress test). If B2 flips Phase E default-on without first resolving this, real workloads under memory pressure might hit the missed-root → silent corruption.

Per `GC_VS_TW_ANALYSIS_2026-05-23.md` §4.2: "resolve Phase E v0.2 stress-mode missed-root (1-3 days)" listed as part of the flip path.

**Affects:** B2 implementation; whether flip is safe at all

**Mitigation:** B2 explicit sub-task: before flipping default-on, EITHER resolve the missed-root OR confirm via real-workload stress testing (e.g., haskell-nix-example under `NIX_V3_GC_STRESS=1000`) that the missed-root doesn't fire on production paths.

**Tracking:** B2 ship criterion adds: "Phase E v0.2 stress-test PASS with `NIX_V3_GC_STRESS=1000` on hello.drvPath + firefox.drvPath + haskell-nix-example."

### Category 3: Long-term architectural risks

These risks affect strategic direction; not blocking any single week, but shaping the multi-month picture.

#### AR8 — Schema versioning ratchet vs AOT distribution

**Concern:** schema 11 → 12 → 13 in 3 days (per #803 / #807 / #814). Each schema bump invalidates older cached CUs. If AOT distribution (C1 / R8) ships, each schema bump invalidates the shipped artifacts. At current schema-bump rate, AOT distribution model is unstable.

**Affects:** R8 AOT distribution feasibility

**Mitigation:** before R8 can fire, EITHER:
- Schema-bump rate must decrease (< 1 bump per month). This requires architectural stability that R1 (Full de Bruijn IR) partially provides.
- OR: versioned-artifact distribution model accepted. cache.nixos.org URL includes schema version (e.g., `nixpkgs-eval-result-cache-v13.mmap`). Older versions stay available; clients fetch matching schema.
- OR: forward-compat deserialiser. New binary can read N-1 schema. Currently doesn't (correct safety behavior); changing this is a separate architectural commitment.

**Tracking:** R8 trigger explicitly includes schema-stability check. If 30 days pass without schema bump AND wall is competitive AND Nix-team buy-in: R8 fires.

#### AR9 — Cross-process cache + Boehm conservative scan

**Concern:** Boehm GC scans process memory looking for pointers. Cached CUs loaded from disk become live objects. If cache deserialization creates objects with stale process-local references (e.g., SymbolId values that meant something different in the original process), Boehm conservative scan may mishandle them as pointers — false-positive root retention, increased heap pressure, or worse.

This is closely related to the #815 class of bug but at the GC level. Light variant doesn't directly defend; A4 lint doesn't catch it.

**Affects:** memory accounting correctness; potential silent leaks on heavy cache use

**Mitigation:** R1 (Full de Bruijn IR) structurally addresses this — no process-local references in cached IR. Until R1: cache hygiene observation runs (T1.2 elsewhere RSS decomposition from PROFILING_IMPROVEMENTS) should look for Boehm overhead correlating with cache hits.

**Tracking:** if T1.2 surfaces Boehm overhead disproportionate to nursery + tenured live data, this risk is materializing; escalate AR9 to R1 trigger conditions.

#### AR10 — Process-local SymbolId + parallel eval (Stage 13) = #815-class inside one process

**Concern:** v3's symbol table interns names at runtime; each process's intern order is its own. Multiple processes have different SymbolId values for the same symbol name — this is the #815 root cause class. Stage 13 (multi-core parallel eval) introduces multiple THREADS within ONE process. If symbol interning is not strictly serialised across threads, the SAME bug class appears intra-process: thread A interns "name" as SymbolId=5, thread B interns it as SymbolId=12, both write to the same cache → divergence.

**Affects:** R5 Stage 13 feasibility AND safety

**Mitigation:** Stage 13 cannot land WITHOUT either:
- Strictly serialised symbol interning (lock contention scales with parallelism)
- OR: R1 (Full de Bruijn IR) so that IR carries no process-local SymbolId values; only attrset keys remain symbol-keyed, and those can use a deterministic naming scheme (e.g., name string directly)

Per R5 prerequisites: R1 is explicit prerequisite for R5. This risk is the architectural reason.

**Tracking:** R5 trigger pre-condition includes R1 landed.

#### AR11 — Disk cache size growth over time (no vacuum / size-cap / LRU)

**Concern:** Phase 4b default-on writes to EvalResults table. CU disk cache (#770/#771) also writes. Over time, these tables grow. No documented vacuum, size-cap, or LRU eviction policy exists.

On long-running CI environments, the cache could grow to GB scale, fragmenting disk usage and slowing SQLite operations. Eventually causes operational pain.

**Affects:** long-term operability; not blocking any single workload

**Mitigation:**
- Document expected size growth rate (measure on a CI farm over 30 days)
- Add `NIX_V3_CACHE_MAX_SIZE_MB` env-var with documented default (e.g., 1 GB)
- LRU eviction when size cap exceeded (cheap: SQLite query by last-access timestamp)
- Periodic `VACUUM` (could be tied to size growth threshold)

**Effort:** ~2 days infrastructure + cross-team conversation about default size

**Tracking:** worth surfacing as Tier C item. Not urgent; becomes urgent if a CI environment reports disk-fill issues.

### Category 4: Methodology / measurement risks

#### AR12 — Bench dependency on `getFlake` semantics

**Concern:** standard benchmark `(getFlake "nixpkgs").hello.drvPath` depends on Nix flake registry semantics + a particular nixpkgs revision + TW's flake-output construction (the 51 ForceAttr bridges on haskell.nix come from TW reading v3-built Bindings via lazy bridge during flake output construction).

If flake semantics evolve in Nix (cross-team work), or registry changes default nixpkgs, bench numbers shift without v3 changes. Comparability across measurement sessions degrades.

**Affects:** wall ratio measurements over time

**Mitigation:**
- Pin specific nixpkgs revision in benchmark scripts (`builtins.fetchTree { type = "github"; ... rev = "..."; }`)
- Avoid impure flake lookups in measurement
- Document the pinned revision in benchmark output

**Tracking:** worth adding to bench scripts in `src/libexpr-v3/bench/`. Small task; ~30 min.

#### AR13 — Strategic doc maintenance lag

**Concern:** ROADMAP has Stage 1-9 + candidate 10-13. Reality:
- Stage 2 closed
- Stage 3 (nursery) not flipped default-on yet
- Stage 4 v4 dormant (#776)
- Stages 5 + 6 KILLED
- Stage 9 KILLED
- Stage 12 deferred-with-data per JIT_CONFIDENCE
- Stage 13 candidate

The post-Stage-2 sequence is fuzzy. The strategic-doc-set landed 2026-05-15; partial updates since then have kept it usable but stage ordering rationale may no longer match current reality.

**Affects:** new-team-member onboarding; cross-doc consistency over time

**Mitigation:** periodic "state snapshot" doc (e.g., `ROADMAP_PROGRESS_SNAPSHOT_2026-05-23.md` precedent). Recommend monthly cadence.

**Tracking:** worth scheduling a snapshot after Tier A + B land (end of this week + next week).

### Category 5: Boundary risks (V3 ↔ TW)

#### AR14 — Formals bridge default-on (#792) creates haskell.nix orthogonal slow path

**Concern:** per `d22e1bfd3`, v3ToTreeWalker's formals-closure refusal lifted as default. **Known orthogonal**: haskell-nix-example v3-direct completes >12 min where TW = ~2 min — v3 over-forces module-system option evaluation, triggers builds (python3, jq, apple-sdk) TW left lazy. Separate task track (#754/#757/#757c family).

This is real production-relevant slowness on haskell.nix module-system traversal. NOT addressed by A1 / A2 / A3 / B1 / B2.

**Affects:** haskell.nix-class workloads beyond `.hello.drvPath` — anything that traverses module options

**Mitigation:** this is an explicit known issue tracked under #754/#757/#757c. Not on this week's plan because the immediate goal (`.hello.drvPath` byte-identical + 1.42× wall) is achieved. Bigger module-system workloads will surface this; when they do, the #754/#757/#757c track resumes.

**Tracking:** explicitly note in current state snapshot §2.1 that `.hello.drvPath` is the bench, NOT full module-system traversal. The 1.42× wall is for the narrow scenario.

#### AR15 — Bridge surface (51 ForceAttr) still load-bearing post #806a revert

**Concern:** #806a (retire `primV3ForceListElem`) was implement-then-reverted in <3 hours because of breakage. ForceAttr (51 crossings on haskell-nix-example) + CallBridge1 (8) + Import-ctx (10) are still load-bearing.

Future bridge-elimination work has the same risk pattern: looks dead from one workload's perspective, breaks on another. The #806a discipline (revert when broken, with measurement data) is the right response, but the cost is real.

**Affects:** R11 (haskell.nix wall compression beyond 1.42×) and any future bridge-retirement work

**Mitigation:** before retiring any bridge primop, sweep broader workload set:
- 5+ standard nixpkgs workloads
- haskell-nix-example (now in standard test set)
- cardano-node M5 if available
- Cross-process cache hits (could surface latent dependencies)

Effort: ~1 day cross-workload sweep before each bridge retirement.

**Tracking:** when R11 fires, this sweep is part of the pre-work.

### AR summary table

| AR# | Risk | Horizon | Affects | Mitigation priority |
|---|---|---|---|---|
| AR1 | V3_RELEASE strips diagnostic counters | This week | A2 → A3 / B1 / future investigations | Pre-A2 implementation |
| AR2 | B2 nursery flip moots A1 sites | This week | A1 implementation | A1 staged in measurement + impl phases |
| AR3 | Phase 4b default-on inflates A1 RSS | This week | A1 measurement accuracy | Run A1 twice (with/without cache) |
| AR4 | B1 outcome feeds back into EVAL_CACHE §13.3(d) | This week | post-B1 docs | Budget Day 5 evening for write-up |
| AR5 | Light canonical brittleness at other emit sites | Near-term | future #815-class bugs | 1-2h audit task, slot Day 4 |
| AR6 | Test coverage gap for broader cache-coherence | Near-term | future bug classes | Property test ~2-3 d; Tier B if surfaced |
| AR7 | Phase E v0.2 missed-root | B2 ship | B2 ship safety | Stress-test before flip |
| AR8 | Schema ratchet vs AOT distribution | Long-term | R8 feasibility | R1 reduces; OR versioned artifacts |
| AR9 | Cross-process cache + Boehm conservative | Long-term | memory accounting | T1.2 surfaces; R1 structurally fixes |
| AR10 | Process-local SymbolId + parallel eval | Long-term | R5 Stage 13 | R1 is prereq |
| AR11 | Disk cache size growth | Long-term | operability | Size-cap + LRU; ~2 d |
| AR12 | Bench getFlake dependency | Measurement | wall ratio over time | Pin revision in bench scripts |
| AR13 | Strategic doc maintenance lag | Measurement | onboarding + consistency | Monthly snapshot cadence |
| AR14 | Formals bridge default-on slow path | Boundary | haskell.nix module-system | #754/#757/#757c track |
| AR15 | Bridge surface retire risk | Boundary | R11 + future bridge work | Cross-workload sweep pre-retirement |

### Architectural risk operating rule

**Before any commit that affects an instrumented subsystem, multi-process semantics, or cache layer:** check this AR table for relevant risks. If a risk is materially affected, the commit body should acknowledge the risk and document mitigation. Pattern precedent: `35564703f` Phase 4b RCA explicitly documented the scope-bug pattern + how mitigation works.

---

## 9. Composition map

Visual / textual dependency graph:

```
                ┌──────────────────────────┐
                │ A1 mem-attr haskell-nix  │ ──compounds──┐
                └──────────────────────────┘              │
                                                          │
                ┌──────────────────────────┐              ▼
                │ A2 V3_RELEASE flag       │     ┌────────────────────────┐
                └──────────────────────────┘     │ B2 nursery default-on  │
                       (independent)             └────────────────────────┘
                                                  (mortality data; flip
                                                   decision per rules)
                ┌──────────────────────────┐
                │ A3 T1.1 cache-hook       │ ──gates──┐
                │     instrumentation      │          │
                └──────────────────────────┘          ▼
                                              ┌─────────────────────────┐
                                              │ B1 Phase 3e/5 scope     │
                                              │     audit               │
                                              └─────────────────────────┘
                                                (wall flip OR scope-
                                                 confirmed answer)

                ┌──────────────────────────┐
                │ A4 cache-coherence lint  │
                └──────────────────────────┘
                       (independent;
                        codifies §2.6 rules)

                ┌──────────────────────────┐
                │ B3 per-IR-pass cost      │
                └──────────────────────────┘
                       (independent;
                        informs opt-pass
                        future investment)
```

Critical dependencies:
- **B1 gated on A3** (T1.1 instrumentation is the tool B1 needs)
- **B2 composes with A1** (A1 data shapes B2 expected outcome; B2 makes A1 wins compound)
- **A1 + A2 + A4 are pairwise-independent** (can be parallelised by separate people)

Compounding effects:
- A2 (V3_RELEASE) + A1 (per-site memory fixes) + B2 (nursery default-on) → multiplicative RSS reduction on haskell-nix-example
- A3 (T1.1) + A4 (CI lint) → prevents BOTH cache-scope AND cache-coherence bugs at the methodology level

---

## 10. What's overlooked or understated in current docs

These are observations from the conversation arc that haven't fully made it into the strategic doc set yet. Worth landing in follow-on commits:

### 10.1 Memory > wall for production deployment

The team's optimization focus has been wall-heavy this week (#814 wall reduction; #815 wall reduction; Phase 4b wall validation). The 5.3× RSS gap on haskell-nix-example is the actual binding constraint on real workloads. **Memory work is overdue and should outrank wall optimization for the next cycle.** This framing is in [[memory-first-class]] but hasn't been re-emphasised post-#814.

### 10.2 The standard-workload wall is net basically flat over 4 days of work

hello.drvPath went 1.41× → 2.55× (regression from disk_cache 0 % hit) → 1.67× (post-#814). The substantive wins this week were **correctness + caching architecture + haskell.nix unblock**, not standard-workload wall. That's worth being clear about in any external communication.

### 10.3 Cache-coherence rules are load-bearing organizational knowledge

Two rules codified in one week is a signal. Both should be CI-enforced (A4). The third instance — when it arrives (and it will arrive given how the team is evolving the serialised schema) — will be cheaper if A4 lands first.

### 10.4 The methodology blind-spot pattern is at 3 instances now

Each cost real time. T1.1 + A4 + T2.3 methodology lint move from "Tier 1 priority" to "do BEFORE the next investigation." This is the operational consequence of [[measure-twice-cut-once]] §5.7.

### 10.5 v3-NATIVE on standard workloads is publishable

0 bridge crossings on hello.drvPath / bash.drvPath / ifd-heavy-multi is a clean architectural milestone. Per `V3_TRUE_NATIVE_RCA_2026-05-24.md`, the V3-NATIVE goal is achieved for standard workloads. This should be communicated outward at some point as the major Q2 milestone.

---

## 11. Pre-committed falsifiers (consolidated)

For convenience, all Tier A + B falsifiers in one place:

| Item | Ship if | Revert if |
|---|---|---|
| A1 — memory attribution | Top-3 sites ≥50 % concentration → apply playbook | Diffuse <30 % → pivot to B2 |
| A2 — V3_RELEASE | ≥2 % wall OR ≥20 MB RSS reduction (n=10) | <2 % wall AND <20 MB |
| A3 — T1.1 instrumentation | Probe surfaces historical bug pattern at pre-`35564703f` state | Doesn't surface; refine design |
| A4 — cache-coherence lint | Catches synthetic bad commit AND allows good commit | Refine detection logic |
| B1 — Phase 3e/5 scope audit | Scope bug found → fix → ≥3 % wall (n=10) | Scope correct → document confirmation; partial un-retraction of §13.3(d) |
| B2 — nursery default-on | ≥50 % mortality + ≤5 % wall regression → flip | <30 % mortality → kill for that workload class; 30-50 % → tune |
| B3 — per-IR-pass cost | Identifies actionable top-3 passes | Pipeline balanced; no action |

---

## 12. Operating rules emerged (consolidated, this week)

| Rule | Source | Enforcement path |
|---|---|---|
| **Schema bump on LambdaDescriptor field add** | `e364f7695` (#803 H10 RCA) | A4 lint LANDED (`521277ac9`) |
| **Schema bump on deserialise-path change** | `455995138` (#815 RCA) | A4 lint LANDED (`521277ac9`) |
| **Canonical alphabetical ordering at symbol-keyed container construction in lower.cc** | `1b7496844` (#815 Light variant) | Manual discipline; AR5 audit task pending; CI test would extend A4 lint |
| **Cache-key includes resolved source path (not just content hash)** | `1b7496844` (#815 Light variant) | Implemented in `computeKeyForString` site; pattern to follow for any future cache layer |
| **Methodology audit before structural conclusion** | [[measure-twice-cut-once]] §5.7 | Manual discipline + T2.3 methodology lint subset; FOUR instances of "RESOLVED → reopened → deeper cause" cycle this week reinforce |
| **Per-site instrumentation before declaring lever too small** | Phase 4b RCA + disk_cache PK RCA pattern | T1.1 (A3) becomes the standard tool; team has built ad-hoc variants 4× now |
| **Cross-workload measurement before generalising hello.drvPath findings** | Workload heterogeneity audit (`cbb870174`) | Existing `bench/workload-heterogeneity.sh` |
| **Memory delta required alongside wall delta in every optimization claim** | [[memory-first-class]] | Convention; could be PR-template enforced |
| **Diagnostic-on-demand counters use their own env-gate, NOT V3_RELEASE-stripped** | AR1 (this doc §8.5) | A2 PR review check; PROFILING_AUDIT §2/§3 catalogue is the audit basis |
| **Cache schema bumps invalidate AOT artifacts; AOT requires schema stability OR versioned-artifact model** | AR8 (this doc §8.5) | R8 trigger pre-condition; informs when AOT can fire |
| **Stage 13 (multi-core parallel eval) requires Full de Bruijn IR (R1) as prerequisite for symbol semantics safety** | AR10 (this doc §8.5) | R5 trigger pre-condition |
| **Pre-retirement cross-workload sweep for any bridge primop** | AR15 (this doc §8.5) + #806a revert precedent | Convention; ~1 day per retirement |

---

## 13. Honest limits

- Effort estimates assume sustained team velocity. The team has executed faster than my estimates consistently; the week ordering may compress.
- "Memory > wall" claim depends on which workloads the team is shipping into. If the primary target shifts to single-process drvPath wall (e.g., for nix-eval CLI UX), wall returns to primary. The framing assumes haskell.nix-class scaling is a real target.
- A3 (T1.1) effort might extend if probe design requires iteration; budget 1-2 days but allow up to 3.
- B1 outcome is genuinely uncertain — ⅓/⅔ split on scope-bug-existing is a gut estimate, not data-grounded. The audit is high-value either way.
- The "what's overlooked" §10 items are observations from conversation arc, not measurements; they could be wrong about strategic emphasis.
- AOT distribution (C1) timing is hostage to Nix-team coordination outside v3-team control. Listed as deferred but real timing could shift either way based on cross-team conversations.
- Contingency §8 enumerates likely branches but cannot cover all possibilities. Treat as planning aid, not exhaustive decision tree.
- The compound-win scenario (§8.9) assumes the items don't interact negatively. There IS a possibility that A2 (V3_RELEASE) strips counters used by A3 (T1.1) debugging; the implementations should account for this.

---

## 14. Cross-references

This doc operationalises and links:

- [[profiling-audit-improvements-2026-05-24]] — Tier 1 items (T1.1, T1.2, T1.3) map to A3 / future-T1.2-as-A1b / T1.3 follow-on; T2.x map to B3 / B2-companion; T3.x map to follow-on tasks
- [[memory-reduction-opportunities-2026-05-23]] — A1 uses §3 Tier A spike framework; per-site optimization playbook from §2 (#748/#750/#752) applies if A1 finds concentration
- [[gc-vs-tw-analysis-2026-05-23]] — B2 decision rules verbatim from §4; expected outcomes from §5 projections
- [[warm-eval-instrumentation-2026-05-23]] — A2 V3_RELEASE flag from §6.1; falsifier from §6
- [[measure-twice-cut-once]] — all Tier A + B items follow the rule; §5.7 anti-pattern motivates A3+A4 priority
- [[eval-cache-architecture-2026-05-23]] — §13.3(d) retraction status updated by B1 outcome
- [[jit-confidence-2026-05-23]] — JIT remains deferred-with-data; this week doesn't move that needle
- [[v3-true-native-rca-2026-05-24]] — living RCA log for #795-#815 arc; new operating rules added per #803/#815 commit bodies
- [[workload-heterogeneity-audit-2026-05-23]] — cross-workload measurement infrastructure; relevant for B1 generalisation
- [[memory-first-class]] — the rule that elevates A1 above wall optimization
- [[falsification-rule]] — every Tier A + B item answers "what hypothesis does this kill"

**Tier R cross-references:**
- `LINKING_DESIGN_2026-05-17.md` Phase L1 (R1 Full de Bruijn IR specification)
- `RCA_815_CROSS_WORKLOAD_2026-05-25.md` §"Lessons for the Full variant" (R1 most recent motivation)
- `UNISON_IDEAS_2026-05-07.md` Item 2 (R1 ABT-shaped IR rationale)
- `STAGE_5_6_KILLED_2026-05-23.md` (R2 background)
- `STAGE_9_KILLED_2026-05-22.md` (R3 background; Trigger B requires R1)
- `JIT_CONFIDENCE_2026-05-23.md` §7 (R4 revival trigger)
- `PARALLEL_EVAL_CAPABILITIES_2026-05-18.md` (R5 background + self-correction; AR10 architectural reason)
- `BOEHM_DEPENDENCY_2026-05-21.md` (R6 trigger basis)
- `EVAL_CACHE_ARCHITECTURE_2026-05-23.md` §4.3 + §7 + §13 (R7 mmap'd L2 spike)
- `WARM_EVAL_AND_INSTRUMENTATION_2026-05-23.md` §6.4 (R8 AOT distribution)
- `WORKLOAD_HETEROGENEITY_AUDIT_2026-05-23.md` (R9 .name-class)
- `IFD_DEEP_DIVE_2026-05-21.md` (Item 2 NOT-landed flag relevant to R1)
- `V3_TRUE_NATIVE_RCA_2026-05-24.md` (AR14 #754/#757/#757c track)
- `[[stage4-v4-2-2026-05-21]]` (R10 background)

**Commits referenced:**
- `ed8fa0669` #814 disk_cache schema-13 fix (39 % wall reduction)
- `1b7496844` #815 TRULY RESOLVED (two compounding causes; cache-key + lowerLambda canonical ordering)
- `455995138` #815 (first close; later reopened — stale-cache surface explanation only)
- `9e1ddf3bb` #815 opt-is-NOT-cause RCA (bytecode remap mis-permutes REC_SET)
- `93da764fd` #815 RCA Light Phase 1+2 (canonical iteration in lowerLetRec + lowerAttrs)
- `e99e2594e` #815 regression test
- `521277ac9` + `7d14733c0` A4 cache-coherence CI lint (LANDED)
- `e364f7695` #803 H10 killed via schema-11 (LambdaDescriptor schema rule)
- `35564703f` Phase 4b cache scope RCA
- `fe678273a` + `297f900971` CU-disk-cache cold-tax artifact RCA
- `9e09a7e4c` disk_cache PK collision (reverted then re-landed as schema-13)
- `4093696bf` Phase A1 V3-NATIVE bridge measurement (0 crossings standard workloads)
- `2af711d90` post-#803 perf + bridge baseline
- `bb5eb80a4` Phase 4b 1M-element scale (1.85× faster)
- `297f900971` Phase 4b multi-IFD heavy (1.91× faster, true COLD = OFF)
- `d22e1bfd3` Phase 4b + formals-bridge default-on (AR14 origin)
- `a5b0c152b` + `90f2ff840` #806a primV3ForceListElem retirement + revert (AR15 precedent)

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
