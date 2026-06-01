# FFI_KILL_TODO status — checkpoint at parser-project pivot

**Date:** 2026-06-01 (afternoon)
**Per:** user directive "save the remaining tasks to the todo list, and then pivot to the .nix → v3 ast parser"
**Status:** SNAPSHOT — captures completed / falsified / deferred items from `FFI_KILL_TODO_2026-06-01.md` before the strategic pivot to v3-native parser

---

## 1. Items LANDED in this session (commits)

| Tier | Item | Commit | Outcome |
|---|---|---|---|
| T1.3 | `primTrace` v3-native printer | `2acbca5a7` | slot 10 eliminated; v3 ValuePrinter via `printNixValueRich` |
| T1.4 | `primImport` string-ctx inline TW Value alloc | `8c6a6eb0f` | slot 2 eliminated; -10 v3→TW on HNE |
| T1.4-rd | `primReadDir` string-ctx inline TW Value alloc | `2acbca5a7` | slot 4 eliminated; -2 v3→TW on HNE |
| (new) | **Install-bridge elimination** | `f3569630d` | **-14 entries on ALL workloads (1+2: 14→0; HNE: 33→19; M5: 10061→10047)** |

**Cumulative measurement (HNE):**
* v3→TW call count: 74 → 62 (-12, ~16%)
* Bridge entries at end-of-eval: 33 → 19 (-42%)
* drvPath byte-equal across hello / firefox / python3 / HNE / M5
* Lang tests 143/143 PASS
* Wall: within noise
* v3_arena: 1476.4 MB (unchanged — bridge entries are 64 B each)

## 2. Items FALSIFIED

| Tier | Item | Outcome doc | Falsified hypothesis |
|---|---|---|---|
| T2.1 | D1 dedupe in `v3ToTreeWalker` | `8c6a6eb0f` commit | "Same v3 Closure bridged twice is common on M5" — measured 0/10047 hits (every M5 closure is unique).  HNE got 18%/9% hits but below ≥25% M5 gate. |

## 3. Items DEFERRED

| Tier | Item | Reason | Re-open condition |
|---|---|---|---|
| T1.1 | Retire `v3BridgeLists()` table | 17 reader sites; pure code-hygiene; risk-vs-yield poor | If a future bridge-rewrite work touches these sites anyway |
| T1.2 | Retire `primDerivationStrict` fake-store path | Load-bearing for `v3-eval` tests (no nixEvalState) | Reroute test harness to wire up a stub nixEvalState |
| T1.5 | Retire BP1/BP2 fallbackExpr re-eval paths | Load-bearing for M5's 18-layer overlay chain (cardano-node) | If overlay chain becomes v3-native (eliminates the depth-bound trigger) |
| T2.2 | BP1 lazy result marshalling | 1-2 wk architectural; HIGH risk on M5's 20034-call chain | Post v3-parser project (frees engineering bandwidth) |
| T2.3 | BP2 lazy result marshalling | 1-2 wk architectural | Same |
| T2.4 | Fetcher flat-arg variants | Per-fetcher 1-2 d; fetchers idle on cache-hit so yield is small | Bundle if other strict-leaf work bundles |
| T2.5 | `Kind::LazyTW` Bindings flavor | Multi-week architectural; 5-pivot risk (per Chain Bindings history) | Not recommended |
| T3.1 | `realisePath(path_str, ctx)` primitive overload | TW API constraint — `realisePath` takes `nix::Value &`, no primitive overload exists upstream | Requires upstream patch OR custom v3 wrapper that constructs the minimal TW Value (already done at the inline sites today) |
| T3.2 | Audit all `state.nixEvalState->` calls | All already minimal per FFI_AUDIT §1.3 | — |
| T4.1 | TW retention after `lowerNixExpr` audit | Structurally blocked behind v3-native parser (T5.1) | See `T4_1_TW_RETENTION_AUDIT_2026-06-01.md` |
| T4.2-6 | Per-parse TW state release | Same as T4.1 | — |

## 4. Items PROMOTED to active project

| Tier | Item | Promotion |
|---|---|---|
| **T5.1** | **v3-native parser** | **PROMOTED 2026-06-01 → see `PARSER_PROJECT_PLAN_2026-06-01.md`** |
| **T5.2** | Replace `parseExprFromFile` call sites (6 of them) | Part of v3-native parser project |
| **T5.3** | Direct `.nix → v3 IR` (skip `nix::Expr`) | Part of v3-native parser Stage 2 |

## 5. The arithmetic of remaining wins

Per the user's goal hierarchy:

### 5.1 What CAN still be done without parser-project effort

* **Tier 1 (eliminate sites)**: nothing material remains.  T1.1 / T1.2 / T1.5 are either deferred or load-bearing.  All structurally-eliminable v3→TW sites in the histogram have been retired.
* **Tier 2 (slim calls)**: T2.2 / T2.3 are the high-yield remaining items but 1-2 weeks each + risk.  Deferring per single-bet-risk discipline.
* **Tier 3 (strict leaves)**: T3.1 blocked on upstream API.  T3.2/3.3 confirmed clean.
* **Tier 4 (TW release)**: T4.1-6 blocked behind T5.1.

### 5.2 Net remaining surface (post Phase 1-3)

After today's commits, v3→TW marshalling on HNE:
* Slot 1 (`primReadDir_attrset`): 3 calls (rare v3-native fallback)
* Slot 7 (BP1 result): 8 calls
* Slot 11 (BP2 result): 51 calls

Bridge entries on HNE: 19 (down from 33).
Bridge entries on M5: 10047 (down 14 from 10061).

The bulk-retention sources (BP1/BP2 result paths) remain.  These are addressable only by T2.2/T2.3 (architectural lazy result marshalling) OR by eliminating the v3↔TW callback pattern entirely (which is what the v3-native parser project enables long-term).

### 5.3 Why pivot to parser now

The TODO doc §4.1 ordering: Tier 4 (release TW after parse) was blocked behind Tier 5 (v3-native parser).  Per `T4_1_TW_RETENTION_AUDIT_2026-06-01.md`: full TW retention release requires replacing the parser.

The remaining FFI items are either:
* Small marginal yield (T2.4 fetcher flat-args) — bundled if/when other work touches them
* Architectural high-risk (T2.2/T2.3 lazy result) — better to address AFTER parser project removes one variable
* Load-bearing for current FFI surface (T1.5 fallback)

**The strategic pivot is justified** when the cheap FFI cleanups are exhausted (they are, post Phase 1-3) and the next-bigger lever requires parser-level surgery.

## 6. Resumption points

If a future session wants to resume FFI work without the parser project:

1. **T2.2 BP1 lazy result marshalling** — primops.cc:4908.  Replace `v3ToTreeWalker(state, out)` with a deferred-bridge mechanism.  Risk: M5 20034-call chain correctness.  Recommendation: scope to a measured subset of BP1 paths.
2. **T2.4 fetcher flat-args** — primops.cc:11829-11857.  Per-fetcher 1-2 d.  Yield small on cache-hit.
3. **T1.5 BP1/BP2 fallback retirement** — investigate whether ForceChainGuard + Phase D barriers cover all cases the fallbackExpr was designed for.  Risk: M5 18-layer overlay chain.

## 7. Cross-references

* `FFI_KILL_TODO_2026-06-01.md` — original TODO
* `FFI_AUDIT_2026-06-01.md` — empirical audit
* `FFI_BRIDGE_INVENTORY_2026-05-31.md` — structural inventory
* `T4_1_TW_RETENTION_AUDIT_2026-06-01.md` — why Tier 4 is blocked
* `NATIVE_PARSER_FEASIBILITY_2026-06-01.md` — parser-project feasibility (the pivot anchor)
* `PARSER_PROJECT_PLAN_2026-06-01.md` — active parser project (this commit)
* Session commits: `2acbca5a7`, `8c6a6eb0f`, `f3569630d`

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
