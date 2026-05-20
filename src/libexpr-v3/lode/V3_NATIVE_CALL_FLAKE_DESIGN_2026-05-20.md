## V3-native callFlake — design (2026-05-20, follow-on to #697)

**Issue**: When v3-direct evaluates `builtins.getFlake "X"`, the current
implementation (post-#695) bridges to TW's `prim_getFlake`, which then
calls `nix::flake::callFlake` (libflake/flake.cc:928).  Inside,
`callFlake` calls `state.callFunction(vCallFlake, args, vRes)` —
**TW evaluates call-flake.nix** to build the flake-output attrset.

`call-flake.nix` is **pure Nix** (105 lines, no FFI inside).  By the
V3-NATIVE rule, v3 should own its evaluation; TW should only handle
the FFI leaves (`parseFlakeRef`, `lockFlake`, `fetchTreeFinal`).

#697 fixed the *re-entry ping-pong* (TW calling `builtins.foldl'` etc.
that were bridged to v3 wrappers), but the *outer* "TW evaluates a
.nix file" violation remains.

This doc plans the architectural fix.  Phase 1 scaffolding landed in
`a56a26b9c`; Phase 2 (this plan) wires up the body.

## §0 — Rule 0: the hypothesis we kill

**Claim**: "TW evaluation of pure-Nix files is necessary inside v3's
getFlake path" — the post-#697 fallback that's been load-bearing for
3 commits.

**Falsification**: if `callFlakeV3` produces byte-identical output to
the TW-routed path on a representative set of flakes (trivial fixture +
17 run-* tests + cardano-node M4 OR M5 + 200 entries from
`nixpkgs#legacyPackages.x86_64-linux flake outputs`), the claim is
killed.  Land the v3-native path default-on, delete the
`bridgeBuiltin<1>("getFlake", ...)` body in primops.cc:9452, drop the
`NIX_V3_NATIVE_CALL_FLAKE_OFF` gate.

**Anti-Rule-0 failure mode** to avoid: landing the new path as opt-in
and "letting both coexist" indefinitely.  See ACTION_PLAN Part 1 rule 1
and `feedback_falsification_rule.md`.  If we cannot get to byte-
identical on cardano-node within Phase 2 effort budget (§10), the
right exit is to file a fresh top-level issue documenting which
bytecode-level divergence blocked us — NOT to bake in the gate.

## §0.5 — Motivation: why now

Two concrete pressures:

1. **Phase 1 of #698 already landed** (`a56a26b9c`): the libflake link
   + meson generated-header + skeleton are committed.  Phase 1 throws
   `PHASE 1 SCAFFOLD` once invoked; leaving it unfinished is a
   half-done implementation in the tree (CLAUDE.md "no half-finished").
   Either Phase 2 lands or Phase 1 is reverted.

2. **Cardano-node IFD path** (post-#696) compiles flake.nix via v3's
   `primImport`, but the *outer* call-flake.nix still runs in TW.
   This means the IFD compilation chain is half-v3, half-TW, and any
   divergence at the boundary is harder to attribute (was it the v3
   import or the TW call-flake driver?).  Single-evaluator-owns-the-
   chain is a debuggability win independent of correctness.

**Not motivated by perf.**  call-flake.nix runs once per top-level
`getFlake` call; the workload-amortised saving is a small constant.
We will *measure* the perf delta (§7) but do not claim it as the
reason to land.

## Current flow (post-#697)

```
v3 primGetFlake(flakeRefStr)
  → bridgeBuiltin<1>("getFlake", ...)              # bridge to TW
    → TW prim_getFlake(flakeRefStr)
      → nix::parseFlakeRef(flakeRefStr)             [FFI leaf — OK]
      → nix::flake::lockFlake(...)                  [FFI leaf — OK]
      → nix::flake::callFlake(state, lockedFlake, vRes)
        → emitTreeAttrs / buildBindings (TW args)
        → state.callFunction(vCallFlake, args, vRes)  ← VIOLATION (TW evaluates call-flake.nix)
  → treeWalkerToV3Public(vRes)                      # convert back
```

## Proposed v3-native flow

```
v3 primGetFlake(flakeRefStr)
  → callFlakeV3Entry(state, flakeRefStr)            # v3-native dispatch
    → nix::parseFlakeRef(...)                       [FFI leaf]
    → nix::flake::lockFlake(...)                    [FFI leaf]
    → callFlakeV3(state, lockedFlake)
      → g_cachedCallFlake.get(ns)
        → parse call-flake.nix (TW parser; FFI leaf)
        → lower + optimise + computeFreeVars + compile (v3)
        → run() → closureValue (Tag::Closure)
        # cu and closureValue kept alive via importCache.cus
      → buildOverridesTW(state, lockedFlake) → vLocks, vOverrides
      → look up fetchFinalTree primop in ns.internalPrimOps
      → bridge each arg via treeWalkerToV3Public
      → vm = activeV3VM() (caller's; not fresh — STG-10)
      → callClosure(vm, vClosure, v3Locks)
      → callClosure(vm, r1,        v3Overrides)
      → callClosure(vm, r2,        v3FetchFinal)
  → return v3 result (no post-conversion needed)
```

Inside the v3-side execution, call-flake.nix calls `import (outPath
+ "/flake.nix")`.  That dispatches to v3's `primImport` (which post-
#696 handles IFD via `realisePath`).  When the user's flake.nix
references `inputs.X.outputs`, the call is a v3 closure call on a v3
closure.  When it calls `fetchTreeFinal node.info`, the call dispatches
through OP_CALL on a bridge-wrapped TW primop — single-FFI per input,
no ping-pong.

## §1 — Phase 1 status (LANDED 2026-05-20, `a56a26b9c`)

The following is in the tree at HEAD:

- `src/libexpr-v3/meson.build`:
  - `dependency('nix-flake')` added.  Direction OK: libflake depends on
    libexpr but not libexpr-v3, so libexpr-v3 → libflake is acyclic.
  - `gen_header.process('../libflake/call-flake.nix')` produces
    `call-flake.nix.gen.hh` from the canonical libflake source — one
    source of truth.
- `src/libexpr-v3/v3_call_flake.cc`:
  - `CachedCallFlake::get(ns)` parses call-flake.nix via TW's
    `parseExprFromString` and runs `bindVars` against
    `ns.staticBaseEnv`.  Throws `PHASE 1 SCAFFOLD` immediately after.
  - Public `callFlakeV3(state, lockedFlake)` calls `get`, then throws
    `PHASE 2 implementation pending`.

Verification:
- Compiles cleanly; no link errors.
- `NIX_V3_NATIVE_CALL_FLAKE_OFF=1` default is implicit (no gate yet
  reads the var; primGetFlake still calls `bridgeBuiltin<1>("getFlake",
  ...)`).
- Nothing calls `callFlakeV3` yet, so the runtime impact is zero —
  scaffolding is dead code until Phase 2 wires it in.

## §2 — Phase 2 subtasks

### 2.1 Finish `CachedCallFlake::get` (lower + compile + run-to-closure)

Currently the function throws after `bindVars`.  Replace lines 96-99
of v3_call_flake.cc with the primImport pipeline (primops.cc:7180-7194):

```cpp
auto module = lowerNixExpr(e, ns.symbols, ns.positions);
nix::v3::ir::optimise(module);
nix::v3::ir::computeFreeVars(module);
auto & cache = importCache();
cache.cus.push_back(compile(module));
closureValue = run(cache.cus.back());
if (closureValue.tag() != Tag::Closure)
    throw std::runtime_error(
        "v3::callFlakeV3: call-flake.nix did not compile to a 3-arg closure "
        "(expected Tag::Closure, got tag=" +
        std::to_string((int)closureValue.tag()) + ")");
```

**CompilationUnit lifetime** (the #676 hazard):
- The CU is held by `importCache().cus` — a `std::deque<CompilationUnit>`
  that primImport already uses; deque-push never invalidates prior
  elements, so closures embedding `c->cu = &cache.cus.back()` stay
  valid for the lifetime of the EvalState.
- The cached `closureValue` references `cu->stringConstants` for any
  `OP_LIT_STR` / `OP_LIT_PATH`; those live as long as the deque element.
- Do NOT use a `std::unique_ptr<CompilationUnit>` static here.  The
  static would survive the EvalState; if EvalState is recreated
  (test harnesses, daemons) the closure points into a CU bound to a
  dead EvalState's symbol table.  Use `importCache()` which is per-
  EvalState (it's a static inside primops.cc that the user can clear
  via `clearImportCache()` at the appropriate boundary).

**Thread safety**:
- `std::once_flag` makes initialisation atomic at the call site.
- After initialisation, `closureValue` is read-only.
- Concurrent primGetFlake calls in different VMState threads (when v3
  ever runs concurrent — not today) are safe because the underlying
  CU is immutable.

### 2.2 Implement `buildOverridesTW(state, lockedFlake)`

This replicates libflake/flake.cc:932-961 (the part of `callFlake`
that assembles the TW `overrides` attrset).  Place it as a `static`
in `v3_call_flake.cc`:

```cpp
namespace {

/// Build the TW `overrides` attrset that call-flake.nix consumes.
/// Mirrors libflake/flake.cc:934-961 byte-for-byte.  Stays in TW
/// land because emitTreeAttrs + buildBindings are libflake API that
/// operate on `nix::Value`; we bridge the whole attrset shallowly
/// to v3 after assembly.
struct BuiltArgs {
    nix::Value * vLocks;
    nix::Value * vOverrides;
};

BuiltArgs buildArgsTW(nix::EvalState & ns,
                       const nix::flake::LockedFlake & lockedFlake,
                       const std::string & lockFileStr)
{
    auto overrides = ns.buildBindings(lockedFlake.nodePaths.size());
    for (auto & [node, sourcePath] : lockedFlake.nodePaths) {
        auto override = ns.buildBindings(2);
        auto & vSourceInfo = override.alloc(ns.symbols.create("sourceInfo"));
        auto lockedNode = node.dynamic_pointer_cast<const nix::flake::LockedNode>();
        auto [storePath, subdir] = ns.store->toStorePath(sourcePath.path.abs());
        nix::flake::emitTreeAttrs(
            ns, storePath,
            lockedNode ? lockedNode->lockedRef.input
                       : lockedFlake.flake.lockedRef.input,
            vSourceInfo, false,
            !lockedNode && lockedFlake.flake.forceDirty);
        auto & keyMap = lockedFlake.lockFile.to_string().second;
        auto key = keyMap.find(node);
        assert(key != keyMap.end());
        override.alloc(ns.symbols.create("dir"))
                .mkString(nix::CanonPath(subdir).rel(), ns.mem);
        overrides.alloc(ns.symbols.create(key->second)).mkAttrs(override);
    }
    auto * vOverrides = ns.allocValue();
    vOverrides->mkAttrs(overrides);

    auto * vLocks = ns.allocValue();
    vLocks->mkString(lockFileStr, ns.mem);

    return {vLocks, vOverrides};
}

} // namespace
```

**Gotcha — `keyMap` lifetime**: `lockFile.to_string()` returns
`std::pair<std::string, std::map<...>>` BY VALUE.  Calling it twice
(once for `lockFileStr`, once inside the loop) is wasted work.  Hoist
to a single call and capture both halves:

```cpp
auto [lockFileStr, keyMap] = lockedFlake.lockFile.to_string();
auto built = buildArgsTW(ns, lockedFlake, lockFileStr, keyMap);
```

(Update the signature accordingly; the snippet above is illustrative.)

**Gotcha — string context on `vLocks`**: the lockfile JSON contains
store paths that, under post-#682 invariants, must carry DrvDeep
context to other primops down the chain.  However, `vLocks` is
consumed by `builtins.fromJSON` inside call-flake.nix, which returns
context-stripped strings (TW's `prim_fromJSON` resets context).  So
context on vLocks itself does NOT need to be preserved.  Verify this
empirically by comparing a flake's drvPath against TW's; if the hash
diverges on a getFlake-only path, revisit.

### 2.3 Wire `callFlakeV3` body

Replace the throwing stub (v3_call_flake.cc:130-164) with:

```cpp
Value callFlakeV3(EvalState & state, const nix::flake::LockedFlake & lockedFlake)
{
    if (!state.nixEvalState)
        throw std::runtime_error("v3::callFlakeV3: no TW EvalState wired");
    auto & ns = *state.nixEvalState;

    Value vClosure = g_cachedCallFlake.get(ns);  // Tag::Closure, asserted above

    auto [lockFileStr, keyMap] = lockedFlake.lockFile.to_string();
    auto built = buildArgsTW(ns, lockedFlake, lockFileStr, keyMap);

    auto * pFetchFinal = nix::get(ns.internalPrimOps, "fetchFinalTree");
    if (!pFetchFinal || !*pFetchFinal)
        throw std::runtime_error(
            "v3::callFlakeV3: fetchFinalTree primop missing from "
            "internalPrimOps (libflake not initialised?)");

    Value v3Locks      = treeWalkerToV3Public(ns, *built.vLocks);
    Value v3Overrides  = treeWalkerToV3Public(ns, *built.vOverrides);
    Value v3FetchFinal = treeWalkerToV3Public(ns, **pFetchFinal);

    VMState * vm = activeV3VM();
    if (!vm)
        throw std::runtime_error(
            "v3::callFlakeV3: no active VMState — getFlake must be "
            "invoked from inside a v3 dispatch (primGetFlake context)");

    Value r1 = callClosure(*vm, vClosure,     v3Locks);
    Value r2 = callClosure(*vm, r1,           v3Overrides);
    Value r3 = callClosure(*vm, r2,           v3FetchFinal);
    return r3;
}
```

**VMState rationale (STG-10 lesson)**:
- `primGetFlake` is invoked from inside `OP_CALL` / `OP_CALL_PRIMOP`
  in v3's dispatch loop.  At that point `activeV3VM()` returns the
  *outer* v3 VMState — the one currently bridging out.
- Using that VMState (rather than spawning a fresh one) keeps the
  black-mark table coherent: any thunks in flight on the outer VM
  remain visible to the re-entered dispatch loop.  Cross-VMState
  black-mark divergence was the failure mode that motivated #466 in
  the first place.
- If `activeV3VM()` returns nullptr, we are NOT inside a v3 dispatch
  — that's a programming error in the caller and we throw to surface
  it.  (In practice, primGetFlake's caller IS v3 dispatch; this guard
  fires only if someone wires callFlakeV3 from a non-v3 path, which
  the design forbids.)

### 2.4 Modify `primGetFlake` to use the v3-native path

Replace the body at primops.cc:9452:

```cpp
void primGetFlake(EvalState & s, Value * args, Value & out)
{
    // Gate: NIX_V3_NATIVE_CALL_FLAKE_OFF=1 falls back to the legacy
    // bridge path (kept for A/B comparison).  Retirement criterion:
    // delete this branch (and the gate) once cardano-node M4 + 200-
    // package sweep are byte-identical via the v3-native path.
    static const bool nativeOff =
        std::getenv("NIX_V3_NATIVE_CALL_FLAKE_OFF") != nullptr;
    if (nativeOff)
        return bridgeBuiltin<1>("getFlake", s, args, out);

    if (!s.nixEvalState)
        throw std::runtime_error(
            "v3 primop getFlake: no TW EvalState wired");
    auto & ns = *s.nixEvalState;

    if (!args[0].isString())
        typeError("getFlake", "string");
    std::string flakeRefS = args[0].payload.str;

    auto flakeRef = nix::parseFlakeRef(
        ns.fetchSettings, flakeRefS, std::nullopt, true);
    if (ns.settings.pureEval && !flakeRef.input.isLocked(ns.fetchSettings))
        throw nix::Error(
            "cannot call 'getFlake' on unlocked flake reference '%s' "
            "(use --impure to override)", flakeRefS);

    extern nix::flake::Settings flakeSettings;  // defined in libcmd
    auto lockedFlake = nix::flake::lockFlake(
        flakeSettings, ns, flakeRef,
        nix::flake::LockFlags{
            .updateLockFile  = false,
            .writeLockFile   = false,
            .useRegistries   = !ns.settings.pureEval
                              && flakeSettings.useRegistries,
            .allowUnlocked   = !ns.settings.pureEval,
        });

    out = v3::callFlakeV3(s, lockedFlake);
}
```

**flakeSettings access — transitional approach**: the `extern
nix::flake::Settings flakeSettings;` declaration accesses the symbol
defined in libcmd/common-eval-args.cc:51.  This is acceptable as a
transitional measure because:
- libcmd is always linked into binaries that exercise getFlake
  (`nix`, `nix-eval`); libexpr-v3 alone never calls getFlake.
- The symbol is `extern` (linker-resolved at binary-link time, not at
  library load).  The libexpr-v3 library does not itself require
  libcmd; only the final binary does.
- A future cleanup (call it Phase 3) plumbs flakeSettings as a primop-
  registration-time parameter, matching how libflake itself exposes
  `nix::flake::primops::getFlake(settings)` (flake-primops.hh:14).  We
  do not block Phase 2 on that cleanup.

If linking the extern fails (e.g. unit tests that don't pull in
libcmd), we add a weak fallback that returns `flake::Settings{}` — but
that's a deferred concern.  First make the happy path work.

## §3 — Verification plan

### 3.1 Smoke tests (Phase 2 exit criteria)

Each must produce byte-identical output between v3-native and the
legacy bridge path (compare via `nix eval --impure --raw ...` with
`NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1`):

- [ ] `(builtins.getFlake "/path/to/trivial-flake").outputs.smoke`
  with a trivial flake at `test/flake-trivial/` (output is `"ok"`).
- [ ] `(builtins.getFlake "/path/to/flake-with-input").outputs.combined`
  with a flake that imports a single input (exercises `allNodes`
  recursion).
- [ ] `(builtins.getFlake "/path/to/ifd-flake").outputs.result`
  with a flake whose flake.nix calls `import (drvPath)` (exercises
  the v3 → primImport IFD path in concert with callFlakeV3).
- [ ] `(builtins.getFlake "/path/to/flake-using-toFile").outputs.refPath`
  with a flake that interpolates store paths into toFile content
  (exercises lockfile context invariants).
- [ ] `(builtins.getFlake "/path/to/flake-fetchTreeFinal-error").outputs`
  with a flake whose locked input points at an unreachable URL
  (exercises error path through v3-to-TW fetchTreeFinal bridge).

Add each fixture under `test/flake-*/`; driver script
`test/run-698-tests.sh` exercises all five.

### 3.2 Regression sweep

Must all still PASS (current status: all PASS pre-Phase-2):

- [ ] `make tests` — 142/142 lang tests.
- [ ] `test/run-695-tests.sh` — getFlake basic.
- [ ] `test/run-696-tests.sh` — IFD in primImport.
- [ ] `test/run-697-tests.sh` — re-entry ping-pong gone.
- [ ] All other `test/run-*-tests.sh` drivers (17+ at HEAD).

### 3.3 Real-world parity

These bind the kill criterion (§0):

- [ ] **Cardano-node M4** (use whichever of M4/M5 is lighter; M4 is
  the standard probe per `project_cardano_node_feasibility`):
  `nix eval --impure --raw .#packages.x86_64-linux.cardano-node.outPath`
  byte-identical between `v3-native` and `NIX_V3_NATIVE_CALL_FLAKE_OFF=1`
  paths.  Resource limits per CLAUDE.md "Running v3 probes safely".
- [ ] **200-package sweep**: pick the first 200 entries from
  `nix flake show nixpkgs --json`; for each, `nix eval ...#${pkg}.drvPath`
  on both paths, byte-identical.

### 3.4 Lifetime + memory

- [ ] **Re-entrancy**: invoke `builtins.getFlake` 100 times in the
  same process; verify
  - `importCache().cus` grows by exactly 1 (the call-flake.nix CU is
    cached after the first call).
  - RSS plateaus after first call (modulo per-flake fetched-tree
    storage outside our scope).
- [ ] **CU lifetime under EvalState recycle**: if a daemon mode
  recreates EvalState between requests, verify call-flake.nix is
  re-compiled in the new EvalState's symbol table (not crashing on a
  symbol-table mismatch — the #455-class symptom).

### 3.5 Perf measurement (no claim until measured)

- [ ] Run cardano-node M4 with `NIX_V3_BRIDGE_TIMING=1` on both paths;
  compare:
  - `bridge_ms` (TW-side time): expected to drop, because TW no
    longer evaluates call-flake.nix.
  - `vm_ms` (pure v3 dispatch): expected to rise, because v3 now
    evaluates call-flake.nix.
  - Net wall time: **unknown**.  Hypothesis is "near-zero delta";
    record the actual delta in `lode/V3_NATIVE_CALL_FLAKE_PERF_2026-XX-XX.md`
    once measured.  Do not write a perf delta in this doc before
    measurement.

## §4 — A/B gate

Add `NIX_V3_NATIVE_CALL_FLAKE_OFF=1` to fall back to the legacy bridge
path (note: OFF, not ON — default is native once §2.4 lands).

**Retirement criterion (concrete, inline at the getenv site)**:
The gate is removed when ALL of the following hold:
1. §3.1 smoke tests all PASS.
2. §3.2 regression sweep all PASS.
3. §3.3 cardano-node M4 byte-identical between paths.
4. §3.3 200-package sweep byte-identical between paths.
5. §3.4 no RSS regression > 5% on the cardano-node probe.
6. Documented in a follow-up commit titled
   "v3 #698 Phase 2 cleanup: retire NIX_V3_NATIVE_CALL_FLAKE_OFF gate".

## §5 — Dependencies + risks (with concrete resolutions)

### 5.1 libflake link — RESOLVED in a56a26b9c
- libexpr-v3 → libflake added.  Direction acyclic because libflake
  → libexpr (not libexpr-v3).  Confirmed by clean build.

### 5.2 flakeSettings global access — TRANSITIONAL via extern
- Use `extern nix::flake::Settings flakeSettings;` at the call site
  inside primGetFlake (libcmd defines the symbol).
- Risk: tests that don't link libcmd will fail to resolve.  Mitigation:
  Phase 3 (out of scope here) plumbs settings at primop-registration
  time.  Until then, document that v3 primGetFlake requires libcmd in
  the final link.

### 5.3 VMState — caller's, via activeV3VM()
- v3_call_flake.cc:151 sketches `VMState * vm = activeV3VM();`.
- Guard with explicit null-check; throw if not in v3 dispatch.
- Lesson from STG-10: cross-VMState black-mark tables diverge — never
  spawn a fresh VMState for nested closure calls.

### 5.4 String context on vLocks — VERIFIED at design time, RE-VERIFY at landing
- `builtins.fromJSON` (TW) strips context.  Context on vLocks is
  therefore irrelevant to the downstream evaluation chain.
- Add an explicit smoke test (§3.1 toFile fixture) that triggers a
  context-sensitive output path so any regression surfaces in CI.

### 5.5 fetchFinalTree as a TW-bridged primop, called from v3 bytecode
- This is a **new call path**: v3 bytecode → OP_CALL → bridge-wrapped
  TW primop.  Post-#697 closed re-entry ping-pong, but did not
  exercise this exact pattern.
- Risk: the bridged primop may expect a TW VMState context for its
  builtin allocations.  fetchFinalTree internally calls
  `fetchTreeFinal` which allocates via TW's `state.allocValue()` —
  those allocations route through the TW EvalState carried by
  primGetFlake's caller (not the v3 VMState).
- Mitigation: §3.1 the `flake-fetchTreeFinal-error` fixture forces
  fetchFinalTree to fire so any TW-side allocation/error-propagation
  hazard surfaces during Phase 2.

### 5.6 importCache lifetime under multi-EvalState
- `importCache()` is a `thread_local` (or process-static — verify at
  implementation time).  If thread_local, no concern.  If process-
  static, multi-EvalState test harnesses may see stale CUs.
- Mitigation: read importCache's declaration in primops.cc and confirm.
  If process-static, add a hook on EvalState destruction that clears
  CUs bound to that EvalState's symbol table.  (For Phase 2 ship-it,
  document the constraint and skip the cleanup hook unless tests fail.)

### 5.7 call-flake.nix calling builtins we haven't audited
- call-flake.nix uses: `mapAttrs`, `fromJSON`, `isList`, `head`, `tail`,
  `substring`, `isFunction`, `removeAttrs`, `import`, `?`, `or`,
  `assert`, `//`, attribute selection, list literals.
- All are v3-native (registered in v3 primops.cc) or v3 opcodes.
- `import` (line 65) dispatches to v3's primImport — covered by §3.1
  IFD fixture.
- `fetchTreeFinal` (line 55) — the one TW-bridged call — covered by
  §3.1 + §5.5.

## §6 — Open questions for review

These are NOT mitigations; they need answers BEFORE Phase 2 implementation.

1. **Q**: Should `g_cachedCallFlake` be process-static or per-EvalState?
   **Default**: per-EvalState via `importCache()`.  See §5.6.

2. **Q**: When `lockFlake` itself throws (network failure, malformed
   lockfile), do we want to fall back to the bridge path automatically?
   **Default**: no.  Throw the same error TW would.  Surface
   divergences early.

3. **Q**: Do we need to register a new env-var
   `NIX_V3_NATIVE_CALL_FLAKE_VERBOSE=1` for the parse/lower/compile
   trace?  **Default**: no.  Existing `V3_DBG_IMPORT=1` already covers
   the v3-side compile pipeline (call-flake.nix runs through it).

4. **Q**: cardano-node M4 has *historically* failed v3-direct on
   non-getFlake paths.  Is the Phase 2 success criterion "byte-
   identical *modulo* known pre-existing M4 failures"?
   **Default**: yes — only the getFlake-specific delta matters here.
   Document which packages are pre-existing failures (cross-ref to
   `project_cc_wrapper_*`, `project_670_*`, etc.).

5. **Q**: What's the rollback procedure if Phase 2 lands and the
   sweep surfaces divergence on N out of 200?
   **Default**: flip the default of `NIX_V3_NATIVE_CALL_FLAKE_OFF` to
   1 (off by default again), open a sub-issue per divergent package,
   close them one at a time.  Do not revert Phase 2 wholesale unless
   the divergences cluster on a structural cause.

## §7 — Effort estimate (revised)

Phase 1 (a56a26b9c) was ~4-6 hours.  Phase 2 realistic breakdown:

| Sub-task                                              | Optimistic | Realistic | Pessimistic |
|-------------------------------------------------------|-----------:|----------:|------------:|
| 2.1 lower + compile + cache + run-to-closure          |        1h  |       3h  |         6h  |
| 2.2 buildOverridesTW (replicates 30 LoC of libflake)  |        2h  |       4h  |        8h  |
| 2.3 wire callFlakeV3 body + VMState + bridge args     |        2h  |       3h  |        6h  |
| 2.4 modify primGetFlake + gate + flakeSettings extern |        1h  |       2h  |        4h  |
| 3.1 smoke fixtures + driver                           |        2h  |       4h  |        8h  |
| 3.2 regression sweep                                  |        1h  |       2h  |        4h  |
| 3.3 cardano-node M4 + 200-package sweep               |        2h  |       4h  |        8h  |
| 3.4 lifetime + memory checks                          |        1h  |       2h  |        4h  |
| 3.5 perf measurement (separate from claim)            |        1h  |       2h  |        4h  |
| Emergent edge cases (historical baseline: 1-2 days)   |        0h  |       8h  |       16h  |
| **TOTAL**                                              |   **13h** |   **34h** |    **68h** |

The original "1 day" / "2 days if edges surface" estimate was 3-5×
optimistic relative to this codebase's track record (#455 / #516 /
#670 / #694 each surfaced edges spanning multiple sessions).  Pessimistic
column reflects "an edge surfaces that needs cross-functional debug"
not "everything goes wrong."

**Recommended budget for landing default-on**: 1 full working week,
not 1 day.  If the 200-package sweep surfaces > 10 divergences,
publish the sub-issues per §6.5 and continue.

## §8 — Why not "port call-flake.nix to v3 bytecode" (the user's correction)

I had initially proposed `installBytecodePrimop("__internalCallFlake",
<call-flake.nix source as C-string>)`.  The user pushed back: that's
hardcoding the .nix source into v3's binary.  Better to **load the
.nix file** and compile it through v3's normal parse → lower →
compile path, exactly as `primImport` does for user .nix files.

This design follows that guidance: v3 reads the same source file
that libflake uses (via the shared generated header from §1), parses
it, compiles it through the standard v3 pipeline, and runs it.
call-flake.nix remains the canonical pure-Nix helper; v3 just uses
its own compiler instead of TW's.

## §9 — Cross-references

- `lode/ACTION_PLAN_2026-05-15.md` — Rule 0 (falsification) and
  Part 1 rule 1 (no new RCA letter on an open one).
- `lode/LESSONS_LEARNED_2026-05-15.md` §1.1 — V3-NATIVE rule
  rationale; commit `cf12c1880` "architectural mistake".
- `lode/LESSONS_LEARNED_2026-05-15.md` §1.6 — Boehm allocator
  constraints.
- `lode/FFI_AUDIT_2026-05-20.md` — categorisation of TW dependency
  mechanisms; getFlake falls under the "Tier 1 architectural" class.
- `feedback_v3_native_constraint.md` — the V3-NATIVE rule.
- `feedback_falsification_rule.md` — Rule 0.
- `project_v3_v3_native_constraint.md` (memory) — historical commits
  that violated V3-NATIVE and were retracted.
- `project_session_2026-05-18.md` — STG-10 cross-VMState lesson
  (motivates §2.3 VMState choice).
- `project_676_apply_cu_stability.md` — the CU lifetime hazard that
  motivates §2.1 deque-not-static-unique_ptr.
- `project_694_*` — recent #694 reminded us that "wrappers are red
  herrings; same bug fires without them"; applies here as
  "don't add gates that hide the divergence we want to falsify."

## Copyright

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group.  SPDX-License-Identifier: Apache-2.0
