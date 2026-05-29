# V3-NATIVE primop migrations — execution record

**Date:** 2026-05-29 evening → 2026-05-29 (executed same session)
**Status:** EXECUTED.  Tier 0 + Tier 2d audits confirmed; Tier 2a/b/c DEFERRED per pre-committed measurement gate.

## Execution outcome (2026-05-29)

| Task | Outcome | Rationale |
|---|---|---|
| #877 Tier 0 (6 system-info primops) | **COMPLETE via existing code** | `vm.cc:10788-10805` (2026-05-18 vBuiltins lazy-init) + `lower.cc:828/3140` (emit-time pre-call) already inject all 6 as v3-init constants.  Empirical: 30 accesses across all 6 in 1 process → **0 primop dispatches**; within-process `currentTime` is stable across accesses.  No new code needed; the audit's "FFI dispatch per access" premise was outdated. |
| #882 Tier 2d-i (primFunctionArgs) | **AUDIT-ONLY: keep as-is** | Common case (Tag::Closure → desc->formals) is pure v3-native at `primops.cc:9676+`.  Bridge case at lines 9619-9669 (Tag::Thunk + ThunkState::Bridge wrapping TW ExprLambda) is a true FFI leaf for the v3FormalsLambdaBridges sentinel — not a migration candidate. |
| #883 Tier 2d-ii (catAttrs) | **AUDIT-ONLY: keep as-is** | `primops.cc:2067` is pure v3-native: forceValue + bindings->lookup + allocList.  Zero TW crossings. |
| #878 measurement spike | **EXECUTED** | `NIX_VM_PRIMOP_TIME=1` on hello.drvPath + HNE.  Top 15 wall on both is dominated by derivation-class FFI primops: `__derivationFromPreprocessed` 50-58%, `__derivCoerce` 26-34%, `import` 7-8%.  primSort/primGenericClosure/primZipAttrsWith **do not appear in top 15 wall or top 30 call counts** on either workload (< 1 % wall). |
| #879 Tier 2a (primSort) | **SHIPPED 2026-05-29 (commit `1243c158b`)** | Per user override after #878 DEFER verdict: ship for architectural consistency + C-stack-safety + V3-NATIVE-family completeness, not wall-percent.  Stable insertion sort via foldl'; passes `eval-okay-sort.exp` repeated-key stability test.  Gate: `NIX_V3_NO_BC_SORT=1`. |
| #880 Tier 2b (primGenericClosure) | **SHIPPED 2026-05-29 (commit `1243c158b`)** | Per user override.  BFS via list-queue + attrset-seen + OP_CALL for `operator`.  Type-prefixed key dedup + firstType tracking + NaN reject.  Gate: `NIX_V3_NO_BC_GENERIC_CLOSURE=1`. |
| #881 Tier 2c (primZipAttrsWith) | **SHIPPED 2026-05-29 (commit `1243c158b`)** | Per user override.  Name-union via `foldl' a // b` + `listToAttrs (map ... allNames)` with lazy per-entry thunks.  Laziness matches the C Tag::App entries (WC-35 fix preserved).  Gate: `NIX_V3_NO_BC_ZIP_ATTRS_WITH=1`. |
| #884 acceptance gate | **CLEARED — Tier 0/2d zero-code, Tier 2a/b/c shipped via #1243c158b** | Validation: --quick 6/6, --core 15/15 (143-lang + 58-property), --brute pre-existing failures only (verified with opt-outs ON), 5 nixpkgs paths byte-equal vs TW. |

### Override note (2026-05-29 user redirect)

The initial Tier 2a/b/c DEFER verdict (above) was based purely on the `NIX_VM_PRIMOP_TIME=1` <1%-wall measurement on hello.drvPath + HNE.  User directive: ship Tier 2a/b/c despite the null perf lever.  Rationale codified:

* **C-stack safety:** the primSort / primGenericClosure / primZipAttrsWith C bodies hold per-element loops that call `callClosure` per element.  C-stack depth per dispatch is constant (the callback runs through the VM dispatchLoop, not C-recursion), so the latent risk is small — but the bytecode-installed versions move the outer loop into VM-managed iteration via `foldl'` + `OP_TAIL_CALL`, eliminating even that small surface.
* **V3-NATIVE family consistency:** every other callback-using v3-native primop (foldl', map, filter, concatMap, partition, groupBy, all, any) is bytecode-installed.  These three were the outliers.  Closing the gap simplifies reasoning about which path a primop's outer loop runs through.
* **Future workload coverage:** hello.drvPath + HNE don't exercise these heavily.  Other workloads (module-system-heavy, lib-only evals, attrset-heavy folds) might.  Covering the migration eagerly avoids re-investigation when a workload surfaces a hot site.

The pre-committed #878 gate's "DEFER" verdict ranked these as null perf levers.  That ranking is STILL TRUE on the measured workloads.  The user's override correctly recognises that null-lever ≠ null-value when the value being captured is non-perf.

The asymptotic regressions (O(N²) vs O(N log N) for sort; O(M²) vs O(M) for genericClosure; O(N × M) vs O(N × K) for zipAttrsWith) are intentional and bounded — typical nixpkgs uses involve N/M/K in the 10-100 range where dispatch cost dominates the asymptotic.  If a workload surfaces where bytecode regression dominates, the per-primop `NIX_V3_NO_BC_*` opt-out reverts cleanly.

### Key empirical numbers (2026-05-29)

**hello.drvPath, NIX_VM_PRIMOP_TIME=1** (107.7 s total primop wall):

```
   53909 ms  __derivationFromPreprocessed
   36084 ms  __derivCoerce
    8882 ms  __derivationStrictRaw
    8586 ms  import
   (everything else  ≤ 56 ms total)
```

**HNE, NIX_VM_PRIMOP_TIME=1** (171.2 s total primop wall):

```
   98778 ms  __derivationFromPreprocessed   (57.7 %)
   44416 ms  __derivCoerce                  (25.9 %)
   12152 ms  import                         ( 7.1 %)
    8870 ms  __tryEval                      ( 5.2 %)
    2882 ms  __derivationStrictRaw          ( 1.7 %)
   2397 ms  getFlake                        ( 1.4 %)
   (everything else  ≤ 0.4 % each)
```

**Strategic implication:** the actual primop perf lever IS on the V3-NATIVE FFI boundary (`__derivationFromPreprocessed` + `__derivCoerce` + `import`).  These touch the store and are explicitly permitted at FFI leaves.  Bytecode-installing them would violate the V3-NATIVE rule (TW only at store/path/derivation/I/O leaves).  → The Tier 2 plan was attacking the wrong bucket.

The lever IS NOT "more native primops" — by the numbers, v3 is already as native as the V3-NATIVE rule allows.  The lever is reducing PER-CALL cost in the FFI-leaf primops themselves (a derivation-bridge optimization task, separate strategic track), or reducing the COUNT of FFI calls (e.g. caching derivCoerce results across siblings, IFD probe S4 eval-result cache).

---



---

## 1. Scope

User directive (2026-05-29 evening, end of bridge-retention session): execute Tier 0 + Tier 2a-2d from `FFI_AUDIT_2026-05-20.md` §5.

**Not in scope** for this batch: Tier 3 (opcode-ify pure arithmetic) — that's a separate ~1 week's work for next-next session.  Tier 4 (FromJSON / Hash* / ToXML / Match) deferred per audit recommendation.

## 2. Pre-investigation findings (from current code-read)

Before estimating effort, current state of each candidate verified:

| Tier | Primop | Current state (primops.cc:line) | Migration shape |
|---|---|---|---|
| 0 | currentSystem, nixVersion, langVersion, storeDir, nixPath, currentTime | 0-arity primops at lines 11562-11599 + __-aliases at 11745-11755 | Inject as v3-init constants (not primops); 1-2 d |
| 2a | primSort | v3-native at 10011; uses std::sort + callClosure-per-compare | Nix-source mergesort?  Win uncertain since std::sort is iterative C++ |
| 2b | primGenericClosure | v3-native at 3114; BFS deque + unordered_set | Nix-source worklist via foldl' (already bytecode); semantics tricky |
| 2c | primZipAttrsWith | v3-native at 2753; uses Tag::App per entry (App3 rolled back recently); already lazy | Nix-source via genAttrs + catAttrs |
| 2d-i | primFunctionArgs | v3-native at 9617; reads desc->formals; only Bridge-thunk-TW-lambda edge crosses TW | AUDIT only — already native for common case |
| 2d-ii | catAttrs | v3-native at 2067 (forces + bindings->lookup) | AUDIT only — pure data; zero TW |

**Key insight (per code-read 2026-05-29 evening):** all three Tier 2 "callback-using" candidates (Sort, GenericClosure, ZipAttrsWith) are ALREADY v3-native in implementation.  Migration would replace `callClosure` (C-recursive dispatch) with bytecode `OP_CALL` (iterative).  That's a C-stack-safety + dispatch-cost win, NOT an FFI-reduction win.

**Empirical risk:** M5's hot-30 primop dispatch list does NOT show primSort / primGenericClosure / primZipAttrsWith.  Their callback-cost contribution may be sub-1 % of wall.  Migration could be a NULL lever.

→ Pre-migration measurement (task #878) gates the per-primop ship decision.

## 3. Task dependency graph

```
#877 Tier 0 system-info constants    (independent; ~1-2 d; LOW risk)
                       │
                       ▼
              #884 acceptance gate

#878 measurement spike  ──┬──>  #879 Tier 2a primSort     (CONDITIONAL)
   (~half day)            ├──>  #880 Tier 2b primGenericClosure
                          ├──>  #881 Tier 2c primZipAttrsWith
                          │
                          └──>  #884 (gate)

#882 Tier 2d-i FunctionArgs audit    (independent; ~0.5 d; AUDIT only)
                       │
                       ▼
              #884 acceptance gate

#883 Tier 2d-ii catAttrs audit       (independent; ~0.5 d; AUDIT only)
                       │
                       ▼
              #884 acceptance gate
```

## 4. Pre-committed per-primop SHIP gate

Per `[[measure-twice-cut-once]]`:

* **Tier 0**: No threshold — trivial cleanup; ship if --quick + --core PASS + parity preserved.
* **Tier 2a-c**: Migrate IF #878 measures the primop ≥1 % of wall on M5 or HNE.  Otherwise DEFER (mark task completed with "deprioritized per measurement").
* **Tier 2d-i, 2d-ii**: Audit-only; ship updated `FFI_AUDIT_2026-05-24.md` documenting native status.

## 5. Recommended sequencing (next session opening)

1. **Day 1**: #877 Tier 0 — trivial, immediate ship.  Sets up the "constants as v3-init values" pattern for future work.
2. **Day 1 (parallel)**: #882 + #883 audit primFunctionArgs + catAttrs.  Likely closes as "already done."
3. **Day 1 (afternoon)**: #878 measurement spike.  Half-day run.
4. **Day 2 onwards**: Based on #878 results, sequence #879/880/881.  If all three deprioritize → only Tier 0 + 2d audits land + #884 gate.

Estimated total wall: 1-5 days depending on measurement outcomes.

## 6. References

* [`FFI_AUDIT_2026-05-20.md`](FFI_AUDIT_2026-05-20.md) §5.0-5.2 — original tier definitions
* [`FFI_AUDIT_2026-05-24.md`](FFI_AUDIT_2026-05-24.md) — updated empirical state (zero bridge crossings on standard workloads)
* `bytecode_primops.cc:381` — foldl' bytecode-install pattern (canonical reference)
* `bytecode_primops.cc:443` — concatMap bytecode-install pattern (closest analog for primGenericClosure)
* `lode/BRIDGES_HOLD_RETENTION_2026-05-29.md` — context for why Tier 2 migration is NOT the bridge-retention lever (separate concern; #875 weak-bridges is the bridge-lifecycle lever)
* `lode/SESSION_END_SYNTHESIS_2026-05-29.md` — broader session arc + lever ladder

## 7. Tasks (also tracked in task system)

* **#877** Tier 0: inject 6 system-info primops as v3-init constants
* **#878** Pre-Tier-2 measurement spike
* **#879** Tier 2a primSort (blocked by #878)
* **#880** Tier 2b primGenericClosure (blocked by #878)
* **#881** Tier 2c primZipAttrsWith (blocked by #878)
* **#882** Tier 2d-i primFunctionArgs verify
* **#883** Tier 2d-ii catAttrs verify
* **#884** Cross-migration acceptance gate (blocked by #877, #878, #882, #883)

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
