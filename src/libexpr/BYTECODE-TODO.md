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

### Next step
Debug the callFunction→vmExec env layout mismatch. The bodyThunkIdx
code offset expects the env from ExprBytecodeThunk's eval context, not
from callFunction's env2. Need to verify that callFunction's formals
binding produces the same env slot layout as the bytecoded prologue.

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
