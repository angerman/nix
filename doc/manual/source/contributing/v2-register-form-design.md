# Generalized register-form opcodes (B4-design)

Design pass for Phase B4 of the v2 bytecode VM perf plan.  Goal:
expand register-form coverage so every non-trivial IR binding can
write its result directly to a stack slot instead of round-tripping
through the operand stack with `OP_SET_STACK_SLOT`.

## Why

Phase 1.1's ANF de-materialization peephole only fires when the
binding's result is consumed by the very next `IRVarRef` AND the
result is referenced exactly once in the block.  Multi-use bindings
and bindings whose consumer is several emit-steps later still pay
the SET/GET round-trip.

The bottleneck investigator (2026-04-26 multi-agent review) measured
10.7M dispatched instructions on `nixpkgs#hello.name`.  Roughly half
of remaining SET_STACK_SLOT opcodes survive Phase 1.1.  A generalized
register-form story could eliminate ~10-15% of dispatched
instructions — the largest single execution-side lever remaining.

## Existing register-form opcodes

| Opcode             | Encoding          | Semantics                              |
|--------------------|-------------------|----------------------------------------|
| `OP_RFORCE_FROM`   | `[dst:8|src:16]`  | `dst = force(slot src)`                |
| `OP_RGET_UV_TO`    | `[dst:8|uvIdx:16]`| `dst = upvalue[idx]`                   |
| `OP_RUVF_TO`       | `[dst:8|uvIdx:16]`| `dst = force(upvalue[idx])`            |
| `OP_RCALL1_R`      | `[dst:8|f:8|a:8]` | `dst = call(slot f, slot a)`           |
| `OP_RADD_R`        | `[dst:8|l:8|r:8]` | arithmetic (l + r)                     |
| `OP_RSUB_R/RMUL_R` | same shape        | arithmetic                             |
| `OP_RLESS_R/REQ_R` | same shape        | comparison                             |
| `OP_RATTR_SELF_R`  | `[dst:8|a:8|c:8]` | `dst = (slot a).<attr>` (cached)       |

## Proposed additions

Two flavours: **fixed-arity** (operands fit in 24 bits — encode
inline) and **variable-arity** (operands consumed from the operand
stack — encode just dst slot).

### Fixed-arity (operands inline)

| Opcode               | Encoding              | IRExpr           |
|----------------------|-----------------------|------------------|
| `OP_RLIT_INT`        | `[dst:8|imm:24]`      | `IRLitInt`       |
| `OP_RLIT_BOOL`       | `[dst:8|val:1\|...]`  | `IRLitBool`      |
| `OP_RLIT_NULL`       | `[dst:8|0:24]`        | `IRLitNull`      |
| `OP_RCONST`          | `[dst:8|constIdx:24]` | `IRLitFloat/String/Path` (via constants pool) |
| `OP_RUPDATE_R`       | `[dst:8|l:8|r:8]`     | `IRUpdate`       |
| `OP_RCONCATLIST_R`   | `[dst:8|l:8|r:8]`     | `IRConcatLists`  |
| `OP_RHASATTR_R`      | `[dst:8|s:8|sym:8]`   | `IRHasAttr`      |
| `OP_RNOT_R`          | `[dst:8|src:16]`      | `IRNot`          |
| `OP_RNEG_R`          | `[dst:8|src:16]`      | `IRNeg`          |

Handler shape (template):
```cpp
case OP_RXXX: {
    uint32_t operand = decodeOperand(CUR_INSTR);
    uint8_t dst = unpackDst(operand);
    /* unpack args */;
    /* compute */;
    size_t base = stackBase;
    vm.ensureCapacity(base + dst + 1, &Value::vNull);
    vm.stack[base + dst] = result;
    DISPATCH();
}
```

### Variable-arity (operands on operand stack, dst encoded)

The args (attrset values, list elements, primop args, string
parts, MakeThunk/MakeClosure upvalues) are pushed via `emitVarRef`
calls before the constructor opcode.  Register form just writes
the final result to `dst`.

| Opcode                   | Encoding                  | IRExpr           |
|--------------------------|---------------------------|------------------|
| `OP_RMAKE_THUNK_V2`      | `[dst:8|thunkIdx:24]` + `[nUpvalues:24]` | `IRMkThunk`      |
| `OP_RMAKE_CLOSURE_V2`    | `[dst:8|lambdaIdx:24]` + `[nUpvalues:24]` | `IRLambda`       |
| `OP_RATTRS_INIT`         | `[dst:8|nAttrs:24]` + `[sym/pos]*nAttrs` | `IRAttrSet`      |
| `OP_RLIST_INIT`          | `[dst:8|nElems:24]`       | `IRList`         |
| `OP_RCONCAT_STRINGS`     | `[dst:8|nParts:24]`       | `IRConcatStrings`|
| `OP_RCALL_PRIMOP_R`      | `[dst:8|primIdx:24]` + `[nArgs:24]` | `IRPrimOpCall` |

Handler shape: identical to existing op except `vm.push(result)`
becomes `vm.stack[base + dst] = result; vm.sp doesn't change`.

## ir-emit.cc plumbing

In `emitBlock`'s binding loop, add a register-form branch BEFORE
the operand-stack branch for each new opcode:

```cpp
// IRLitInt example
if (auto * lit = std::get_if<ir::IRLitInt>(&binding.expr)) {
    uint32_t dstSlot = ctx.allocSlot(binding.result);
    if (dstSlot <= 0xFF
        && lit->value >= 0
        && lit->value <= kImm16Max) {
        unit.emit(OP_RLIT_INT, packDstImm(dstSlot, lit->value));
        continue;  // SET_STACK_SLOT skipped
    }
}
```

For variable-arity ops the pattern is similar but emits `emitVarRef`
calls first to push the operands, then the register-form opcode.

## Cost / complexity

Per new opcode: ~50 lines spread across:
* 1 line in `bytecode.hh` enum
* 1 line in disasm operand-decode
* ~20 lines in `vm.cc` handler
* ~10 lines in `ir-emit.cc` register-form pattern match

Estimated ~500 LOC total for ~10 new opcodes.  Implementation can
land incrementally — each opcode is independent and testable in
isolation.

## Risk / verification

* The compiler's existing ANF peephole still fires for the
  immediate-consumer case; register-form is the next layer for
  multi-use bindings.  Both can co-exist.
* Test approach: run the existing 718 unit tests after each opcode
  lands.  Add explicit register-form coverage tests that compare
  emit output before/after to catch regressions.
* Negative-test: when dst slot exceeds 0xFF or operand exceeds
  encoding range, fall through to the existing operand-stack path.
  This mirrors what `OP_RCALL1_R` already does.

## Projected impact

* Compile cost: +5-10ms per CU (extra branches in the binding
  loop's pattern match).  Negligible.
* Execution cost: -10-15% of dispatched instructions on
  `nixpkgs#hello.name` per the bottleneck investigator's
  estimate.  Each saved SET/GET pair is ~2 dispatches at ~38ns
  each = ~76ns per pair.  At 10.7M instructions executed and
  ~50% currently going through ANF round-trips, halving that
  saves ~2.6M instructions = ~100ms wall.

## Sequencing

Land in this order — each opcode independently testable:

1. `OP_RLIT_INT` (smallest payoff, validates encoding).
2. `OP_RCONST` (covers IRLitFloat/String/Path).
3. `OP_RNOT_R` / `OP_RNEG_R`.
4. `OP_RUPDATE_R` / `OP_RCONCATLIST_R`.
5. `OP_RHASATTR_R`.
6. `OP_RMAKE_THUNK_V2` (largest payoff — thunks are everywhere).
7. `OP_RMAKE_CLOSURE_V2`.
8. `OP_RATTRS_INIT`.
9. `OP_RLIST_INIT`.
10. `OP_RCONCAT_STRINGS`.
11. `OP_RCALL_PRIMOP_R`.

After step 6 we should already see most of the projected win since
thunks dominate compiled bytecode counts in nixpkgs.
