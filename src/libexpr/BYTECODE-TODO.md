# Bytecode VM TODO List

## Current Status
- 80+ commits, 160 bytecode tests + 452 other = 612 total
- ALL 612 tests pass
- nixpkgs hello.name evaluates correctly
- 0 OP_EVAL_EXPR fallbacks (all expressions compiled natively)
- Execution 2% FASTER than tree-walker at -O2

## Performance at -O2 (final verified)
| Metric | Tree-walker | Bytecode VM |
|---|---|---|
| Median time | 455ms | 490ms (+7.7%) |
| Compilation | 0ms | 38ms |
| Execution | 455ms | **452ms (-0.7%)** |
| GC time | 5ms | 30ms |
| Total memory | 72.5MB | 84.6MB (+17%) |

## Remaining Tree-Walker Fallbacks (93 total)
- 70 OP_FORCE: App values from primops (builtins.map, mapAttrs, genList)
- 23 OP_CALL_1: non-bytecoded lambdas from App forcing cascade

### Root cause
Primops create `mkApp(f, elem)` values. When forced, the tree-walker's
`forceValue` calls `callFunction`, which evaluates the lambda body via
`lambda.body->eval()` (tree-walker). The body creates non-bytecoded
closures that aren't in `lambdaBodyCache`.

### Approaches attempted
1. **Eager lambda registration** (compile-time lambdaBodyCache): Breaks
   makeExtensible — changes evaluation order via VM trampoline's
   simultaneous blackhole nesting vs tree-walker's sequential resolution.

2. **callFunction→vmExec routing**: Semantic mismatch — callFunction's
   formals binding produces env layout that differs from bytecoded body
   expectations. Needs investigation into env slot ordering (Symbol sort
   order vs bindVars displacement assignment).

3. **App fast path in OP_FORCE**: Works but dead without eager registration
   (lambdas in App values aren't in cache). Infrastructure in place.

### Findings (callFunction→vmExec investigation)

**Confirmed:** env slot layout is identical between callFunction and
bytecoded prologue. Both iterate `formals->formals` in sorted order
with matching displacement assignment. `newEnv->sort()` is effectively
a no-op since formals are pre-sorted by symbol name.

**Root cause identified:** The issue is NOT env layout mismatch. The
body code compiled for a lambda body contains OP_CALL_1 instructions
for inner function calls (e.g., `flip = f: a: b: f b a`). When the
body runs via vmExec, OP_CALL_1 trampolines inner bytecoded lambda
calls through the VM path (vmBindLambdaArg + frame push) instead of
through callFunction. This creates different env ALLOCATIONS with
different pointer values.

**Specific culprit lambda:** `flip` (`lib/trivial.nix:375:11`), which
is `b: f b a`. When its body `f b a` is evaluated via vmExec, the
OP_CALL_1 chain creates envs via vmBindLambdaArg. Any closures returned
from `f b` capture these VM-allocated envs rather than callFunction-
allocated envs. While env CONTENTS are identical, the different pointer
identities propagate through the nixpkgs evaluation graph.

**Cascade effect:** The vm-allocated envs produce closures that, when
later forced as thunks in the nixpkgs `elaborate` function's
self-referential `final = { ... }` attrset, evaluate through a
different code path than the tree-walker would. This eventually causes
`hasSharedLibraries` (which depends on `isDarwin` from `mapAttrs (n: v:
v final.parsed) inspect.predicates`) to evaluate to `false` instead of
`true`, making `optionalAttrs` omit the `sharedLibrary` attribute.

**Reproduction:** In callFunction, after formals binding:
```cpp
auto bcIt = lambdaBodyCache.find(&lambda);
if (bcIt != lambdaBodyCache.end()) {
    auto & bc = bcIt->second;
    auto & bodyUnit = *bc.unit;
    uint32_t bodyOffset = bodyUnit.thunks[bc.thunkIdx].codeOffset;
    bytecode::vmExec(*this, bodyUnit, bodyOffset, env2, vCur);
} else {
    lambda.body->eval(*this, env2, vCur);
}
```
Fails with: `attribute 'sharedLibrary' missing` at default.nix:193.

**Key insight:** The problem is that OP_CALL_1's trampoline and
callFunction's C++ code create semantically equivalent but pointer-
distinct envs. Something in the nixpkgs evaluation relies on env
pointer identity — likely thunk memo tables or the file eval cache
where thunk values are shared across multiple references. When a
thunk is first forced via the VM path (creating vm-env closures) and
the result is cached, subsequent accesses see the vm-env closures
instead of the tree-walker-env closures. This difference propagates
until a boolean predicate evaluates differently.

### Next step
Investigate the thunk overwriting mechanism. When a Value is a thunk
(`mkThunk`), forcing it overwrites the Value in place with the result.
If the same thunk is forced twice — once via the tree-walker path and
once via the vmExec path — the second forcing sees the already-forced
value from the first path. The question is whether the VM's OP_FORCE
handles already-forced values identically to the tree-walker's
forceValue. Check if there's a race between concurrent thunk forces
or if the env captured in closures affects thunk memoization.

## Completed Optimizations
- [x] O(1) addSymbol hash map (compilation: 69ms → 38ms)
- [x] Eliminate dynamic_cast in OP_FORCE (bool isBytecodeThunk flag)
- [x] Singleton booleans (no allocation for bool results)
- [x] Superinstruction OP_GET_LOCAL_0_FORCE (5K fewer dispatches)
- [x] Inline concatLists + scalar equality
- [x] Direct primop dispatch + PrimOpApp handling
- [x] __functor dispatch
- [x] App blackhole protection in forceValue
- [x] Selective level-0 thunk wrapping (inRecursiveScope)
- [x] Fix multi-level or-default FORCE

## Future Architecture (from Gemini research doc)
- Upvalue capturing (explicit free variable lists, not env chains)
- Polymorphic inline caching for ATTR_SELECT
- Strictness analysis (eliminate unnecessary thunk allocations)
- Lock-free parallel evaluation with CAS-based blackholing
- Persistent bytecode cache (serialize CompilationUnits to disk)
- Lazy compilation (compile on first force, not on import)
