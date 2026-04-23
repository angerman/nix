# Nix Bytecode VM v2 Design

## Overview

The v2 architecture addresses three fundamental limitations of v1:
1. **Env chain pointer identity** — closures capture the entire Env chain by pointer, making VM/tree-walker interop path-dependent
2. **No optimization infrastructure** — single-pass AST→bytecode with no intermediate representation
3. **No parallelism foundation** — single-threaded with non-atomic blackhole mechanism

## Pipeline

```
AST (after bindVars)
  |
  | [1] IR Builder (ir-builder.cc)
  |     - A-normal form conversion
  |     - Free variable analysis → upvalue lists
  |     - Desugar: inherit(expr), with, rec attrsets
  v
IrModule (unoptimized)
  |
  | [2] Optimization passes (ir-opts.cc)
  |     a. Constant folding
  |     b. Dead code elimination
  |     c. Thunk elimination (trivial thunks)
  |     d. Strictness analysis
  |     e. Inline cache annotation
  v
IrModule (optimized)
  |
  | [3] Bytecode emitter (ir-emit.cc)
  |     - Register allocation (IrRef → stack slots)
  |     - Upvalue capture instructions
  |     - CompilationUnitV2 generation
  v
CompilationUnitV2 (bytecode)
```

## Key Architecture Changes

### 1. Upvalue-Based Closures (solves pointer identity)

**v1**: closure = (ExprLambda*, Env*)
- Env is a linked chain; different allocations = different pointers
- Thunk memoization is path-dependent

**v2**: closure = (CodePointer, Value* upvalues[N])
- Flat array of exactly the N free variables needed
- Two closures with same free vars are semantically identical
- No Env chain, no pointer identity issues
- Variable access: O(1) for both locals (stack slots) and upvalues

### 2. IR with Explicit Free Variables

36 IR node types in A-normal form (all subexpressions named):
- Literals, VarRef, Lambda, App, Force, MkThunk
- AttrSelect, HasAttr, MakeAttrs, MakeList
- BinOp, UnaryOp, If, Assert, With, ConcatStrings
- Each Lambda/MkThunk carries explicit FreeVars list

Free variable analysis: bottom-up walk, threading captures through
function boundaries via UpvalueDesc chains.

### 3. Optimization Passes (priority order)

| # | Pass | Impact | Effort | Description |
|---|------|--------|--------|-------------|
| 1 | Thunk elimination | HIGH | EASY | Inline trivial thunk bodies (single var/const) |
| 2 | Inline caching | HIGH | MEDIUM | Monomorphic IC for ATTR_SELECT (O(log n) → O(1)) |
| 3 | Dead code elimination | MEDIUM | MEDIUM | Remove unused let-bindings |
| 4 | Constant folding | LOW | EASY | Fold pure ops on known constants |
| 5 | Strictness analysis | HIGH | HARD | Avoid thunk alloc for strict params |

### 4. Parallelism-Ready Thunks

ThunkV2 with atomic state machine:
- Suspended → Pending (CAS) → Evaluated
- Lock-free blackholing
- Per-thread value stacks, shared GC heap

## Migration Roadmap

1. **IR infrastructure** — IrModule types, AST→IR builder, IR→bytecode emitter
2. **Upvalue closures** — OP_GET_UPVALUE, OP_MAKE_CLOSURE_V2, CallFrameV2
3. **Optimization passes** — constant folding → DCE → thunk elim → strictness
4. **Atomic thunk state** — CAS-based forcing, single-threaded first
5. **Parallel evaluation** — per-thread VMState, fork-join for builtins.map

Each milestone is independently shippable. v1 opcodes remain for interop.
The existing 612 tests serve as regression suite at every step.
