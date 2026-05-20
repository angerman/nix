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

This doc plans the architectural fix.

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
  → treeWalkerToV3(vRes)                            # convert back
```

## Proposed v3-native flow

```
v3 primGetFlake(flakeRefStr)
  → primGetFlakeV3(flakeRefStr)                    # v3-native dispatch
    → state.nixEvalState->parseFlakeRef(flakeRefStr)  [FFI leaf]
    → nix::flake::lockFlake(...)                    [FFI leaf]
    → buildCallFlakeArgsTW(state, lockedFlake) → (vLocks, vOverrides, vFetchTreeFinal)
      # Reuse libflake's existing buildBindings / emitTreeAttrs logic;
      # these produce TW Values for the args attrset and overrides.
    → v3::callFlakeV3(state, vLocks, vOverrides, vFetchTreeFinal, v3Out)
      → load + lower + compile call-flake.nix (v3-side; cached on a static)
      → bridge TW args → v3 Values (shallow, via treeWalkerToV3)
      → callClosure × 3 → v3 result Value
  → return v3Out (no TW conversion)
```

The user's `outputs (inputs // {self=result;})` call inside
call-flake.nix becomes a **v3 closure call** on a **v3 closure**
(loaded via v3's primImport when the file is `import (outPath +
"/flake.nix")`).  v3 owns the entire post-FFI evaluation chain.

## Subtasks

### 1. Meson: generate `call-flake.nix.gen.hh` for libexpr-v3

libflake already generates this header from `src/libflake/call-flake.nix`
via `gen_header.process` (meson.build:35-40).  Mirror that rule into
`src/libexpr-v3/meson.build` so v3 has access to the same content.

Two options:
  - **(a)** Re-process the libflake source via `subdir` and
    `files('../libflake/call-flake.nix')`.  Single source of truth.
  - **(b)** Symlink `src/libexpr-v3/call-flake.nix` →
    `../libflake/call-flake.nix` (or copy at configure time).

Prefer (a).  Cleaner, no symlink hazard.

### 2. New file `src/libexpr-v3/v3_call_flake.cc`

Exports a single function:

```cpp
namespace nix::v3 {
  /// Build v3 Values from a LockedFlake's args (mirrors
  /// libflake's callFlake body lines 932-969) and run v3-compiled
  /// call-flake.nix on those args.  Returns a v3 Value (the flake's
  /// outputs attrset).
  ///
  /// FFI boundary: this function calls libflake helpers
  /// (emitTreeAttrs, buildBindings) to materialise TW Values for
  /// args; those are bridged shallow to v3.  No Nix-source
  /// evaluation happens in TW.
  Value callFlakeV3(EvalState & state,
                    const nix::flake::LockedFlake & lockedFlake);
}
```

Implementation:

```cpp
Value callFlakeV3(EvalState & state, const LockedFlake & lockedFlake) {
    // 1. Build TW args (replicate libflake/flake.cc:932-969)
    auto [lockFileStr, keyMap] = lockedFlake.lockFile.to_string();
    nix::Value vLocks; vLocks.mkString(lockFileStr, ns.mem);
    nix::Value vOverrides = buildOverrides(state, lockedFlake, keyMap);
    nix::Value * vFetchTreeFinal = state.internalPrimOps["fetchFinalTree"];

    // 2. Get the v3-compiled call-flake.nix closure (cached)
    Value vCallFlakeV3 = getCachedCallFlakeClosure(state);

    // 3. Bridge TW args to v3 Values
    Value v3Locks = treeWalkerToV3(state, vLocks);
    Value v3Overrides = treeWalkerToV3(state, vOverrides);
    Value v3FetchTreeFinal = treeWalkerToV3(state, *vFetchTreeFinal);

    // 4. Apply args via callClosure
    VMState vm;  // or reuse activeV3VM
    Value r1 = callClosure(vm, vCallFlakeV3, v3Locks);
    Value r2 = callClosure(vm, r1, v3Overrides);
    Value r3 = callClosure(vm, r2, v3FetchTreeFinal);
    return r3;
}
```

`getCachedCallFlakeClosure` does the parse + lower + compile + run-to-
closure dance, cached on a static.

### 3. Modify `primGetFlake` to use the v3-native path

Replace the `bridgeBuiltin<1>("getFlake", ...)` body with:

```cpp
void primGetFlake(EvalState & s, Value * args, Value & out) {
    if (!args[0].isString()) typeError("getFlake", "string");
    std::string flakeRefS = args[0].payload.str;

    auto & ns = *s.nixEvalState;
    auto flakeRef = nix::parseFlakeRef(ns.fetchSettings, flakeRefS, {}, true);
    if (ns.settings.pureEval && !flakeRef.input.isLocked(ns.fetchSettings))
        throw Error("cannot call 'getFlake' on unlocked flake reference '%s' (use --impure to override)", flakeRefS);

    auto lockedFlake = nix::flake::lockFlake(
        nix::flakeSettings, ns, flakeRef,
        nix::flake::LockFlags{
            .updateLockFile = false,
            .writeLockFile = false,
            .useRegistries = !ns.settings.pureEval && nix::flakeSettings.useRegistries,
            .allowUnlocked = !ns.settings.pureEval,
        });

    out = callFlakeV3(s, lockedFlake);
}
```

### 4. Verify

  - `(builtins.getFlake "/path/to/trivial-flake").outputs.smoke` →
    matches TW + faster than current bridge.
  - `(builtins.getFlake "/path/to/cardano-node") ? outputs` →
    matches TW + ideally faster (no TW eval of call-flake.nix).
  - run-695, run-696, all 17+ run-* tests still PASS.

### 5. A/B gate

Add `NIX_V3_NO_NATIVE_CALL_FLAKE=1` to fall back to the current
bridge path.  Retirement criterion: drop the gate once a real-world
workload (cardano-node M4 / M5) completes byte-identical between
v3-direct (default) and v3-direct + gate.

## Dependencies + risks

  - **libflake link** (currently libexpr-v3 doesn't depend on
    libflake; need to add).  Risk: circular deps.  Mitigation:
    libflake already depends on libexpr; libexpr-v3 wraps libexpr;
    adding libflake to libexpr-v3 should be fine but check.

  - **flakeSettings global access** (`nix::flakeSettings` lives in
    `src/libcmd/common-eval-args.cc:51`).  libcmd depends on libexpr/
    libstore; v3 ↔ libcmd direction not yet established.  May need to
    pass settings explicitly from the caller (CmdEval / installables).

  - **Closure call from a fresh VMState**: existing `callClosure(vm,
    fun, arg)` requires a VMState.  When called from primGetFlake's
    body, we're already in a VMState (the caller's).  Use that one,
    not a fresh one — avoids cross-VM thunk-Black-mark issues
    (lesson from STG-10).

  - **String context** on `vLocks` (lockfile JSON string): the JSON
    contains paths that may have DrvDeep context after #682.  Verify
    treeWalkerToV3 preserves context.

  - **fetchFinalTree primop**: this is a TW-internal primop, not a
    builtins entry.  When v3-side call-flake.nix calls it, the call
    goes via OP_CALL on a bridge-wrapped TW Value.  Per #697 the
    re-entry ping-pong cost is gone (no TW interior eval); the only
    call is fetchFinalTree itself which is a single I/O FFI per
    input.  Should be fine.

## Estimated effort

  - Meson rule + skeleton: 1 hour
  - buildOverrides + callFlakeV3 helper: 3-4 hours
  - primGetFlake rewrite: 1 hour
  - Verification + regression sweep: 2 hours

Total: ~1 working day for a careful first-cut.  Risk: 2 days if
libflake link or fetchFinalTree bridging surfaces edge cases.

## Why not "port call-flake.nix to v3 bytecode" (the user's correction)

I had initially proposed `installBytecodePrimop("__internalCallFlake",
<call-flake.nix source as C-string>)`.  The user pushed back: that's
hardcoding the .nix source into v3's binary.  Better to **load the
.nix file** and compile it through v3's normal parse → lower →
compile path, exactly as `primImport` does for user .nix files.

This design follows that guidance: v3 reads the same source file
that libflake uses (via the shared generated header), compiles it in
v3, and runs it.  call-flake.nix remains the canonical pure-Nix
helper; v3 just uses its own compiler instead of TW's.
