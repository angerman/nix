# AOT distribution strategy — v3-team-owned cache + Nix-team future integration

**Date:** 2026-05-26 (evening)
**Author:** session synthesis
**Status:** strategic — splits R8 + revives R7; informs Tier C / Tier R re-classification
**Triggering observation:** the cache.nixos.org-coordinated AOT distribution path has multi-month cross-team coordination cost. A v3-team-owned cache keyed on flake ref ships unilaterally in 4-6 weeks, targets IOG priorities, and serves as a stepping stone to upstream integration.

Companion docs:
- [`WARM_EVAL_AND_INSTRUMENTATION_2026-05-23.md`](WARM_EVAL_AND_INSTRUMENTATION_2026-05-23.md) §6.4 — original AOT distribution proposal (universal model)
- [`EVAL_CACHE_ARCHITECTURE_2026-05-23.md`](EVAL_CACHE_ARCHITECTURE_2026-05-23.md) §4.3 + §7 — mmap'd L2 design (distribution format)
- [`NEXT_STEPS_2026-05-25.md`](NEXT_STEPS_2026-05-25.md) §5 (Tier C) + §6.5 (Tier R) — updated per §10 below
- [`IDEAL_GC_DESIGN_2026-05-26.md`](IDEAL_GC_DESIGN_2026-05-26.md) §3.4 — cross-process snapshot semantics (composes)
- [`ARCHITECTURE_CRITIQUE_2026-05-26.md`](ARCHITECTURE_CRITIQUE_2026-05-26.md) §8.5 AR8 — schema-stability vs AOT distribution tension

---

## 1. Position (TL;DR)

**Split R8 (AOT distribution) into R8a + R8b.** R8a is v3-team-owned and ships unilaterally in 4-6 weeks; R8b is cache.nixos.org integration and requires multi-month Nix-team coordination. R8a is the obvious near-term move; R8b becomes a later pitch from a position of proven value.

**Revive R7 (mmap'd L2)** — previously dropped post-Phase-4b. The mmap'd flat file is structurally the right format for AOT distribution (cross-process, demand-paged, OS page-cache shared). R7's design from `EVAL_CACHE_ARCHITECTURE_2026-05-23.md` §4.3 + §7 becomes the distribution format for R8a.

**Phase 1 spike (2-3 weeks):** build the cache for ONE strategic flake (`haskell-nix-example` or cardano-node-minimal); measure end-to-end. **Pre-committed threshold: ≥30 % warm-eval ratio improvement on haskell-nix-example** to commit to broader R8a investment.

**Strategic value:** **for the IOG context, this is the single highest-leverage opportunity available.** Multi-tenant CI on cardano-node + haskell.nix workloads with v3 + AOT cache could deliver warm-eval performance that TW *structurally cannot match*. Combined with Phase 4b (already wall-positive) and #777 disk cache (default-on), AOT distribution closes the warm-eval-at-scale story.

---

## 2. Why v3-team-owned vs cache.nixos.org-coordinated

The original AOT proposal (`WARM_EVAL_AND_INSTRUMENTATION_2026-05-23.md` §6.4) framed distribution as a cache.nixos.org-substituted artifact. This has two hard dependencies the v3-team doesn't control:

1. **Nix-team coordination.** Multi-month timeline. Cross-team commitment to v3's architecture is a substantial ask.
2. **Schema stability for universally-distributed artifacts.** v3 schema went v8→v14 in 11 days (per AR8). cache.nixos.org users can't tolerate that churn.

A v3-team-owned cache lifts both constraints:

| Constraint | cache.nixos.org-coordinated | v3-team-owned |
|---|---|---|
| Cross-team buy-in | Required | Not required |
| Schema iteration freedom | Locked by upstream | v3-team controls |
| Time to first ship | 3-6 months | **4-6 weeks** |
| Coverage scope | Universal nixpkgs | Targeted IOG priorities |
| Strategic asset ownership | Shared with Nix project | v3-team independent |
| Pitch leverage to Nix-team | Up-front commitment ask | "Here's proven value at scale — want to integrate?" |
| Trust model | Standard Nix substituter | Custom (SHA-256 + signature) |
| Cache miss handling | Normal eval (defensive) | Normal eval (defensive) — same |

**These are NOT mutually exclusive paths.** R8a is the stepping stone; R8b is the longer-term ambition. R8b becomes substantially easier after R8a proves the value proposition + irons out the schema-stability question.

---

## 3. Architecture sketch

### 3.1 Cache keying — flake ref as identifier

```
Cache key:   (flake_ref, schema_version, v3_binary_version)
Example:     github:NixOS/nixpkgs/abc123def + schema=14 + v3=1.2.3
```

Flake refs are stable identifiers (`github:owner/repo/rev`) that include the exact revision. They map directly to v3's content-addressed cache substrate. The mmap'd flat file design from `EVAL_CACHE_ARCHITECTURE_2026-05-23.md` §4.3 is the natural distribution format.

### 3.2 Build pipeline (v3-team operated)

```
Input:  flake ref to cache
        + list of "popular" attribute paths to materialize
Process:
  1. nix flake archive <ref> to local store
  2. v3 evaluates the listed attributes WITH:
     - NIX_V3_DISK_CACHE=1 (CU cache; default-on already)
     - NIX_V3_IFD_IMPORT_CACHE_DISK=1 (Phase 4b; default-on already)
     - NIX_V3_AOT_BUILD_MODE=1 (NEW; tracks all serialized cache entries)
  3. Collect resulting cache files (~/.cache/nix/v3-bytecode-v1.sqlite + EvalResults)
  4. Compact to mmap'd flat file per EVAL_CACHE_ARCHITECTURE §4.3
  5. Sign with v3-team / IOG key
  6. Upload to distribution endpoint (S3 / IOG Hydra / GitHub releases)
Output: <flake_ref_hash>.aot-cache.mmap + signature + index.json metadata
```

### 3.3 Client-side fetch + mmap

```
NIX_V3_AOT_CACHE_URL=https://aot.cache.iohk.io/    (opt-in)

On v3 startup:
  1. Compute current flake ref hash
  2. HTTPS GET /index.json (small, <10KB) — cache index
  3. If entry exists for (flake_ref, schema_version):
     a. Check local cache directory (~/.cache/nix/v3-aot/)
     b. If not local: download .aot-cache.mmap (lazy / streaming if possible)
     c. Verify signature; verify schema_version match
     d. mmap() the file READ-ONLY
     e. Register mmap region with v3 GC as snapshot roots
     f. Continue eval — cache hits fire at parse + primop boundaries
  4. If miss: normal compile path (no behaviour change)
```

### 3.4 Composition with GC + cache stack

Per [`IDEAL_GC_DESIGN_2026-05-26.md`](IDEAL_GC_DESIGN_2026-05-26.md) §3.4: the mmap'd cache region is a GC-aware "shared read-only roots" region. Multiple processes mmap'ing the same file share physical pages via the OS page cache; no IPC, no daemon, no double-buffering.

**Update 2026-05-26 (evening): R1-trigger CLOSED.** The cache is now process-independent at the bytecode level. V3_DBG_DESERIALIZE_VERIFY DIFFs went 353 → 0 across three landings ending at `b17ab3359`. Bytecode is process-invariant across SymbolId / PosIdx / local-slot allocators. The trust model is structurally enforced **as of today, not "once R1 lands"** — R8a Phase 1 spike no longer needs R1-Full as a prereq.

R1-Full (the IR-level de Bruijn refactor) remains deferred; its motivation is now schema-iteration cost reduction + Stage 13 parallel-eval prereq, not AOT-distribution correctness.

### 3.5 Cache hierarchy (post-R8a)

```
Hot path lookup order:
  L1: in-process memory cache (T1.1 instrumentation surface)
  L2: process-local SQLite disk cache (#777, schema 14)
  L3: AOT-distributed mmap region (NEW; flake-ref keyed)
  L4: compile from source (fallback)

L3 entries are READ-ONLY shared (multi-process safe via OS page cache).
L2 + L1 are process-local mutable.
```

---

## 4. R8 split — R8a (v3-team) vs R8b (cache.nixos.org)

Replaces the original R8 in `NEXT_STEPS_2026-05-25.md` §6.5:

### R8a — v3-team-owned AOT cache (~4-6 weeks v3-team-only)

**Triggers (more permissive than R8b):**
- ~~v3 schema stable for 2+ weeks~~ — partially superseded post-R1-trigger closure (`b17ab3359`, 2026-05-26): bytecode is now process-invariant. Schema bumps for LambdaDescriptor field changes still invalidate cache, but cross-machine coherence is structurally enforced
- Phase 1 spike (§7 below) meets threshold ≥30 % warm-eval ratio improvement on haskell-nix-example
- IOG infrastructure (Hydra / S3) confirmed available for distribution
- v3-team bandwidth available (~1 person × 4-6 weeks)

**Effort:** 4-6 weeks; all v3-team-controlled.

**Coverage:** targeted IOG priorities (cardano-node, haskell.nix, plutus, cardano-base). Add flake refs incrementally.

**Trust model:** SHA-256 + v3-team/IOG signature. Standard schema_version check via kSchemaVersion. Client opt-in via env var.

**Composes with:** R1 (cleaner determinism), R7 (mmap'd L2 is distribution format), Phase 4b (already default-on; cache contents include EvalResults table).

### R8b — cache.nixos.org integration (~1-2 weeks v3 + indefinite cross-team)

**Triggers (stricter than R8a):**
- R8a shipped AND proved value at scale (≥X CI runs/week using the cache)
- Schema bump rate decreased to < 1 bump per month OR versioned-artifact distribution model accepted by Nix-team
- Nix-team coordination commitment confirmed
- Standard wall ratio competitive (≤ 1.5× TW on hello.drvPath)

**Effort:** 1-2 weeks v3 + indefinite cross-team.

**Coverage:** universal nixpkgs.

**Trust model:** standard Nix substituter (existing infrastructure).

**Composes with:** R8a (proven value; substituter model is upgrade not new build).

---

## 5. R7 (mmap'd L2 cache) — revived as AOT distribution format

R7 was previously deprioritised after Phase 4b shipped wall-positive on SQLite-backed L2 at primop scope (per `EVAL_CACHE_ARCHITECTURE_2026-05-23.md` §13). The deprioritisation was correct for the *primop-scope wall* question.

**But for AOT distribution, mmap is structurally required:**
- Cross-process page-cache sharing (OS handles deduplication across N processes)
- Demand-paged (download/load only what's touched)
- Snapshot semantics native (read-only)
- No SQLite transactional overhead at distribution scale
- Compatible with multi-process simultaneous reads
- Trust model simpler (hash file contents; no DB schema)

R7's design from `EVAL_CACHE_ARCHITECTURE_2026-05-23.md` §4.3 + §7.1 (the 3-5 day spike) is exactly the format needed for R8a. **R7 is no longer a standalone Tier R item; it's a dependency of R8a.**

Updated framing:

- **R7 status:** REVIVED 2026-05-26 as R8a dependency
- **R7 trigger:** R8a Phase 1 spike commits to mmap'd L2 as distribution format (essentially: any time R8a starts)
- **R7 effort:** 3-5 days (already estimated in EVAL_CACHE §7.1) + 1-2 weeks integration with R8a build pipeline

---

## 6. Why mmap-vs-SQLite for distribution (vs primop-scope)

The earlier `EVAL_CACHE_ARCHITECTURE_2026-05-23.md` §13 retraction was specifically about whether the SQLite-backed L2 was wall-positive at the *primop-call boundary*. Phase 4b validated that it IS wall-positive when correctly scoped (1.85-1.91× faster on IFD-heavy workloads).

That finding does NOT contradict choosing mmap for AOT distribution. The two are different design questions:

| Question | Right answer | Why |
|---|---|---|
| **Primop-call-scope L2 cache** | SQLite-backed (Phase 4b shipped) | Per-process; mutable; ACID transactions worth their cost |
| **AOT distribution format** | **mmap'd flat file** | Cross-process shared; immutable; OS page cache deduplicates |

Both can coexist. The hierarchy in §3.5 above shows them at different layers (L2 = SQLite per-process; L3 = mmap'd AOT distributed).

---

## 7. Phase 1 measurement gate (per measure-twice-cut-once)

Before committing to the broader R8a investment (4-6 weeks), a Phase 1 spike validates the value proposition. **Pre-committed thresholds** per `MEASURE_TWICE_CUT_ONCE_2026-05-23.md`:

### 7.1 Phase 1 spike scope (~2-3 weeks)

**Goals:**
- Build AOT cache for ONE strategic flake (recommend: `haskell-nix-example` — already a deep v3 test workload)
- Distribute via simple HTTPS endpoint (S3 / GitHub releases — no fancy CDN)
- Measure end-to-end client warm-eval improvement
- Surface the cache-build cost (cycle time per nixpkgs revision)

**Concrete steps:**
1. Day 1-3: implement `NIX_V3_AOT_BUILD_MODE=1` flag in v3 (records all serialized cache entries)
2. Day 4-6: build mmap'd flat file from CU disk cache + EvalResults (per EVAL_CACHE §7.1)
3. Day 7-9: HTTPS distribution endpoint + signing + client fetch
4. Day 10-12: client mmap + integration; cache-miss fallback
5. Day 13-15: measurement on haskell-nix-example end-to-end

### 7.2 Pre-committed thresholds (Rule 0 falsifier)

- **SHIP if:** warm-eval ratio on haskell-nix-example improves from 1.42× TW to **≤ 1.10× TW** (≥ 30 % improvement). Commit to Phase 2 (generalize to N flake refs).
- **REVERT WITH DATA if:** warm-eval ratio improves < 15 %. Document the limited value; reassess whether AOT is the right strategic move.
- **TUNE if:** improvement is 15-30 %. Investigate cache-miss patterns; tune cache contents; re-measure before Phase 2 commit.

### 7.3 Expected outcome

Conservative estimate: warm-eval improvement of 30-60 % on haskell-nix-example. Reasoning:
- haskell-nix-example warm eval is 7155 ms (vs TW 5026 ms = 1.42×)
- Parse + lower + emit residue: ~200-500 ms (estimable from #777/#781b)
- IFD result residue: per Phase 4b multi-IFD test, ~700 ms savings on synthetic
- AOT eliminates parse residue + pre-populates Phase 4b EvalResults

If parse residue is ~300 ms + IFD residue is ~700 ms, total recoverable is ~1 second = 14 % wall reduction on haskell-nix-example. **Borderline meets the 15 % "tune" threshold.**

If cache also captures Bindings interning + cross-process Closure/Thunk reuse, additional 200-500 ms possible. **Then 20-30 % wall reduction; meets ship threshold.**

The 2-3 week spike settles which regime applies.

---

## 8. Risks and mitigations

| Risk | Mitigation |
|---|---|
| Schema churn at v3 dev pace invalidates cache frequently | Cache only RELEASE branches; tie cache version to v3 binary; nightly rebuild on releases only |
| Build cost — building AOT for N flake refs × M packages = expensive | Incremental builds via mmap delta-encoding; piggyback on existing IOG Hydra; cache only stable pins not every commit |
| Distribution bandwidth — 500 MB-1 GB cache files × N users | CDN distribution; deduplicate shared CUs across flake refs (Bindings interning at distribution time); streaming download + lazy mmap |
| Cache cold-load latency — downloading 1 GB before first eval defeats purpose | Lazy mmap via HTTP range requests; progressive download as pages touched; small manifest fetched eagerly with byte offsets |
| Cache miss handling — user requests unbuilt flake ref | Fall back to normal eval gracefully; this is already how v3 cache-miss works |
| Trust model — users need to verify cache integrity | SHA-256 signed by v3-team/IOG key; existing Nix substituter trust infrastructure could be reused (cache file IS a Nix store path) |
| Versioning — users want to pin to specific cache versions | Cache index includes versioning; client supports `NIX_V3_AOT_CACHE_VERSION=...` for pinning |
| What if Phase 1 measurement shows < 15 % improvement | Pre-committed revert with data; document; pivot resources to other big items per "next big items" analysis |

---

## 9. Strategic alignment with IOG context

For an Input Output Group / IOHK context, this proposal aligns naturally with strategic priorities:

- **cardano-node** is the flagship workload. AOT-cached cardano-node evaluations are *exactly* what CI needs.
- **haskell.nix** is central to the cardano workflow. Already deeply tested in v3 (haskell-nix-example).
- **CI patterns** at IOG involve many evaluations against same nixpkgs revisions. AOT cache captures this asymmetric advantage.
- **IOG Hydra infrastructure** already exists; can serve as distribution endpoint. Marginal infrastructure cost: ~zero.
- **v3 is IOG-funded**; v3-team-owned AOT cache aligns v3's competitive advantages with IOG's product needs.

**Strategic differentiator:** when v3 + AOT cache + IOG infrastructure is deployed in IOG CI, **cardano-node CI evaluations could be faster than ANY currently-deployed Nix evaluator can achieve on the same workload.** This is a publishable competitive position.

---

## 10. What this changes in NEXT_STEPS_2026-05-25.md

### Tier C — strategic infrastructure (updated)

**Original C1:** `AOT distribution spec` (1-2 weeks v3 + cross-team Nix coordination)

**Updated:**
- **C1a (NEW):** v3-team-owned AOT cache (R8a operationalisation) — 4-6 weeks v3-only; targeted IOG priorities; opt-in via env var. Phase 1 spike (2-3 wk) is the measure-twice gate.
- **C1b (formerly C1):** cache.nixos.org integration — gated on C1a proven value + schema stability + Nix-team coordination.

### Tier R — trigger-gated refactors (updated)

**Original R7 (mmap'd L2):** "Priority dropped post-Phase 4b validation."

**Updated:**
- **R7 REVIVED 2026-05-26 as R8a dependency.** mmap'd flat file is the AOT distribution format. Trigger: R8a Phase 1 spike commits to mmap as format. Effort: 3-5 days spike + 1-2 weeks integration with R8a build pipeline.

**Original R8 (AOT distribution):** Single item with universal scope.

**Updated:**
- **R8a (NEW):** v3-team-owned AOT cache. Permissive triggers; 4-6 weeks v3-only effort.
- **R8b (formerly R8):** cache.nixos.org integration. Stricter triggers; gated on R8a proven value.

### Tier A — addition

If the team commits to AOT, **add Tier A item A6:**
- **A6:** Phase 1 spike for R8a (build AOT cache for haskell-nix-example; measure warm-eval). Effort: 2-3 weeks. Pre-committed threshold: ≥30 % warm-eval improvement to ship Phase 2.

### §8.5 AR list — update

**AR8 (schema versioning ratchet vs AOT distribution):** updated framing — R8a is more tolerant of schema churn than R8b (v3-team can rebuild caches on schema bump). AR8 remains a concern for R8b but not for R8a until release-branch cadence settles.

### §12 Operating rules — add

- **"AOT cache distribution model is v3-team-owned by default."** Universal cache.nixos.org integration is a later upgrade path, not the initial commitment. Reason: schema iteration freedom + targeted IOG value + faster time-to-ship.

---

## 11. Honest limits

- **Phase 1 expected outcome (§7.3)** is estimated, not measured. Could be 14 % (below tune threshold) or 50 % (well above ship threshold). The 2-3 week spike settles this.
- **"4-6 weeks v3-team-only"** assumes one engineer dedicated; could be 8-10 weeks if part-time. IOG infrastructure availability is also assumed.
- **"Bandwidth ≤ zero marginal cost on IOG Hydra"** is an assumption; if IOG Hydra is at capacity, this cost is real.
- **"Strategic differentiator" framing in §9** is author position; IOG product team should validate.
- **"R7 was deprioritised correctly for primop scope but should be revived for AOT"** is reframing; both can be true simultaneously since they address different questions.
- **The trust model in §3.3 (SHA-256 + v3-team signature)** is sketched; real implementation needs key management + revocation story.
- **"Lazy mmap via HTTP range requests" mitigation** is technically achievable (NFS/SAS/CDN support range requests) but adds complexity; first cut should be "full download cached locally" with range requests as Phase 3.
- **Composition with cardano-node M5 ritual measurement** is not explicit here. The two should coordinate — cardano-node M5 AOT cache could be in Phase 2.
- **The "cardano-node could be faster than any Nix evaluator" claim in §9** is conditional on cache hit rate at IOG CI scale. Unmeasured. Worth validating once R8a Phase 1 lands.
- **"IOG-funded v3-team" framing in §9** is contextual; the strategic alignment claim holds independent of funding source, but the prioritisation argument is sharper given the IOG context.

---

## 12. Cross-references

- [`WARM_EVAL_AND_INSTRUMENTATION_2026-05-23.md`](WARM_EVAL_AND_INSTRUMENTATION_2026-05-23.md) §6.4 — original AOT proposal (universal model); this doc refines
- [`EVAL_CACHE_ARCHITECTURE_2026-05-23.md`](EVAL_CACHE_ARCHITECTURE_2026-05-23.md) §4.3 + §7 — mmap'd L2 design (now R8a distribution format); §13.3(d) retraction context
- [`NEXT_STEPS_2026-05-25.md`](NEXT_STEPS_2026-05-25.md) §5 (Tier C C1) + §6.5 (Tier R R7, R8) + §8.5 (AR8) — updates per §10
- [`IDEAL_GC_DESIGN_2026-05-26.md`](IDEAL_GC_DESIGN_2026-05-26.md) §3.4 — cross-process snapshot semantics (R8a composes with this)
- [`ARCHITECTURE_CRITIQUE_2026-05-26.md`](ARCHITECTURE_CRITIQUE_2026-05-26.md) §8.5 AR8 — schema-stability vs AOT tension (R8a addresses; R8b inherits)
- [`MEASURE_TWICE_CUT_ONCE_2026-05-23.md`](MEASURE_TWICE_CUT_ONCE_2026-05-23.md) §3.8 — Phase 1 spike pre-committed threshold methodology
- [`CARDANO_NODE_FEASIBILITY_2026-05-18.md`](CARDANO_NODE_FEASIBILITY_2026-05-18.md) — strategic workload definition; cardano-node-minimal candidate for Phase 2
- [`MEMORY_REDUCTION_AVENUES_2026-05-26.md`](MEMORY_REDUCTION_AVENUES_2026-05-26.md) — independent of R8a but relevant: AOT cache adds memory cost (mmap'd region) but releases compile-time memory
- [`ROADMAP_PROGRESS_SNAPSHOT_2026-05-26.md`](ROADMAP_PROGRESS_SNAPSHOT_2026-05-26.md) §9 — recommended near-term ordering (AOT slot)

**Commits referenced:**
- `#741 Phase 4b series` (default-on; EvalResults table becomes part of AOT cache contents)
- `#777` disk cache default-on (CU cache becomes part of AOT cache contents)
- `#781b` sparse symbolTable (reduces cache size for distribution)
- `ed8fa0669` Schema 13 (current schema baseline for distribution)
- `a7b41ddce` Schema 14 (R1-adjacent; reduces future schema churn)

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
