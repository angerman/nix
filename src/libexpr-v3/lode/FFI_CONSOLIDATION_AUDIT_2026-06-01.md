# FFI consolidation audit — toward a single `ffi.h` surface

**Date:** 2026-06-01
**Author:** session synthesis (3-agent: include inventory + ffi.h design + migration plan + critical review)
**Status:** STRATEGIC AUDIT — single-FFI-surface goal is ~98% achievable; ~70% can ship without depending on the parser project
**Triggering question:** "Do we still #include any foreign (TW) stuff? Can we get rid of all of those and replace them with one `ffi.h`?"

Companion docs:
- [`FFI_BRIDGE_INVENTORY_2026-05-31.md`](FFI_BRIDGE_INVENTORY_2026-05-31.md) — current bridge surface
- [`NATIVE_PARSER_FEASIBILITY_2026-06-01.md`](NATIVE_PARSER_FEASIBILITY_2026-06-01.md) — parser project (Phase 5 dependency)
- [`FFI_KILL_PLAN_2026-05-31.md`](FFI_KILL_PLAN_2026-05-31.md) — bridge minimization track
- `include/v3/ffi.hh` — current SKELETON (533 LoC, ~20% complete)

---

## 0. PROGRESS UPDATE (2026-06-02) — re-baselined after parser completion

The audit (written 2026-06-01) assumed the parser project was at Stage 1.4.
**The parser project has since COMPLETED end-to-end** (Stage 1 + Stage 2:
`.nix` → v3 IR directly, `lower.cc` + the AST→nix::Expr bridge DELETED).
This materially changes the FFI landscape — re-baseline:

**What the parser completion already did (Phase 5 ~done up front):**
- **Category B (PARSER headers) is RETIRED from the lowering path.**
  `lower.cc` (the 3,446-LoC consumer of `nix/expr/nixexpr.hh` +
  `symbol-table.hh` + `nix/util/position.hh`, and the audit's one "honest
  exemption" §2.3/§3.5 #1) **no longer exists**.  Those parser includes
  now live only in the new translation-boundary files:
  `cli/lower_v3.hh` (forward-declares `nix::SymbolTable`; includes
  `nix/util/pos-table.hh`) + `parser/v3-parse-api.cc` + `parser/*`.
- The "lower.cc exemption that RETIRES when parser Stage 2 ships" (§5,
  §8) — **shipped.**

**Inventory shift:** the audit's 18 files / 77 TW-include lines is stale.
Current (2026-06-02): **21 files / 94 lines** — the count rose because the
parser work ADDED translation-boundary files (`lower_v3.hh`,
`tw_baseenv.hh`, `v3-parse-api.cc`) that legitimately include TW headers,
while `lower.cc`'s went away.  `tw_baseenv.hh` (a small documented FFI
leaf: reads `staticBaseEnv` for free-name resolution) is new since the
audit.

**Done since the audit:**
- **Phase 1 (trivial-leaf cleanup) — DONE** (`a0f5e0aac`): removed the 3
  dead/redundant includes — `primops.cc` `print.hh` (the §2.4 #1 dead
  include), `bytecode_primops.cc` `nixexpr.hh` (newly dead after the
  installer went native), `bridge_yield.cc` `value.hh` (redundant given
  `eval.hh`).  97 → 94 lines.
- **Phase 6 (CI lint) — DONE** (`5147072cb`): `test/lint-no-direct-tw-include.sh`
  + `test/tw-include-baseline.txt`, registered in meson + all-v3-tests
  core (16/16).  Deployed as a **RATCHET** (baseline the current 21 files;
  fail on any NEW offender; report files that became clean) rather than
  the strict end-state lint, since Phases 2-4 are still pending.  Verified
  it bites (injecting a TW include into a non-baselined file → exit 1).
- **§2.4 #4 (config.hh) — DONE** (`4e8b0fe72`): `NIX_USE_BOEHMGC` (a
  build macro, not eval-state) centralized behind the v3-owned
  `include/v3/gc-config.hh` (includes the generated header — value still
  derived from `bdw_gc.found()`, no hardcoding, non-Boehm fallback
  preserved).  alloc.hh / nursery.hh / bridge_root_registry.cc became
  TW-include-free.  **Ratchet baseline: 21 → 19** TW-touching files.

- **Lint refactored** (`5906eeeb3`): permanent leaves/exemptions
  (ffi.{hh,cc}, disk_cache.cc, parser/, test/, lower_v3.hh, tw_baseenv.hh,
  gc-config.hh) are now PATH-PATTERN EXEMPT (audit §5.1); the baseline file
  holds ONLY genuine library migration targets and shrinks to EMPTY at the
  end state.
- **Phase 2 migrations — DONE** (`f537593c0`, `c30623c50`, `9e4933ad1`,
  `1fafc421e`): added the ffi shim surface — `nix::v3::ffi::forceValue`
  (§3.4), `ffi::setTreeWalkerBuiltin`, and the synthetic-source
  `runRootExprFromString(state, source)` entry (in run.cc, the parse+lower+
  run owner) — plus broadened `gc-config.hh` to be the single Boehm/GC
  indirection (NIX_USE_BOEHMGC + traceable_allocator + initGC, via
  eval-gc.hh) and added `nix/util/hash.hh` to ffi.hh's Layer-0.  Migrated
  fully behind ffi.hh (all validated lang 143/143 + drvPath byte-equal):
  `bridge_yield.cc` (cold forceValue), `bytecode_primops.cc` (the wrapper
  installer → runRootExprFromString + setTreeWalkerBuiltin), `vm.hh`
  (traceable_allocator), `value_serialize.cc` (Hash).  **Baseline: 10 → 6.**

**Remaining baseline = 6 genuine library targets** — the genuinely-hard /
blocked core:
- **primops.cc, vm.cc** — HOT-path `eval.hh` (forceValue/allocValue/
  callFunction/realisePath/…) + the value-graph marshallers
  (treeWalkerToV3 / v3ToTreeWalker in primops.cc).  **PERF-GATED (R1/R4):
  the out-of-line shim is only safe on cold paths; the hot sites need the
  `ffi-inline.h` private-header trick + a hyperfine ≤2% gate on a
  release+LTO build — which this `-O0`/no-LTO debug tree CANNOT measure.
  `ffi::forceValue` is the foundation; adoption waits for a release build.**
- **v3_call_flake.cc, cli/v3-eval.cc** — carry genuine Category-A leaves
  (flake/fetcher/store/settings + CLI-main env setup); the audit's Phase 4
  moves the bridge marshallers + these calls into ffi.cc (multi-day).
- **run.cc** — the parse→lower→run orchestrator (EvalState field access +
  the setup pipeline); like ffi.cc, a boundary file.
- **include/v3/run.hh** — `PosTable::Origin` in the public
  runRootExprFromString signature (a position-boundary type; needs the
  Level-2 PosIdx/Origin mirror or a run-entry exemption).

**Audit correction:** §2.4 #5 ("`vm.hh:11` includes `eval-gc.hh`, could
move to .cc-only") is **FALSE** — `vm.hh` uses `traceable_allocator`
(Boehm) from it, inline in the header.  Build falsifies the move (Rule 0).

**Revised remaining work (the bulk):** Phase 0 (expand `ffi.hh` with the
Option-A EvalState shim layer) + **Phase 2/3** (opacify `nix/expr/eval.hh`
+ `value.hh`, centered on `primops.cc`'s ~285 EvalState calls + the
`treeWalkerToV3`/`v3ToTreeWalker` marshallers) + Phase 4 (flake/fetcher).
This is the perf-sensitive, multi-day core (the audit's §3.5 R1/R4
inline-`forceValue` concern is real — needs the `ffi-inline.h` private
header + hyperfine gates).  NOTE: the EvalState surface is ~14 methods
(forceValue/allocValue/callFunction/realisePath/addOrigin/forceAttrs/
coerceToString/copyPathToStore/…) plus FIELD access (`state.symbols`,
`state.positions`, `staticBaseEnv`, `store`, `rootFS`) — the field access
is Level-2 (accessor functions), broader than the §3.4 "4 functions."
Fully cleaning even a cold single-method file (e.g. `bridge_yield.cc`,
`forceValue` only) also needs a `nix/util/pos-idx.hh` mirror (`noPos`).

---

## 1. TL;DR

**Yes, you can consolidate to ~one `ffi.h`.** The audit findings:

- **18 of 96 v3 source files** include TW headers (19%). **77 total `#include` lines**, **34 unique TW headers**.
- **6 files carry 90% of the weight**: `primops.cc` (20 includes), `v3_call_flake.cc` (9), `vm.cc` (4), `disk_cache.cc` (5), `cli/v3-eval.cc` (7), `lower.cc` (3).
- **Only ONE confirmed dead include** (`nix/expr/print.hh` at `primops.cc:47`). **The migration discipline has been holding.** Every other include backs at least one actual call site.
- **The pinch is `nix/expr/eval.hh`** in 6 .cc files for `EvalState` methods (`forceValue`, `parseExprFromString`, `coerceToString`, `copyPathToStore`). 4 wrapper functions in `ffi.h` close most of this.
- **`lower.cc` requires the only honest exemption**: AST traversal pattern-matches on 27 `ExprXxx` subclasses via `dynamic_cast`. Wrapping that in a visitor (~500 LoC shim) defeats the abstraction and locks v3 to TW's AST shape. **Accept as documented exemption.**
- **The existing `include/v3/ffi.hh`** is already half-right (532 LoC, has opaque forward declarations for `Store`, `Logger`, `Expr`, `StaticEnv`, `PrimOpRegistry`) but is undersized — missing the load-bearing `v3↔TW value bridge` and `EvalState` shim layer.

**Effort estimate:** 29-36 person-days across 6 phases. **70% of the win (Phases 0-4) is independent of the parser project** and can run concurrent. Last 30% (Phase 5) gated on parser Stage 1 SHIP, which is making rapid progress (Tier 4b complete = 123/123).

**Strategic recommendation:** START PHASE 0 NOW. Run Phases 0-4 in parallel with the parser project. Reserve Phase 5 for the week parser Stage 1 lands. **Expected timeline: 70% milestone at ~week 6; 100% milestone at ~week 8-10** (given current parser velocity).

---

## 2. The include inventory (Agent A)

### 2.1 Aggregate statistics

| Metric | Value |
|---|---|
| v3 source files total | 96 |
| Files with TW includes | 18 (19%) |
| Total TW `#include` lines | 77 |
| Unique TW headers referenced | 34 |
| Per-subsystem: `nix/util/*` | 13 includes / 10 headers |
| Per-subsystem: `nix/expr/*` | 14 includes / 8 headers |
| Per-subsystem: `nix/store/*` | 18 includes / 10 headers |
| Per-subsystem: `nix/fetchers/*` | 4 includes / 4 headers |
| Per-subsystem: `nix/flake/*` | 4 includes / 4 headers |
| Per-subsystem: `nix/main/*` | 2 includes / 1 header |

### 2.2 Heaviest TW-include consumers

| File | TW includes | `nix::*` references |
|---|---|---|
| `primops.cc` | 20 | 477 across 12,214 LoC |
| `v3_call_flake.cc` | 9 | ~50 |
| `cli/v3-eval.cc` | 7 | ~30 |
| `disk_cache.cc` | 5 | ~40 (SQLite) |
| `vm.cc` | 4 | 25 |
| `lower.cc` | 3 | 105 across 27 AST subclasses |
| `bytecode_primops.cc` | 3 | ~20 |
| `bridge_yield.cc` | 3 | ~15 |
| `run.cc` | 3 | ~10 |
| `value_serialize.cc` | 1 | ~8 |

The top-6 files carry 50 of 77 total TW includes (65%).

### 2.3 Per-category inventory

#### Category A — TRUE FFI leaves (25 headers, legitimate)

Store, fetchers, flake, file I/O. Must stay because TW IS the implementation:
- `nix/store/store-api.hh`, `path.hh`, `derived-path.hh`, `derivations.hh`, `content-address.hh`, `globals.hh`, `store-open.hh`, `sqlite.hh`, `path-references.hh`
- `nix/fetchers/fetchers.hh`, `fetch-settings.hh`, `fetch-to-store.hh`, `attrs.hh`
- `nix/flake/flake.hh`, `flakeref.hh`, `settings.hh`, `lockfile.hh`
- `nix/main/shared.hh`
- `nix/util/users.hh`, `file-system.hh`, `serialise.hh`, `memory-source-accessor.hh`, `posix-source-accessor.hh`
- `nix/expr/eval-settings.hh`

#### Category B — PARSER (3 headers + 1 hybrid; retires with parser project)

- `nix/expr/nixexpr.hh` (lower.cc:29, bytecode_primops.cc:18) — 27 AST kinds + Expr base
- `nix/expr/symbol-table.hh` (lower.cc:30) — SymbolTable, Symbol interning
- `nix/util/position.hh` (lower.cc:31) — `nix::Pos` for AST node positions
- `nix/util/pos-idx.hh` (bridge_yield.cc:11, include/v3/ffi.hh:63) — hybrid; v3 keeps its own PosIdx

**These RETIRE entirely when parser Stage 1 ships.** Current parser tempo (Stage 1.4 Tier 4b = 123/123) suggests within ~1-2 weeks.

#### Category C — EVAL-STATE bridge (3 heavy headers in 6 .cc files)

- `nix/expr/eval.hh` — `EvalState` + `forceValue`/`forceList`/`parseExprFromString`/`coerceToString`/`copyPathToStore`. Used by primops.cc, vm.cc, run.cc, bridge_yield.cc, bytecode_primops.cc, v3_call_flake.cc.
- `nix/expr/value.hh` — `nix::Value` (TW Value, distinct from v3 Value). Forward-declared in 4 v3 headers.
- `nix/expr/value/context.hh` — `NixStringContext`, `NixStringContextElem`. Shared encoding with TW (drvPath byte-equality requires this).

**This is the consolidation pinch.** 4 wrapper functions in ffi.h would close it (see §3.4).

#### Category D — SHARED UTILITIES (~10 headers, genuinely shared)

- `nix/util/hash.hh` (4 sites) — `Hash`, `HashAlgorithm`. Domain-correctness contract.
- `nix/util/source-path.hh`, `source-accessor.hh`, `canon-path.hh` — file path types.
- `nix/util/experimental-features.hh` — feature flag enum.
- `nix/util/sync.hh` — mutex wrapper for disk_cache.
- `nix/util/error.hh` — `EvalError` base.
- `nix/util/eval-trace.hh` — shared `NIX_TRACE_EVAL` format.
- `nix/expr/config.hh` — `#define NIX_USE_BOEHMGC` build macro (NOT eval-state).
- `nix/expr/eval-gc.hh` — `nix::initGC()` Boehm bootstrap.

#### Category E — LEAKAGE

**Exactly ONE confirmed dead include:**
- `nix/expr/print.hh` at `primops.cc:47` — `nix::ValuePrinter` references only exist in a retired-code comment. Zero-cost cleanup.

**Everything else backs at least one actual call site.** The discipline has been holding.

### 2.4 Surprising finds

1. **`nix/expr/print.hh` is dead** at `primops.cc:47` — trivial cleanup commit.
2. **`bridge_yield.cc:10` `#include "nix/expr/value.hh"`** could be dropped (forward decl already in `.hh`; only `nix::Value *` references in the .cc).
3. **`bytecode_primops.cc:18` `nix/expr/nixexpr.hh`** is included only for `Expr *` return type of `parseExprFromString`. Forward decl would suffice.
4. **`nix/expr/config.hh`** (4 includes) is NOT eval-state — it's a build-config macro that happens to live under `nix/expr/`. Could be relocated to v3-owned config header.
5. **`include/v3/vm.hh:11`** includes `nix/expr/eval-gc.hh` — meaning the bare VM header drags in TW GC init. Could move to .cc-only.
6. **The seed `include/v3/ffi.hh` is itself almost compliant** — only pulls in 4 TW headers (the Layer-0 concessions the FFI plan calls out as acceptable).

---

## 3. The opacity ladder + ffi.h design (Agent B)

### 3.1 Existing `include/v3/ffi.hh` status

**532 LoC SKELETON** (line 14: "This is a SKELETON"). Declares the target surface but is not consumed by v3's hot path.

**What it has:**
- 13 categories of declarations (parser, symbol table, FS, store, derivation, settings, logger, EvalScope framework, etc.)
- ~59 function declarations
- ~20 forward-declared types
- 4 TW header includes at the top (lines 63-66)
- IMPLEMENTED: `EvalScope`/`ClosureHandle` framework (`ffi.cc:137-232`), `readFile`/`readDir`/`pathExists` shims (`ffi.cc:351-390`)

**What it lacks (the gap):**
- The Value↔Value marshaller surface (`treeWalkerToV3` / `v3ToTreeWalker`) — load-bearing FFI primitive, absent
- The `EvalState` shim layer (285 method calls in primops.cc go through `tlNixEvalState` thread-local, not through ffi.hh)
- The bridge plumbing primops (BP1/BP2) — declared but not abstracted

**Distance to "single FFI surface": ~20% complete.**

### 3.2 Opacity ladder applied to top TW types

For each TW type v3 uses, the feasible level of opacity:

| TW type | Uses | Opacity Level | Rationale |
|---|---|---|---|
| `nix::EvalState` | 285x primops, 25x vm.cc | **Level 1** (opaque + accessors) | v3 only calls methods; never reads fields, sizeofs, or inherits |
| `nix::Value` (TW) | bridge sites | **Level 0** (opaque pointer) | Always referenced as `nix::Value *`; `closure.hh:201` already stores as `void *` |
| `nix::Store` | ns.store->X (40+ refs) | **Level 1** | All method calls |
| `nix::SourcePath` | constructor + .abs() + .readFile() | **Level 1 with factory** | Add `ffi::makeSourcePath(FS, string_view)` |
| `nix::PosIdx` | 32-bit u32 wrapper | **Level 2 (POD copy)** | Just `uint32_t`; mirror as `V3PosIdx` with `static_assert(sizeof)` |
| `nix::Hash`, `HashAlgorithm` | ~10 sites | **Level 1** | Wrap construction/print in ffi:: helpers |
| `nix::Expr *` | parser → lower input | **Level 0** outside lower.cc | dynamic_cast only in lower.cc |
| `nix::Symbol`, `SymbolId` | 13 sites | **Level 1** | Free functions `ffi::intern`/`symbolStr` |
| `nix::Bindings` (TW) | bridge attrset | **Level 0** | Never structurally accessed |
| `nix::StorePath` | by-value returns | **Level 1 by-value** | Either include `nix/store/path.hh` OR force pointer; recommend include |
| `nix::Expr` subclasses | lower.cc dynamic_cast | **Level 3 (FULL include)** | The exemption |
| `nix::flake::LockedFlake` | v3_call_flake.cc walk | **Level 1 via callback** | `ffi::walkLockedFlake(callback)` hides type |

**9 of top-10 collapse to Level 0/1.** Only `lower.cc`'s AST traversal genuinely needs Level 3.

### 3.3 Function declaration strategy: Option A (typed link-time)

```cpp
namespace v3::ffi {
    void forceValue(nix::EvalState &, nix::Value &, nix::PosIdx);
}
```

v3 calls `v3::ffi::forceValue(ns, v, pos)`; linker resolves to a shim in `ffi_impl.cc` that calls `ns.forceValue(v, pos)`.

**vs Option B (function pointers):**
- A: zero runtime overhead with LTO; compile-time signature checking; trivial refactor
- B: enables true plugin model (alternate hosts); runtime null-check; install-order bugs

**Recommendation: Option A.** The runtime-replacement scenario is hypothetical; v3 always links against host cppnix. Reserve Option B for the small set already using it (`tlNixEvalState`, `tlFlakeSettings`).

### 3.4 The 4 wrapper functions that close most of `nix/expr/eval.hh`

```cpp
namespace nix::v3::ffi {
    void   forceValue       (nix::EvalState&, nix::Value&, nix::PosIdx);
    nix::Expr*  parseString (nix::EvalState&, std::string_view, nix::SourcePath);
    std::string coerceToString(nix::EvalState&, nix::PosIdx, nix::Value&, 
                              nix::NixStringContext&, std::string_view, bool, bool);
    nix::StorePath copyPathToStore(nix::EvalState&, nix::NixStringContext&, nix::SourcePath);
}
```

**These 4 functions cover ~85% of EvalState method calls across the 6 heavy users.** Add v3-local typedefs/forward-decls and the 6 .cc files drop `nix/expr/eval.hh` entirely.

### 3.5 Real technical blockers

| # | Blocker | Resolution |
|---|---|---|
| 1 | `lower.cc` `dynamic_cast` on 25 ExprXxx subclasses | **Document exemption.** lower.cc is a designated translation boundary. |
| 2 | `eval-inline.hh` inline `forceValue`/`forceAttrs`/`allocValue` | LTO + `[[gnu::always_inline]]` on shim; needs perf measurement |
| 3 | `StorePath` by-value usage | Keep `nix/store/path.hh` in ffi.h (acceptable concession) |
| 4 | `SourcePath` construction | Factory function `ffi::makeSourcePath(FS, string_view)` |
| 5 | Templated APIs (`state.error<EvalError>(...)`) | Non-template wrappers (~15 categories, ~200 LoC) |
| 6 | `SymbolTable::operator[]` returns `SymbolStr` | Free function `ffi::symbolStr(state, id) -> string_view` |
| 7 | `nix::SQLite` / `nix::SQLiteStmt` in disk_cache.cc | **Lint exempt disk_cache.cc** — SQLite IS the storage impl, not a bridge |

### 3.6 Volume estimate

- ffi.h: ~1000-1300 LoC (current 532 + ~800 for EvalState shim + Value accessors + bridge surface)
- ffi_impl.cc: ~1500-2000 LoC (absorbs `treeWalkerToV3` + `v3ToTreeWalker` from primops.cc, BP1/BP2 machinery, EvalState wrapping)
- Optional `ffi-inline.h`: ~600 LoC private header for inline accessors

---

## 4. Migration plan (Agent C)

### 4.1 Six phases, 29-36 person-days total

| Phase | Effort | Independent of parser? |
|---|---|---|
| 0 — Audit + expand ffi.h skeleton | 3 d | ✓ |
| 1 — Trivial-leaf consolidation | 4 d | ✓ |
| 2 — EvalState opacification | 5-7 d | ✓ |
| 3 — Value graph accessor functions | 8-10 d | ✓ |
| 4 — Flake/fetcher consolidation | 4 d | ✓ |
| 5 — Parser inclusion retirement | 3-5 d | **GATED on parser Stage 1** |
| 6 — CI lint + verification | 2-3 d | ✓ |

**Phases 0-4 + 6 = 70% of the win, INDEPENDENT of parser. ~25-30 person-days.**
**Phase 5 = last 30%, gated on parser Stage 1 SHIP.**

### 4.2 Dependency ordering — what unblocks what

1. **Parser Stage 1 SHIP** unblocks the most: retires `nix/expr/nixexpr.hh`, `symbol-table.hh`, `parser-state.hh`, `position.hh`. Reaches into 5 of top-6 files.
2. **bindVars decoupling** (Parser Stage 2): retires `nix/expr/eval.hh` from `lower.cc`.
3. **`primDerivationStrict` native completion** (#806 Phase C6): DONE 2026-05-31. Residual `derivations.hh` includes stay (legitimate libstore-side).
4. **BP1/BP2 lazy marshalling** (FFI_KILL_TODO T2.2/T2.3): retires `value.hh`, `value/context.hh` from `primops.cc`. Cannot fully land until bridge retention story closed.

### 4.3 Phased gates

| Phase | Pass gate | Falsification |
|---|---|---|
| 0 | ffi.hh compiles standalone; LoC ≤ 1500 | LoC > 2000 OR pulls > 5 nix/... headers |
| 1 | 5 files build with ffi.hh only; wall unchanged | Any v3 .cc needs additional nix/... |
| 2 | 6 .cc files compile sans eval.hh; hello.drvPath wall ≤ +2% | Wall > +2% OR any bridge primop crashes on M5 |
| 3 | bridge_yield.cc + v3_call_flake.cc sans value.hh; HNE BP1+BP2 wall ≤ +5% | Wall > +5% OR `treeWalkerToV3` indirection breaks |
| 4 | flake/fetcher at ZERO nix/ includes outside ffi.h + disk_cache.cc | SQLiteStmt leaks OR fetcher tests fail |
| 5 | lower.cc + bytecode_primops.cc at ZERO nix/ includes; 143/143 lang | Any ExprXxx still referenced outside parser/ |
| 6 | `test/lint-no-direct-tw-include.sh` returns 0 | Lint fires on any non-exempt file |

Every phase requires: `--quick 6/6` + `--core 15/15` + drvPath byte-equal on hello + HNE + M5.

### 4.4 Risk register (top 5)

| # | Risk | Likelihood | Severity | Mitigation |
|---|---|---|---|---|
| R1 | Inline-function overhead on bridge hot path (Phase 2/3) | medium | medium | Pre-commit hyperfine; ≤2% per phase; `ffi-inline.h` private header trick |
| R2 | Template instantiation cycle (`std::variant<NixStringContextElem>`) | high | medium | Hybrid: full type in private inline header; opaque in public ffi.h |
| R3 | Parser project slips beyond 12-16 wk | medium-high | LOW for FFI work | Phases 0-4 complete independently; Phase 5 is opt-in |
| R4 | `EvalState::forceValue` inline retirement degrades wall | medium | medium | Measure first; private inline header if needed |
| R5 | Mechanical lint false-positives | high (immediate) | low | Exempt list: ffi.cc, ffi.hh, parser/, disk_cache.cc |

---

## 5. End-state vision

After Phases 0-6:

| Component | LoC | Notes |
|---|---|---|
| `include/v3/ffi.hh` | ~1500 | Public; current 532 skeleton + ~1000 opaque accessors |
| `include/v3/ffi/ffi-inline.hh` | ~600 | Private; consumed only by ffi.cc |
| `src/libexpr-v3/ffi.cc` | ~2000 | Grown from 100 LoC skeleton; absorbs bridge marshallers + EvalState wrapping |
| `primops.cc` | ~9000 (was 12,214) | Bridge marshallers moved to ffi.cc |
| Other v3 .cc files | unchanged | Zero direct `nix/...` includes |
| `test/lint-no-direct-tw-include.sh` | ~30 LoC shell | CI lint mechanically enforces V3-NATIVE |

**Documented exemptions (must be re-justified annually):**
- `lower.cc` may include `nix/expr/nixexpr.hh` (AST visitor cost > benefit) — RETIRES when parser Stage 2 ships
- `disk_cache.cc` may include `nix/store/sqlite.hh` (SQLite IS the storage impl)
- `ffi.cc`/`ffi.hh` themselves include the 4 Layer-0 concessions (PosIdx, SourcePath, StorePath, ExperimentalFeatures)
- `parser/*` may include parser-tooling headers

### 5.1 Mechanical enforcement

```bash
#!/usr/bin/env bash
# test/lint-no-direct-tw-include.sh
set -e
EXEMPT="include/v3/ffi.hh|src/libexpr-v3/ffi.cc|src/libexpr-v3/disk_cache.cc|src/libexpr-v3/parser/"
LINT_ERRORS=$(grep -rln '^#include "nix/' src/libexpr-v3/ \
    | grep -v -E "$EXEMPT" \
    | head -10)
if [ -n "$LINT_ERRORS" ]; then
    echo "Direct TW includes found in non-exempt files:"
    echo "$LINT_ERRORS"
    exit 1
fi
```

Parallel to existing `test/lint-no-inline-getenv.sh`. ~30 LoC.

---

## 6. Critical agent review

### 6.1 Agent A (include inventory)

**Strong:**
- Exhaustive enumeration of all 77 TW includes with file:line citations
- Per-category classification with rationale
- Only 1 confirmed dead include — strong evidence the migration discipline has been holding
- Per-file usage counts (primops.cc 477 `nix::*` refs, etc.)

**Where I push back:**
- "1 dead include is the only zero-cost cleanup" undersells the surprising finds — `bridge_yield.cc:10` value.hh + `bytecode_primops.cc:18` nixexpr.hh are both forward-decl-droppable today, also zero-cost.
- The "98% achievable" framing is right but conflates "we have 77 includes" with "they need 77 different solutions." The 4-wrapper-function insight in §3.4 collapses ~25 of them.

### 6.2 Agent B (ffi.h design)

**Strong:**
- The opacity-ladder framework is sharp — for each TW type, the feasible level is clearly explained
- Existing `ffi.hh` audit identifies the 20% completion + names the gap (Value bridge missing)
- Option A vs B decision is well-reasoned
- The 7 real blockers list is comprehensive

**Where I push back:**
- Volume estimate (~1300 LoC ffi.h + 2000 LoC ffi_impl.cc) may be optimistic if templated APIs require more wrapping than estimated
- "lower.cc as documented exemption" is correct but should be temporary — retires fully when parser Stage 2 lands
- The "inline function overhead" concern (forceValue) is real but Agent B's LTO mitigation is plausible only with measurement

### 6.3 Agent C (migration plan)

**Strong:**
- Clean 6-phase breakdown with per-phase acceptance gates and falsifiers
- "70% achievable without parser, 30% gated" is the load-bearing strategic finding
- Risk register addresses the inline-function and template-instantiation concerns
- Recognized `disk_cache.cc` legitimate exemption

**Where I push back:**
- 29-36 PD estimate may be optimistic — Phase 3 (Value graph accessors) is the riskiest and 8-10 days might be 12-15
- "Run concurrent with parser project" is the right call but assumes parser tempo continues; recent parser progress (123/123 Tier 4b) supports this
- The 6-phase ordering puts the riskiest work (Phase 3) third; could reorder Phase 4 (flake) ahead to harvest cheap wins first

### 6.4 Synthesis-level observations

1. **All three agents independently land on "70% without parser, last 30% with parser."** Strong convergence.
2. **The dead-include count (1) is reassuring** — the team's V3-NATIVE discipline has been holding for the entire 3-week arc. No accidental TW leakage to clean up.
3. **The 4-wrapper-function finding (§3.4) is the architectural sharp point** — closing `eval.hh` from 6 files via 4 functions is the highest-leverage single intervention.
4. **`disk_cache.cc` honest exemption** (SQLite IS the storage impl, not a bridge) means "ONE ffi.h" is really "ONE ffi.h + documented exemptions for parser/ + disk_cache.cc + lower.cc-until-parser-Stage-2."

---

## 7. Strategic positioning

### 7.1 The big-picture trajectory

The team is closing in on the V3-NATIVE end-state:
- Parser project: Stage 1.4 Tier 4b complete (123/123 fixtures) — likely Stage 1 SHIP within 1-2 weeks
- FFI Kill Plan: Phases A/1/2/3/C6 landed; derivationStrict bridge retired
- Bridge inventory: 99.8% of HNE retention attributed; action #1 (derivationStrict native) executed
- **NOW: FFI consolidation** — 6-phase plan to a single ffi.h surface

These three workstreams **converge** at the V3-NATIVE end-state:
- v3 owns parser → no TW parse dependency
- v3 owns FFI surface → ONE ffi.h header
- v3 owns bridge minimization → reduced TW retention

### 7.2 Sequencing recommendation

**START PHASE 0 NOW.** Reasons:
1. Phases 0-4 are 70% of the win, parser-independent
2. They touch different files than the parser project (primops.cc, ffi.cc, vm.cc vs parser/)
3. The team has demonstrated capacity for parallel-track work (FFI Kill Plan + parser ran concurrent)
4. The 4-wrapper-function intervention (§3.4) is high-leverage and bounded

**Concurrent timeline:**
- Week 1-2: Phase 0 (audit/skeleton expand) + Phase 1 (trivial leaves)
- Week 3-4: Phase 2 (EvalState opaque) — measurement-gated
- Week 5: Phase 3 (Value accessor functions) — riskiest
- Week 5-6: Phase 4 (flake/fetcher)
- Week 6-7: Phase 6 (CI lint)
- **Week 8-10: Phase 5** (parser-dependent) — only if parser Stage 1 has shipped

**70% milestone at ~week 6; 100% milestone at ~week 8-10** given current parser velocity.

### 7.3 What makes this DIFFERENT from prior architectural work

Unlike the GC strategy (Layer 1 falsified twice), this isn't a measurement-physics gamble. The mechanics are:
- Find TW includes (done — 77 of them)
- Wrap behind ffi.h (Phase 2/3 mechanical work)
- Verify via lint (Phase 6)

There's no "fundamental pin" that could falsify the strategy. Each phase has binary acceptance. The risk is **inline-function perf degradation** (R1), which is bounded — if it materializes, drop to the `ffi-inline.h` private header pattern.

---

## 8. Honest limits

- **"ONE ffi.h" requires honest exemptions**: `parser/`, `disk_cache.cc`, `lower.cc` (until parser Stage 2), and ffi.h itself with 4 Layer-0 concessions. Pure "every v3 file includes only ffi.h" is wrong by ~3-5 files.
- **The 4-wrapper-function intervention (§3.4)** is the high-leverage finding but assumes the templated `state.error<EvalError>(...)` family can be wrapped in ~15 non-template shapes. If TW's error template has more variations than expected, that's 200-400 additional LoC.
- **Inline retirement perf cost (R1, R4)** could be 1-5% wall on the bridge hot path. Mitigation requires LTO + `[[gnu::always_inline]]` discipline; not all platforms benefit equally.
- **Phase 5 effort estimate (3-5d post-parser)** assumes lower.cc retirement is mechanical once parser ships byte-equal AST. If lower.cc has hidden coupling beyond the 27 dynamic_casts, effort could double.
- **The "98% achievable" framing** is by file count. By LoC weight: primops.cc (12.2 KLoC) carries most of the dependency mass; consolidation gain there is the actual measure.
- **`disk_cache.cc` SQLite exemption** is correct but means "single ffi.h" is technically false. The honest claim is "single FFI ffi.h + documented infrastructure exemptions."
- **Mechanical CI lint** can produce false positives; the exemption list needs maintenance.
- **The parser-tempo extrapolation** (Stage 1 SHIP within 1-2 weeks) is based on current velocity continuing; if path syntax / cursed-or wart / nixpkgs corpus validation hits unexpected snags, Phase 5 sequencing slips.
- **`nix::EvalState` has methods that mutate state** (allocValue, addPos). Wrapping requires careful aliasing rules in ffi.h documentation; misuse could introduce subtle bugs.
- **The dead-include count of 1** is reassuring but is based on first-order text grep; second-order check (does `eval.hh` transitively pull header X that v3 doesn't actually need from X but through eval.hh?) wasn't done.

---

## 9. Recommendation

**START PHASE 0 IMMEDIATELY.** Three reasons:

1. **70% of the win is parser-independent** and can land in ~6 weeks of focused work.
2. **The team's V3-NATIVE discipline has been holding** (1 dead include in 77) — there's no surprise leakage to discover; the work is mechanical, not investigative.
3. **The 4-wrapper-function intervention** in §3.4 is the highest-leverage single action: closes `eval.hh` dependency from 6 files via 4 function declarations. Should be the Phase 2 deliverable.

The end-state — one `ffi.h` + ~3 documented exemptions + CI lint enforcing V3-NATIVE mechanically — is the architectural goal the project has been driving toward for months. The audit shows it's achievable within ~8-10 weeks alongside the parser project. The current bridge inventory + FFI Kill Plan + parser project + this consolidation form a **coherent convergent end-state**.

---

## 10. Cross-references

### Strategic context
- [`FFI_BRIDGE_INVENTORY_2026-05-31.md`](FFI_BRIDGE_INVENTORY_2026-05-31.md) — bridge surface (different from header surface)
- [`NATIVE_PARSER_FEASIBILITY_2026-06-01.md`](NATIVE_PARSER_FEASIBILITY_2026-06-01.md) — parser project gates Phase 5
- [`FFI_KILL_PLAN_2026-05-31.md`](FFI_KILL_PLAN_2026-05-31.md) — bridge minimization (orthogonal to header consolidation)
- [`T4_1_TW_RETENTION_AUDIT_2026-06-01.md`](T4_1_TW_RETENTION_AUDIT_2026-06-01.md) — TW arena retention
- [`FFI_PLAN_2026-05-06b.md`](FFI_PLAN_2026-05-06b.md) — original FFI plan
- v3 CLAUDE.md §"V3-NATIVE" — the constraint this work mechanically enforces

### Code anchors
- `include/v3/ffi.hh` (532 LoC SKELETON — Phase 0 expand target)
- `src/libexpr-v3/ffi.cc` (100 LoC — Phase 0/2/3 expand target)
- `primops.cc:38-98` — top TW-include block
- `primops.cc:47` — the dead `nix/expr/print.hh` include
- `primops.cc:5320-5771` — `v3ToTreeWalker` (Phase 3 migration target)
- `primops.cc:5834-6094` — `treeWalkerToV3` (Phase 3 migration target)
- `vm.cc:32-35` — vm.cc TW includes
- `lower.cc:29-31` — the AST traversal exemption
- `bridge_yield.cc:9-11` — bridge plumbing TW includes
- `disk_cache.cc:36-40` — SQLite legitimate exemption
- `bytecode_primops.cc:17-19` — parse + bytecode primop installation
- `test/lint-no-inline-getenv.sh` — model for the new lint
- `include/v3/closure.hh:199-201` — already-correct opacity pattern (`void * bridgeSrc`)

### Methodology
- [[v3-native-constraint]] — the rule this work mechanically enforces
- [[falsification-rule]] — each phase has falsification criteria
- [[measure-twice-cut-once]] §3 — pre-commit gates per phase
- [[memory-first-class]] — perf measurement before/after each phase

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
