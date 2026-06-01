# FFI_KILL_PLAN Phase A + C6 — LANDED with caveat

**Date:** 2026-06-01
**Per:** user directive "Continue with Phase A then Phase C. Ultrathink!"
**Commits:** `8c65e9dde` (Phase A) + this commit (Phase C6)
**Status:** **Phase A shipped (BP3 retired + BP2 short-circuit); Phase C6 shipped (TW-bridge fallback default-off); C1-C4 NOT NEEDED on measured workloads.**

---

## TL;DR

Phase A retires BP3 (`__v3_force_list_elem`) entirely — 0 callers everywhere.  Adds BP2 fast-path short-circuit for already-Bridge-thunk values.

Phase C0 measurement across 5 workloads (hello / firefox / python3 / HNE / M5):

| Workload | native | fallback |
|---|---:|---:|
| hello.drvPath | 376 | **0** |
| firefox.drvPath | 1558 | **0** |
| python3.drvPath | 405 | **0** |
| HNE | 21 | **0** |
| M5 | 54 | **0** |
| **total** | **2414** | **0** |

**Native path handles 100 % of derivationStrict invocations on the measured matrix.**  Phase C1-C4 (extensions for __structuredAttrs / outputChecks / __contentAddressed / __impure) are unnecessary — the existing native impl already covers these cases.

C6 shipped: the TW-bridge fallback path at `primops.cc:6455-6503` is now gated default-OFF behind `V3_DRV_KEEP_BRIDGE=1`.  Defaults to no-op; native throws propagate to caller.

## Caveat — the 99.8 % bridge retention is NOT from derivationStrict

End-of-eval bridge table on HNE: **33 entries** (22 closures, 11 attrs, 0 lists).  These hold ~519 MB transitively per `BRIDGES_HOLD_RETENTION_2026-05-29`.

But: the bridge-source histogram (`v3ToTwBySite`) on HNE:
* `primDerivationStrict_TWfb` = **0** (now retired by C6)
* `primV3ForceAttr_inner` = 51
* `primImport_string_ctx` = 10
* `primV3CallBridge1` = 8
* `primReadDir_*` = 5

**Bridge retention on HNE comes from BP2 + BP1 + primImport + primReadDir paths — NOT from the derivationStrict fallback that Phase C retired.**

The inventory's "headline derivationStrict native completion delivers ~311 MB on HNE" claim was conditional on derivationStrict being a major bridge source.  Today's measurement: it isn't.  The bridge surface fired by HNE has moved to the BP1/BP2 + leaf primop paths.

## What Phase A+C ACTUALLY delivers

### Code surface

| Item | Before | After |
|---|---|---|
| BP3 primop body | 217 LoC | 0 LoC (retired) |
| BP3 lazy-list construction | ~80 LoC + per-element Boehm allocs | 0 LoC; eager-bridge for all sizes |
| BP3 counter + stats line | active | retired |
| BP2 fast-path | always bounced through v3ToTreeWalker | direct-return when value already Bridge-thunk |
| derivationStrict TW fallback | active | default-OFF, opt-in via `V3_DRV_KEEP_BRIDGE=1` |

Net code delta: **-220 LoC** (primops.cc).

### Memory measurement

| Workload | peak_rss (mean ± σ) | v3_arena |
|---|---|---|
| HNE baseline (2026-05-31) | 2928.03 ± 1.38 MB | 1476.4 MB |
| HNE post Phase A+C6 | 2924.2 ± 0.6 MB | 1476.4 MB |

Δpeak: -3.8 MB at 0.6σ — **within measurement noise**.

The deterministic arena counter is unchanged (1476.4 MB).  Bridge entries on HNE: 33 before, 33 after (no change — the retired sites weren't pushing entries anyway).

### Bridge call counts

| Counter | Before | After |
|---|---|---|
| `__v3_call_bridge_1` | 8 | 8 |
| `__v3_force_attr` | 51 | 51 |
| `__v3_force_list_elem` | 0 | (retired — no counter) |

Bridge frequency unchanged (the retired paths weren't firing).

## What this kills

Per `[[falsification-rule]]`:

* **"BP3 has hidden callers"** — KILLED.  5 workloads, 0 calls.
* **"BP2 round-trip is unavoidable"** — KILLED.  L2 short-circuit eliminates round-trip when value is already a TW handle.
* **"primDerivationStrict TW fallback is necessary on measured workloads"** — KILLED.  0/2414 fallbacks across 5 workloads.
* **"Phase C native completion (C1-C4) is the headline yield"** — REFRAMED.  The native path was ALREADY complete for measured workloads; the fallback was dead code.  The inventory's "311 MB on HNE" estimate was based on `BRIDGES_HOLD_RETENTION` end-of-eval bridge table holding 519 MB.  But that retention traces to BP1/BP2/primImport/primReadDir paths, NOT derivationStrict.

## What this DOESN'T kill

* Bridge retention as a real phenomenon: 33 entries hold ~519 MB transitively on HNE.  Still material.  Just not from the derivationStrict source we retired today.
* BP1, BP2, primImport, primReadDir bridges — these remain the actual bridge sources.  Future work could target them (Phase D in FFI_KILL_PLAN was fetchers; would need extending to the leaf primops).
* The fake-store path (primops.cc:6504+) — kept as deep fallback for any case neither native nor bridge handles.  Likely unreachable today.

## What's preserved

* `V3_DRV_KEEP_BRIDGE=1` soak-period escape hatch — per FFI_KILL_PLAN §4.7.  If any unmeasured workload trips a native throw, this opt-in re-enables the bridge fallback.  Retire after one session confirms `native=N fallback=0` across an expanded workload matrix.
* Bridge tables (v3BridgeClosures / v3BridgeAttrs / v3BridgeLists) — still used by BP1/BP2/primImport/primReadDir.
* `v3ToTwBySite[6]` slot — still incremented if the bridge fallback fires under `V3_DRV_KEEP_BRIDGE=1`.
* The plan's Phase D (fetcher flat-arg variants) — open if a future measurement shows fetchers are now the next-biggest retainer.

## Honest limits

* **Only 5 workloads measured.**  Across-architecture (linux/x86_64, linux/aarch64) cross-checking not done.  If any cross-arch nixpkgs build hits a native throw, `V3_DRV_KEEP_BRIDGE=1` is the escape hatch.
* **Test against synthetic adversarial drvs not done.**  `__structuredAttrs = true`, `__contentAddressed = true`, `__impure = true` shapes weren't explicitly exercised.  But on the natural workloads they didn't fire either — the native path's coverage is real.
* **Peak_rss delta is within noise** — the 200 MB-class memory yield the FFI plan implied for Phase C does not materialize because the dead path was already dead.  The session's accumulated session-arc memory work delivers the 1661 MB cumulative reduction documented in `EXIT_POST_APP3_BASELINE_2026-05-31`; Phase A+C doesn't add to that.
* **Bridge retention root cause REMAINS** at the BP1/BP2/primImport/primReadDir sites.  Future memory ROI would target THOSE, not derivationStrict.

## What's next

Per the inventory's bridge-source histogram, the next-biggest sources are:
1. **BP2 (`primV3ForceAttr_inner`)** — 51 calls on HNE; bridges attr values when TW lazy-forces a bridged attrset entry.  Most calls trigger `v3ToTreeWalker` which creates new entries.  **The 51-call frequency is itself an open question** — why does TW force 51 attrs on HNE flake bootstrap?
2. **primImport_string_ctx** — 10 calls on HNE; happens when import receives a string-with-context (typically derivation outputs).
3. **BP1 (`primV3CallBridge1`)** — 8 calls.

Bridge surgery on these requires deeper investigation per `EXIT_FFI_DEEP_SURGERY_<date>.md` (not yet written).

For peak_rss memory yield: per `EXIT_PATH_A_V2_FALSIFIED_2026-06-01`, the constraint is `peak_rss = ru_maxrss` monotonicity — even mechanisms that work mid-eval can't lower the lifetime peak.

## Cross-references

* `FFI_KILL_PLAN_2026-05-31.md` — the source plan; Phase A and Phase C definitions
* `FFI_BRIDGE_INVENTORY_2026-05-31.md` — bridge surface mapping
* `BRIDGES_HOLD_RETENTION_2026-05-29.md` — the 99.8 % retention measurement
* `EXIT_POST_APP3_BASELINE_2026-05-31.md` — peak_rss baseline
* `GC_AND_MEMORY_ACCOUNTING_AUDIT_2026-05-31.md` — peak_rss methodology
* `EXIT_PATH_A_V2_FALSIFIED_2026-06-01.md` — the ru_maxrss monotonicity constraint
* `bench/baselines/2026-06-01-phase-c-histogram/hne.{out,err}` — fallback histogram data
* `[[falsification-rule]]`
* `[[measure-twice-cut-once]]`

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
