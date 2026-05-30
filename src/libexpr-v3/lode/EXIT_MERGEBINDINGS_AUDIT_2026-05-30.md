# EXIT mergeBindings audit — what's left after 2-pass + Tag::App3

**Date:** 2026-05-30
**Per:** user directive "(continue) measure, then mergeBindings fix"
**Status:** measurement complete; the "several hundred MB" original estimate is **PRE-2-pass**.  Current state already captures the slack-elimination win.  Remaining mergeBindings lever requires Chain Phase C (3-attempt-falsified; multi-day prereq audit).

---

## 1. The hypothesis being killed

> "mergeBindings can be further optimized to recover the 'several hundred MB on HNE' estimate from MEMORY_REDUCTION_AVENUES, beyond what the 2-pass + short-circuits already deliver."

QUALIFIED KILL — the headline number is already captured.

The "several hundred MB" originated from `MEMORY_REDUCTION_AVENUES_2026-05-26.md` Category 1, citing the pre-#747 single-pass allocator's worst-case slack at na+nb upfront vs k filled.  #747 landed two-pass + #748 landed zero-operand short-circuit + #752 inline PosIdx.  Today's measurement (this commit) confirms the slack has dropped to literally 0 bytes.

---

## 2. Current measurement (HNE end-of-eval)

* **mergeBindings calls:** 161,130 total across 6 sites
* **mergeBindings bytes:** 558.8 MB (77.8 % of all Bindings allocations)
* **Bindings total:** 718.1 MB across 22 origins, 1.1 M allocs, **slack = 0.0 MB** ← #747's two-pass goal achieved
* **Dominant site:** `OP_ATTRS_UPDATE_TAIL` (the `//` operator) — 112,072 calls (69.6 %), 549.1 MB (98.3 % of merge bytes)

### OP_ATTRS_UPDATE_TAIL histograms

**na (LHS size):**
| Bucket | Count | % |
|---|---|---|
| 0 | 6,407 | 5.7 % (short-circuits to b) |
| 1 | 3,865 | 3.5 % |
| 2-4 | 32,147 | 28.7 % |
| 5-8 | 20,754 | 18.5 % |
| 9-16 | 11,126 | 9.9 % |
| 17-32 | 8,499 | 7.6 % |
| 33-64 | 25,787 | 23.0 % |
| 65-128 | 2,223 | 2.0 % |
| 129-256 | 225 | 0.2 % |
| 257+ | 1,039 | 0.9 % |

**nb (RHS size):**
| Bucket | Count | % |
|---|---|---|
| 0 | 15,114 | 13.5 % (short-circuits to a) |
| 1 | **40,130** | **35.8 %** ← single-key patch, the open lever |
| 2-4 | 23,720 | 21.2 % |
| 5-8 | 17,842 | 15.9 % |
| 9-16 | 7,114 | 6.3 % |
| 17-32 | 6,460 | 5.8 % |
| 33-64 | 1,010 | 0.9 % |
| 65-128 | 219 | 0.2 % |
| 129-256 | 130 | 0.1 % |
| 257+ | 333 | 0.3 % |

40 k of 112 k calls (35.8 %) are nb=1 — single-key patches.  These are the canonical "single-attr override" pattern (e.g. `pkgs // { hello = newHello; }`).

---

## 3. Bytes-yield estimate for the nb=1 lever

If nb=1 calls used a "patched overlay" (share base + 1 overlay entry) instead of full copy:

* Each nb=1 call's bytes ≈ na × 24 B (per Entry).
* Joint distribution unknown; estimating average na for nb=1 calls ≈ 8 (most na buckets are 2-32):
  * 40,130 × 8 × 24 B = ~7.5 MB
* Worst-case (assume nb=1 correlates with large na=33-64):
  * 40,130 × 48 × 24 B = ~46 MB

Per [[measure-twice-cut-once]]: I don't have joint (na,nb) data; the estimate is 7-46 MB.  That's substantial but smaller than the original "several hundred MB" claim.

To get joint na×nb data would require an additional dump pass; deferred.

---

## 4. Chain Phase C status

Per the inline comment at `vm.cc:1172-1210` (commit 4eafe2cf2 falsification record):

> Phase C — FALSIFIED across three attempts this session (per measure-twice-cut-once §3.8 "three failed pivots = falsification").

Falsification ledger:
- **v1** (`6f8095cd5`): missed Phase D barrier in `materialize()`; reverted.
- **v2** (`2cf14fdce`): fixed barrier + consumer-site fallbacks.  Lang + core PASS; nixpkgs hello.name FAILED with `attribute 'buildPythonApplication' missing` on a 2-entry Bindings.
- **v3** (this session, reverted): added serializeAttrs + valuesEqual chain-materialise to fix suspected Phase 5 cache corruption.  Brute audit clean; nixpkgs hello.name still failed.

Phase C revival prerequisites:
1. Build a Nix-level minimal repro that triggers the `{} // overlay` collapse under chain spike.
2. Identify which value-flow within nixpkgs `lib.makeOverridable` / `callPackageWith` / `python3.pkgs` machinery silently returns `{}` under chain interaction.
3. Audit the 208 `entries[]` sites across the v3 tree.
4. Either convert all iteration sites to `forEach` / `materialize()` OR keep chain construction gated on a whitelist of confirmed-safe call patterns.

Estimated effort: multi-session.  Not ship-able within this session's scope.

---

## 5. Alternative levers worth considering

### 5.1 nb=1 patched-overlay (gated)

Implement a NEW lazy-overlay representation for the nb=1 case ONLY.  Gate it behind a whitelist of CALL SITES known not to trigger the Phase C iteration-vs-materialize bug.

Risks:
* If iteration sites that hit nb=1 patched overlays haven't been audited, same Phase C class of bug fires
* The 208-entry audit is the same prereq

Yield: 7-46 MB on HNE (per joint-distribution range above)

### 5.2 mergeBindings call deduplication

Hashmap memo on `(a_ptr, b_ptr) → result`.  If the same exact pair is merged twice, return cached result.

Risks:
* Memo hashmap costs ~5-10 MB (161k × 32 B per entry)
* Hit rate likely LOW (merge inputs typically distinct per call)
* No data on actual hit rate without instrumentation

Likely yield: 0-5 MB.  Probably below cost.  Falsification-likely.

### 5.3 Smarter allocation pool

`Alloc::allocBindings` always allocates from the arena (mmap-backed, no release).  A separate Bindings-specific small-object pool with bucketed sizes COULD reduce fragmentation.  But the arena is bump-allocated, not fragmented — no fragmentation to recover.

Yield: 0 MB.  Falsified by design.

---

## 6. Honest call

The session's Tag::App3 win (`3d64028e8`: -28.9 MB HNE) IS the realistic mergeBindings-class achievement for this scope.  The deeper Chain Phase C work is multi-session and prerequisite-blocked.

Recommendation:
- Stop the mergeBindings track here.  Document the audit (this doc).
- The session's memory contribution is Tag::App3 alone (-28.9 MB HNE, projected -50-100 MB M5).
- Closing M5's 247 MB gap requires either Chain Phase C (multi-session) or other levers that haven't been quantified at this scale.

Per [[measure-twice-cut-once]]: the "several hundred MB" estimate was a pre-2-pass artifact.  Two-pass shipped; the slack is gone.  Further mergeBindings work is in the prerequisite-gated bucket.

---

## 7. Cross-references

* `EXIT_DAY4_TAG_APP3_LANDED_2026-05-30.md` — the session's memory win
* `vm.cc:1070-1274` — mergeBindings implementation (2-pass, short-circuits, falsification ledger)
* `MEMORY_REDUCTION_AVENUES_2026-05-26.md` — original "several hundred MB" claim (pre-2-pass)
* `[[falsification-rule]]` — Rule 0
* `[[measure-twice-cut-once]]` — pre-committed methodology

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.  SPDX-License-Identifier: Apache-2.0.*
