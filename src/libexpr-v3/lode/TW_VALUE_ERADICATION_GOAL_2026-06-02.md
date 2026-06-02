# GOAL: eradicate TW-value marshalling — call libraries, not builtins

**Date:** 2026-06-02
**Status:** GOAL DOCUMENT + **DRIVER** for the FFI-consolidation's last phase (combined 2026-06-02 — see §7.1; the audit's Phase 4 IS this fetcher work, delete-not-relocate). Sets the target state + the eradication path.
**Author:** session synthesis (code-grounded trace of remaining `forceValue`/`callFunction`/TW-value sites post-parser)

Companion docs:
- [`FFI_BRIDGE_INVENTORY_2026-05-31.md`](FFI_BRIDGE_INVENTORY_2026-05-31.md) — the bridge surface (frequency numbers predate HEAD; re-measure)
- [`FFI_CONSOLIDATION_AUDIT_2026-06-01.md`](FFI_CONSOLIDATION_AUDIT_2026-06-01.md) — the one-`ffi.h` header-consolidation track (orthogonal to this; see §7)
- [`BRIDGES_HOLD_RETENTION_2026-05-29.md`](BRIDGES_HOLD_RETENTION_2026-05-29.md) — why bridge retention is the RSS story (this goal kills the feeder)
- [`NATIVE_PARSER_FEASIBILITY_2026-06-01.md`](NATIVE_PARSER_FEASIBILITY_2026-06-01.md) — the parser completion that motivated the question

---

## 1. The goal, stated plainly

**Eradicate all v3 ↔ TW `nix::Value` marshalling.** v3 must never construct a TW `nix::Value`, never force a TW `nix::Value`, and never hand a v3 value to a TW builtin that round-trips it back.

**Calling C++ library functions is perfectly fine.** `fetchers::Input::fetchToStore(...)`, `store->writeDerivation(...)`, `realisePath(...)`, `lockFlake(...)` — these are legitimate FFI leaves and stay. The C++ subsystems (libfetchers, libstore, libflake) are the irreducible boundary and we have no intent to reimplement them.

**What we want gone is the BRIDGING** — the `v3ToTreeWalker → callFunction(builtins.X) → treeWalkerToV3` pattern and everything it drags in:
- the three bridge tables (`v3BridgeAttrs` / `v3BridgeClosures` / `v3BridgeLists`)
- the TW→v3 Bridge thunks (`Thunk::bridgeSrc`)
- the BP callback primops (`__v3_force_attr`, `__v3_call_bridge_1`, `__v3_force_list_elem`)
- `forceValue`/`forceAttrs`/`forceList` on TW values
- `callFunction` into TW builtins

The distinction is the whole point: **the leaf (a library call) is irreducible; the marshalling (a `nix::Value` round-trip) is a choice we want to stop making.**

### 1.1 Why this matters

1. **RSS.** Per [`BRIDGES_HOLD_RETENTION_2026-05-29.md`](BRIDGES_HOLD_RETENTION_2026-05-29.md), bridge tables hold **99.8% of live arena on HNE / 99.9% on M5** at end-of-eval — because a bridged TW value transitively pins the entire `nix::Value` graph it reaches. Marshalling a fetcher/derivation result back as a TW `nix::Value` can reconstruct a **massive TW graph** (a nixpkgs-shaped attrset) that then lives forever in the bridge table. Killing the marshalling kills the feeder. This is the single largest memory lever identified in the whole arc.
2. **Correctness.** Every `nix::Value` round-trip is a place where v3 and TW semantics can diverge (string-context encoding, laziness, attr ordering). Fewer crossings = fewer divergence surfaces. The native-VM code review (2026-06-02) found the marshalling boundary is exactly where silent-permissiveness bugs cluster.
3. **Architecture.** The V3-NATIVE rule says "TW permitted ONLY at FFI leaves." A *library call* is a clean leaf. A *`nix::Value` round-trip through a TW builtin* is a leaf wearing an evaluator-graph costume — it pulls TW's value representation, GC ownership, and forcing semantics across the boundary. We want leaves, not costumes.

---

## 2. The key insight: two orthogonal TW axes

The natural intuition — "we did `.nix → v3 AST → VM` natively, so TW values should be gone" — conflates two **independent** dependencies on TW:

| Axis | What it was | Status |
|---|---|---|
| **Compile-time** (parse + lower) | `.nix` → TW `nix::Expr` → v3 IR | **ERADICATED** (parser Stage 2, 2026-06-01: `lower.cc` + AST→nix::Expr bridge DELETED) |
| **Runtime** (FFI-leaf values) | v3 value ⇄ TW `nix::Value` at fetcher/derivation/path/flake leaves | **STILL PRESENT** — this goal targets it |

The parser only ever fed v3's own pipeline. It **never** created a bridge-table entry, **never** called `callFunction`, **never** forced a TW value. So completing it left the runtime axis wholly intact — a separate problem with a separate lever. This document is about the runtime axis.

---

## 3. Where TW values still come from (the inventory)

Code-grounded trace at HEAD (`5d29d96e6`). Every remaining TW-value use traces to an FFI-leaf primop — **zero** trace to parsing.

### 3.1 The dominant source: fetchers (`bridgeBuiltin<N>`)

`primops.cc:11768` (`bridgeBuiltin<N>`) implements 7 fetchers + `fetchFinalTree` by:
```
nargs[i] = v3ToTreeWalker(state, args[i]);          // v3 attrset → TW attrset
ns.callFunction(builtins.fetchTree, *nargs[i], cur); // invoke the TW BUILTIN
result   = treeWalkerToV3(state, cur);               // TW attrset → v3 (Bridge thunks)
```
This is the pattern we want gone. It calls the TW **builtin wrapper** (`builtins.fetchTree`), not the libfetchers **library function** — so it trades in `nix::Value` both directions, populating the bridge tables and arming the BP callbacks.

Fetchers covered: `fetchurl`, `fetchTree`, `fetchGit`, `fetchMercurial`, `fetchClosure`, `fetchTarball`, `filterSource`, `fetchFinalTree`.

### 3.2 Already-clean leaves (no TW value — the model to copy)

These FFI leaves were already converted to library-call + plain-data and produce **no** TW value:
- **derivation writes** — `buildAndWriteDrvNative` calls `store->writeDerivation` directly. The TW `derivationStrict` fallback (`primops.cc:6494`) is **default-off** since 2026-06-01 (`V3_DRV_KEEP_BRIDGE=1` gates it; 2414 native / 0 fallback measured).
- **IFD / realisePath** — returns a `SourcePath` (path string), not a TW value. The `nix::Value` at the call is a throwaway dispatch arg, never bridged back.
- **flake locking** — `ffi::lockFlakeAndRead` returns plain `LockedFlakeInfo`; the flake `outputs` lambda is applied via **native `callClosure`** (`v3_call_flake.cc:504`); `v3EmitTreeAttrs` (`v3_call_flake.cc:206`) builds the result attrset v3-natively from plain data.

**`v3EmitTreeAttrs` + `ffi::lockFlakeAndRead` are the proven pattern.** They already do for flakes exactly what we want for fetchers: call the library, get plain data, build the result v3-native, never touch a TW `nix::Value`.

### 3.3 The downstream machinery (dies when the source dies)

Everything below exists ONLY to service TW values that entered via §3.1. With no TW value entering, all of it becomes unreachable:
- bridge tables `v3BridgeAttrs/Closures/Lists` (`primops.cc:3930-3989`) — fed only by `v3ToTreeWalker`
- TW→v3 Bridge thunks (`Thunk::bridgeSrc`; `allocBridgeThunk`)
- BP callbacks `primV3CallBridge1` / `primV3ForceAttr` / `primV3ForceListElem` (`primops.cc:4541/4925/5183`)
- `forceValue`/`forceAttrs`-on-TW-value sites (force the `builtins` attrset to find a fetcher; force fetcher results; force bridged-back values)
- the `fallbackExpr` blackhole/eviction safety nets (`primops.cc:4570/4668/4764/11050/11151`) — near-zero frequency already

---

## 4. The eradication path

### 4.1 The move

For each FFI-leaf primop that currently round-trips a TW value, replace
```
v3ToTreeWalker(args) → callFunction(builtins.X) → treeWalkerToV3(result)
```
with
```
extract plain args (strings/bools) from the v3 value
  → call the C++ LIBRARY function directly (fetchers::Input::fetchToStore, etc.)
  → build the result attrset V3-NATIVE from the returned plain data
```
This is the `v3EmitTreeAttrs` / `ffi::lockFlakeAndRead` pattern, generalized to fetchers.

### 4.2 The shim shape

Keep libfetchers/libstore/libflake **types** out of the consumer TU. Expose a flat-data shim in `ffi.hh` (consistent with the one-`ffi.h` consolidation):
```cpp
// ffi.hh — plain-data in, plain-data out; no nix::Value, no libfetchers types leaked
namespace nix::v3::ffi {
    struct FetchTreeResult {
        std::string outPath;        // store path
        std::string narHash;
        std::optional<uint64_t> lastModified, revCount;
        std::map<std::string, std::string> lockAttrs;  // for the lockfile fields
        // ... whatever the result attrset needs, as plain data
    };
    FetchTreeResult fetchTree(const FetchInput & in);  // wraps fetchers::Input::fetchToStore
}
```
v3 then builds the `outPath`/`narHash`/`lastModified` attrset with its OWN `Alloc::allocBindings` — never a TW `nix::Value`.

### 4.3 What the TW wrapper does that the direct path must replicate

This is the real work (the obstacle, ~1-2 days per fetcher). The TW `builtins.fetchTree` wrapper does non-trivial normalization the direct call must reproduce:
- attrset → `fetchers::Input::fromAttrs` (+ `fetchSettings` application)
- lockfile / registry indirection
- pure-eval gating (refuse unlocked inputs in pure mode)
- result formatting: `lastModified`, `narHash`, `rev`/`revCount`, the lockfile attr subset

None of this requires a `nix::Value` — it's all `fetchers::Attrs` (a `std::map`-like) ⇄ plain data. The wrapper happens to express it as `nix::Value` because it's a *builtin*; the *library* layer underneath is already plain-data.

### 4.4 The cascade (why this collapses the whole apparatus)

Once §3.1 produces v3-native data instead of a TW value:
1. No TW value enters v3 → `treeWalkerToV3` is never called on a fetcher result.
2. No v3 value is handed to a TW builtin → `v3ToTreeWalker` + `callFunction` gone.
3. No bridged value → BP1/BP2/BP3 callbacks never fire → **deletable**.
4. No `Thunk::bridgeSrc` created → Bridge-thunk OP_CALL / forceValue paths unreachable → **deletable**.
5. No `forceValue`-on-TW-value → the `builtins`-attrset `forceAttrs` (to find the fetcher) also gone.
6. **The three bridge tables become unreachable → deletable.**
7. The bridge-retention RSS story (99.8% HNE / 99.9% M5) loses its feeder.

---

## 5. Phasing

| Phase | Work | Pre-commit gate |
|---|---|---|
| **F0 — re-measure** | Re-run bridge frequency + bridge-retention under HEAD (the inventory's 61-crossings / 0.014% / 99.8% numbers predate today's commits). Establish the current TW-value source breakdown. | Fresh `NIX_V3_BRIDGE_TIMING` + `dumpV3BridgeRetention` numbers on hello/HNE/M5 |
| **F1 — fetchTree direct** | Convert the single highest-frequency fetcher (`fetchTree`, the flake/IFD path) to `ffi::fetchTree` plain-data + v3-native result. Leave the other fetchers on `bridgeBuiltin` behind the same gate. | drvPath byte-equal on flake + IFD workloads; bridge-table entries for fetchTree drop to 0; no perf regression |
| **F2 — remaining fetchers** | `fetchurl`/`fetchGit`/`fetchMercurial`/`fetchTarball`/`fetchClosure`/`filterSource`/`fetchFinalTree`, one at a time, same pattern. | each: drvPath byte-equal; its bridge feeder → 0 |
| **F3 — builtins.path filter** | The one non-fetcher `callFunction` (`primops.cc:9840`): wire the filter closure to re-enter v3's VM per dir-entry instead of `callFunction(builtins.path)`. | `builtins.path` with filter byte-equal |
| **F4 — retire the apparatus** | With all TW-value sources gone: delete BP1/BP2/BP3, the Bridge-thunk OP_CALL paths, `v3ToTreeWalker`/`treeWalkerToV3`, the three bridge tables, the `fallbackExpr` safety nets. | full suite + nixpkgs sweep green; `grep nix::Value` in v3 → only ffi.cc leaf shims |
| **F5 — retire derivationStrict fallback** | Already default-off; delete after a second cross-arch soak confirms `fallback=0`. | `V3_DRV_KEEP_BRIDGE` removed; no TW `derivationStrict` path |

**F1 is the keystone.** It both proves the pattern end-to-end and (per F0's breakdown) likely removes the dominant remaining TW-value source on flake/IFD workloads.

---

## 6. The irreducible minimum (what STAYS)

After eradication, the legitimate TW/library surface is:
1. **libfetchers** — `fetchers::Input::fetchToStore` + friends (network/git/tarball/lockfile). Called directly; returns plain data; **no `nix::Value`**.
2. **libstore** — `store->writeDerivation`, `addToStoreFromDump`, `computeFSClosure`, `ensurePath`, `realisePath`. Already direct; returns `StorePath`/`SourcePath`; **no `nix::Value`**.
3. **libflake** — `parseFlakeRef`, `lockFlake`. Already via `ffi::lockFlakeAndRead` plain-data; **no `nix::Value`**.
4. **libutil** — `Hash`, `CanonPath`, `SourcePath` value types (shared domain types, by-value, not evaluator graphs).

All four are **library calls trading plain data or small value types**. None constructs, forces, or returns a TW evaluator `nix::Value`. That is the target state: TW is a set of libraries v3 links against, not an evaluator v3 trades graphs with.

---

## 7. Relationship to the one-`ffi.h` consolidation

This goal and [`FFI_CONSOLIDATION_AUDIT_2026-06-01.md`](FFI_CONSOLIDATION_AUDIT_2026-06-01.md) are **complementary but distinct**:
- **Header consolidation** (that doc): all `#include "nix/..."` collapse to one `ffi.h`. About *compile-time* coupling — which TUs see TW types.
- **Value eradication** (this doc): no `nix::Value` ever crosses. About *runtime* coupling — whether evaluator graphs flow across the boundary.

They reinforce each other: the plain-data shims (§4.2) are exactly what lets `ffi.h` expose fetchers without leaking libfetchers types — so doing the value eradication makes the header consolidation's fetcher phase (audit Phase 4) cleaner, and vice versa. **Sequence them together on the fetcher path.** When both are done: v3 source includes one `ffi.h`, and that `ffi.h` trades only plain data + small value types — the V3-NATIVE rule becomes mechanically true, not aspirational.

### 7.1 DECISION (2026-06-02): combined — eradication DRIVES the consolidation's last phase

The two tracks are now **one plan**, and **this eradication goal is the driver** for the consolidation's final phase.  Committed in `FFI_CONSOLIDATION_AUDIT_2026-06-01.md §0b` (the unified F1–F5 blueprint).  Concretely:

1. **The audit's Phase 4 IS this doc's fetcher work** — not a parallel effort.  The audit no longer "relocates the #875 bridge subsystem"; instead this goal's F1–F4 **kill the feeder and DELETE the apparatus**.  When the fetcher round-trips become plain-data library calls, the bridge tables + `v3ToTreeWalker`/`treeWalkerToV3` + BP1/2/3 + Bridge thunks become dead code → deleted (F4) → and that simultaneously drops `value/context.hh` + the `nix::Value`/`forceValue`-on-TW `eval.hh` surface + memory-source-accessor from `primops.cc` (the audit's biggest remaining coupled chunk).  **Delete > relocate.**
2. **The irreducible-leaf cleanup follows** (audit's remaining): the derivation-build leaf (store-api/content-address/derivations) + the parse/path leaf (canon-path/positions/corepkgsFS) get the same plain-data shim treatment proven this session (`storeRefsContextFor` / `pathFetchToStore` / `addTextToStore` / `lockFlakeAndRead` / `v3EmitTreeAttrs`), derivationStrict LAST; then `eval.hh` falls out → baseline 0 → flip the lint to strict end-state.

**F0 evidence (2026-06-02, `NIX_VM_STATS=1 NIX_V3_DUMP_BRIDGE_RETENTION=1`):**
- hello.drvPath: **0 bridge entries** (no fetch) — confirms the bridge is now SOLELY fetcher-fed post-native-flake + post-native-derivationStrict.
- `builtins.fetchGit {url=<local repo>}`: result byte-identical TW↔v3 (rev/shortRev/revCount/lastModified/outPath), yet **0 retained bridge-table entries** — because a fetchGit result is flat scalars (no closures/lazy-attrs to retain).  ⇒ the 99.8%-HNE/99.9%-M5 retention comes specifically from **large-result** fetchers (haskell.nix-shaped materialization); the round-trip cost + the F4 deletion goal apply to ALL fetchers regardless of retention.
- **F1/F2 normalization risk made concrete:** `builtins.fetchGit` uses `emptyRevFallback=TRUE` (dirty-repo empty-sha1 rev), which the flake-path `ffi::readTreeAttrs` deliberately dropped (dead in the flake path).  Reusing the flake machinery for fetchers REQUIRES restoring that branch + per-fetcher arg normalization (the §4.3 work) — and needs a **dirty-repo + large-result oracle**, not just the clean-repo offline oracle.  This is the §9 divergence risk, confirmed.

**Net:** the combination is in place and bidirectional (this §7.1 ⇄ audit §0b).  Execution = the F1–F5 arc, eradication-driven, gated per-fetcher on drvPath/rev byte-equality with the full oracle set (clean + dirty + large-result).

---

## 8. Acceptance — what "done" looks like

- `grep -rn 'callFunction' src/libexpr-v3/*.cc` → only the ffi.cc shim definitions (no consumer calls into TW builtins).
- `grep -rn 'v3ToTreeWalker\|treeWalkerToV3' src/libexpr-v3/` → deleted.
- The three bridge tables + BP1/BP2/BP3 + Bridge-thunk machinery → deleted.
- `forceValue`/`forceAttrs`/`forceList` on a TW `nix::Value` → zero sites (only v3-Value forces remain).
- Bridge-retention measurement (`dumpV3BridgeRetention`) → tables empty / not present; the 99.8%-HNE / 99.9%-M5 retention feeder is gone.
- nixpkgs sweep + lang 143/143 + hello/HNE/M5 drvPath byte-equal throughout.
- The only remaining TW touch is library calls returning plain data / `StorePath` / `SourcePath`.

---

## 9. Honest limits / risks

- **Per-fetcher normalization is real work** (~1-2 days each). The TW wrapper's arg-normalization + result-formatting (lockfile fields, `lastModified`, pure-eval gating) must be reproduced exactly or drvPath/lock divergence results. This is the bulk of the effort and the main divergence risk — gate each fetcher on drvPath byte-equality.
- **`filterSource` / `builtins.path` filter** (F3) is the awkward one: the filter is a *Nix closure* that must run per directory entry. The direct path needs to re-enter v3's VM from inside the libstore path-copy callback — solvable (v3 already has callClosure) but more involved than a pure data fetch.
- **`fetchFinalTree`** uses `internalPrimOps`, not a public builtin — confirm the library entry point exists.
- **Frequency caveat:** the inventory's numbers predate HEAD. F0 must re-measure before committing F1's threshold — do not assume fetchers are the dominant source without the fresh measurement (they almost certainly are post-derivationStrict-retirement, but verify per [[same-host-bisect]]).
- **This is a multi-week arc** (F1 ~2-3 days incl. shim + re-measure; F2 ~1-2 days × 7; F3 ~3-5 days; F4 cleanup ~2-3 days). It can run concurrent with the header consolidation's Phase 4 (they share the fetcher work).
- **Not a wall lever.** Bridge crossings are 0.014% of wall — this is an RSS + correctness + architecture goal, not a speed goal. Frame and measure it RSS-primary per [[memory-first-class]].

---

## 10. Cross-references

- [`FFI_BRIDGE_INVENTORY_2026-05-31.md`](FFI_BRIDGE_INVENTORY_2026-05-31.md) — the bridge surface (frequency numbers stale; re-measure in F0)
- [`FFI_CONSOLIDATION_AUDIT_2026-06-01.md`](FFI_CONSOLIDATION_AUDIT_2026-06-01.md) — header consolidation (§7: sequence the fetcher work together)
- [`BRIDGES_HOLD_RETENTION_2026-05-29.md`](BRIDGES_HOLD_RETENTION_2026-05-29.md) — the RSS motivation (99.8%/99.9% retention this goal de-feeds)
- [`NATIVE_PARSER_FEASIBILITY_2026-06-01.md`](NATIVE_PARSER_FEASIBILITY_2026-06-01.md) — the compile-time axis that's already done
- [`NATIVE_VM_CODE_REVIEW_2026-06-02.md`](NATIVE_VM_CODE_REVIEW_2026-06-02.md) — the marshalling boundary is where silent-permissiveness bugs cluster
- `v3_call_flake.cc:206` (`v3EmitTreeAttrs`) + `ffi::lockFlakeAndRead` — THE proven plain-data pattern to generalize
- `primops.cc:11768` (`bridgeBuiltin<N>`) — the pattern to eliminate
- `primops.cc:3930-3989` (bridge tables) — the apparatus to retire
- [[v3-native-constraint]] — "TW permitted only at FFI leaves"; this makes it mechanically true
- [[memory-first-class]] — RSS-primary framing
- [[bridges-hold-retention]] — the retention finding

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
