# FFI surface kill — methodical TODO list

**Date:** 2026-06-01
**Per:** user directive "Let's create a todo list for this, and methodically work it down. Ultrathink!  We want few FFI calls; if we must, slim FFI calls; ideally strict leaves where we already know the realized values to pass.  We really don't want to bridge V3 to TW or TW to V3 (except maybe AST parse TW→V3, and even then throw away TW after).  Make no mistakes."

## 0. Goal hierarchy (the user's stated preferences)

In order, from most-preferred to least:

1. **ZERO FFI calls** — eliminate call sites entirely
2. **STRICT LEAF calls** — primitive args/results (string, int, struct); v3 has already resolved everything before the call; TW state has nothing to retain after the call
3. **NO v3↔TW Value graph bridging** — never give TW a v3 Value that holds a subgraph; never accept a TW Value that lazily bridges to v3
4. **AST parse TW→V3 is permitted (transient)** — only because no v3 parser exists yet.  THE TW SIDE OF PARSE MUST BE DISCARDED right after v3 holds the IR

## 1. Empirical state (from FFI_AUDIT_2026-06-01)

Phase A+C6 already retired BP3 (`__v3_force_list_elem`), the lazy-list construction, and the derivationStrict TW-bridge fallback.  Remaining live FFI surface:

| Category | Sites | Active on |
|---|---|---|
| TW→v3 callbacks | BP1 (`__v3_call_bridge_1`), BP2 (`__v3_force_attr`) | M5 (20034), HNE (51-84) |
| v3→TW marshalling (active) | 3 sites (BP1 result, BP2 result, primImport string-ctx) | M5 + HNE |
| FFI leaves (store/path/fetcher) | ~20 sites | All workloads (primImport realisePath dominant) |
| Hidden bridges (HX1-HX5) | 5 sites — all are FFI leaves | wall-time dominant |
| Tier 0 system info | Y1, Y5 (vBuiltins lazy-init intercepts) | empirically 0 dispatches |

## 2. The TODO list

### Tier 1 — Eliminate call SITES (highest priority per goal §0.1)

| # | Action | Effort | Yield | Status |
|---|---|---|---|---|
| **T1.1** | **Retire `v3BridgeLists()` table + 11 reader sites** | 0.5 d | code hygiene; -50 LoC | Phase A1 left the table empty.  Removing the accessor + reader iterations cleans the surface. |
| **T1.2** | **Retire `primDerivationStrict` fake-store path** (primops.cc:6504+) | 0.5-1 d | code hygiene; -200 LoC | Native already covers 100% on measured.  Bridge already default-off.  Fake-store path is deep fallback; soak-period escape hatch via `V3_DRV_KEEP_BRIDGE=1` already exists. |
| **T1.3** | **v3-native `primTrace` printer** (eliminate v3ToTw slot 10) | 0.5 d | empirically 0 calls today; structural elimination | Implement a v3-native value printer; remove `v3ToTreeWalkerPublic` dependency at `primops.cc:2848`. |
| **T1.4** | **Audit `primImport` v3→TW marshalling** (slot 2 — `primImport_string_ctx`) | 0.5 d investigation | 10-11 active calls on HNE/M5; replace with direct realisePath(path, ctx) call | The marshalling at primops.cc:8275 wraps a string+ctx in a TW Value.  If realisePath has a primitive overload, use it. |
| **T1.5** | **Retire bridge fallback paths in BP1/BP2** | 0.5 d each | code hygiene; only soak escape is left after gates flip | The fallbackExpr re-eval paths exist as safety nets for blackhole cycles.  With Phase D barriers + cycle-detection, are they still needed?  Audit and retire if dead. |

### Tier 2 — Slim remaining calls (per goal §0.2)

| # | Action | Effort | Yield | Status |
|---|---|---|---|---|
| **T2.1** | **D1 dedupe in `v3ToTreeWalker`** | 1-2 d | M5: ~50% bridge entries; HNE: smaller | Single unordered_map<v3Value.payload.raw, handle> before each push_back. |
| **T2.2** | **BP1 lazy result marshalling** | 1-2 wk | M5: most of 731 MB retention | Replace eager v3ToTreeWalker at primops.cc:4908 with a TW Bridge thunk; only force-bridge when TW reads the result. |
| **T2.3** | **BP2 lazy result marshalling** | 1-2 wk | HNE: most of 519 MB retention | Same pattern at primops.cc:5126.  Phase A2 short-circuit handles the already-Bridge case; full lazy is broader. |
| **T2.4** | **Fetcher flat-arg variants** | 1-2 d per fetcher | small (idle on cache-hit) | `__fetchTreeFlat(url, rev, sha256, type)` instead of attrset round-trip. |
| **T2.5** | **`treeWalkerToV3` lazy attrset wrap** | 1-2 wk | TW→v3 unmeasured retention | Introduce `Kind::LazyTW` Bindings flavor; defer per-entry wrap until first read. |

### Tier 3 — Convert leaves to strict-primitive (per goal §0.2)

| # | Action | Effort | Yield | Status |
|---|---|---|---|---|
| **T3.1** | **`realisePath(path_str, ctx)` overload** | 1 d | structural — no TW Value construction | Today every primImport/ReadDir/ReadFile/PathExists call wraps the path in a TW Value, then calls realisePath.  A primitive overload that takes path-string + context directly is what "strict leaf" looks like. |
| **T3.2** | **Survey all `state.nixEvalState->` calls for primitive shape** | 1 d audit | classification | Per FFI_AUDIT §1.3: all listed leaves already take primitive args (path strings, context).  This is an AUDIT to confirm — no fix needed. |
| **T3.3** | **`primToFile` / `primStorePath` / `primOutputOf`** | 0 (already minimal) | — | Audited: already primitive. |

### Tier 4 — Throw away TW after parse (per goal §0.4)

| # | Action | Effort | Yield | Status |
|---|---|---|---|---|
| **T4.1** | **Audit TW retention after `lowerNixExpr`** | 1 d investigation | identifies retention | After primops.cc:9214 `lowerNixExpr(e, ...)`, the `e` Expr is no longer needed by v3.  Does TW retain it (e.g., via parser cache)?  If yes: enumerate retention sources. |
| **T4.2** | **Explicit `e` release after lowering** | 0.5 d | small per-import retention | Set `e = nullptr` in the local scope; ensure no other v3-side reference holds it. |
| **T4.3** | **Audit `ns.symbols` retention from parse** | 1 d | TW symbols may be interned in TW's symtab; v3 has its own globalSymbolTable | bindVars(ns, ns.staticBaseEnv) resolves SymbolIds.  Once v3 has these in `module`, are TW's parser-time symbols releasable? |
| **T4.4** | **Audit `ns.positions` retention** | 1 d | TW position info | bindVars resolves positions.  v3 has its own positionPool.  Same question as T4.3. |
| **T4.5** | **Audit TW's per-file parser cache** | 1 d | TW's `evalState.fileExprCache` (or similar) keys parsed files | If TW caches parsed Expr for re-imports, that retention compounds on multi-file evals (8179 imports on M5). |
| **T4.6** | **Release TW parser-cache entries after disk_cache insert** | 1-2 d | depends on T4.5 yield | After CU lands in disk_cache + cache.cus, the TW Expr is fully redundant. |

### Tier 5 — Long-term (v3-native parser)

| # | Action | Effort | Yield | Status |
|---|---|---|---|---|
| **T5.1** | **Design v3-native parser** | multi-week | parse-time TW dependency removed entirely | Bypass `ns.parseExprFromFile` with a hand-written or generated parser. |
| **T5.2** | **Replace `parseExprFromFile` calls** | dependent on T5.1 | structural | All TW parse calls swap to v3 parser. |
| **T5.3** | **v3 lower from v3 AST directly** | dependent on T5.1 | no Expr→IR walk required if v3 AST IS the IR | Could skip lower.cc entirely; v3 parser emits IR directly. |

NOT in scope for this session — multi-month design + implementation.

### Tier 6 — Cleanup / housekeeping

| # | Action | Effort | Yield | Status |
|---|---|---|---|---|
| **T6.1** | **Update `kSiteNames[16]` in run.cc** to retire dead slots (12 = BP3) | 0.1 d | code hygiene | Already partially done by Phase A1 ("dead_slot_12_was_BP3_force_list_elem"). |
| **T6.2** | **Update USAGE.md** to remove BP3 doc references | 0.1 d | doc accuracy | Was line 197 ref. |
| **T6.3** | **Update FFI_BRIDGE_INVENTORY** with current empirical numbers | 0.5 d | strategic doc accuracy | Per FFI_AUDIT_2026-06-01 §0.1 the inventory is superseded for frequencies. |
| **T6.4** | **Retire `g_bridgeForceListElemCalls` references in run.cc kSiteNames** | done | — | Phase A1 renamed to dead_slot_12. |

## 3. Execution order (this session + queued)

### This session

1. **T1.1 — Retire `v3BridgeLists()` table** (0.5 d)
2. **T2.1 — D1 dedupe** (1-2 d, but core impl is small)
3. **T1.4 — primImport string-ctx audit** (0.5 d investigation)
4. **T4.1 — TW retention after lowerNixExpr audit** (1 d investigation)
5. **Plan T2.2 (BP1 lazy)** as next session's centerpiece

### Pre-committed acceptance (each tier)

Per `[[measure-twice-cut-once]]`:

* T1.1 acceptance: build clean + correctness gate + 0 reader-site assertions on non-empty
* T2.1 acceptance: M5 bridge entries ≤ 5500 (~50% reduction from 10061) + wall regression ≤ 1% + correctness gate
* T1.4 acceptance: realisePath direct-primitive overload available, OR documented as not feasible
* T4.1 acceptance: enumerate TW retention sources; quantify per-import retention if any

### Falsification criteria

* T1.1: if any reader site relies on bridge-list iteration semantics → REVERT
* T2.1: if hit rate <10% on M5 → REVERT (lookup overhead exceeds win)
* T1.4: if realisePath has no primitive overload → DOCUMENT and defer
* T4.1: if TW retention is structurally unavoidable → DOCUMENT and pursue v3-native parser (T5.1)

## 4. Strategic framing

### 4.1 Why this ordering

Per goal hierarchy §0:

* Tier 1 reduces SITE COUNT → strongest direct alignment with "few FFI calls"
* Tier 2 reduces per-call cost → "slim FFI calls"
* Tier 3 makes leaves strict-primitive → "we already know which values"
* Tier 4 closes the AST-parse escape hatch → "throw away TW after"
* Tier 5 eliminates even the parse path → "construct V3 directly"

### 4.2 Why deferring some

* T2.2, T2.3 (BP1/BP2 lazy result) are LARGE (1-2 wk each) but high-yield.  Plan for next session.
* T5 (v3-native parser) is multi-month — strategic, not tactical.
* T4 audit (Tier 4) is 1-day investigations each, but the FIXES may be small or large depending on findings.

### 4.3 Why this is different from past plans

Per `[[chain-bindings-phase-c-falsified]]`: previous Phase C attempts pursued one architectural lever to ship; falsified 5×.  This plan targets multiple LOW-RISK items + deferred high-yield items.  No single-bet ship gate.  Each Tier-1 item independently small + correctness-preserving.

## 5. Honest limits

* **Tier 2.2/2.3 are 1-2 weeks each** and may falsify if lazy bridging has correctness edge cases I can't predict from the audit.
* **Tier 3.1 (realisePath primitive overload)** may not be feasible if `realisePath` is intrinsic to nix::EvalState's coercion machinery and unrelocatable.
* **Tier 4 (TW retention audit)** may discover that TW's parser cache is unbounded; remedy may require upstream patch.
* **Tier 5 (v3 parser)** is a multi-month effort that competes with other v3 work for engineering bandwidth.

## 6. Cross-references

* `FFI_AUDIT_2026-06-01.md` — the empirical audit this TODO derives from
* `FFI_BRIDGE_INVENTORY_2026-05-31.md` — structural inventory (still current for layout)
* `FFI_KILL_PLAN_2026-05-31.md` — Phase A/B/C/D plan (superseded by this TODO for tactical work; the phase definitions remain)
* `EXIT_PHASE_AC_LANDED_2026-06-01.md` — Phase A + C6 lands
* `BRIDGES_HOLD_RETENTION_2026-05-29.md` — 99.8% retention measurement
* `GC_AND_MEMORY_ACCOUNTING_AUDIT_2026-05-31.md` — peak_rss = ru_maxrss monotonicity (any current-RSS lever won't shift peak)
* `[[falsification-rule]]`, `[[measure-twice-cut-once]]`, `[[memory-first-class]]`

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
