# Bytecode VM TODO List

## Current Status
- 40+ commits, 143 unit tests, ~6,500 LOC
- 595/595 existing nix-expr-tests pass (100%)
- nixpkgs hello.name evaluates correctly with OP_CALL_1 trampoline + OP_FORCE trampoline
- cardano-node (haskell.nix) evaluates correctly
- Non-recursive attrset inherit(expr) compiled natively (OP_INHERIT_FROM_INIT/SET)
- Formals-binding prologue separated from body (prologueOffset in LambdaDescriptor)
- Stack reallocation-safe (offset-based stackBase)
- Result aliasing fixed (push resultSlot, not retVal)

## Performance (debug build -O0)
- nixpkgs hello: ~6% slower (3.21s vs 3.03s tree-walker)
- VM stats: 46,582 instructions, 57 trampolined calls, 5,272 thunk forces
- OP_EVAL_EXPR: 22 fallbacks (all `let inherit(expr)` patterns)

## Remaining OP_EVAL_EXPR Fallbacks

### `let inherit(expr)` (22 hits in nixpkgs hello — ALL remaining fallbacks)
- [ ] Implement flattened env or SET_ENV_SLOT_UP for let+inherit(expr)
  - Challenge: inherit env is nested inside let env; SET_ENV_SLOT writes to
    curEnv (inherit env) instead of the let env.
  - Option A: Add OP_SET_ENV_SLOT_UP opcode that writes to curEnv->up
  - Option B: Flatten inherit sources into extra let env slots (requires
    adjusting ExprInheritFrom levels, complex because bindVars bound them
    relative to a separate inherit env with up=newEnv)
  - Option C: Push inherit env BEFORE let env (reversed order), then use
    levelOffset. But this changes the env chain order vs bindVars.

### Recursive attrsets with inherit or non-plain bindings (0 hits in hello)
- [ ] `rec { inherit (expr) ...; }` — needs chooseByKind + inheritEnv
- [ ] `rec { inherit x; }` — needs inherited binding support
- [ ] `rec { __overrides = ...; }` — deprecated, low priority

### Dynamic attribute names (0 hits in hello)
- [ ] `{ ${name} = value; }` — runtime name evaluation
- [ ] `a.${name}.c` in select — runtime name in attr path
- [ ] `a ? ${name}` in hasAttr — runtime name in attr path

## Inherent Tree-Walker Dependencies (cannot eliminate)
- Primop calls (`state.callFunction` for C++ builtins)
- `state.coerceToString` (calls `__toString` functor)
- `state.eqValues` for compound types (recursive deep comparison)
- App values (partial primop application → `callFunction`)
- Functor calls (`__functor` attrset)
- Store operations in coercion (copyPathToStore)

## Optimization Opportunities

### Quick Wins
- [ ] Inline scalar equality in OP_EQ/OP_NEQ (int/string/bool compare without eqValues)
- [ ] Inline concatLists in OP_LIST_CONCAT (force+memcpy, avoid EvalState call)
- [ ] Direct primop dispatch in OP_CALL_1 (check isPrimOp, call fn->impl directly)
- [ ] Fast-path string coercion in vmStrConcat (if already string, skip coerceToString)
- [ ] Remove redundant forceValue in arithmetic/comparison opcodes (compiler should emit FORCE before)

### Compiler Improvements
- [ ] Constant folding (1 + 2 → 3 at compile time)
- [ ] Tail call optimization (OP_TAIL_CALL)
- [ ] Superinstructions (GET_LOCAL_0_FORCE, etc.)

### Memory Optimization
- [ ] Reduce thunk allocation via strictness analysis
- [ ] Share CompilationUnits across identical file imports

## Code Quality
- [ ] Run full nix functional test suite with NIX_EVAL_BYTECODE=1
- [ ] Add property-based tests for bytecoded expressions
- [ ] Performance regression benchmarks
- [ ] Fix position tracking in inline thunk trampoline
- [ ] Add RAII cleanup for heap-allocated arrays in exception paths
