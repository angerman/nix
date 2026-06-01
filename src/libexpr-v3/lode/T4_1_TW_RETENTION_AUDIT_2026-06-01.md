# T4.1 — TW retention after `lowerNixExpr` (investigation)

**Date:** 2026-06-01
**Per:** FFI_KILL_TODO_2026-06-01 Tier 4 ("throw away TW after parse")
**Status:** INVESTIGATION COMPLETE — structural retention identified; full clean-up requires v3-native parser (T5.1)

---

## 1. What was investigated

Per the user's stated goal §0.4: "Even if we need to do the AST parse TW→V3, then we want to throw away the TW stuff right after we have the V3 in hand."

Audit: after `auto module = lowerNixExpr(e, ns.symbols, ns.positions)` at `primops.cc:9214`, what does TW retain that v3 doesn't need?

## 2. TW state involved in parse (from upstream `src/libexpr/eval.cc:3647-3669` + `eval.hh`)

```cpp
Expr * EvalState::parse(...) {
    auto result = parseExprFromBuf(
        text, length, origin, basePath,
        mem.exprs,        // ← Expr nodes allocated INTO this
        symbols,          // ← SymbolTable mutated (new symbols interned)
        settings, positions,  // ← PosTable mutated (new positions interned)
        *tmpDocComments, rootFS);
    result->bindVars(*this, staticEnv);  // ← installs StaticEnv refs
    positionToDocComment->emplace_or_visit(...);  // ← per-file doc map
    return result;
}
```

TW state populated by each `parseExprFromFile`:
1. **`mem.exprs`** — arena holding ALL parsed Expr nodes for the EvalState lifetime
2. **`symbols`** (SymbolTable) — interned symbol names from parse
3. **`positions`** (PosTable) — interned source positions
4. **`positionToDocComment`** — per-`SourcePath` doc-comment map
5. **`fileEvalCache`** — `SourcePath → Value*` cache.  v3 does NOT populate this (v3 doesn't call TW eval) — empty under v3-direct mode.
6. **`rootFS`** + **`lookupPath`** + various FS state

## 3. What v3 duplicates

After `lowerNixExpr`:
- v3 `globalSymbolTable` interns symbol names (independent of TW's `symbols`)
- v3 `posSnapshotPool` interns positions (independent of TW's `positions`)
- v3 `CompilationUnit` contains bytecode + IR descriptors (no Expr ref)

Per `bench/baselines/2026-05-31-post-app3-combined/hne-raw.txt`:
- v3 `globalSymbolTable`: 71165 entries / 7.1 MB
- v3 `posSnapshotPool`: 753512 entries / 63.7 MB

Likely TW-side: equivalent or larger (TW's symbol/position tables are unbounded; no eviction in TW EvalState lifetime).

## 4. Why TW retention is structurally hard to release

### 4.1 Expr nodes (mem.exprs)

* Allocated via TW's `mem.exprs` arena (per parse.y parser implementation).
* After `lowerNixExpr` returns, `e` (the root) is no longer referenced FROM V3.
* BUT: setting `e = nullptr` doesn't free anything — `mem.exprs` is the OWNER of the Expr nodes, not v3.
* `mem.exprs` lives as long as the EvalState.  v3 doesn't have a "clear parsed Exprs" API on EvalState.
* To clear: we'd need to either (a) retire the entire EvalState after each parse (impossible — store/fetcher state lives there too) OR (b) add an upstream API `EvalState::releaseParsedExprs(SourcePath)`.

### 4.2 Symbols (TW SymbolTable)

* `parseExprFromBuf` mutates `symbols` (interns new names).
* Symbols persist for EvalState lifetime.
* Per-import release would require: track which symbols were added per-parse; release them after lower; ensure no other Expr references them.
* But: bindVars(staticEnv) installs SymbolId references in Expr nodes.  Those references persist as long as the Expr does.

### 4.3 Positions (TW PosTable)

* Same retention shape as symbols.
* Per-import release similarly entangled.

### 4.4 fileEvalCache

* In v3-direct mode (no TW eval), this stays EMPTY.  No retention from v3's path.
* Verified: HNE end-of-eval has 0 entries in `fileEvalCache` per `clearPostEvalGlobalRoots`.

### 4.5 positionToDocComment

* Populated per-parse.
* Lifetime same as positions.
* Minor footprint vs Expr nodes.

## 5. Verdict

**TW retention after `lowerNixExpr` is STRUCTURALLY UNAVOIDABLE without one of:**

1. **Upstream API: `EvalState::releaseParsedExprs(SourcePath)`** — clears `mem.exprs` for a specific file's Expr subtree.  Requires upstream Nix patch.  Not feasible from v3 alone.

2. **Per-parse EvalState** — spin up a fresh EvalState for each parse, then destroy it.  But destroying EvalState breaks Store/Fetcher/positions/symbols continuity — those need to be SHARED across parses (positions span all files; symbols are interned globally).

3. **v3-native parser (T5.1 in FFI_KILL_TODO)** — replace `parseExprFromFile` with a v3-written parser.  v3's parser writes directly into v3's globalSymbolTable + posSnapshotPool; TW's `mem.exprs` / `symbols` / `positions` are never populated.

Only option 3 is actionable from v3.  Per FFI_KILL_TODO Tier 5: deferred multi-month effort.

## 6. What this DOES kill

* **"We can release TW Expr nodes after lowering by setting local pointer to nullptr"** — KILLED.  `mem.exprs` is the owner; v3 doesn't own.
* **"Per-import retention release is a small targeted fix"** — KILLED.  Requires upstream API change OR per-parse EvalState (not feasible) OR v3 parser.

## 7. What this DOESN'T kill

* **The IDEA that v3 should eventually have its own parser** — preserved as T5.1.  Strategic, multi-month.
* **Whole-program release at end-of-eval** — `clearPostEvalGlobalRoots` already clears v3-side bridge tables + import-cache results.  TW's mem.exprs is still alive (EvalState owns it) but no longer reachable from v3-side after the post-eval clear.

## 8. Honest limits

* **TW's `mem.exprs` size on real workloads is UNMEASURED.**  Could be 10 MB or 1 GB depending on parse counts.  M5 has 8179 imports — substantial parse work.  Per-parse Expr bytes are unknown without instrumentation.
* **`mem.exprs` may itself be a Boehm arena** — releasing pages would require Boehm-side support, not v3 control.
* **The `peak_rss = ru_maxrss` monotonicity constraint** (per GC_AND_MEMORY_ACCOUNTING_AUDIT) means even if TW retention is high, addressing it post-peak doesn't help the watchdog.

## 9. Recommendation

* Mark T4.1 - T4.6 as **deferred behind T5.1** (v3-native parser).
* The "throw away TW after parse" goal can ONLY be fully achieved by eliminating TW from the parse pipeline.
* Until then, accept that TW retains `mem.exprs` + `symbols` + `positions` + `positionToDocComment` for the EvalState lifetime.
* `fileEvalCache` is already empty in v3-direct mode (a good outcome).

## 10. Cross-references

* `primops.cc:9203-9214` — parse + lower call site
* `/Users/angerman/Projects/iohk/nix/src/libexpr/eval.cc:3647-3669` — TW's `parse()`
* `/Users/angerman/Projects/iohk/nix/src/libexpr/include/nix/expr/eval.hh` — `mem.exprs` / `symbols` / `positions` fields
* `FFI_KILL_TODO_2026-06-01.md` Tier 4 (T4.1-T4.6) + Tier 5 (T5.1)
* `FFI_AUDIT_2026-06-01.md` §4 — TW retention as the "throw away after parse" goal
* `GC_AND_MEMORY_ACCOUNTING_AUDIT_2026-05-31.md` §4.1 — peak_rss = ru_maxrss monotonicity

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
