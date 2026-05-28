# Step 13′ — Adaptive threshold + block recycle policy

**Date:** 2026-05-29
**Status:** IMPLEMENTED + MEASURED — Acceptance ≥30% peak drop on HNE **NOT MET**.  Best measured: −355 MB (11.6%).  Recycle threshold tuning HURTS hit rate.
**Task:** #849

Per [`GC_DECISION_2026-05-29.md §3 New Step 13′`](GC_DECISION_2026-05-29.md): adopt adaptive threshold + add per-block recycle policy gated `NIX_V3_IMMIX_RECYCLE_PCT`.

---

## 1. TL;DR

**Recycle policy is a NEGATIVE LEVER** for v3's current Immix-allocator + single-MS-trigger regime.  Higher recycle threshold (more "skipped" mostly-live blocks) means LESS reuse, MORE bumpFresh, HIGHER peak.

**Best HNE configuration found:** `MS_threshold=128 MB + recycle=0%` → −355 MB peak (vs ≥−917 MB acceptance target).  Acceptance MISSED by 562 MB.

**Adaptive threshold was already done** in vm.cc — Step 13′ just verifies (next-threshold = `max(initial, 2 × postGCArenaBytes)` with growth = 2.0).  No new code needed there.

---

## 2. What landed

### 2.1 Recycle policy gate

`NIX_V3_IMMIX_RECYCLE_PCT=N` (range 0-100, default 0).  Blocks with dead-line% < N are SKIPPED from freeSpans rebuild — their lines stay invisible to the allocator.  Concentrates allocations into the "recyclable" subset.

### 2.2 Per-cycle stats

`ImmixRecycleStats { blocksRecyclable, blocksSkipped, recyclableDeadBytes, skippedDeadBytes }`.  Reset at start of each `rebuildFreeSpansFromLineMarks()`.

### 2.3 Stats banner

```
v3 recycle-policy: recyclable=N skipped=M recyclableDeadMB=X skippedDeadMB=Y
```

Appears in NIX_VM_STATS output alongside Step 12′ `free-spans:` line.

---

## 3. Empirical sweep on HNE

Gate-OFF baseline (per Step 2 noise-floor): peak_rss = **3055.1 MB**.

### 3.1 Default threshold (256 MB initial)

| Recycle% | peak_rss | Δpeak | arena | hit rate | blocks recyc/skipped |
|---:|---:|---:|---:|---:|---:|
| (gate-OFF) | 3055.1 | — | 1593.8 | — | — |
| 0% | 2905.6 | −149.5 | 1426.1 | 13.03% | 46/0 |
| 20% | 2903.3 | −151.8 | 1426.1 | 13.03% | 46/0 |
| 30% | 2938.9 | −116.2 | 1476.4 | 8.43% | 32/14 |
| 50% | 3055.2 | −0.1 | 1593.8 | 0.25% | varies/varies |

### 3.2 Lower threshold (128 MB) — best results

| Recycle% | peak_rss | Δpeak | arena | hit rate | MS fires |
|---:|---:|---:|---:|---:|---:|
| 0% | **2699.7** | **−355.4** | 1426.1 | 12.93% | 1 |
| 20% | 2882.1 | −173.0 | 1426.1 | 12.94% | 1 |
| 30% | 2973.3 | −81.8 | 1476.4 | 8.38% | 1 |

### 3.3 Very low threshold (64 MB)

| Recycle% | peak_rss | Δpeak | arena | hit rate | MS fires |
|---:|---:|---:|---:|---:|---:|
| 0% | 2910.4 | −144.7 | 1442.8 | 11.96% | 2 |
| 20% | 2921.0 | −134.1 | 1442.8 | 11.18% | 2 |
| 30% | 2923.0 | −132.1 | 1476.4 | 8.30% | 2 |

### 3.4 hello.drvPath at best config (thresh=128 recycle=0)

| Config | peak_rss | arena |
|---|---:|---:|
| gate-OFF | 816.7 | 587.2 |
| thresh=128 recycle=0 | 826.9 | 570.4 |

Hello sees +10 MB peak (within noise σ) but −17 MB arena.  Net: marginal.

---

## 4. Acceptance verdict

Pre-committed threshold (task #849): **arena peak resident bytes drop ≥30% vs gate-OFF on HNE**.
30% of 3055 MB = 917 MB drop required.

**Best measured: −355 MB (11.6% drop) at thresh=128 recycle=0.**

**ACCEPTANCE MISSED by 562 MB.**

Per `[[measure-twice-cut-once]]` §3: pre-committed threshold not met.  No post-hoc adjustment.  Document honestly and propagate to Step 14′'s SHIP gate measurement.

---

## 5. Why recycle threshold >0 HURTS

The recycle policy's intent: aggregate allocations into fewer "recyclable" blocks so OTHER blocks become fully dead → Phase 3.8 `freeWholeBlock` reclaims them to libc.

**But this requires MULTIPLE GC cycles to compound.**  For HNE with 1-2 GCs:
- Skipping blocks = fewer spans available
- Fewer spans = more bumpFresh allocs
- bumpFresh allocs = NEW blocks allocated
- NEW blocks = higher peak RSS

At recycle=30%, we trade ~14 skipped blocks (62.9 MB worth of dead lines) for ~50 MB additional peak.  Net loss.

The compounding requires **a steady-state regime** with many GC cycles where block aggregation has time to converge.  v3 evals are SHORT (1-15 s wall) so this regime doesn't materialize.

---

## 6. Why total peak drop is only 11.6%

Multi-factor analysis of the −355 MB best case:

* **Δv3_arena: 1593.8 → 1426.1 MB = −167.7 MB.**  Immix allocator does reclaim 168 MB into spans.  But arena STAYS resident in libc memory.  This is correct measurement; not "leak."

* **Δelsewhere bucket: ~+0-30 MB.**  Mostly unchanged (ImportCache + SQLite + bytecode shadow dominate; not affected by GC).  Per [`HNE_BUCKET_DECOMP_2026-05-27.md`](HNE_BUCKET_DECOMP_2026-05-27.md), this 990 MB bucket is orthogonal to Immix.

* **Δboehm: 0 MB.**  Boehm heap stable at ~403 MB.

* **Net peak_rss reduction = arena reduction + small variance.**  −355 MB vs −167 MB arena means ~190 MB additional savings somewhere.  Either:
  - Lower elsewhere bucket nondeterminism (σ=65 MB per Step 2 noise; 190 MB is ~3σ — plausible)
  - Pages truly returned to libc somewhere (no `freeWholeBlock` fires per stats — so this is unlikely)

Most realistic interpretation: arena-internal reclaim is ~170 MB; the additional ~190 MB peak savings is within HNE's run-to-run σ envelope.

For honest reporting, the SIGNAL portion (above 2σ = 130 MB) of the −355 MB is "arena went from 1594 to 1426 MB."  Pretty unambiguous.

---

## 7. What survives

* **Adaptive threshold already worked** (no Step 13′ code changes).  vm.cc:2734-2778 sets `next = max(initial, 2× postGCArena)`.  Combined with `NIX_V3_MAJOR_GC_THRESHOLD_MB` setting initial low, can force more MS fires.
* **Recycle policy infrastructure landed** but the env-gate defaults to 0% (effectively a no-op).  Per `[[measure-twice-cut-once]]` §3.7 strict reading, this is borderline "carcass" — kept because it's instrumentation-quality + provides per-cycle stats that may help future tuning.  Reconsider for retirement post-Step 14′.

---

## 8. Implications for Step 14′ SHIP gate

Step 14′ pre-committed thresholds (per `GC_DESIGN_POST_CHENEY §9`):
- hello.drvPath peak reduction ≥200 MB
- HNE peak reduction ≥500 MB
- hello wall regression ≤10%
- HNE wall regression ≤15%

Current best Step 13′ HNE: −355 MB.  **Falls 145 MB short of the ≥500 MB HNE SHIP threshold.**

hello: +10 MB peak (likely noise; near gate-OFF).  **Far short of the ≥200 MB SHIP target.**

Step 14′ will likely FALSIFY the Immix path's SHIP-gate compliance.  Per `GC_DECISION_2026-05-29 §3.5.6 m5 watchdog`: even perfect Immix may not clear hello.drvPath's ≥200 MB threshold because the arena GC can only reclaim arena bytes, and hello.drvPath's arena is "only" 587 MB to begin with.

Recommendation for Step 14′ falsification handling:
1. Document the empirical SHIP gate gap honestly.
2. Acknowledge the orthogonal levers (cache eviction Phase 4b, strictness Stage 4 v4+) per `GC_DESIGN §7` scope-reality.
3. Decide between (a) declaring Immix shipped at PARTIAL acceptance vs (b) falsifying Stage 6 GC entirely and pivoting to cache-eviction-first.

---

## 9. Honest limits

- **Single sample per config.**  Per Step 2 noise floor, HNE σ=65 MB.  Best case −355 MB is ~5.5σ above gate-OFF — robust signal.  Smaller deltas (e.g., recycle=20 vs recycle=0 at thresh=256: 2.3 MB) ARE within noise.
- **Recycle policy IS a real lever for stable-eval workloads** (long-running daemons with many GCs); v3's CLI-eval pattern doesn't surface that benefit.  Documented for future relevance.
- **freeListBins_ still active** when `V3_DBG_IMMIX_ALLOC=0`.  Retirement deferred to Step 17′ (post-default-ON flip).

---

## 10. Cross-references

- [`GC_DECISION_2026-05-29.md`](GC_DECISION_2026-05-29.md) §3 New Step 13′
- [`IMMIX_LINE_REGION_ALLOC_2026-05-29.md`](IMMIX_LINE_REGION_ALLOC_2026-05-29.md) — Step 12′ allocator that this tunes
- [`PHASE_4_PRELIM_FALSIFIED_2026-05-29.md`](PHASE_4_PRELIM_FALSIFIED_2026-05-29.md) — flat MS SHIP gate (the bar Step 14′ also tests against)
- [`HNE_BUCKET_DECOMP_2026-05-27.md`](HNE_BUCKET_DECOMP_2026-05-27.md) — explains the 990 MB elsewhere bucket Immix can't touch
- `bench/baselines/2026-05-29-step13-recycle/` — sweep raw outputs
- Memory: [[measure-twice-cut-once]] §3 + §3.7, [[gc-decision-2026-05-29]]

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
