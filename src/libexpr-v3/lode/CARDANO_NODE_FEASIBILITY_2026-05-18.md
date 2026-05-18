## v3 → cardano-node feasibility (research agent report) — 2026-05-18

**Constraint**: No modifications to cardano-node. Add capabilities to v3
only. Prefer VM-native (Nix-source-compiled bytecode); FFI only at
system boundaries.

## Executive summary

1. **`getFlake` is trivially bridgeable** via v3's existing
   `bridgeBuiltin<N>` pattern (`primops.cc:8525`). TW registers it
   through `evalSettings.extraPrimOps` at `libflake/settings.cc:14`;
   once `libflake` is linked into v3, the bridge picks it up
   automatically. **Effort: ~30 lines.**
2. **IFD is the real blocker.** v3's `primImport` (`primops.cc:6207`)
   takes the path arg verbatim and *never calls `realisePath` /
   `realiseContext`*. TW does (`libexpr/primops.cc:434-468`). The bridge
   needs to call `state.nixEvalState->realisePath(noPos, twPath)` with
   string-context preserved. **Effort: ~50 lines + correctness audit.**
3. **Flake-output value is just a recursive attrset** built by
   `call-flake.nix`. No special VM support needed once IFD works.
4. **haskell.nix's plan-to-nix is one primary IFD** for cabalProject
   without materialization. cardano-node uses the non-materialized
   path; we cannot change it, so we must support IFD.
5. **Per-force perf gap (~200×) is the long-pole completion blocker**.
   Capability fixes are independent and small; perf is the separate
   IR-optimization track.

## A. `getFlake` primop in TW

| Item | Location |
|---|---|
| Primop definition | `src/libflake/flake-primops.cc:29-77` |
| Factory | `nix::flake::primops::getFlake(const Settings &)` — captures `settings` by reference |
| Body operations | `parseFlakeRef` → `lockFlake` → `callFlake` |
| FFI leaf | `void callFlake(EvalState &, const LockedFlake &, Value &)` at `src/libflake/flake.cc:928` |
| Registration in TW | `evalSettings.extraPrimOps.emplace_back(primops::getFlake(*this))` at `src/libflake/settings.cc:14` |
| Internal helper | `requireInternalFile(state, "call-flake.nix")` at `flake.cc:920` + `state.internalPrimOps["fetchFinalTree"]` |

**Return value**: TW `callFlake` writes a recursive attrset matching
`call-flake.nix:73-89`: `outputs // sourceInfo // { outPath; inputs;
outputs; sourceInfo; _type = "flake"; }`.

### v3 bridge strategy — two options

**Option 1 (one-liner, recommended)**: Add
```cpp
void primGetFlake(EvalState & s, Value * a, Value & o) {
    bridgeBuiltin<1>("__getFlake", s, a, o);
}
```
next to `primFetchTree` (`primops.cc:8557`) and register both `getFlake`
and `__getFlake` in `registerBuiltinPrimOps`. Because TW already
publishes the primop via `extraPrimOps`, `getBuiltins()` in
`bridgeBuiltin` finds it. **No new linkage**.

**Option 2 (deeper bridge)**: Skip TW's `prim_getFlake` lambda and call
`nix::flake::callFlake(state.nixEvalState, lockedFlake, twValue)`
directly, then `treeWalkerToV3`. Adds direct `libflake` linkage. Choose
only if Option 1 hits a bridge-conversion edge case.

## B. Flake-output evaluation pattern

When TW evaluates `(getFlake "github:...").packages.aarch64-darwin.cardano-node.name`:

1. `prim_getFlake` returns a TW `Value` whose root is the result of
   `callFlake → call-flake.nix:105 (allNodes.<root>.result)`.
2. Structurally: `outputs (inputs // {self=result;}) // sourceInfo // {
   outPath, inputs, outputs, sourceInfo, _type="flake" }`
   (`call-flake.nix:71-89`). All input fetches are **lazy** thunks; only
   the inputs reachable from the selected attrpath get forced.
3. Selecting `.packages` reaches into the user's outputs lambda;
   `.aarch64-darwin` selects per-system; `.cardano-node` is a derivation
   (attrset with `type="derivation"`, `drvPath`, `outPath`, `name`, ...);
   `.name` is a plain string from `drv.env["name"]`.
4. **Just attrset traversal + thunk forcing.** v3 already handles this.
   Risk: per-force cost compounding on the haskell.nix→nixpkgs chain.

## C. IFD path in TW

| Step | Location |
|---|---|
| `import` primop entry | `prim_import` impl lambda inline at `libexpr/primops.cc:517-537`; dispatches to `import()` at `libexpr/primops.cc:434` |
| Path realisation | `state.realisePath(pos, vPath, std::nullopt)` at `libexpr/primops.cc:436` |
| Context realisation (the IFD step) | `EvalState::realiseContext` at `libexpr/primops.cc:72`; declared `libexpr/include/nix/expr/eval.hh:1127` |
| Build daemon invocation (the FFI leaf) | `buildStore->buildPaths(buildReqs, bmNormal, store)` at `libexpr/primops.cc:258` (or `buildPathsWithResults` at line 178) |
| IFD gate | `settings.enableImportFromDerivation` at `libexpr/primops.cc:111` |
| Result: `derivationToValue` | `libexpr/primops.cc:359-397` |

### v3 bridge strategy

In `primImport` (`primops.cc:6207`), before parsing the file, call
`state.nixEvalState->realisePath(noPos, twPath)` where `twPath` is
built from `args[0]` *with its string context preserved*. This is
**already partially done** for `pathExists` (`primops.cc:2218-2247`)
and `readFile` (`primops.cc:2640-2663`); replicate the pattern.

**String-context bridging is the subtle part**: v3 strings carry context
via the side-table at `lookupStringContextEntries` (used in
`primOutputOf`, `primops.cc:8488`). For `import drv-output-string`, we
must construct a TW `Value` with the same `NixStringContext` entries;
then `realisePath` recognises the `DrvDeep` entry and calls
`buildPaths`.

**Synchronization**: TW pauses eval synchronously while the build runs.
v3 can do the same — eval is single-threaded inside a CompilationUnit;
calling `buildPaths` from `primImport` simply blocks. No new thread /
fiber.

## D. haskell.nix IFD pattern

- **Single primary IFD per project**:
  `pkgs.haskell-nix.callCabalProjectToNix` → builds a derivation that
  runs `cabal new-configure` + `plan-to-nix` → produces `.nix` files
  → imports them as `pkgs.nix`. https://input-output-hk.github.io/haskell.nix/
- **Secondary IFDs**: `hackage-package`, materialization checks,
  fixup scripts in `mkCabalProjectPkgSet`. Each is a single
  build → import. Not a deep chain.
- **Avoidance (NOT applicable to us)**: `plan-sha256` + `materialized =
  ./materialized-plan` lets users skip the IFD. Cardano-node does
  **not** use materialization.
- **Implication**: v3 only needs to support **one IFD pattern** —
  `import storePath` where the path has `DrvDeep` context — to unblock
  the haskell.nix pipeline. Generic IFD via `import (drv).outPath`
  falls out of the same `primImport` fix.

## E. Minimum-viable plan

### Fast path (mostly TW bridges) — Phase MVP

| # | Task | Effort | Depends |
|---|---|---|---|
| 1 | Add `primGetFlake` + register `getFlake` / `__getFlake` via `bridgeBuiltin<1>` | ~1 hr | — |
| 2 | Verify v3 links `libflake` in `meson.build`; add if missing | ~30 min | (1) |
| 3 | Extend `primImport` (`primops.cc:6207`) to call `nixEvalState->realisePath` when `args[0]` is a string with `DrvDeep` context (mirror `pathExists` §1.7 pattern) | ~3-4 hr | (1) |
| 4 | Add `V3_DBG_IFD` env-tracer with kill-criterion comment (Rule 0) | ~30 min | (3) |
| 5 | Regression repros: `test/repro-ifd-trivial.nix`, `test/repro-getflake-min.nix` | ~1 hr | (1)(3) |
| 6 | Smoke test: `nix-instantiate --eval --expr '(builtins.getFlake "github:nixos/nixpkgs/release-24.05").lib.version'` via v3 | — | (1)-(5) |

**Estimated total: ~1 day.** Enough to *attempt* cardano-node eval —
likely to bottleneck on perf, not capability.

### V3-native path (after MVP)

| # | Task | Notes |
|---|---|---|
| 7 | Port `call-flake.nix` to v3 source-compiled bytecode (`installBytecodePrimop`) | Reduces flake-eval bridge tax; pure data |
| 8 | Port `imported-drv-to-derivation.nix.gen.hh` similarly | Touched on every IFD |
| 9 | Leave `callFlake`'s C++ surface (lock parsing, fetch) as the FFI leaf | Honors V3-NATIVE rule |

Pure-data optimisations, consistent with `LESSONS_LEARNED §1.2`.

### Perf track (separate)

ACTION_PLAN Phases 1-2 (iterative `forceValue`, A12b depth=5000) are
prerequisites for actually *completing* eval, not just attempting it.

## F. Progressive milestones

1. **M1** — trivial IFD: `import (builtins.toFile "x.nix" "42")` → `42`.
2. **M2** — real IFD: `import (runCommand "x" {} "echo 42 > $out")` → `42`.
3. **M3** — `getFlake` + lazy outputs: `(builtins.getFlake "github:nixos/nixpkgs/release-24.05").lib.version`.
4. **M4** — haskell.nix small project: tiny package via `getFlake "github:input-output-hk/haskell.nix"` → `.pkgs.hello.name` (or similar).
5. **M5** — cardano-node: `(builtins.getFlake "github:IntersectMBO/cardano-node").packages.aarch64-darwin.cardano-node.name`.

Stop at any milestone that reveals a v3-native blocker not on the
roadmap — file as a sub-letter on A12 (per ACTION_PLAN Rule 3).

## Code-citation appendix

### TW reference points (read-only)
- `src/libflake/flake-primops.cc:29-77` — `prim_getFlake` body
- `src/libflake/flake.cc:920` `requireInternalFile`, `928` `callFlake`, `404` `getFlake(EvalState&)`
- `src/libflake/call-flake.nix:1-106` — pure-Nix outputs assembly
- `src/libflake/settings.cc:14-16` — extraPrimOps registration
- `src/libexpr/primops.cc:72` `realiseContext`, `178/258` `buildPaths`, `434-468` `import()`, `359-397` `derivationToValue`, `517` `primop_import`
- `src/libexpr/include/nix/expr/eval.hh:911` `internalPrimOps`, `1127` `realiseContext`

### v3 modification points (touch in the MVP)
- `src/libexpr-v3/primops.cc:6207` — `primImport`: extend with `realisePath` call
- `src/libexpr-v3/primops.cc:8525-8561` — `bridgeBuiltin` + fetch primop pattern: add `primGetFlake` here
- `src/libexpr-v3/primops.cc:8749` block — register `getFlake` / `__getFlake`
- `src/libexpr-v3/primops.cc:2218-2247` — existing `pathExists` `realisePath` pattern to replicate
- `src/libexpr-v3/meson.build` — verify `libflake` in `dependencies`

### Uncertainty markers
- "v3 already links `libflake`" — **unverified**; check `meson.build` first.
- "Option 1 vs Option 2 same behavior" — **uncertain**: `prim_getFlake`'s capture of `Settings &` (`flake-primops.cc:31`) is bound at TW startup; if v3 disables the experimental-feature gate differently, Option 2 may be needed.
- "haskell.nix is a single primary IFD" — based on docs; exact derivation count not verified by source-walking haskell.nix itself.
- "200× per-force gap" — quoted from task brief; not independently measured here.

---

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0
