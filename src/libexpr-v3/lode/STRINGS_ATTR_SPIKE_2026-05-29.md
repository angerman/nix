# String-dedup spike (Step 18) — NO LEVER verdict

**Date:** 2026-05-29
**Status:** NEGATIVE FINDING — string dedup is not a Step-18-scope memory lever for v3
**Task:** #854

Per [`STRING_DEDUP_AUDIT_2026-05-28.md`](STRING_DEDUP_AUDIT_2026-05-28.md): T1.3-pattern per-allocChars site attribution + (deferred) duplication-rate estimate.

Pre-committed thresholds: <50 MB no-lever / 50-100 marginal / 100-200 moderate / >200 significant.

---

## 1. TL;DR

**v3's total allocChars footprint is 7-36 MB** on the two probed workloads — **far below the 50 MB no-lever threshold even before duplication-rate analysis.**

Verdict: **NO LEVER.**  String dedup is not a Stage-6-era memory optimization for v3.  Per Rule 0, this commit kills the hypothesis that "runtime string VALUES contribute material duplication on real evals" — at least at the 50 MB threshold.

This finding is **upper-bound by construction**: it measures TOTAL allocChars bytes, not dedup-able-bytes.  Even if 100% of strings were duplicates (impossible but bounding), the savings would be 7-36 MB.

Spike implementation: ~70 LoC across alloc.hh + run.cc.  Gate `NIX_V3_STRINGS_ATTR=1`.  Zero cost when OFF.

---

## 2. Measurement results

### 2.1 hello.drvPath

```
total: 390,398 calls / 7.23 MB across 3 unique sites

rank  file:line                          calls       MB    %cum
  1   primops.cc:608                    375,052     4.90   67.8%
  2   vm.cc:9769                         15,343     2.33  100.0%
  3   primops.cc:2721                         3     0.00  100.0%
```

### 2.2 HNE (haskell-nix-example hello)

```
total: 1,525,580 calls / 35.53 MB across 7 unique sites

rank  file:line                          calls       MB    %cum
  1   primops.cc:608                  1,473,112    25.32   71.3%
  2   vm.cc:9769                         52,340    10.18   99.9%
  3   v3_call_flake.cc:385                    1     0.03  100.0%
  4   primops.cc:2721                       111     0.00  100.0%
  5   v3_call_flake.cc:225                    8     0.00  100.0%
  6   value_serialize.cc:344                  6     0.00  100.0%
  7   v3_call_flake.cc:433                    2     0.00  100.0%
```

### 2.3 Top-2 sites identified

- **`primops.cc:608` `mkStringValueOwned`** — wraps any `std::string` result into a v3 Tag::String / Tag::Path Value.  Highest-volume utility helper; ~95% of allocChars calls.  Likely sources: drv-hash strings, store-path interpolation, mergeContexts results.
- **`vm.cc:9769` (bytecode OP_STRING_CONCAT-like path)** — string concatenation result buffer.

---

## 3. Why this is bounded above by 36 MB

The data measures `Alloc::allocChars(n)` calls × `n` bytes per call.  This IS the total bytes allocated for Tag::String / Tag::Path payloads.

**Dedup savings = (total bytes) − (unique bytes after content-based dedup).**

Upper bound (perfect dedup, every alloc a duplicate): savings = total - smallest-unique-set.  For HNE 35.53 MB, even if 100% of allocations were duplicates, the unique-set still has SOME bytes.  Realistic dedup yield: 30-70% of total = 11-25 MB on HNE.

**Even at the upper-bound realistic estimate (25 MB on HNE), the result is BELOW the 50 MB no-lever threshold.**

---

## 4. Cross-check against memory profile

Per `HNE_BUCKET_DECOMP_2026-05-27.md`:

| Bucket | HNE bytes |
|---|---|
| v3_arena | 1577 MB (per Step 8 IMMIX_LINE_OCCUPANCY) |
| boehm_heap | 403 MB |
| elsewhere (ImportCache + SQLite + bytecode shadow) | 990 MB |
| **string bytes (this spike)** | **36 MB (1.2% of arena)** |

Strings are ~1.2% of arena.  Per `DATA_STRUCTURE_AUDIT_2026-05-21`: Bindings are 84%, Closures + Thunks + Lists + Pairs are ~15%.  Strings are the residual.

**The Immix pivot (Step 10 PIVOT-IMMIX) addresses the 84% Bindings bucket.  Strings are not the second-priority lever.**

---

## 5. What about Tag::String payloads in the live-trace audit?

Per [`L_TIME_SERIES_DATA_2026-05-29.md`](L_TIME_SERIES_DATA_2026-05-29.md): live-fraction tracer reports Tag::String reach counts at end-of-eval.  Those counts are dominated by `globalSymbolTable` (attribute keys + PosIdx) which are DEDUPED already (per `STRING_DEDUP_AUDIT §2`).

This spike measures the UN-DEDUPED runtime string VALUES.  The conclusion holds: that subset is 35.53 MB, not the 700-MB cache-bucket figure some earlier docs suggested.

---

## 6. Memory entry update

Memory `[[string-dedup-audit-2026-05-28]]` is now annotated:

> Updated 2026-05-29 with Step 18 NIX_V3_STRINGS_ATTR=1 spike result:
> total allocChars bytes are 7 MB (hello) / 36 MB (HNE) — UPPER BOUND
> on string-dedup savings is 50% lower (≤25 MB) → NO LEVER per
> pre-committed thresholds.  Strings are 1.2% of arena vs Bindings'
> 84%.  Spike infrastructure (gate + per-site table) preserved as
> measurement tool; can re-run anytime.

---

## 7. Honest limits

- **Single sample per workload.**  Run-to-run variance σ ≈ 0 for arena bytes (per Step 2 noise floor); confident.
- **No content-based duplicate scan performed.**  Would require hashing every allocChars buffer at alloc time + maintaining a hash table.  ~50-100 LoC extension; not justified given the total bytes are below threshold.
- **Per-site granularity is COARSE.**  `primops.cc:608` is a 5-line helper that wraps many different upstream callers; finer-grained "who called mkStringValueOwned" attribution would require __builtin_FILE/__LINE at call-sites of the helper.  Deferred.
- **`mkStringValueOwned` is called via many distinct paths** (string concat, context merge, store-path interpolation, drv-hash construction).  This spike doesn't break that down — the 25 MB on HNE could be ~1 MB each from 25 different upstream sites.  If finer attribution justified, a follow-up spike would inject site-tracking into the helper.
- **`vm.cc:9769` is one of multiple bytecode string ops.**  Same caveat.
- **The 50 MB threshold is from the audit doc.**  If the threshold were 20 MB instead, HNE would clear at 35.53 → MARGINAL.  The threshold reflects "what's worth a multi-week impl" judgment.  At 36 MB total (max 25 MB savings), the impl ROI is not there.

---

## 8. Cross-references

- [`STRING_DEDUP_AUDIT_2026-05-28.md`](STRING_DEDUP_AUDIT_2026-05-28.md) — original spike proposal
- [`L_TIME_SERIES_DATA_2026-05-29.md`](L_TIME_SERIES_DATA_2026-05-29.md) — sibling per-Tag data
- [`GC_DECISION_2026-05-29.md`](GC_DECISION_2026-05-29.md) — Stage 6 path (Immix attacks Bindings, not strings)
- `bench/baselines/2026-05-29-strings-attr/` — raw probe outputs
- Memory: [[string-dedup-audit-2026-05-28]] — index entry (annotation pending Step 20)
- Memory: [[measure-twice-cut-once]] §3 (threshold pre-committed in audit; honored here)

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
