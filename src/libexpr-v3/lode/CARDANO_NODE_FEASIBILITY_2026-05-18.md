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

---

## G. Execution plan — detailed day-by-day (added 2026-05-18)

This section is the implementation plan for §E's Minimum-Viable Plan. The user's task framing: **#5 — cardano-node feasibility execution; effort 3-5 days; value: validates "v3 owns a real-world workload"**.

**Critical caveat upfront**: §A-F's 30+50 LoC + "~1 day MVP" estimate is optimistic. Realistic estimate after considering integration debugging, context-bridging subtleties, and verification of the four explicit uncertainty markers in §H is **7-10 days for cardano-node end-to-end M5**. The 3-5 day budget would land M1-M3 (getFlake + trivial IFD + nixpkgs getFlake works) but not cardano-node itself unless everything goes well. Plan accordingly.

### G.0 — Pre-flight (½ day, MUST happen before Day 1)

The feasibility doc declares 4 explicit uncertainty markers (§H). All must be resolved before committing to the plan:

| Uncertainty | Verification | Cost | If wrong |
|---|---|---|---|
| "v3 already links `libflake`" | `grep libflake src/libexpr-v3/meson.build` | 5 min | Add 1 line to meson.build; rebuild |
| Option 1 vs Option 2 — `Settings &` capture works under v3's gate | Try Option 1 first; if `getBuiltins()` returns nullptr for `getFlake`, fall back to Option 2 | covered in Day 1 | Adds ~30 LoC direct libflake call |
| "haskell.nix is a single primary IFD" | Read `pkgs.haskell-nix.callCabalProjectToNix` source in current haskell.nix | 30 min | If multi-IFD, M4-M5 needs more IFD calls per eval (still works, just slower) |
| "200× per-force gap" | This is the EXTEND_DERIVATION_INVESTIGATION number — but it's pre-IR-Phases-A-G. **Re-measure after today's Phases A-G landed.** | 1-2 hr | If post-A-G gap is still 200× or worse: cardano-node likely won't complete in reasonable time; downgrade M5 to "starts eval, measure where it stalls" |

**Pre-flight is mandatory.** Skipping it converts uncertainty into wasted Day 1-2 work. The most consequential check is the **200× re-measurement** — if Phases A-G haven't moved it materially, cardano-node M5 is structurally unreachable in this sprint and we should pivot to fixing the perf gap first (per yesterday's risk #1).

**Exit criterion**: a one-paragraph note in this section answering each of the 4 uncertainties with a measured/confirmed answer.

**Kill criterion**: if 200× re-measurement shows the gap unmoved post-A-G, **stop**. Cardano-node won't complete; the Phase 1.5 measurement spike must come first.

### G.1 — Day 1: `getFlake` bridge (Option 1 path)

**Goal / hypothesis killed**: "v3 cannot resolve `builtins.getFlake` because of libflake linkage." Falsified if a trivial `(builtins.getFlake "github:nixos/nixpkgs/release-24.05").lib.version` evaluates and returns a string under v3-direct.

**Deliverables**:
- `primGetFlake` wrapper in `primops.cc` (mirror of `primFetchTree` at line 8557 per feasibility §A).
- Registration of `getFlake` and `__getFlake` in `registerBuiltinPrimOps` (block around line 8749).
- Test fixture: `test/repro-getflake-min.nix` with `# RUN: v3-eval --file %s | v3-check %s` and a `# CHECK: "24.05"` or similar.
- Verify libflake linkage in `meson.build`; add if missing.

**Sub-tasks** (estimated time per):
1. (5 min) Verify libflake linkage check (G.0 pre-flight item).
2. (1 hr) Implement `primGetFlake`. Follow `primFetchTree` boilerplate. Inline `getenv` call sites get retirement-criterion comments per ACTION_PLAN rule 2.
3. (30 min) Register `getFlake` and `__getFlake`.
4. (1 hr) Add fixture + verify it passes via `test/run-ir-checks.sh` or via a direct `v3-eval` invocation in a `test/run-getflake-tests.sh`.
5. (30 min) Add Rule 0 retirement-criterion comment for any new env var introduced.
6. (1-3 hr) DEBUGGING BUFFER for the inevitable conversion-edge-case if `getBuiltins()` doesn't return `getFlake` cleanly. Falls back to Option 2 if so.

**Day 1 exit criterion**: M3 milestone (`getFlake` lazy outputs) completes end-to-end on the nixpkgs flake. Specifically: `v3-eval --file <fixture> --expr '(builtins.getFlake "github:nixos/nixpkgs/release-24.05").lib.version'` returns a Nix string like `"24.05"`.

**Day 1 kill criterion**: if Option 1 + Option 2 both fail to bridge `getFlake` by EOD, the assumption "TW's `extraPrimOps` registration is visible to `getBuiltins()`" is false. Pause. Diagnose via `V3_DBG_PRIMOP_REGISTRY` or equivalent. Likely root cause: `evalSettings.extraPrimOps` is populated late vs v3's `getBuiltins()` lookup. Fix needs more research; expand scope.

**Risk: HIGH**. The whole approach assumes TW's `extraPrimOps` mechanism plays nicely with v3's bridge. This is the feasibility doc's Uncertainty #2 — partial verification only.

### G.2 — Day 2: IFD trivial case (M1 + M2 milestones)

**Goal / hypothesis killed**: "v3's `primImport` cannot drive a build daemon to realise a derivation output." Falsified if `import (builtins.toFile "x.nix" "42")` returns `42` AND `import (runCommand "x" {} "echo 42 > $out")` returns `42`.

**Deliverables**:
- Extend `primImport` at `primops.cc:6207` to detect a Value with `DrvDeep` string context, then call `state.nixEvalState->realisePath(noPos, twPath)`.
- String context bridging from v3 side-table to TW `NixStringContext`. Mirror pattern from `primOutputOf` (line 8488 per feasibility doc).
- Test fixtures:
  - `test/repro-ifd-toFile.nix` — M1: trivial `toFile`-based IFD.
  - `test/repro-ifd-runCommand.nix` — M2: real build via `runCommand`.
- `V3_DBG_IFD=1` env-tracer with inline retirement-criterion comment.

**Sub-tasks**:
1. (1 hr) Read `pathExists` `realisePath` pattern (`primops.cc:2218-2247`) carefully. Understand the v3 side-table → TW context conversion.
2. (2-3 hr) Implement primImport extension: detect path-with-DrvDeep-context, call `realisePath`, then proceed with existing import flow on the realised path.
3. (1 hr) Add `V3_DBG_IFD` tracer (each IFD invocation logs: path before realisation, path after, time taken, exit status).
4. (2 hr) M1 fixture + verification. `toFile` produces a no-build derivation; tests the realisePath plumbing without a real build.
5. (2 hr) M2 fixture + verification. `runCommand` triggers actual daemon invocation. **Requires a working nix-daemon in the test env.**
6. (1-2 hr) DEBUGGING BUFFER — context-bridging is the subtle part; expect surprises.

**Day 2 exit criterion**: M1 and M2 both green. v3 can drive a build via IFD.

**Day 2 kill criterion**: if string-context bridging fails such that `realisePath` doesn't see the `DrvDeep` entry, the assumption "v3 side-table → TW `NixStringContext` is a straight port" is false. Mitigation: inspect what `primOutputOf` (line 8488) does for an OutputOf case — it's the closest existing pattern. If still stuck: this is the new architectural risk; escalate.

**Risk: HIGH**. The string-context bridging is feasibility doc's "subtle part" — partial verification. CI build-daemon setup is its own issue.

### G.3 — Day 3: cardano-node attempt (M4 → M5)

**Goal / hypothesis killed**: "v3 cannot evaluate haskell.nix's IFD-based plan-to-nix path." Falsified if M4 (haskell.nix small project) completes; M5 (cardano-node) is the stretch target.

**Deliverables**:
- M4 fixture: a haskell.nix small project getFlake + name query. Identify a known-small haskell.nix sample if available.
- M5 attempt: `v3-eval --expr '(builtins.getFlake "github:IntersectMBO/cardano-node").packages.${builtins.currentSystem}.cardano-node.name'`. Run with `V3_TIMING=1 V3_DBG_FORCES=1 V3_DBG_IFD=1` to profile.

**Sub-tasks**:
1. (1 hr) Find or construct M4 fixture (small haskell.nix-using project).
2. (1-2 hr) M4 verification. Expect either: works (good), or surfaces a secondary IFD pattern the feasibility doc didn't predict.
3. (4-6 hr) M5 attempt and debugging. Plausible failure modes:
   - **(a)** Eval starts but doesn't finish within tolerable time. Most likely outcome.
   - **(b)** Eval hits a v3-direct correctness bug not exposed by smaller workloads.
   - **(c)** Eval OOMs (no MAX_HEAP cap; Boehm heap grows beyond machine memory).
   - **(d)** TW interop hits an edge case (e.g., a TW thunk in the getFlake result that v3 can't force).
   - **(e)** haskell.nix uses a secondary IFD pattern we didn't account for.

For each failure mode, the diagnosis differs:
   - (a): bench-style profile; report where time goes; this becomes the perf-decomposition document.
   - (b): file as a new top-level investigation (NOT a new A-letter per Rule 3 of CLAUDE.md).
   - (c): document the memory profile; argue for `MAX_HEAP_LIMIT` implementation (separate work item).
   - (d): patch the FFI bridge; document the pattern in feasibility doc.
   - (e): document the additional IFD pattern; extend primImport handling.

**Day 3 exit criterion**: M4 passes. M5 either passes OR has a documented failure mode + decomposition (which is itself valuable; per Rule 0, the "M5 eval is achievable" hypothesis is killed/confirmed/refined).

**Day 3 kill criterion**: if M4 fails with a v3-direct bug not on the roadmap, pause M5 and file the bug. Stop trying to push to M5 on a foundation that isn't solid.

**Risk: VERY HIGH**. The 200× per-force gap (from EXTEND_DERIVATION_INVESTIGATION) means cardano-node eval, which is ~60s in TW, plausibly extrapolates to **1-12 hours in v3-direct**. M5 may complete but take so long that it doesn't "validate v3 owns a real-world workload" in any practical sense.

### G.4 — Days 4-5: regression hardening + buffer

**Goal**: lock in the work. If M5 succeeded, validate it stays working across IR phases. If M5 failed, document failure mode + propose follow-up.

**Sub-tasks**:
1. (2 hr) Add fixtures from M1-M5 to `test/ir-fixtures/` (or `test/run-cardano-tests.sh` for non-FileCheck tests). Wire into `ninja test`.
2. (2 hr) Update `CARDANO_NODE_FEASIBILITY_2026-05-18.md` with measured perf numbers (if M5 succeeded) or characterized failure mode (if not).
3. (2 hr) If MVP succeeded: add `V3_DBG_IFD` and `getFlake` retirement criteria — these are sticky environment hooks now, not bisect-only.
4. (Buffer) Days 4-5 are deliberately under-loaded to absorb the inevitable Day 1-3 slippage. Realistically, much of "Day 4" will be finishing Day 2's IFD work, and "Day 5" will be debugging M5.

**Days 4-5 exit criterion**: cardano-node feasibility doc updated with measured outcomes for M1-M5; fixtures added to the test corpus.

### G.5 — Total realistic effort and decision tree

**Realistic estimate**: 5-7 days for M1-M4; **7-10 days for M5** if no major surprises. **3-5 days if cardano-node moves to materialized or if M5 is downgraded to "starts eval and reports decomposition."**

Decision tree at end of Day 3:

```
Day 3 end state
├── M4 passed, M5 passed → SUCCESS. Validate + ship.
├── M4 passed, M5 stuck at perf → Document decomposition; this becomes input to Phase 1.5 spike.
├── M4 passed, M5 stuck at correctness bug → File as `#cardano-node-bug-X`; defer M5 to a future sprint.
├── M4 failed at haskell.nix-specific pattern → Document the pattern; extend feasibility doc; this is real progress even if M5 unreachable.
└── M4 failed at v3-direct bug → STOP. This isn't a cardano-node sprint; it's a v3 bug. Fix the bug; revisit the sprint when v3 is stable on the smaller workload.
```

## H. Critical self-review of this plan (added 2026-05-18)

Following the LESSONS Part 0 discipline. Eight things that might be wrong with the plan above:

**(H1)** **The 3-5 day budget is optimistic by ~2×.** Realistic: 7-10 days for M5. The user's task framing presumably came from §E's "~1 day MVP" estimate, which only covers M1-M2 trivial cases. M3-M5 add days for debugging the haskell.nix-specific behavior. Honest framing: 3-5 days lands M1-M4 confidently; M5 is a stretch.

**(H2)** **The 30+50 LoC estimate is optimistic by ~3-5×.** Real code is boilerplate-heavy. Realistic: ~100 LoC for getFlake bridge, ~200 LoC for IFD bridge plus context-handling and tests. Total ~300-500 LoC, not 80.

**(H3)** **The "single primary IFD" claim for haskell.nix is unverified.** The feasibility doc cites haskell.nix docs but not haskell.nix source. If cardano-node's flake uses materialization (`materialized = ./materialized-plan`), getFlake works without IFD AT ALL and the plan should simplify dramatically. **Day 0 pre-flight item**: read `flake.nix` of `github:IntersectMBO/cardano-node` and check for materialization usage. If present, halve the plan.

**(H4)** **Perf risk is the showstopper, not capability.** Feasibility doc §A says "per-force perf gap (~200×) is the long-pole completion blocker" — and we're not addressing it in this sprint. M5 might literally take 6+ hours of eval. "Validates v3 owns a real-world workload" is false if eval takes longer than rebooting cppnix and running TW. **Mitigation**: re-measure 200× post-IR-Phases-A-G in pre-flight. If still 200×+, downgrade scope to "M4 + decomposition of where cardano-node stalls."

**(H5)** **Memory risk is undiscussed in the existing doc.** `hello.drvPath` uses ~1GB+ Boehm heap; cardano-node likely uses 10-50GB. Without `MAX_HEAP_LIMIT` enforced + Stage 3 nursery default-on, M5 may OOM on most developer machines. **Mitigation**: implement `MAX_HEAP_LIMIT` first (3 days of independent work, per MAX_HEAP_LIMIT_DESIGN doc). OR run M5 only on machines with 64GB+ RAM.

**(H6)** **Rule 0 compliance for the daily plan is shallow.** Each day's "hypothesis killed" is loose. The real hypothesis at M5 is "v3 can evaluate cardano-node in X time using Y memory." Without bounding X and Y, M5 is unfalsifiable — "it eventually completes" is not a useful exit criterion. **Mitigation**: bound M5's success criterion as "M5 completes in ≤10× TW time AND uses ≤4× TW memory." If it succeeds but exceeds these, that's a falsification of the "practical viability" hypothesis even if correctness is achieved.

**(H7)** **The strategic priority of this work is questionable RIGHT NOW** vs higher-leverage alternatives. Per yesterday's review:
   - **Higher priority**: re-measure 200× decomposition post-IR-Phases-A-G (1-2 days; informs ALL next-step decisions).
   - **Higher priority**: Phase 1.5 measurement spike (2-3 days; gates Stages 10-13 commitment indefinitely).
   - **Higher priority**: Stage 3 (nursery) Phase D implementation (3-4 weeks; load-bearing for ≤3× drvPath).
   - **Higher priority**: Phase D singleton-closure vs Stage 5/6 PIC interaction decision (1-paragraph note; prevents future rework).
   
   Cardano-node feasibility execution is a **demo-quality** outcome that doesn't unlock other work. The above 4 items each unlock multiple downstream decisions. Doing cardano-node first is choosing visibility over leverage.

   **However**: there's a counter-argument. M5 is a hard real-world test that exposes integration bugs we can't see otherwise. M5 failure modes are themselves measurement data. So even M5-doesn't-complete teaches us things the spike wouldn't.

**(H8)** **CI implications are undiscussed.** IFD tests require a working `nix-daemon`. CI runners may not have one or may not allow IFD builds. M2 fixture (which requires real `runCommand` execution) is fundamentally different from M1 (which is `toFile` and runs without daemon). **Mitigation**: tag M2+ fixtures as `requires-daemon`; run them in a separate CI stage.

## I. Honest reassessment

**Original task framing**: "3-5 days, validates v3 owns real-world workload."

**Honest reframing**:
- M1-M3 (trivial IFD + nixpkgs getFlake): **2-3 days; high probability of success.**
- M4 (haskell.nix small project): **+1-2 days; moderate probability.**
- M5 (cardano-node end-to-end with reasonable perf): **+2-5 days IF perf is acceptable; LOW probability without 200× gap closing.**

So the task as framed (3-5 days, validates) needs disambiguation:
- "3-5 days, validates v3 can EVALUATE cardano-node correctness": yes, plausible.
- "3-5 days, validates v3 OWNS cardano-node as a usable workload (perf comparable to TW)": no, not without prior perf work.

**My recommendation**: 
1. **Do G.0 (pre-flight, half day) FIRST and unconditionally.** Resolve the 4 uncertainty markers; re-measure 200× decomposition post-Phases-A-G. This is a no-regret investment.
2. **If 200× has materially shrunk** post-A-G: proceed with G.1-G.4 as written; expect M1-M4 in 5-7 days, M5 as stretch.
3. **If 200× is unmoved**: stop the cardano-node sprint. Do the Phase 1.5 measurement spike instead (which produces a defensible decomposition of where the 200× lives now). Re-attempt cardano-node after the bottleneck is identified and addressed.
4. **In all cases**: budget 7-10 days realistic, not 3-5. The user's task framing is best-case; communicate the spread.

The "validates v3 owns real-world workload" claim is **conditional on perf**. M5 completing in 6+ hours doesn't validate ownership; it validates correctness only. Make sure the team agrees on the success bar before the sprint starts.

---

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0
