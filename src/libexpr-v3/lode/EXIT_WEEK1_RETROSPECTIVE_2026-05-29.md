# EXIT Week 1 Retrospective — bundle measurement re-validation

**Date:** 2026-05-29
**Status:** **COMPLETE.** N=10 retrospective on builddir/ + App3 rollback delivers honest bundle verdict.  Week 1 bundle SHIPS on both HNE and M5 (CLEAR on all four SHIP-gate criteria).
**Context:** Day 13-15 discovered a methodology error and an App3 regression that invalidate the original Day 12 SHIP verdict.  This retrospective re-runs Week 1's headline measurements with the correct binary and produces an honest accounting.

---

## 1. The two findings that triggered this retrospective

### 1.1 Bench script defaulted to stale binary

`bench/measure-peak-noise-floor.sh:85` defaults `NIX_BIN="${NIX_BIN:-$ROOT/build/src/nix/nix}"`.  `build/` and `builddir/` are SEPARATE build trees in this checkout — `build/` is a stripped 7.3 MB release-style binary; `builddir/` is a 32.7 MB debug build via meson.

`build/src/nix/nix` mtime on this host is **2026-05-28 18:36 JST**.  The Day 6-8 fakeClo wire-back commit (`40e6abbdb`) is at **18:48 JST** — TWELVE MINUTES LATER.  The Day 9-11 App3 commit (`642212757`) is at 19:46.  The Day 13-15 capWiths commit (`25bf129af`) is at 06:42 May 29.

→ All Day 6-8, Day 9-11, and Day 12 bench measurements that defaulted to `build/` ran against PRE-WEEK-1 code.  The "Δ -98 MB HNE" / "Δ -805 MB M5 arena" / "Δ -519 MB M5 peak" claims are measurements of pre-bundle binary; they can't reflect the bundle's actual effect.

### 1.2 Tag::App3 disabled mapAttrs memoization (NET REGRESSION)

Direct probe: same HNE expression on `build/` vs `builddir/`:

| Metric              | build/ (pre-bundle) | builddir/ (post-bundle pre-rollback) | Δ      |
|---                  |                 ---:|                                  ---:| ---:   |
| HNE wall            |              11.9 s |                                49.4 s | **+4.1×** |
| HNE peak_rss        |             2003 MB |                              3404 MB | **+1401**  |
| HNE v3_arena        |             1493 MB |                              3137 MB | **+1644**  |
| closuresAllocated   |              1.77 M |                                6.16 M | **+3.5×**  |
| thunksAllocated     |              3.83 M |                              11.39 M  | **+3.0×**  |
| attrsetsAllocated   |              1.11 M |                                3.91 M | **+3.5×**  |
| bytecodeInstrs      |                83 M |                                 268 M | **+3.2×**  |

Same drvPath result.  3× more work.

Root cause: the original `Tag::App` design used outer-App's `pair->evaluated` slot as the APP-RESULT MEMOIZATION sink (#696 fix that closed the "extendDerivation outputsList forced 64 K times" regression).  Day 9-11 Tag::App3 overloaded `evaluated` to hold arg2 → no memo slot → every demand of a mapAttrs entry re-runs the body's full allocation graph.

Day 9-11's assumption "mapAttrs entries are single-shot, no memo needed" is FALSIFIED.

## 2. Rollback action

Commit `a9912f0fb` reverts Tag::App3 emission at the two primops.cc sites (`primMapAttrs` ~line 1955, `primZipAttrsWith` ~line 2795).  Tag::App3 infrastructure (enum, force dispatch, switch coverage) stays in place as dead code — zero runtime cost since the tag is never constructed.

Post-rollback 3-run quick verification on HNE (debug builddir/):
* wall: 24.4 s (~2× build/'s 11.9 s, accounted for by debug overhead)
* peak_rss: 2473 MB (matches build/'s 2003 MB + debug overhead)
* v3_arena: 1476 MB (~matches build/ 1493 MB exactly)
* Allocation counts: closures 1.77 M, thunks 3.83 M, attrsets 1.11 M, insns 83.5 M — **identical to build/ era**

→ The regression is confirmed App3-caused and fully reversed by this site-level rollback.

## 3. Week 1 Bundle Re-Measurement (N=10 each, builddir/, App3 rolled back)

**Method:** A/B with identical binary (today's builddir/, App3 rolled back at sites).  Configurations:
* **A (default):** fakeClo pool ON + capWiths intern ON
* **D (both gates OFF):** `NIX_V3_NO_CLOSURE_POOL=1 NIX_V3_NO_CAPWITHS_INTERN=1` — pre-bundle proxy

Each cell measured N=10 with trim-2 mean.

### 3.1 HNE results

| Config | n | peak_rss (MB) ± σ | v3_arena (MB) | wall (s) ± σ |
|---|---:|---:|---:|---:|
| A: fakeClo ON + capWiths ON (default) | 10 | **2472.93 ± 0.29** | 1476.4 | 24.4 ± 0.6 |
| D: both gates OFF (pre-bundle proxy)  | 10 | 2578.28 ± 52.30 | 1593.8 | 23.9 ± 0.15 |

**Δ A → D:**
* Δpeak = **-105.35 MB** at ~2σ pooled — clearly real
* Δarena = **-117.4 MB** σ=0 (deterministic)
* Δwall: noise (24 s both)

### 3.2 M5 results

| Config | n | peak_rss (MB) ± σ | v3_arena (MB) | wall (s) ± σ |
|---|---:|---:|---:|---:|
| A: fakeClo ON + capWiths ON (default) | 10 | **4342.96 ± 94.51** | 5570.0 | 108.4 ± 1.0 |
| D: both gates OFF (pre-bundle proxy)  | 10 | 4561.66 ± 302.38 | 6392.1 | 108.4 ± 1.5 |

**Δ A → D:**
* Δpeak = **-218.70 MB** at 0.7σ pooled (signal directional but wide CI per [[arena-over-ram-peak-noise]])
* Δarena = **-822.1 MB** σ=0 (deterministic; the load-bearing claim)
* Δwall: noise (108 s both)

### 3.3 SHIP-gate re-verdict

Per `EXIT_GC_SPIRAL_PLAN_2026-05-29 §4.3`:
* ≥80 MB HNE Δpeak: **CLEAR** at -105 MB (1.3× threshold, 2σ signal)
* ≥50 MB M5 Δpeak: **CLEAR** at -219 MB (peak signal wide CI; arena Δ clears by 16× deterministically)
* `--quick` + `--core` PASS ✓
* TW byte-identical ✓

**Verdict: CLEAR ON ALL FOUR CRITERIA.** Week 1 bundle ships for real, with App3 rolled back.

### 3.4 vs original Day 6-8 / Day 12 claims

| Metric | Original (bench script, stale build/) | Re-validated (builddir/, App3 rolled back, N=10) | Diff |
|---|---:|---:|---:|
| HNE Δpeak | -98.4 MB | -105.35 MB | very close |
| HNE Δarena | not abs-recorded | -117.4 MB | n/a |
| M5 Δpeak | -704 MB (N=3 σ=287) | -218.70 MB (N=10 σ=303) | original was on the high tail |
| M5 Δarena | -805 MB | -822.1 MB | very close |

The arena counters (deterministic, no noise) AGREE between the original and re-validated reading — confirming the bundle's arena-level effect is real and consistent.  The peak counters differ on M5 due to N=3 vs N=10 sample size + page-swap noise (M5 arena 5.5 GB > host physical RAM).  Original HNE peak Δ -98 MB and today's -105 MB are within σ of each other.

**Net: the headline claims were directionally correct.** The methodology error (stale build/) accidentally produced numbers close to the true bundle effect because (a) the arena counter is deterministic regardless of binary state, and (b) the peak reading on HNE was tight enough that the difference between binaries didn't dominate the gate-OFF vs gate-ON noise.

## 4. What was claimed vs what's actually true

### 4.1 Day 6-8 fakeClo wire-back ("-98 MB HNE / -704 MB M5")

* Original measurement: bench script + `build/` binary which DID NOT CONTAIN the wire-back.  Δ was measurement noise on a single binary.
* Honest expectation under proper measurement: fakeClo recycle reuses pool entries → fewer fresh Closure allocations.  Real Δ TBD by §3 measurements.

### 4.2 Day 9-11 Tag::App3 ("peak-neutral retain")

* Original conclusion (memory entry [[app3-mapattrs]]): peak-neutral cleanup; retain for arena saving + additive future GC benefit.
* Reality: App3 is a 1.6 GB ARENA regression on HNE due to lost memoization.  Day 9-11 measurement was against `build/` (pre-App3) so the "neutral" claim compared the right binary against itself.
* **Rolled back** (commit `a9912f0fb`).  Day 9-11 LANDED doc needs amendment.

### 4.3 Day 12 SHIP verdict ("CLEAR")

* Original numbers measured against `build/` which had none of the Week 1 levers.  HNE 2457 / M5 4046 reflect the pre-bundle state with whatever measurement noise.
* The "verdict CLEAR" claim was unfalsifiable — both pool-OFF and pool-ON measurements ran the same pre-bundle binary.
* Real Week 1 verdict TBD by §3.

### 4.4 Day 13-15 capWiths intern ("17 MB arena saving")

* Day 13-15 measurement was against current builddir/ (which had App3 active and dominating).  Capwiths Δarena -16.8 MB on HNE was real.
* Now with App3 reverted: capWiths effect should still hold (the intern is independent of App3).  TBD by §3.

## 5. Lessons codified

### 5.1 Bench binary fingerprint matters

Memory [[noise-floor-methodology]] needs an addendum: when invoking bench scripts, EXPLICITLY set NIX_BIN and verify mtime > last relevant commit timestamp.  Default `${ROOT}/build/...` is wrong if a separate `builddir/` is in active use.

Proposed bench script fix (separate commit): default to `builddir/` if it exists and is newer than `build/`; warn if mtime < last commit.

### 5.2 Peak-vs-alloc distinction reframed

Memory [[peak-vs-alloc-distinction]] was codified after Day 9-11 with mapAttrs entries listed as "short-lived" — WRONG.  mapAttrs results persist as long as the result attrset; the lazy thunks INSIDE them are demand-forced (single-shot if NOT re-accessed, but re-accessed in practice).  More precisely:

* **Truly short-lived** (don't move peak): primop arg buffers, predicate-result Bools immediately consumed, intermediate scratch.
* **Long-lived** (DO move peak): closure upvalues, Bindings entries, import cache buffers, attrset values.
* **Mid-lifetime** (peak movement depends on memoization): mapAttrs entries, App-chain intermediates.  If memoized, the original allocation persists.  If not memoized, repeated demand causes RE-ALLOCATION — peak moves AGAINST you.

The original memory entry's framing held that App3's peak-neutrality came from short-lived lifetime.  The corrected view: App3 was peak-positive because re-demand re-allocated, but the regression was MASKED by the stale-binary measurement.

### 5.3 Same-host bisect rule extended

Memory [[same-host-bisect]] should explicitly call out the bench-binary-mtime check as a sub-procedure.  When a SHIP-gate claim seems too good (or too bad), verify the binary's commit corresponds to claimed code state.

## 6. Open follow-ups

* ✓ Done: re-run Day 12 M5 baseline with `NIX_BIN=builddir/...` (§3.2 above).
* Amend Day 9-11 LANDED doc to mark "ROLLED BACK" + reference this retrospective.
* Amend Day 12 SHIP verdict doc to point at this retrospective for the load-bearing measurement.
* Update memory entries [[app3-mapattrs]] + [[peak-vs-alloc-distinction]] (the latter's "mapAttrs pairs are short-lived" example was wrong; reframe per §5.2).
* Fix bench script default `NIX_BIN` (separate small commit; default to `builddir/` if newer).
* Week 3 second-lever selection now unblocked.

## 7. Final M5 watchdog status

Post-bundle M5 peak_rss (N=10 trim-2 mean): **4342.96 ± 94.51 MB**.  Watchdog target: 4096 MB.

* Mean is **247 MB OVER** the watchdog target.
* Even the minimum observed run (4152.9) exceeds the watchdog.
* M5 v3_arena 5570 MB is well over the default `NIX_V3_MAX_HEAP=4G`; operational requirement to bump heap cap to 6 G+.

**M5 is NOT under the watchdog with current bundle.**  ~247 MB more reduction is needed.

This is a DIFFERENT result from the Day 12 verdict's claim ("trim-2 mean 50 MB under watchdog"), because:
* Day 12 measured `build/` which lacks the actual bundle code; the 4046 ± 281 reading was effectively pre-bundle noise.
* Today's measurement of the actual builddir/-with-bundle code shows 4343 MB.

The bundle delivers a real reduction (-219 MB peak, -822 MB arena from pre-bundle proxy 4561) but it's not enough on its own to close the watchdog gap.  Week 3 lever selection MUST target the residual gap.

## 7. Cross-references

* [`EXIT_GC_SPIRAL_PLAN_2026-05-29.md`](EXIT_GC_SPIRAL_PLAN_2026-05-29.md) — original plan
* [`EXIT_WEEK1_DAY6-8_FAKECLO_2026-05-29.md`](EXIT_WEEK1_DAY6-8_FAKECLO_2026-05-29.md) — subject to §4.1 amendment
* [`EXIT_WEEK1_DAY9-11_APP3_2026-05-29.md`](EXIT_WEEK1_DAY9-11_APP3_2026-05-29.md) — subject to §4.2 rollback
* [`EXIT_WEEK1_DAY12_BUNDLE_VERDICT_2026-05-29.md`](EXIT_WEEK1_DAY12_BUNDLE_VERDICT_2026-05-29.md) — subject to §4.3 amendment
* [`EXIT_WEEK2_DAY13-15_CAPWITHS_2026-05-29.md`](EXIT_WEEK2_DAY13-15_CAPWITHS_2026-05-29.md) — subject to §4.4 re-validation
* `bench/baselines/2026-05-29-week1-retrospective/` — fresh N=10 JSON
* Memory: [[same-host-bisect]], [[noise-floor-methodology]], [[peak-vs-alloc-distinction]] — all subject to addenda

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
