# Eval-order divergence: `(import <nixpkgs> {}).lib` under STG_KEEP_HOOKS=1

**Date**: 2026-05-08
**Repro**: `NIX_USE_V3=1 NIX_V3_STG=1 NIX_V3_STG_KEEP_HOOKS=1 nix eval --impure --expr 'builtins.length (builtins.attrNames (import <nixpkgs> {}).lib)'` HANGS;
without `STG_KEEP_HOOKS=1` returns `494`.

## TL;DR

The cycle is **NOT** in `lib.makeExtensible' / extends / composeExtensions`. It is a **fresh-thunk-per-call** divergence in `prepHookUpvaluesAndWiths` for `Direct` upvalues: each TW→v3 entry allocates a brand-new `Tag::Bridge` `Thunk*` wrapping the same `nix::Value *`. v3-side BlackHole cycle detection keys on `Thunk*` identity, so identical force chains are seen as fresh every time. The TW-equivalent path (`callFunction`) reuses `lambda.env` directly without per-call allocation, so TW's per-`Value` `tBlackhole` tag fires and throws InfiniteRecursion at the right point.

## Recursion source identified by tracing

V3_DBG_FORCE_CALLSITE + V3_DBG_FORCES samples show the cycle is **`pkgs/stdenv/generic/make-derivation.nix`'s `args/finalPackage/origArgs/result/commonAttrs` fixed-point**, NOT `lib.makeExtensible'`. The hot inner lambda is `lib/systems/parse.nix:60:44` (the `name: value:` body inside `setTypes`'s `mapAttrs`), forced 1.3M+ times before timeout. BLACK-on-frames thunks are repeatedly named `args` / `finalPackage` / `perl5` (a stdenv participant) — nested, with the SAME `Thunk*` (e.g. `0xf501faa60`) caught at codeOff=1054 inside cu=`0x9240fc6c8` (the make-derivation cu). Frame depth oscillates 17 → 20 → 220+ between progress strides while `thunksAllocated` climbs steadily at ~4 thunks/force; the v3 stack DOES unwind, but *fresh thunks for the same logical computation are reissued on each unwound re-entry*.

Cited:
- `nixpkgs/pkgs/stdenv/generic/make-derivation.nix` (the `args` / `finalPackage` / `origArgs` / `result` / `commonAttrs` lambdas — the `mkDerivation` knot-tied attrset)
- `nixpkgs/lib/systems/parse.nix:60:44` (the hot `mapAttrs` lambda in `setTypes`)

The recursion is not infinite at the TW Expr level — it's a real fix-point that *should* settle. v3 fails to settle it because each re-entry generates fresh thunks for the same upvalue chain.

## Where v3 dispatches the re-entry

Two specific call sites repeatedly create FRESH Bridge thunks for the SAME `nix::Value *`s:

1. **`prepHookUpvaluesAndWiths` Direct upvalue path** — `src/libexpr-v3/v3_hook.cc:2599-2606`:
   ```cpp
   if (src.kind == UpvalueSource::Kind::Direct) {
       nix::Value * srcV = cur->values[src.displ];
       Thunk * bridge = Alloc::allocBridgeThunk(static_cast<void *>(srcV));
       allocStats().thunksAllocated++;
       Value entry; entry.tag_payload = static_cast<uint64_t>(Tag::Thunk);
       entry.payload.thunk = bridge;
       upvalues.push_back(entry);
   }
   ```
   No cache. Every call from `tryDispatchTWLambdaInV3` (vm.cc:1939) or `v3CallFunctionEntry` (v3_hook.cc:3652) into the same TW lambda re-allocates a fresh Thunk per upvalue.

2. **outer-with carriage path** — `v3_hook.cc:2750-2757`. Same pattern: fresh `allocBridgeThunk(srcV)` per call.

The `RecBuild` path (v3_hook.cc:2616 onward) DOES cache via `recBuildCache()` keyed by `(env, names)` with an ABA stamp. The `Direct` and `outer-with` paths do not.

When the recursion is deep enough (mkDerivation's fix-point chain hits ~250 frames), each level emits a fresh ~10-thunk upvalue array; the BLACK detector inside `forceValue` (vm.cc:5704+) does eventually find a match on the OUTER `Thunk*`, throws `BlackholeError`, and the call hook blacklists the lambda — but the SUBSEQUENT TW retry walks the same expression with `mkDerivation` re-entered through a DIFFERENT thunk (because the args attrset just allocated a new `nix::Value` for `outTwHeap`), and the cycle starts over with new identity. Hence the unbounded growth.

## TW's eval order at the equivalent point

`src/libexpr/eval.cc:1797-1997` (`EvalState::callFunction`):

- Line 1810: `forceValue(fun, pos);` — TW sees `fun` is the SAME `nix::Value *` across re-entries; if it's currently mid-force, `tBlackhole` fires here.
- Line 1850-1957: lambda dispatch. `Env & env2 = mem.allocEnv(size); env2.up = vCur.lambda().env;` — TW allocates ONE env per call, but env values are direct `nix::Value *` (shared identity across calls).
- Line 1925: `env2.values[displ++] = i.def->maybeThunk(*this, env2);` — only DEFAULTS get fresh thunks; supplied formals get the caller's `args[0]` *directly*.
- Line 1978: `lambda.body->eval(*this, env2, vCur);` — the body's `ExprVar::eval` walks `env2` upward, hits an upvalue `nix::Value *` that was allocated when the upvalue's binding was first introduced. Each `ExprVar::eval` runs `forceValue(*v, pos)` on that same pointer, so the per-Value `tBlackhole` cycle protector (set inside `forceValue`) catches re-entry on the original `nix::Value`.

Net: TW's single allocation per `Value`-identity-class means the cycle either terminates (real fix-point) or `tBlackhole` throws. v3 wraps each upvalue in a fresh `Thunk*` per call, so v3-level cycle detection can't see the repeat.

## The divergence — two specific sites

1. **`v3_hook.cc:2599` (Direct)** — fresh `allocBridgeThunk` per call. TW does not allocate per-call upvalue wrappers; it walks the env chain.
2. **`v3_hook.cc:2750` (outer-with)** — same pattern.

Both bypass the `recBuildCache`-style memoisation that the RecBuild path already uses.

A secondary contributor is **`vm.cc:1984` OP_CALL Bridge fallthrough**:
```cpp
nix::Value * outTwHeap = ns->allocValue();
ns->callFunction(*funTw, *argTw, *outTwHeap, nix::noPos);
```
Each Bridge-out OP_CALL creates a fresh `outTwHeap`. If the result is a thunk, it's wrapped in yet another fresh Bridge thunk (vm.cc:1988). Repeated calls to the same TW lambda from the same v3 frame produce N independent Bridge thunks pointing at N different `nix::Value`s — TW's blackhole was the only thing that could detect a cycle here, but TW's blackhole keys on the underlying `nix::Value*`, not on `outTwHeap`. Since `outTwHeap` is freshly allocated each call, it never matches.

## Recommended fix (design idea, not implementation)

**Add a `Direct/OuterWith` Bridge-thunk cache parallel to `recBuildCache`** in `v3_hook.cc`. Keyed by `(nix::Value * srcV)` (the underlying TW pointer), valued by `Thunk * bridge`. When `prepHookUpvaluesAndWiths` materialises a Direct upvalue, look up `srcV` in the cache; on hit, reuse the cached `Thunk *` so all subsequent calls see the SAME v3-side identity. On miss, allocate, store, return. The cache must:

- Be GC-aware: `recBuildCache`'s ABA-stamp pattern (compare `cur->values[0]` at lookup vs insert) is the existing template.
- Be scoped (thread-local + cleared at top-level eval boundaries) to bound memory.
- Match the fast-path scalar bridge in `tryFastBridgeScalarTwToV3` so already-forced scalars don't pay the cache cost.

With Direct cached, v3's BlackHole detection on `Thunk * t` will trigger on re-entry because both calls now reach the SAME `Thunk *`. The mkDerivation fix-point will either settle (the typical case) or throw a clean cycle (a real bug case), matching TW's semantics.

A complementary fix on the OP_CALL Bridge side: cache `outTwHeap` keyed by `(funTw, argTw)` for the duration of the outer v3 call frame, so a repeated TW lambda call within the same frame returns the same TW result Value (and therefore the same Bridge thunk wrapping it). This addresses contributor #2 above.

## Why STG mode flipped this on

Without KEEP_HOOKS, STG-4 disables `useV3Call` (v3_hook.cc:3084-3086) and `s_stgMode && !s_stgKeepHooks` short-circuits the eval hook (v3_hook.cc:1570). TW handles all of mkDerivation natively, never enters v3 for this expression, so the fresh-thunk-per-call defect is unreachable. KEEP_HOOKS re-enables both hooks; v3 starts owning the dispatch, and the per-call Bridge allocation surfaces.

## Cross-references

- `SLOT_TAGGING_AUDIT_2026-05-07.md` lines 69-80: identifies the cross-VMState fresh-VMState pattern as architectural; this memo refines it: the FRESH-IDENTITY pattern is on `Thunk *` issuance in `prepHookUpvaluesAndWiths`, not on `VMState` itself.
- `vm.cc:5359` STG-12 anchor for the hello.name cycle source.
- `v3_hook.cc:2616` recBuildCache: existing precedent for caching Direct upvalues.

## Probe budget used

3 of 5 nix invocations (1 baseline-pass, 1 OP_CALL_BRIDGE+EVAL_HOOK debug, 1 FORCE_CALLSITE+FORCES). Stats-dump probe was a 4th but produced no atexit output (SIGKILL on timeout).

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
