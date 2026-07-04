# Comprehensive TW FFI bridge inventory + marshalling minimality audit

**Date:** 2026-05-31
**Author:** session synthesis (3-agent deep enumeration + marshalling analysis + delta-vs-prior-audits cross-check + critical review)
**Status:** AUTHORITATIVE CURRENT-STATE INVENTORY — supersedes `FFI_AUDIT_2026-05-20.md` and `FFI_AUDIT_2026-05-24.md`. Captures every v3 ↔ TW bridge site as of 2026-05-31 with marshalling details and minimality assessment.
**Triggering question:** "Carefully and thoroughly review what TW bridges are left that the VM calls out to via FFI. Also do we have a clear marshalling of the _minimally_ required data for each?"

Companion docs:
- [`FFI_AUDIT_2026-05-20.md`](FFI_AUDIT_2026-05-20.md) — original 4-tier audit (104 sites)
- [`FFI_AUDIT_2026-05-24.md`](FFI_AUDIT_2026-05-24.md) — post-#795 empirical update
- [`BRIDGE_TELEMETRY_2026-05-26.md`](BRIDGE_TELEMETRY_2026-05-26.md) — 0.014% wall measurement
- [`BRIDGES_HOLD_RETENTION_2026-05-29.md`](BRIDGES_HOLD_RETENTION_2026-05-29.md) — 99.8-99.9% retention finding
- [`WEAK_BRIDGE_EVICTION_DESIGN_2026-05-29.md`](WEAK_BRIDGE_EVICTION_DESIGN_2026-05-29.md) — Stage 0-2 landed
- [`GC_STRATEGY_INTEGRATED_2026-05-30.md`](GC_STRATEGY_INTEGRATED_2026-05-30.md) — yesterday's 6-layer strategy (this audit refines Layer 3)

---

## 1. TL;DR — the headline framing

**The bridge inventory's static-count metric (104 → ~92-115) is the WRONG metric.** Bridge crossings on standard workloads (hello / firefox / gcc / bash / python3 / synthetic-IFD) are now **zero**. Bridge wall time on HNE is **0.014%**. The strategic frontier is no longer "shrink the bridge surface" — it is:

1. **Bridge RETENTION**: bridge tables hold 99.8% of HNE live arena, 99.9% of M5.
2. **Bridge MARSHALLING SHAPE**: when bridges DO fire, they marshal the full transitive subgraph instead of only the fields TW actually consumes (`DrvStrictSymbols` 17 fields vs ~28-entry input attrsets).
3. **Hidden bridges missed by prior audits**: `__derivationFromPreprocessed` (50-58% of primop wall) + `__derivCoerce` (26-34%) are FFI leaves that the original tier inventory didn't enumerate but now dominate the primop wall on hello.drvPath + HNE.

**The single most actionable finding:** complete the `primDerivationStrictNative` migration to eliminate the largest bridge entry class. Effort ~1-2 weeks. Yield: most of the 311 MB at `all-packages.nix:9112` bridge retention disappears at source.

---

## 2. The comprehensive bridge inventory

### 2.1 Bridge table infrastructure (the data structures)

The bridge surface is THREE persistent process-lifetime tables + side helpers:

| Table | Accessor | Defined | Per-entry struct | What it holds |
|---|---|---|---|---|
| **`v3BridgeClosures()`** | `primops.cc:3950-3956` | `BridgeClosureEntry` `primops.cc:3930-3941` | `Value v3Value` (Tag::Closure) + `fallbackExpr` + `lastAccessGen` + `accessCount` + `evicted` + `serializedBlob` | v3 Closures handed to TW |
| **`v3BridgeAttrs()`** | `primops.cc:4211-4217` | `BridgeAttrEntry` `primops.cc:3972-3980` | as above (Tag::Attrs) | v3 Bindings handed to TW |
| **`v3BridgeLists()`** | `primops.cc:4220-4226` | `BridgeListEntry` `primops.cc:3981-3989` | as above (Tag::List) | v3 ListVecs handed to TW |
| `v3FormalsLambdaBridges()` | `primops.cc:4257` | side-table | `Env*` → ClosureHandle | TW sentinel for formals lambdas |
| `bridge_root_registry` | `bridge_root_registry.cc` | `std::vector<void*>` per-thread | raw TW `nix::Value*` | Boehm root targeting for TW→v3 direction |

**Per-entry size:** ~64 B including SSO `std::string serializedBlob`.

**Critical:** entries are pushed monotonically (`handle = tbl.size()` at `primops.cc:5464, 5565, 5733`). **No de-duplication.** The same v3 Bindings bridged twice creates two distinct handles. `v3BridgeUniquePtrCounts` at `primops.cc:4369` measures this for diagnostics but is NEVER consulted to dedupe.

### 2.2 Bridge plumbing primops (TW → v3 direction, but listed here for completeness)

These three primops are how TW calls back INTO v3 when forcing a previously-bridged value. They are registered with TW as `__v3_call_bridge_1`, `__v3_force_attr`, `__v3_force_list_elem`.

| # | Primop | File:line | What it does | HNE call freq |
|---|---|---|---|---|
| BP1 | `primV3CallBridge1` | `primops.cc:4541-4917` | TW dispatch on bridged v3 Closure. Args: (handle:int, arg:nix::Value). Marshals arg via `treeWalkerToV3`, runs `callClosure`, bridges result back. Has cycle-guard + Blackhole→fallbackExpr re-eval. | 8 |
| BP2 | `primV3ForceAttr` | `primops.cc:4923-5178` | TW forces a single attr of bridged Tag::Attrs. Args: (handle:int, attrName:string). | 51 |
| BP3 | `primV3ForceListElem` | `primops.cc:5183-5400` | TW forces single list element. Args: (handle:int, idx:int). | 0 |

**BP3 is retirement candidate** (0 calls on all measured workloads). BP1/BP2 still load-bearing for `fetchTree`'s `type`-arg force (per [[head-5-counter-trap]] correction 2026-05-26).

### 2.3 The two conversion functions (the actual marshallers)

| # | Function | File:line | Direction | Eager/lazy | Notes |
|---|---|---|---|---|---|
| C1 | `v3ToTreeWalker` | `primops.cc:5320-5771` | v3 → TW | **Eagerly forces at line 5347 BEFORE switching on tag**; bridges sub-attrs/sub-lists LAZILY when size > 4 (`kEagerAttrMax/kEagerListMax`) | Recursive with `seen` set |
| C2 | `treeWalkerToV3` | `primops.cc:5834-6094` | TW → v3 | **Always shallow** since Phase F 2026-05-18; TW nList/nAttrs entries become v3 Bridge thunks pointing at TW `Value *` via `Thunk::bridgeSrc` | No bridge-table entry in this direction |
| C3 | `tryBridgeAttrLookup` | `primops.cc:11383` | bidirectional | Lookup-without-force fast path | Single-attr from TW attrset |
| C4 | `tryFastBridgeScalarTwToV3` | `primops.cc:11449` | TW → v3 | Skip-VMState fast path for Int/Float/Bool/Null | Direct mkXxx |

### 2.4 Primops that call TW (the v3 → TW bridge sites)

Categorized by status:
- `BRIDGE` = always TW via `bridgeBuiltin<N>` or hand-written
- `HYBRID` = v3-native first, falls through to TW on shape mismatch
- `LEAF` = calls libstore/libfetchers/EvalState API leaf; no v3↔TW Value marshalling
- `INJECT` = system-info constant via vBuiltins (no FFI; per Tier 0 work)
- `NATIVE` = entirely v3-native

#### Fetchers (8 primops — all BRIDGE)

| # | Primop | File:line | TW function | Args marshalled in | Result out |
|---|---|---|---|---|---|
| F1 | `__fetchurl` | `bridgeBuiltin<1>` reg at `primops.cc:11859` | `builtins.fetchurl` | url string OR attrset | string-with-context |
| F2 | `__fetchTarball` | reg at `11879` | `builtins.fetchTarball` | url OR attrset | store path |
| F3 | `__fetchTree` | reg at `11880` | `builtins.fetchTree` | input attrset (type, url, ref, rev, ...) | attrset (outPath + locks + lastModified + ...) |
| F4 | `__fetchGit` | reg at `11881` | `builtins.fetchGit` | url OR attrset | attrset |
| F5 | `__fetchMercurial` | reg at `11882` | `builtins.fetchMercurial` | url OR attrset | attrset |
| F6 | `__fetchClosure` | reg at `11883` | `builtins.fetchClosure` | attrset (fromStore, fromPath, ...) | string-with-context |
| F7 | `__filterSource` | reg at `11884` | `builtins.filterSource` | (filter:Closure, path) | path |
| F8 | `__fetchFinalTree` | reg at `11866` (uses `internalPrimOps`) | `internalPrimOps["fetchFinalTree"]` | flake input attrset | attrset |

**Marshalling pattern (all 8):** `bridgeBuiltin<N>` at `primops.cc:11829-11857`:
1. `nargs[i] = v3ToTreeWalker(state, args[i])` for each arg (eager full conversion)
2. `ns.callFunction(cur, *nargs[i], next, noPos)` invokes TW
3. `treeWalkerToV3(state, cur)` for result

**Justification (legitimate FFI leaves):** libfetchers is hundreds of KLoC of network + git + tarball-extract machinery. Recreating in v3 violates V3-NATIVE thin-FFI principle.

#### Store / derivation operations (4 primops)

| # | Primop | File:line | Status | Notes |
|---|---|---|---|---|
| S1 | `derivationStrict` | `6435-6663` | **HYBRID** | Native path `primDerivationStrictNative` at `6427`; TW fallback via `cachedDrvStrict` at `6650` for `__structuredAttrs`/`outputChecks`/content-addressed cases. **THIS IS THE HEAVIEST BRIDGE — see §3.1.** |
| S2 | `path` | `9901` | HYBRID | `primPathNative` for filter-less; TW `builtins.path` when filter set |
| S3 | `toFile` | `11709` | LEAF | `ns->store->addToStoreFromDump` + `makeFixedOutputPathFromCA` |
| S4 | `storePath` | `11675` | LEAF | `ns->store->isStorePath` + `ensurePath` + `toStorePath` |
| S5 | `outputOf` | `11776` | LEAF | `ns.coerceToSingleDerivedPath` + `mkSingleDerivedPathString` |

#### Path / file I/O (6 primops)

| # | Primop | File:line | Status | Notes |
|---|---|---|---|---|
| P1 | `import` | `8292-8357` | **LEAF + sometimes BRIDGE** | `ns.realisePath(noPos, tw)` for string-w-ctx; `v3ToTreeWalker(args[0])` for attrset arg (H7 native bypass for outPath at `0c2bc6b00`). HNE 9 calls. |
| P2 | `scopedImport` | `10112` | LEAF | Chains to primImport |
| P3 | `readFile` | `3505` | LEAF | `ns.realisePath` + `sp.readFile()` |
| P4 | `readDir` | `3616` | LEAF | H7 attrset bypass; legacy bridges via `v3ToTreeWalker` |
| P5 | `readFileType` | `3823` | NATIVE | `std::filesystem` |
| P6 | `pathExists` | `2943` | LEAF | `ns.realisePath` + `maybeLstat()` |
| P7 | `findFile` | `2700` | LEAF | `ns.findFile(lp, name)` for `<x>` syntax |
| P8 | `nixPath` | `2661` | LEAF | reads `state.nixEvalState->getLookupPath()` |
| P9 | `hashFile` | `3376` | LEAF | needs file I/O via TW only when restricted-eval |

#### Flake operations (3 primops — all migrated)

| # | Primop | File:line | Status | Notes |
|---|---|---|---|---|
| FL1 | `getFlake` | `11899-11947` | **HYBRID-NATIVE** | Pure native via `callFlakeV3` (`v3_call_flake.cc`); FFI leaves are `parseFlakeRef` + `lockFlake` + `emitTreeAttrs` (v3 port). Migrated by #758 (`856e319be`) + #701 Phase 4b |
| FL2 | `parseFlakeRef` | `9641` | NATIVE | Hand-rolled parser (github:/git:/path:) |
| FL3 | `flakeRefToString` | `9703` | NATIVE | Hand-rolled |

#### Type coercion / serialization (4 primops — all NATIVE)

| # | Primop | File:line | Status |
|---|---|---|---|
| T1 | `toXML` | `9612` | NATIVE — own walker |
| T2 | `toJSON` | `10451` | NATIVE — own walker |
| T3 | `fromJSON` | `10442` | NATIVE — nlohmann::json |
| T4 | `fromTOML` | `9864` | NATIVE — toml-cpp |

#### Eval-state introspection (4 primops)

| # | Primop | File:line | Status |
|---|---|---|---|
| E1 | `unsafeGetAttrPos` | `2229` | NATIVE — v3 PosTable; minimal |
| E2 | `addErrorContext` | `3841` | NATIVE |
| E3 | `addDrvOutputDependencies` | `2594` | NATIVE — v3 string-context side-table |
| E4 | `appendContext` | `2533` | LEAF — needs `tlNixEvalState` for ContextElem::parse |
| E5 | `getContext` | `2448` | LEAF — `tlNixEvalState` only for error display |

#### System info / constants (7 primops — all should be Tier 0 INJECT but partially NOT)

| # | Primop | File:line | Status | Tier 0 status |
|---|---|---|---|---|
| Y1 | `currentSystem` | `3442` | LEAF | NOT migrated to vBuiltins constant |
| Y2 | `currentTime` | `3458` | NATIVE | per-access acceptable |
| Y3 | `nixVersion` | `3463` | NATIVE | compile-time constant |
| Y4 | `langVersion` | `3477` | NATIVE | compile-time constant |
| Y5 | `storeDir` | `3487` | LEAF | NOT migrated |
| Y6 | `getEnv` | `1530` | NATIVE | `std::getenv` |

**Per Agent C:** FFI_AUDIT §5.0 Tier 0 ("inject system-info as Values at v3 init") was estimated 1-2 days; AS OF 2026-05-31 IT IS NOT DONE for Y1/Y5. They remain primops, though vBuiltins lazy-init at `vm.cc:10788-10805` reaches them via emit-time pre-call (per `V3_NATIVE_MIGRATIONS_NEXT_SESSION §877`). Empirically: 0 primop dispatches on hot paths.

#### Hash / regex (4 primops — all NATIVE)

| # | Primop | File:line | Status |
|---|---|---|---|
| H1 | `hashString` | `3363` | NATIVE — `nix::HashAlgorithm` |
| H2 | `convertHash` | `3397` | NATIVE — `nix::Hash` header-only |
| H3 | `match` | `3247` | NATIVE — `std::regex` |
| H4 | `split` | `3289` | NATIVE — `std::regex` |
| H5 | `parseDrvName` | `3746` | NATIVE — local C++ |
| H6 | `placeholder` | `11645` | NATIVE — `nix::hashPlaceholder` |

#### Trace / debug (4 primops)

| # | Primop | File:line | Status |
|---|---|---|---|
| TR1 | `trace` | `2832` | NATIVE except display fallback via `nix::ValuePrinter` |
| TR2 | `traceVerbose` | `2878` | NATIVE unless settings.traceVerbose |
| TR3 | `__warn` | n/a | NATIVE |
| TR4 | `break` | n/a | TW debug REPL |

### 2.5 Hidden bridges (FFI_AUDIT 2026-05-20 and 2026-05-24 BOTH missed these)

Per Agent C's cross-check — these are NOT in the original tier inventory but dominate primop wall today:

| # | Primop | File:line | Why missed | Wall share |
|---|---|---|---|---|
| **HX1** | **`__derivationFromPreprocessed`** | `primops.cc:7082` (reg `12164`) | Internal-only (not `builtins.X`); Option 4 hybrid wrapper added 2026-05-17 | **50-58% of primop wall on hello.drvPath + HNE** |
| **HX2** | **`__derivCoerce`** | `primops.cc:12018` | Internal-only string coercion; performs `realisePath` path-copying via `state.nixEvalState` | **26-34% of primop wall** |
| **HX3** | **`primFetchFinalTree`** | `primops.cc:11866` | Uses `internalPrimOps` lookup, not `builtins.<name>`. Not counted in original "9 bridgeBuiltin primops" | called from callFlakeV3 only |
| HX4 | `tryDispatchBridge1Direct` | `primops.cc:11026` | Anonymous-namespace dispatch helper for cross-bridge thunk forcing | sparse |
| HX5 | `tryDispatchFormalsLambdaBridge` | `primops.cc:11157` | Anonymous-namespace dispatch helper for formals-lambda bridge | sparse |

**HX1 + HX2 are critical findings.** Together they're 76-92% of primop wall. The original audit's "Tier" framework didn't classify them — they're FFI LEAVES (calls into `store->X` and `realisePath`), so they're V3-NATIVE-PERMITTED. But their existence means the FFI surface is broader than the audit suggested.

### 2.6 Aggregate totals

**Static call sites** (sum across `v3ToTreeWalker` + `treeWalkerToV3` mentions across v3 tree, per Agent A):
- primops.cc: 85
- vm.cc: 18
- run.cc: 5
- v3_call_flake.cc: 4
- lower.cc: 3
- bytecode_primops.cc: 3
- emit.cc: 1
- **Total: 119** (Agent A counted 92 of v3→TW direction; Agent C counted 115 including all instrumentation; both correct depending on what's counted)

**vs FFI_AUDIT_2026-05-20**: 104 static sites → 119 (slight UP-count from new instrumentation + per-site counters added in `#795 Phase A1` `91a3abbd6`, NOT new TW dependence).

**Empirical bridge call frequency** (per BRIDGE_TELEMETRY_2026-05-26):
- hello.drvPath: 0 crossings
- firefox.drvPath: 0
- gcc / bash / python3: 0
- synthetic-IFD: 0
- **HNE: 61** (per `__v3_force_attr=51 + __v3_call_bridge_1=8 + 2 misc` — all in flake.lock fetch logic)
- **Bridge wall on HNE: 0.014%**

**Bridge-table retention** (per BRIDGES_HOLD_RETENTION_2026-05-29):
- HNE end-of-eval: 519 MB / 99.8% of live arena
- M5 end-of-eval: 731 MB / 99.9% of live arena

---

## 3. Marshalling minimality analysis — top 5 over-marshalling cases

Agent B's analysis is the strongest section. The criterion for "minimal":

> If TW only ever READS field F of object O, can we marshal just F instead of O?

### 3.1 O1: `primDerivationStrict` (the headline)

**Site:** `primops.cc:6619-6651` (TW fallback path)
**Status:** HYBRID — native path at `6427` handles simple drvs; TW fallback for `__structuredAttrs` / `outputChecks` / content-addressed.

**Marshalled today:**
- INPUT: full attrset of derivation inputs (typically 28-30 attrs: name, builder, args, system, outputs, env vars, buildInputs lists, nativeBuildInputs, propagatedBuildInputs, etc.) via `v3ToTreeWalker(state, args[0])` at line 6623. **Each input attr is often ITSELF a derivation attrset** — transitively pulling tens of MB.
- RESULT: full result attrset (drvPath, outPath, per-output paths, type) via `treeWalkerToV3(state, *result)` at line 6651.
- BOTH input AND result land in `v3BridgeAttrs()` lazy-bridged.

**TW's actual consumption surface** (`DrvStrictSymbols` at `primops.cc:6107-6140`):
- 17 specific SymbolIds: name, system, builder, args, outputs, outputHashAlgo, outputHashMode, outputHash, __structuredAttrs, __contentAddressed, __impure, outputChecks, requiredSystemFeatures, allowedRequisites, allowedReferences, disallowedRequisites, disallowedReferences
- Plus iteration over remaining attrs for env-var coercion

TW does NOT recursively retain the input. It coerces each value to string for env-var serialization and discards.

**What minimal would look like:**
- `primDerivationStrictMinimal(EvalState&, BindingsIter)` — v3 calls TW directly without bridging the input attrset
- Pass only iterator-pulled fields
- Eliminates the single largest bridge entry class

**Effort:** ~1-2 weeks per FFI_AUDIT Tier 4-ish work. Extending `primDerivationStrictNative` to handle `__structuredAttrs` + `outputChecks` cases.

**Expected yield:** the 311 MB at `all-packages.nix:9112` bridge retention on HNE substantially disappears at source. **This is the single most actionable bridge-related finding in the entire audit.**

### 3.2 O2: `__filterAttrs` / `__mapAttrs` callback-driven primops (when callback is TW lambda)

**Pattern:** when TW invokes a callback via bridge1 on a v3 attrset, each entry round-trips:
1. TW iterates attrset → `__v3_force_attr(handle, name)` for each entry
2. `primV3ForceAttr` does `treeWalkerToV3(*found)` on each value (eager force)
3. TW lambda body forces value bridge thunk → calls v3 via bridge1
4. v3 evaluates → bridges result back

**What TW actually consumes:** per entry, NAME (string) and VALUE (often only its type checked, e.g. `meta.broken or false`). For 20K-entry nixpkgs attrsets, predicates typically read 1-3 fields per value.

**What minimal would look like:** defer entry-value bridging until the closure body actually reads the entry. Today `primV3ForceAttr` eagerly bridges at `primops.cc:4797/4831`.

### 3.3 O3: Bridge attrs at size > 4 — per-attr `vName` + `vApp` TW allocations

**Site:** `primops.cc:5572-5594` (lazy attrset bridge path)

**Marshalled today:**
- 1 PrimOpApp + 1 vHandle (`primops.cc:5568/5570`)
- Per entry: vName Value (16 B Boehm) + vApp = mkApp(vPartial, vName) (16 B Boehm)
- **For a 28-entry attrset: 58 TW Values** (1 PrimOpApp + 28 vName + 28 vApp + 1 vHandle)

**What TW actually consumes:**
- TW forces lazily — on hello.drvPath with a 28-entry drv attrset, TW typically forces 3-5 entries (outPath, drvPath, type, name, etc.)
- 23+ wasted TW Values per bridge × N bridges
- **10K-100K wasted TW allocations on real workloads**

**What minimal would look like:**
- Custom BindingsBuilder-equivalent that creates entries with a SHARED lazy primop taking (name, index) — eliminates per-entry vName Value
- Or: lazy materialization on first read (parallel to Kind::Chain at `alloc.hh:170`)

### 3.4 O4: `treeWalkerToV3` full attrset wrap (TW → v3 direction)

**Site:** `primops.cc:6015-6088`

**Marshalled today:** every TW attrset crossed to v3 wraps EVERY entry in `Alloc::allocBridgeThunk` (`primops.cc:6074`).
- For a 200-entry attrset: 200 Bridge thunks (~48 B each = ~10 KB)
- The pattern is eager-wrap, lazy-force-on-read

**What v3 actually consumes:** typical pattern is `pkg.outPath` or `pkg.name` — v3 reads ONE attr. Bridge thunks for the other 199 entries are dead-on-arrival.

**What minimal would look like:**
- Lazy wrap on first read — requires new Bindings flavor (Kind::LazyTW)
- Parallel to Kind::Chain enum already at `alloc.hh:170`

### 3.5 O5: `bridgeBuiltin<Arity>` for fetchers

**Site:** `primops.cc:11829-11857`

**Marshalled today:** ALL args full-converted via `v3ToTreeWalker` at line 11838, then result `treeWalkerToV3`'d at line 11856. For `fetchTree { url = "..."; rev = "..."; }` that means crossing an attrset of strings.

**What TW actually consumes:** mostly `url` / `rev` / `sha256` / `type` fields.

**What minimal would look like:**
- Flat-arg primop signature: `__fetchTreeFlat(url, rev, sha256, type)` — eliminates the attrset round-trip
- Per-fetcher: 1-2 days
- Win on HNE: small (fetchers are not hot on a cache-hit eval), but is the "no over-marshalling" pattern for new FFI primops

---

## 4. Lazy-bridge opportunities

| # | Opportunity | Site | Effort | Why valuable |
|---|---|---|---|---|
| L1 | Defer `forceValue` at v3ToTreeWalker entry | `primops.cc:5347` | architectural; needs TW-side cooperation | Eager force kills lazy-bridge benefit at top level |
| L2 | Short-circuit `primV3ForceAttr` when `*found` is already Tag::Thunk-of-Bridge | `primops.cc:5099` | small | Avoids double-bridge round-trip |
| L3 | Per-arg lazy bridging in `bridgeBuiltin<N>` | `primops.cc:11838` | per-fetcher | Many TW fetchers don't force all args (e.g., fetchTree skips narHash when URL stable) |
| L4 | derivationStrict result projection | `primops.cc:6651` | 1 day | Provide `primDerivationStrictPathOnly` returning just `{drvPath, outPath}` instead of full attrset |

---

## 5. De-duplication opportunities

| # | Opportunity | Site | Effort | Yield |
|---|---|---|---|---|
| D1 | Dedupe same v3 Bindings bridged twice | `v3ToTreeWalker` at `primops.cc:5565` | 1-2 days | M5: 10K → ~5K entries (50% reduction) |
| D2 | Dedupe TW Value bridged twice TW→v3 | `treeWalkerToV3` for nList/nAttrs entries | 1 day | Reuse existing `getOrAllocBridgeThunkCached` (line 5894) used only for nFunction/nExternal/nThunk; extend to nList/nAttrs |
| D3 | (Strings: SKIP) | Falsified per `STRING_DEDUP_AUDIT_2026-05-28` | — | — |

**Critical implementation note for D1:** add `std::unordered_map<const void*, size_t>` (v3Value.payload.raw → handle) before push_back at `primops.cc:5565`. Risk: TW's force chain might mutate the wrapper Value chain across re-bridges. Mitigation: audit `tryUnwrapBridge1Closure` (already done; safe per Agent B — compares handle ints not pointers).

---

## 6. Migration status delta (vs FFI_AUDIT_2026-05-20 and 2026-05-24)

### 6.1 Tier-by-tier reconciliation

**Tier 0 (system-info as constants):** **COMPLETE (zero-code via vBuiltins lazy-init)**. Primops remain registered for fallback but 0 dispatches on hot paths. Per `vm.cc:10788-10805` + `lower.cc:828/3140`.

**Tier 1 (Stage 2/3/9 architectural):**
- T1.A `NIX_V3_SKIP_INSTALLABLE_PREEVAL` retired (`3af813638`, #760, 2026-05-22). `NIX_V3_DIRECT_EVAL=1` is the gate now.
- T1.B Stage 9 module linking: **PENDING** (design at LINKING_DESIGN_2026-05-17).
- T1.C Stage 3 nursery default-on: **PAUSED** (per GC_PAUSE_2026-05-29).

**Tier 2 (bytecode-install callback primops):**
- **2a/b/c SHIPPED 2026-05-29** (`1243c158b`): sort, genericClosure, zipAttrsWith. Per-primop opt-out gates.
- 2d functionArgs + catAttrs: AUDIT-ONLY (already v3-native).

**Tier 3 (opcode-ify pure ops):**
- bitAnd/Or/Xor, floor, ceil, parseInt, splitVersion, baseNameOf, dirOf, stringLength.
- **NOT STARTED** — all remain registered as primops at `primops.cc:12119-12124`. Pure v3-native bodies but not opcode-inlined.

**Tier 4 (deferred):**
- fromJSON, toJSON, match, split, hashString, hashFile, toXML — all still primops, all v3-native.

### 6.2 Newly-added bridges (post 2026-05-20)

- **`__derivationFromPreprocessed`** (`primops.cc:7082`, reg `12164`) — **HX1 above; 50-58% of primop wall**. Added 2026-05-17 as Option 4 hybrid bytecode wrapper.
- **`__v3_call_bridge_1` / `__v3_force_attr_inner` / `__v3_force_list_elem_inner`** instrumented in `91a3abbd6` (#795) — not new bridges, newly-instrumented existing ones.
- **#875 Stage 0/1/2 bridge-eviction infrastructure** added 4 fields to all 3 BridgeXxxEntry structs (`primops.cc:3930-3989`). Stage 2 memory FALSIFIED per `07399432d`. Not a bridge addition; lifecycle work.

### 6.3 Eliminated bridges (post 2026-05-20)

- **`getFlake` TW bridge** retired in #758 (`856e319be`); v3-native `callFlakeV3` is sole impl.
- **`emitTreeAttrs` v3-native** (#701 Phase 4b, `856e319be`) — was ~50 bridges per cardano-node invocation.
- **`primReadDir` attrset native bypass** (#804, `0c2bc6b00`).
- **`primImport` attrset H7 fix** (#803, `0319f953a`) — removed 7 of 56 attrset crossings.
- **Eager-bridge for small lists/attrsets** (#804+#805 Phase E1/E2).
- **`primParseFlakeRef` / `primFlakeRefToString`** — now hand-rolled (`primops.cc:9641/9703`).
- Pre-#697 mutating TW builtins bridge — retired by #697.

### 6.4 V3-NATIVE classification (each remaining bridge)

**TRUE FFI LEAVES (must stay TW):**
- `realisePath` invocations (primops.cc:3053, 3531, 3636, 3700)
- `__derivationFromPreprocessed` (HX1) — buildAndWriteDrvNative store boundary
- `__derivCoerce` (HX2) — string coercion + path copy via store
- All 8 fetcher primops (F1-F8) — libfetchers
- `primGetFlake` — calls parseFlakeRef + lockFlake + native callFlakeV3
- `primToFile`, `primStorePath`, `primOutputOf`, `primAppendContext`, `primGetContext`

**V3-NATIVE VIOLATIONS (should be migrated):**
- BP1-BP3: `__v3_call_bridge_1`, `__v3_force_attr_inner`, `__v3_force_list_elem_inner` — TW→v3 callback primops. BP3 retirable (0 calls). BP1+BP2 still load-bearing on flake-fetch.

**BORDERLINE:**
- `primPath` (HYBRID; native path covers no-filter case)
- `primDerivationStrict` (HYBRID; native covers simple drvs; O1 over-marshalling target)

---

## 7. Frequency × marshalling cost — the top 5 ranked

This is the **operational ranking** for which bridges matter:

| Rank | Bridge | Frequency | Per-call cost | Total impact |
|---|---|---|---|---|
| #1 | **`v3BridgeAttrs` retention** (bridge table itself) | 30-10K entries | 17 MB-73 KB per entry transitively | **99.8% of HNE live arena** |
| #2 | **`primDerivationStrict` TW fallback** (O1) | per drv when not native | 28-entry input + result attrset bridged | most of #1 originates here |
| #3 | **`primImport` string-w-ctx → realisePath** (P1) | HNE 9 / hello 0 | full string + context | small wall, no significant retention |
| #4 | **`bridgeBuiltin<N>` for fetchers** (F1-F7) | per fetch call | full args attrset round-trip | small on cache-hit |
| #5 | **`__v3_force_attr_inner` / `__v3_call_bridge_1`** (BP1, BP2) | HNE 51 + 8 | per-call single-attr bridge | 0.014% of HNE wall |

---

## 8. Critical review of agent findings

### 8.1 Agent A (comprehensive inventory)

**Strong:**
- Most thorough enumeration (~92 v3→TW sites with file:line)
- Caught the 12-site delta vs FFI_AUDIT_2026-05-20 with commit attribution
- Correctly identified bridge table retention as the open lever

**Where I push back:**
- "Largest open lever = bridge table lifecycle" is true but doesn't address the Layer 1 page-release pin from yesterday's strategy. Stage 2 weak bridges LANDED but memory-INERT.
- Missed the HX1/HX2 hidden bridges (`__derivationFromPreprocessed` and `__derivCoerce`).
- Static call count (92) is the wrong metric per Agent C's framing.

### 8.2 Agent B (marshalling minimality)

**Strong:**
- The derivationStrict over-marshalling analysis is the sharpest finding in the entire audit
- DrvStrictSymbols 17-vs-28 framing is concrete and actionable
- O1-O5 ranking with effort estimates
- Closure serialization-fails finding (line 5706-5708) explains why Stage 2b is best-effort

**Where I push back:**
- O3 "10K-100K wasted TW allocations" claim needs to be measured; per BRIDGE_TELEMETRY the bridge wall is 0.014% so TW Boehm overhead may be small.
- L1 (defer v3ToTreeWalker forceValue) needs TW-side cooperation — architecturally infeasible without TW source changes; should be downgraded.
- D1 dedup yield "50% reduction on M5" is projection; needs measurement.

### 8.3 Agent C (delta cross-check)

**Strong:**
- Caught FFI_AUDIT_2026-05-24 supersedes 2026-05-20 (I had been treating 05-20 as current)
- **HX1 + HX2 hidden bridges are the most valuable addition to the audit** — agents A + Agent B's "missing list" is corrected here
- Per-tier reconciliation with commit attribution
- V3-NATIVE classification for each remaining bridge

**Where I push back:**
- "Static count is wrong metric" is true but Agent C still spent effort on it; should be marked as informational only
- HX1/HX2 are FFI LEAVES (legitimate per V3-NATIVE rule); they're not violations, just under-enumerated. Frame correctly.

### 8.4 Synthesis-level observations the agents didn't surface

1. **The derivationStrict native migration interacts with Layer 3 of yesterday's strategy.** If we eliminate the bridge at source (native impl), the bridge-lifecycle layer doesn't need to evict that case. **Structural simplification of the GC strategy.**
2. **HX1 + HX2 combined = 76-92% of primop wall.** This is the "wall" picture (not RSS). For wall optimization, these are the targets. For RSS, the bridge table retention is the target. **These are orthogonal levers.**
3. **De-duplication (D1) is cheap and high-yield on M5** — should be a quick win. ~1-2 days, projected 50% bridge-entry reduction on M5.

---

## 9. Honest limits

- **Bridge count totals vary across agents** (92/115/119) because each counts a different thing (v3→TW direction sites vs all instrumentation vs file mentions). Not a real disagreement.
- **HX1/HX2 wall share (50-58% / 26-34%)** is from `V3_NATIVE_MIGRATIONS_NEXT_SESSION_2026-05-29.md` per Agent C — verify in current `NIX_VM_PRIMOP_TIME=1` output.
- **The derivationStrict native completion estimate (1-2 weeks)** assumes existing infrastructure carries; `primDerivationStrictNative` at `primops.cc:6427` handles simple cases; the structuredAttrs/outputChecks/CA-hash cases are the residual work. Effort could be 2-3 weeks if hash-mode coverage is incomplete.
- **D1 dedup yield projection (50% on M5)** is hypothesis; needs measurement spike.
- **L1 lazy-bridge** is architecturally infeasible without TW cooperation; should not be on a near-term roadmap.
- **The 17-symbol DrvStrictSymbols** is the documented enum; verify in `primops.cc:6107-6140` — TW's actual derivationStrict implementation may read additional fields from "user-defined env vars" which is unbounded.
- **Bridge retention 99.8% / 99.9% measurements** are end-of-eval, not peak. Mid-eval distribution unknown beyond DIAG-3 L(t) samples.
- **Migration progress is partial.** Tier 3 (opcode-ify) not started; Tier 4 deferred. The wall picture has been moved more than the RSS picture.
- **`primDerivationStrictNative` extension to structuredAttrs** has not been verified to be feasible per current `buildAndWriteDrvNative` coverage; could uncover store-API limitations.
- **The "no de-duplication" claim** is verified for `v3BridgeUniquePtrCounts` but a future check should confirm no separate dedup happens in `tryUnwrapBridge1Closure` path.

---

## 10. Action ladder (concrete next steps from this audit)

Ranked by impact-per-effort:

| # | Action | Effort | Yield | Depends on |
|---|---|---|---|---|
| 1 | **Extend `primDerivationStrictNative` to cover `__structuredAttrs` + `outputChecks`** | 1-2 wk | Most of HNE 311 MB bridge retention; M5 similar | None |
| 2 | **D1: dedupe v3 Bindings bridged twice** via unordered_map lookup in `v3ToTreeWalker` | 1-2 d | M5: ~50% bridge-entry reduction | None |
| 3 | **D2: extend `getOrAllocBridgeThunkCached` to nList/nAttrs entries** | 1 d | unknown; depends on TW→v3 bridge frequency | None |
| 4 | **L2: short-circuit `primV3ForceAttr` for already-Bridge-thunk values** | 0.5 d | reduces BP2 round-trip cost | None |
| 5 | **L4: `primDerivationStrictPathOnly`** projection variant | 1 d | reduces result-attrset bridge cost when caller only uses drvPath/outPath | None |
| 6 | **BP3 retirement** (0 calls everywhere) | 0.5 d | code hygiene; no perf | None |
| 7 | **Tier 0 finishing: `currentSystem` / `storeDir` as injected constants** | 1-2 d | small wall; complete the migration | None |
| 8 | **L3: per-arg lazy bridging in `bridgeBuiltin<N>`** | 1-2 d per fetcher | small on cache-hit | None |
| 9 | **O3: shared lazy primop for bridge attrs** (eliminate per-entry vName) | 1 wk architectural | 10K-100K fewer TW Values on real workloads | None |
| 10 | **O4: lazy Kind::LazyTW Bindings flavor** for TW→v3 attrsets | 1-2 wk architectural | reduces TW→v3 wrapping cost | None |

**Total #1-#6 cumulative: ~2-3 weeks of focused work for the highest-impact bridge minimizations.**

---

## 11. Cross-references

### Strategic context
- [`GC_STRATEGY_INTEGRATED_2026-05-30.md`](GC_STRATEGY_INTEGRATED_2026-05-30.md) — yesterday's 6-layer strategy; this audit refines Layer 3 (bridge cohort lifecycle)
- [`BRIDGES_HOLD_RETENTION_2026-05-29.md`](BRIDGES_HOLD_RETENTION_2026-05-29.md) — load-bearing 99.8% finding
- [`WEAK_BRIDGE_EVICTION_DESIGN_2026-05-29.md`](WEAK_BRIDGE_EVICTION_DESIGN_2026-05-29.md) — Stage 2 landed but memory-inert
- [`WEAK_BRIDGE_PAGE_RELEASE_2026-05-29.md`](WEAK_BRIDGE_PAGE_RELEASE_2026-05-29.md) — the Path A pin
- [`BRIDGE_TELEMETRY_2026-05-26.md`](BRIDGE_TELEMETRY_2026-05-26.md) — 0.014% wall on HNE
- [`V3_NATIVE_MIGRATIONS_NEXT_SESSION_2026-05-29.md`](V3_NATIVE_MIGRATIONS_NEXT_SESSION_2026-05-29.md) — Tier execution status

### Previous bridge audits (superseded by this doc)
- [`FFI_AUDIT_2026-05-20.md`](FFI_AUDIT_2026-05-20.md) — original 4-tier; 104 sites
- [`FFI_AUDIT_2026-05-24.md`](FFI_AUDIT_2026-05-24.md) — post-#795 empirical update

### Code anchors
- `primops.cc:3930-3989` — three Bridge entry structs
- `primops.cc:3950-3956, 4211-4226` — three table accessors
- `primops.cc:4019-4068` — Stage 1/2/2b gate accessors + sweep cadence
- `primops.cc:4081-4158` — `evictBridgeEntry` / `makeBridgeEntry` / `reviveBridgeEntry`
- `primops.cc:4194-4208` — `bumpBridgeAccess` (hot path)
- `primops.cc:4342-4347` — `clearV3BridgesForDiag`
- `primops.cc:4369-4400` — unique-pointer measurement (D1 dedup opportunity unconsumed)
- `primops.cc:4541, 4923, 5183` — three bridge plumbing primops (BP1-BP3)
- `primops.cc:5320-5771` — `v3ToTreeWalker` (the v3→TW marshaller)
- `primops.cc:5834-6094` — `treeWalkerToV3` (the TW→v3 marshaller)
- `primops.cc:6107-6140` — `DrvStrictSymbols` (TW's actual derivationStrict consumption)
- `primops.cc:6435-6663` — `primDerivationStrict` (O1: the headline over-marshalling)
- `primops.cc:7082` — `__derivationFromPreprocessed` (HX1: hidden bridge, 50-58% primop wall)
- `primops.cc:11026, 11157` — `tryDispatchBridge1Direct`, `tryDispatchFormalsLambdaBridge` (HX4, HX5)
- `primops.cc:11829-11857` — `bridgeBuiltin<Arity>` (O5: fetcher round-trip pattern)
- `primops.cc:11866` — `__fetchFinalTree` (HX3)
- `primops.cc:11899-11947` — `primGetFlake` (v3-native via callFlakeV3)
- `primops.cc:12018` — `primDerivCoerce` (HX2: hidden bridge, 26-34% primop wall)
- `bytecode_primops.cc:539-749` — Tier 2a/b/c bytecode-installs (sort, genericClosure, zipAttrsWith)
- `v3_call_flake.cc:206-324` — `v3EmitTreeAttrs` (already-minimized example: builds Bindings v3-native instead of bridging)
- `include/v3/value.hh:78-228` — Value/ValuePair layout
- `include/v3/closure.hh:60-206` — Closure/Thunk/Bridge layouts
- `include/v3/alloc.hh:99-179` — ListVec/Bindings layouts (incl. Kind::Chain enum)
- `include/v3/ffi.hh` — eventual clean-room FFI surface (532 LoC, mostly skeleton)
- `bridge_root_registry.cc:71-101` — `pushBridgeRoot` for Boehm-root tracking
- `include/v3/value_serialize.hh` — Stage 2 blob serialization API

### Methodology
- [[falsification-rule]] — every bridge-migration commit must answer "what hypothesis does this kill"
- [[memory-first-class]] — bridge retention is the RSS-primary lever; bridge wall is secondary
- [[measure-twice-cut-once]] §3 — pre-commit thresholds for any bridge-elimination migration
- [[same-host-bisect]] — verify bridge frequency on same host before claiming reductions

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
