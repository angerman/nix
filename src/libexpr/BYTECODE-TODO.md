# Bytecode VM TODO List

## Current Status
- 34 commits, 143 unit tests, ~6,200 LOC
- 595/595 existing nix-expr-tests pass with NIX_EVAL_BYTECODE=1 (100%)
- nixpkgs hello evaluates correctly
- cardano-node (haskell.nix) evaluates correctly
- 113 additional pattern comparisons: 0 mismatches
- CompilationUnit caching implemented (per-EvalState Expr* -> Unit* map)
- Tree-walker always available as ground-truth oracle

## Performance Baseline (debug build -O0)
- nixpkgs hello: bytecoded 14% slower (3.18s vs 2.78s CPU)
  - Overhead from compilation of imported files
  - Fib(30): identical (0.23s both paths)
  - Expected to improve with -O2 and bytecoded thunk/lambda bodies
- VM stats: ~10K instructions per eval, ~0% OP_EVAL_EXPR fallback

## Remaining OP_EVAL_EXPR Fallbacks

These expression types fall back to the tree-walking evaluator.
Replace with native bytecoded implementations for performance.

### High Priority (frequently used in nixpkgs)
- [ ] ExprAttrs recursive (rec { })
- [ ] ExprAttrs with inherit(expr)
- [ ] ExprAttrs with dynamic attributes
- [ ] ExprOpUpdate (//) -- needs correct sorted merge with RHS-wins
- [ ] ExprLet with inherit(expr) -- needs inheritEnv support

### Medium Priority
- [ ] ExprSelect with dynamic attribute names
- [ ] ExprSelect multi-level with 'or' default
- [ ] ExprOpHasAttr multi-level or dynamic

### Low Priority
- [ ] ExprPos (__curPos) -- rarely used

## Profiling Infrastructure

### CPU Profiling
- [ ] Add elapsed time measurement around bytecoded vs tree-walked eval
- [ ] Count bytecoded instructions executed vs OP_EVAL_EXPR fallbacks
- [ ] Measure compilation time overhead
- [ ] Compare nix eval time with/without NIX_EVAL_BYTECODE=1
- [ ] Add per-opcode timing (which opcodes are hottest)

### Memory Profiling
- [ ] Track CompilationUnit allocation sizes
- [ ] Count thunks created by bytecoded vs tree-walked paths
- [ ] Measure Value allocation rate in bytecoded vs tree-walked
- [ ] Track VMState stack growth (peak stack depth, grows count)

### Profiling Integration
- [ ] Integrate with existing --show-stats infrastructure
- [ ] Add bytecode-specific stats to the JSON output
- [ ] Wire up nrLookups counter (currently TODO in VM)

## Optimization Strategies

### Phase 6a: Reduce OP_EVAL_EXPR Fallbacks
- [ ] Implement native rec { } compilation
- [ ] Implement native inherit(expr) env
- [ ] Implement native ExprOpUpdate (sorted merge)
- [ ] Implement native dynamic attributes

### Phase 6b: VM Dispatch Optimization
- [ ] Re-enable computed-goto dispatch (disabled due to stack overflow)
      - Split vmExec into thin entry + large dispatch function
      - Or use __attribute__((noinline)) for handler bodies
- [ ] Add superinstructions: GET_LOCAL_0_FORCE, ATTR_SELECT_FORCE
- [ ] Optimize OP_TRUE/OP_FALSE/OP_NULL to use singletons (issue #20)

### Phase 6c: Compilation Optimization
- [ ] Constant folding (1 + 2 -> 3 at compile time)
- [ ] Dead code elimination (if true then A else B -> A)
- [ ] Inline caching for attribute selection
- [ ] Tail call optimization (OP_TAIL_CALL)

### Phase 6d: Memory Optimization
- [ ] Reduce thunk allocation via strictness analysis
- [ ] Stack-allocate intermediate Values where possible
- [ ] Share CompilationUnits across identical file imports

### Phase 6e: Advanced
- [ ] Bytecoded lambda body execution (currently tree-walked via original ExprLambda*)
- [ ] Bytecoded thunk body execution (currently tree-walked via original Expr*)
- [ ] Full trampolining: bytecoded-to-bytecoded calls push CallFrame instead of recursing

## Code Quality

### From Review (23 issues)
- [ ] Add bounds checks for thunk/lambda indices in debug builds (#17)
- [ ] Add runtime stack underflow check (#18)
- [x] Store attribute positions in OP_ATTRS_INIT for unsafeGetAttrPos (#7)
- [ ] Fix position tracking in inline thunk trampoline (#2)
- [ ] Add RAII cleanup for heap-allocated arrays in exception paths (#14)

### Testing
- [ ] Run full nix functional test suite with NIX_EVAL_BYTECODE=1
- [ ] Add property-based tests (rapidcheck) for bytecoded expressions
- [ ] Add fuzzing for bytecoded evaluation
- [ ] Performance regression tests (benchmarks)
