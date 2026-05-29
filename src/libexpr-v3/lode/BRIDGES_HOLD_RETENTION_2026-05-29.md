# Bridges hold retention — the real lever for v3 memory

**Date:** 2026-05-29 evening (post-DIAG spike chain)
**Status:** **STRATEGIC FINDING** — bridge tables hold 99.8-99.9 % of "live" bytes at end-of-eval on BOTH HNE and M5.  Recontextualizes the entire GC track: it was chasing the wrong problem.
**Companion:** [`DIAG_CONCENTRATED_RETENTION_2026-05-29.md`](DIAG_CONCENTRATED_RETENTION_2026-05-29.md)

**Refinement 2026-05-29 (post-instrumentation)** — bridge ENTRY COUNT differs significantly between workloads (HNE = 30 entries, M5 = 10056), but TRANSITIVE retention is the dominant mechanism in BOTH.  The "bridges grow monotonically with eval depth" framing in the initial draft was HNE-specific; M5 shows true growth.  Per-entry retention varies: HNE 17 MB/entry (heavy), M5 73 KB/entry (light).  In both cases the entries TRANSITIVELY pin nixpkgs-root-sized graphs.

---

## 0. Workload-specific bridge growth

Mid-eval periodic samples (NIX_V3_LIVE_TRACE_PERIODIC + bridge-size columns):

| Workload | Sample | alloc MB | bridge closures | bridge attrs | bridge lists |
|---|---|---:|---:|---:|---:|
| HNE | 1 | 720 | 11 | 5 | 0 |
| HNE | 2 | 1072 | 14 | 11 | 0 |
| HNE | 3 (end) | 1408 | 19 | 11 | 0 |
| M5 | 1 | 1600 | 8791 | 9 | 0 |
| M5 | 2 (end) | 5312 | 10042 | 14 | 0 |

HNE bridges stay tiny (O(10s) of entries).  M5 bridges grow to O(10K) entries — cardano-node has a much deeper / wider cross-bridge call pattern (likely from `lib.fix`/`lib.extends` traversals through haskell.nix overlays).

Per-entry transitive retention:
* HNE: 519 MB / 30 entries ≈ 17 MB / entry (each holds a heavy attrset)
* M5:  731 MB / 10056 entries ≈ 73 KB / entry (many lighter closures, shared transitive state)

Either way, the **TOTAL ARENA HELD VIA BRIDGES** is dominant on both workloads.

## 1. The empirical spike chain (HNE, post-bundle, post-App3-rollback)

End-of-eval `dumpV3LiveFraction` walking global roots (VMState torn down), under different root-clearing gates:

| Clear config | Live MB | Bindings MB | Per-PosIdx top retainer |
|---|---:|---:|---|
| Baseline (no clear) | 519 | 400 | 311 MB at all-packages.nix:9112 |
| `NIX_V3_END_OF_EVAL_CLEAR_IMPORT_CACHE=1` | 519 | 400 | 311 MB at all-packages.nix:9112 _(no change)_ |
| `NIX_V3_END_OF_EVAL_CLEAR_BRIDGES=1` | **0.9** | **0.6** | _(small residual)_ |
| Both | 0 | 0 | _(nothing)_ |

**Bridge tables alone hold 99.8 % of live bytes.**  ImportCache.results holds <1 % (the 0.9 MB difference between "bridges only" and "both").

## 2. Why this is a big deal

### 2.1 The "concentrated retention" reframed

DIAG-2 Phase 2 showed 62.8 % of live bytes attributed to `pkgs/top-level/all-packages.nix:9112:3`.  Interpretation a few hours ago: "concentrated retention validates BiBOP-lite."  Actually: **the concentration is a side-effect of bridge-table retention, not a workload property.**

The 319 K Bindings at all-packages.nix:9112 are all reachable from `v3BridgeAttrs()` — every nixpkgs attrset that crossed v3 → TW during eval is in the bridge table.  TW retained references for its own forcing/coercion logic; v3 retains the v3-side value via the bridge map; neither side has a release path.

If the bridge table were emptied, the 319 K Bindings would be unreachable from any root, and a precise GC would free them.

### 2.2 The whole GC debate was looking in the wrong place

Per `GC_PAUSE_2026-05-29 §2.1`, six GC falsifications:
1. Ditch-Boehm wall — falsified
2. Boehm tuning §6.2 — falsified
3. Periodic GC — falsified
4. Arena dereg — falsified
5. Cheney semispace — +486 MB regression (high L)
6. Flat MS — F2 verdict was projection bug (per DIAG-1)
7. Immix — projected short (and has correctness bug per OP_REC_BINDING_SLOT_REF)

**Every GC variant assumed the bottleneck was the v3 arena's per-cell reclamation strategy.**  But DIAG-2 Phase 2 + the spike chain reveals: the bottleneck is BRIDGE-TABLE RETENTION.  No precise GC can free bridge-held bytes — they're reachable from global roots.

The H of "high L" (L = 0.74 on HNE per `LIVE_FRACTION_SPIKE_2026-05-27`, L = 0.41-0.59 across time per `L_TIME_SERIES_DATA_2026-05-29`) is HIGH precisely because the bridges hold everything.  L would drop dramatically if bridges had a release path.

### 2.3 What the bridges actually do

`primops.cc:3934-3987` defines three tables:
* `v3BridgeClosures()` — TW-side `nix::Value *` keyed map to v3 Closures
* `v3BridgeAttrs()` — same for v3 Bindings
* `v3BridgeLists()` — same for v3 ListVecs

Populated when v3 evaluates a sub-expression and needs to hand the result to TW (for primops like `realisePath`, `derivationStrict`, `__filterAttrs`, etc.).  The cross-bridge populates the entry; TW retains a `Value *` referencing it; v3 retains the v3 Value via the entry.  **Neither side can drop the entry without breaking the other.**

In a long-running eval that crosses bridges N times, the table grows monotonically.  Each crossing adds an entry that holds:
* v3-side: a Tag::Attrs / Tag::Closure / Tag::List Value with payload pointer into v3 arena
* TW-side: a `nix::Value` struct allocated by Boehm

**This is a memory leak by design.**  The bridge tables were built for correctness (preserving Value identity across crossings) without a lifecycle protocol.

## 3. Why this changes the v3 strategy

### 3.1 Closing the M5 watchdog gap

M5 arena = 5570 MB at end-of-eval.  Subject to confirmation on M5 (DIAG-2 wasn't run on M5), the same bridge-retention pattern likely applies.  If 95 % of M5's arena is bridge-held (extrapolating from HNE's 99.8 %), then:

* Current state: M5 peak ~4300 MB (env. variance)
* With bridge LRU + end-of-eval clear + precise GC: peak could drop to (~5 % of 5570 MB) ≈ 280 MB

The watchdog goal (4096 MB) becomes trivially achievable.

### 3.2 The lever priorities (re-re-rewritten)

Previous ordering (per `DIAG_CONCENTRATED_RETENTION §4`):
1. End-of-eval GC spike (RECOMMENDED)
2. DIAG-5 flat MS quadratic fix
3. Non-arena attribution
4. Immix correctness debug

**Now:**
1. **Bridge lifecycle management** — single biggest lever, ~95 % arena freeable on HNE.  Effort: 1-3 weeks for ref-counting or LRU.
2. End-of-eval bridge-clear hook — IMMEDIATE peak-RSS win at eval-end (already implemented as DIAG spike).  Zero-cost when default-OFF.
3. End-of-eval ImportCache clear — small additional win.
4. (GC variant work) — DOWN-PRIORITIZED until bridges have a release path.  No GC will help while bridges retain everything.

### 3.3 Validates AND deprioritizes the per-PosIdx work

DIAG-2 Phase 2's source-level attribution gave us "the smoking gun" — concentration at all-packages.nix:9112 led us to investigate WHY that source position retains so much.  The investigation revealed bridges.

But: now that we know it's bridges, FURTHER per-PosIdx work has diminishing returns.  Without bridge lifecycle, every concentration finding will trace back to bridges.

DIAG-2 Phase 2 was the right tool to investigate; the answer is "bridges hold the bytes."  Pursuing DIAG-2 Phase 3 (full posHandle coverage in vm.cc dispatch) is no longer the highest-priority work.

## 4. Bridge lifecycle protocol — design sketch (for next session)

Three candidate designs, ranked by effort:

### 4.1 Reference counting

Each `v3BridgeAttrs()` entry adds a refcount field.  Increment on v3 → TW handoff; decrement when TW Value's destructor fires (callback into v3).  Drop entry at refcount = 0.

**Pros:** correct semantics, no eviction policy needed.
**Cons:** requires hooking TW Value destruction.  TW Values are Boehm-managed → finalizer needed.  Boehm finalizers are unreliable (run order, missed cycles).

### 4.2 LRU eviction (with grace period)

Each entry timestamped on access.  Periodically (e.g., when table exceeds N MB), evict oldest M entries.  Re-bridge cost on next access if TW still holds.

**Pros:** simple, no TW-side cooperation needed.
**Cons:** wrong-eviction could fail TW → v3 callbacks if a still-held entry is dropped.  Needs careful grace-period sizing.

### 4.3 End-of-eval clear (immediate ship)

Single hook: clear all bridge tables when `nix eval` shuts down.  Already implemented as DIAG spike.  Production-ize: thread it into the `runRootExpr` return path under default-ON gate (no env var needed for the eval CLI use case).

**Pros:** trivial to land, immediate peak-RSS win for single-shot evals.
**Cons:** doesn't help interactive `nix repl` (which expects bridges to persist).  Needs CLI-context awareness.

**Recommendation: ship 4.3 first (1-day project) + design 4.1/4.2 for 2026-06.**

## 5. Validation gates (for any bridge-lifecycle work)

Per `[[measure-twice-cut-once]]`, pre-committed thresholds:

* Pre-bridge-lifecycle baseline (today): HNE peak 2473 ± 0 MB, arena 1476 MB, live-at-eval-end 519 MB
* End-of-eval bridge-clear ship gate: peak_rss at moment-of-result-return drops by ≥ 200 MB (free the 311 MB at all-packages.nix:9112 — needs accompanying GC to actually return bytes to OS)
* Combined with end-of-eval precise GC: arena post-shutdown drops by ≥ 1 GB on HNE
* M5 measurement: pre-bundle 4561 → with bridge lifecycle, projected <500 MB end-of-eval residual

Acceptance: any single measurement below threshold → ship the corresponding piece; multi-piece ship requires ALL gates clear.

## 6. Open questions for next session

1. **Does the bridge-retention pattern hold on M5?** Run DIAG-2 Phase 2 + bridge-clear spike on M5.  Predict: similar (95 %+ held by bridges).
2. **What's the mid-eval bridge growth curve?** Sample bridge-table size periodically (extend DIAG-3's per-Tag CSV with bridge-counts).
3. **Can `nix repl` survive bridge LRU?** Test with bridge LRU at modest threshold + repeat interactions.
4. **Reference counting feasibility audit** — survey TW's Boehm finalizer behavior on macOS aarch64.

## 7. What this tells us about v3 architecture

The bridge tables grew organically as v3 added more primop bridges.  Each bridge crossing was a correct-by-construction step; together they accumulate into a leak by design.

This is a structural issue.  The bridges exist BECAUSE v3 + TW share the eval workload — TW handles flake / store / IFD, v3 handles core eval.  Crossing happens at primop boundaries.  Reducing crossings (by moving more primops into v3) reduces bridge growth.

Two parallel paths:
* Architectural: reduce TW dependency (per `FFI_AUDIT_2026-05-20`)
* Lifecycle: drop bridges when they're not needed

Both compose.

## 8. Cross-references

* [`DIAG_CONCENTRATED_RETENTION_2026-05-29.md`](DIAG_CONCENTRATED_RETENTION_2026-05-29.md) — the original "concentration" finding this reframes
* [`DIAG_SUITE_LANDED_2026-05-29.md`](DIAG_SUITE_LANDED_2026-05-29.md) — instrument inventory
* [`SESSION_ARC_2026-05-29.md`](SESSION_ARC_2026-05-29.md) — broader session context
* [`FFI_AUDIT_2026-05-20.md`](FFI_AUDIT_2026-05-20.md) — bridge surface inventory
* `primops.cc:3934-3987` — bridge table definitions
* `primops.cc:7569` — walkV3BridgeRoots
* `primops.cc:7589+` (new) — clearV3BridgesForDiag
* Memory: `[[v3-native-constraint]]` — V3-NATIVE rule limits TW crossings to FFI leaves

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
