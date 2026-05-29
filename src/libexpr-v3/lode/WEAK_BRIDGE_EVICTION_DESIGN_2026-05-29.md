# Weak-bridge eviction via fallbackExpr re-eval — design

**Date:** 2026-05-29
**Status:** DESIGN (no code).  Implementation gated on a measurement spike (Stage 0); SHIP per pre-committed thresholds in §6.
**Companion:** [`BRIDGES_HOLD_RETENTION_2026-05-29.md`](BRIDGES_HOLD_RETENTION_2026-05-29.md) (memory diagnosis), [`BRIDGE_TELEMETRY_2026-05-26.md`](BRIDGE_TELEMETRY_2026-05-26.md) (instrumentation).

---

## 1. Goal

Reduce peak resident memory by evicting cold entries from the v3↔TW bridge tables (`v3BridgeClosures` / `v3BridgeAttrs` / `v3BridgeLists`) during evaluation, with bounded CPU cost.

### Pre-committed memory target (per `BRIDGES_HOLD_RETENTION_2026-05-29.md`)

- HNE end-of-eval bridge-held: **~95 % of arena** (DIAG-2 + bridge-retention walk).
- M5 (cardano-node): **10 042 dense bridges**, each retaining ~952 MB transitively.

A 50 %-of-bridge-held reclamation translates to:
- HNE: ~750 MB peak RSS reduction (out of ~1500 MB peak)
- M5: targets the gap to the 4 GB watchdog (current trim-2 mean = 4 343 MB).

---

## 2. Background — what's already in place

Each bridge entry (`primops.cc:3920-3962`) holds:

```cpp
struct BridgeClosureEntry { Value v3Value; nix::Expr * fallbackExpr; };
struct BridgeAttrEntry    { Value v3Value; nix::Expr * fallbackExpr; };
struct BridgeListEntry    { Value v3Value; nix::Expr * fallbackExpr; };
```

The `fallbackExpr` is **already stored**: it was added by WC-19 (`primops.cc:3949-3954`) so the bridge primops (`primV3CallBridge1` / `primV3ForceAttr` / `primV3ForceListElem`) can re-run the source expression through TW when v3-side eval blackholes on a cycle TW would otherwise resolve.

Today the fallback path fires only on `BlackholeError`.  This design **extends** the same path to fire when the cached `v3Value` has been intentionally evicted under memory pressure.

The tables themselves use `traceable_allocator<>` so Boehm scans the inner Value payloads — they live in the v3 arena and currently stay reachable for the lifetime of the table.  Eviction = remove the v3Value from this reachability path while preserving the fallbackExpr.

---

## 3. Constraints

1. **V3-NATIVE rule** ([CLAUDE.md §1.1](../../../../CLAUDE.md)) — TW is permitted only at FFI leaves.  Routing the eviction-recovery path through TW (via fallbackExpr->eval(ns, baseEnv, tw)) is **already legitimate** (it's the BlackholeError rescue path).  Going further (v3-side re-eval) is preferable for perf but optional for correctness.

2. **Determinism** — Nix evaluation must produce the same Value given the same Expr+Env.  Re-eval-on-eviction is sound only for **pure** fallbackExprs.  Impure primops (`builtins.currentTime`, `fetchTree` with network, `readFile`/`readDir` against a mutable FS, `getEnv`) produce different values on re-eval.  The design must **mark impure bridges non-evictable**.

3. **Idempotent side effects** — IFD probes (writeDerivation, hashDerivationModulo) are idempotent w.r.t. the content-addressed store.  Re-eval re-touches the store but doesn't change semantics.  Wasted CPU only.

4. **No protocol change** — `primV3CallBridge1` / `primV3ForceAttr` / `primV3ForceListElem` keep their (handle, args) protocol.  The eviction mechanism is internal to the bridge tables.

5. **Concurrency** — bridge tables are not currently thread-safe; eviction must run on the same thread as eval (default v3 mode).

6. **Configurable** — gated by `NIX_V3_WEAK_BRIDGES=1` (default OFF).  Pre-committed SHIP gate (§6) flips default per workload.

---

## 4. Design — three-stage rollout

The user's request is "design weak-bridge eviction via fallbackExpr re-eval."  The phrasing privileges the *existing* fallbackExpr path (TW eval).  We propose a stage progression where each stage is independently shippable, and the SHIP gate at each stage decides whether to continue.

### Stage 1 — TW-only re-eval (simplest, lowest-risk)

Mechanism: on eviction, clear `v3Value`; on next dispatch with an evicted entry, run `fallbackExpr->eval(ns, baseEnv, tw)` (the existing BlackholeError path) and convert TW result back to v3 via `treeWalkerToV3` for that dispatch only.  Do NOT repopulate `v3Value` — the bridge stays evicted until the next dispatch, which re-runs TW again.

- Pro: only the cached `v3Value` field changes (set to `Tag::Uninitialized`); no new storage; the rescue path already exists for BlackholeError.
- Con: subsequent dispatches on the same handle pay the TW cost N times.  Acceptable only if eviction policy targets entries with few expected future dispatches.
- Memory: each evicted entry's transitive retention frees on next Boehm GC after the v3Value field is cleared.

Stage 1 is the minimum-viable shape.  It tests the eviction *policy* without confronting the v3-eval-vs-TW-eval cost asymmetry.

### Stage 2 — TW re-eval + v3 re-cache

After Stage 1's TW dispatch produces a value, **store it back** into `v3Value` via `treeWalkerToV3`.  The next dispatch hits the cache again with no TW round-trip.

- Pro: bounded thrash; entries that get re-accessed pay the TW cost once per eviction cycle.
- Con: if the eviction policy churns (evict + re-cache + evict + re-cache), there's no win.  Mitigation: eviction threshold + LRU policy must produce a *stable* working set.
- Memory: same as Stage 1 between dispatches; the re-cached size matches the original after re-cache.

Stage 2 is the natural production shape under "fallbackExpr re-eval" framing.

### Stage 3 — Pure v3 re-eval

Store the v3-lowered closure (or its `CompilationUnit *`) at bridge-creation; on dispatch with an evicted entry, run v3 directly without TW round-trip.  Falls back to TW only on cycle (same BlackholeError gate).

- Pro: re-dispatch cost matches the original v3 eval cost (no TW marshalling).
- Con: new storage per entry (CU + Env pointer ≈ 24-48 B).  Requires deciding which v3-side state to capture at bridge-creation (closure state, evaluator state, store state).
- Memory: trade-off — eviction frees the v3Value payload but holds the CU.  Net win only if `v3Value transitive retention >> CU size`.

Stage 3 is the eventual goal.  Whether it ships depends on Stage 2 measurement.

---

## 5. Eviction policy

Independent of which stage; the same policy applies.

### Policy v1: LRU + threshold

- Each bridge entry tracks `lastAccessGen` (uint64, bumped on every dispatch).
- Periodically (every N opcodes, or at bridge-insert when table grows past a threshold), evict the bottom 25 % of entries by `lastAccessGen` (i.e. coldest).
- Threshold default: `floor(NIX_V3_MAX_HEAP / 3)` if heap cap is set, else 256 MB.  Override via `NIX_V3_BRIDGE_EVICT_THRESHOLD=N` (bytes).

### Bridge byte estimate

Bridge "size" is the transitive byte retention of the v3Value payload.  Computing this per-entry is expensive (`forEachV3BridgeEntry` + transitive walk).  Approximations:

- **Cheap (Stage 1 ship)**: count of entries × average bytes-per-entry sampled at insert time.  Triggers eviction by *count*, not bytes.
- **Mid (Stage 2 ship)**: per-entry byte estimate via `Alloc::transitiveBytes(value)` walking the v3 payload at insert time, cached on the entry.  N entries × O(payload) at insert; O(1) at eviction-policy check.
- **Expensive (deferred)**: walk transitively at each policy tick.  Reserved for diagnostics, not production.

### Impurity gating (non-evictable list)

At bridge-insert time, classify the fallbackExpr as PURE / IMPURE:

- PURE: no `builtins.currentTime` / `fetchTree` / `fetchUrl` / `getEnv` / `readFile` / `readDir` / `pathExists` / `findFile` / `import` of a `<nixpkgs>`-style mutable path.
- IMPURE: any of the above.

Heuristic at insert: walk the fallbackExpr AST counting impure primop references; bridge is PURE iff count == 0.

Conservative: any UNKNOWN primop reference (e.g. user-installed via `addPrimOp`) is treated as IMPURE.

IMPURE bridges are **never evicted**.  Tracked in stats as `nonEvictable`.

### Cycle preservation

The existing BlackholeError rescue path uses `fallbackExpr` to recover from v3-side cycles.  Eviction must NOT interfere with cycle detection: if dispatch enters re-eval and re-eval itself trips a cycle, the existing path handles it.  Test fixture: confirm that an evicted+blackhole-on-re-eval doesn't infinite-loop.

---

## 6. Pre-committed SHIP gates (per [[measure-twice-cut-once]])

Each stage has its own SHIP gate.  Falsification → STOP, revert, document.

### Stage 0 — Measurement spike (1-2 d, instrumentation only, no eviction)

- Add `lastAccessGen` + per-entry transitive-byte estimate to bridge entries.
- Dump distribution at end-of-eval:
  - count of entries by lastAccessGen bucket (accessed once / few-times / many-times / never)
  - cumulative byte retention by bucket
  - HNE + M5 + hello.drvPath
- **Decision input** (not gate): if ≥ 30 % of total bridge bytes live in entries accessed ≤ 2 times → proceed to Stage 1.  Otherwise STOP — eviction's expected ROI is below threshold.

### Stage 1 — TW-only re-eval (1-2 d)

- **Code**: ~100 LoC.  Add eviction trigger; modify bridge dispatch to fall through to TW path when v3Value cleared.
- **Pre-committed SHIP threshold**:
  - HNE peak RSS reduction ≥ 200 MB (vs current ~1500 MB)
  - HNE wall regression ≤ 10 %
  - --core 15/15 + 5 nixpkgs paths byte-equal
  - 0 cycle-detection regressions in lang tests
- **Falsification**: any threshold fail → revert Stage 1 commit (the gate is opt-in, so user-facing behavior is unchanged; revert means delete the gate + code).

### Stage 2 — TW re-eval + v3 re-cache (2-3 d)

Conditional on Stage 1 SHIP.

- **Code**: ~150 LoC additional.  Add `treeWalkerToV3` round-trip; cache result back into v3Value.
- **Pre-committed SHIP threshold**:
  - HNE peak RSS reduction ≥ 300 MB (vs current)
  - HNE wall regression ≤ 5 %
  - M5 wall regression ≤ 8 %
  - M5 peak RSS reduction ≥ 200 MB (vs current 4 343 MB → ≤ 4 143 MB)
  - --core 15/15 + 5 nixpkgs paths byte-equal
- **Falsification**: any threshold fail → keep Stage 1 (its SHIP-green threshold), revert Stage 2 commit.

### Stage 3 — Pure v3 re-eval (3-5 d)

Conditional on Stage 2 SHIP AND a separate decision based on Stage 2 wall regression.

- **Code**: ~250 LoC additional.  Capture v3 CU at bridge-creation; on evicted-dispatch re-run v3.
- **Pre-committed SHIP threshold**:
  - HNE wall improvement ≥ 5 % over Stage 2 (skip-TW-roundtrip)
  - HNE peak RSS at-least-as-good as Stage 2
  - M5 wall regression ≤ 5 % (note: Stage 3 should IMPROVE wall vs Stage 2)
- **Falsification**: if Stage 3 doesn't improve wall, keep Stage 2 + revert Stage 3 commit.

---

## 7. Risks & mitigations

| Risk | Mitigation |
|---|---|
| Re-eval produces different Value than first eval (impure primop missed by static analysis) | Conservative classification: any non-whitelisted primop → IMPURE → never evict.  Differential test: enable eviction with NIX_V3_BRIDGE_EVICT_STRESS=1 (evict every Nth entry regardless of policy) and run lang + nixpkgs golden.  Mismatch falsifies the design. |
| Eviction churn (evict + re-cache + evict ...) | LRU + threshold avoids churn for stable working sets.  Stress test: synthetic workload with N+10 always-hot entries when threshold allows only N → measure re-cache rate.  Threshold low enough to force churn = data point, not a ship blocker. |
| Memory benefit smaller than transitive walk's CPU cost | Stage 0 measurement spike answers this before any implementation.  If transitive walk takes > 5 % of wall, defer to Stage 2's `transitiveBytes` cached at insert. |
| Bridge protocol assumption broken (consumer assumes v3Value is always valid) | Audit `primV3CallBridge1` / `primV3ForceAttr` / `primV3ForceListElem` callers + the v3-side OP_CALL bridge-unwrap path (`tryUnwrapBridge1Closure`, vm.cc:2920) for "valid v3Value" assumptions.  Add the same evicted-check at each. |
| Cycle re-detection regression (eviction + BlackholeError stack) | Existing BlackholeError rescue path uses fallbackExpr; eviction-driven re-eval must compose with it (re-eval inside re-eval).  Test: synthetic blackhole-prone fixture under eviction stress. |
| Eviction makes performance noticeably *worse* on small workloads | Eviction only fires above threshold; small workloads stay below threshold → no eviction → no regression.  Verify in --quick. |

---

## 8. Effort estimate

| Stage | Effort |
|---|---|
| Stage 0 — measurement spike | 1-2 d |
| Stage 1 — TW-only re-eval | 1-2 d |
| Stage 2 — TW re-eval + v3 re-cache | 2-3 d |
| Stage 3 — pure v3 re-eval | 3-5 d (CONDITIONAL on Stage 2 measurement) |
| **Total (Stage 0 + 1 + 2)** | **4-7 d** |
| Total with Stage 3 | 7-12 d |

Stage 3 is genuinely optional — Stage 2's TW round-trip cost may already be acceptable.

---

## 9. Open questions

1. **Eviction trigger**: every-N-opcodes (predictable cost, may evict too often on quiet phases) vs at-bridge-insert (only when growing, fires only at the right time, cost spikes on bursts).  Proposal: at-insert by default; opcode timer as fallback.
2. **Boehm finalizer integration**: Boehm has a `GC_register_finalizer` mechanism that could trigger eviction at GC time.  This is closer to the "weak reference" semantics typically associated with the name "weak-bridge."  Defer to Stage 4 / future work — the explicit-trigger design is simpler and equally effective.
3. **Cross-process persistence**: should evicted entries persist via disk_cache to avoid re-eval cost across processes?  Out of scope; that's the IFD eval-result cache's job ([#885](../../../../src/libexpr-v3/lode/V3_NATIVE_MIGRATIONS_NEXT_SESSION_2026-05-29.md) section).
4. **Interaction with [#885 PRODUCTION cache](../../../../src/libexpr-v3/lode/V3_NATIVE_MIGRATIONS_NEXT_SESSION_2026-05-29.md)**: PRODUCTION cache skips primDerivation* body on hit.  Evicted bridges that feed into a derivation skipped via PRODUCTION cache → no eviction recovery needed.  Confirm: PRODUCTION-cache-hit does not transitively touch any evicted bridge's v3Value.  If it does, eviction must rebuild on demand (Stage 1 path covers this).

---

## 10. References

- `BRIDGES_HOLD_RETENTION_2026-05-29.md` — memory diagnosis (99.8 % of arena is bridge-held at HNE end-of-eval)
- `BRIDGE_TELEMETRY_2026-05-26.md` — existing per-bridge telemetry
- `primops.cc:3920-3962` — current `BridgeEntry` shape
- `primops.cc:4188+` — `primV3CallBridge1` dispatch + BlackholeError rescue path (already uses fallbackExpr)
- `vm.cc:2920` — `tryUnwrapBridge1Closure` (v3-side OP_CALL bridge unwrap)
- `gc.cc:1361` — bridge table as Boehm root (`walkV3BridgeRoots`)
- `precise_root.cc:129` — bridge in precise-root walker
- [[measure-twice-cut-once]] — pre-committed SHIP-gate methodology applied here
- [[bridges-hold-retention-2026-05-29]] — the memory finding this design responds to

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.  SPDX-License-Identifier: Apache-2.0.*
