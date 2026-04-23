# Bytecode VM TODO List

## Current Status
- 70+ commits, 160 bytecode tests + 452 other = 612 total
- ALL 612 tests pass
- nixpkgs hello.name evaluates correctly
- 0 OP_EVAL_EXPR fallbacks (all expressions compiled natively)
- Execution is 2% FASTER than tree-walker at -O2

## Performance at -O2 (final)
| Metric | Tree-walker | Bytecode VM |
|---|---|---|
| Median time | 460ms | 490ms (+6.5%) |
| Compilation | 0ms | 39ms |
| Execution | 460ms | **451ms (-2%)** |
| GC time | 5ms | 32ms (+6x) |
| Total memory | 72.5MB | 84.6MB (+17%) |
| Value allocs | 16.4MB | 17.2MB (+5%) |

## Remaining Tree-Walker Fallbacks (inherent)
- 70 OP_FORCE: App values from primops (builtins.map, mapAttrs, genList)
- 23 OP_CALL_1: non-bytecoded lambdas from App forcing cascade

These cannot be eliminated with the current architecture: the VM's
frame-based trampoline keeps all intermediate App blackholes active
simultaneously, while the tree-walker resolves them sequentially via
C-stack recursion. Eager lambda registration breaks makeExtensible's
fixed-point evaluation in nixpkgs darwin stdenv.

## Memory/GC Optimization (highest priority)
- [ ] Reduce CompilationUnit memory (251 units × vectors)
- [ ] Lazy compilation (compile on first force, not on import)
- [ ] Share thunk descriptors across units
- [ ] Pre-size vectors based on AST size estimates
- [ ] Consider arena allocation for compilation artifacts

## Execution Optimizations
- [x] O(1) addSymbol hash map (compilation: 69ms → 39ms)
- [x] Eliminate dynamic_cast in OP_FORCE (bool flag)
- [x] Singleton booleans (no allocation for bool results)
- [x] Inline concatLists + scalar equality
- [x] Direct primop dispatch + PrimOpApp handling
- [x] __functor dispatch
- [x] App blackhole protection in forceValue
- [x] Selective level-0 thunk wrapping
- [ ] Superinstruction: OP_GET_LOCAL_0_FORCE
- [ ] Inline lambda body cache lookup (avoid hash map per call)
- [ ] Persistent bytecode cache (serialize to disk)
