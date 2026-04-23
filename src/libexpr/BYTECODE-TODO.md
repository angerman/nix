# Bytecode VM TODO List

## Current Status
- 50+ commits, 147 unit tests (4 new inherit regression tests)
- 599/599 existing nix-expr-tests pass (100%)
- nixpkgs hello.name evaluates correctly with full trampoline support
- Non-recursive attrset inherit(expr) compiled natively (OP_INHERIT_FROM_INIT/SET)
- Let-binding Inherited (plain `inherit x;`) compiled with levelOffset
- Lambda body offset reset (fresh env chains)
- OP_SET_ENV_SLOT_UP for writing through inherit env to let env
- Inline concatLists and scalar equality in VM
- Stack reallocation-safe (offset-based stackBase)
- Result aliasing fixed (push resultSlot, not retVal)

## Performance (debug build -O0)
- nixpkgs hello: ~13% overhead (3.56s vs 3.14s tree-walker)
- VM stats: 46,582 instructions, 57 trampolined calls, 5,272 thunk forces
- OP_EVAL_EXPR: 22 fallbacks (all `let inherit(expr)` patterns)

## Remaining OP_EVAL_EXPR Fallbacks

### `let inherit(expr)` (22 hits in nixpkgs hello)
The OP_INHERIT_FROM_INIT approach works for simple tests but crashes
on complex nixpkgs patterns.  The issue: thunks for inherit-from
sources may reference uninitialized recursive let slots (the tree-walker
handles this via ExprVar::maybeThunk creating a thunk when the slot
is null).  Wrapping in explicit thunks helps, but the interaction
between levelOffset in thunk bodies vs lambda bodies causes issues
in deeply nested scopes.

Options to fix:
- [ ] Track which scope-level each thunk body references and apply
      offsets selectively instead of globally
- [ ] Use a separate compilation pass for inherit-from source thunks
- [ ] Implement a smarter ExprVar fast-path that checks for null
      env slots and creates thunks at runtime (like the tree-walker)

### Other fallbacks (0 hits in nixpkgs hello)
- [ ] `rec { inherit (expr) ...; }` — recursive attrset + inherit
- [ ] `rec { inherit x; }` — non-plain bindings in recursive sets
- [ ] Dynamic attribute names — `{ ${name} = val; }`, `a.${n}`, `a ? ${n}`

## Inherent Tree-Walker Dependencies (cannot eliminate)
- Primop calls (state.callFunction for C++ builtins)
- state.coerceToString (calls __toString functor)
- state.eqValues for compound types (recursive deep comparison)
- App values (partial primop application)
- Functor calls (__functor attrset)
- Store operations in coercion (copyPathToStore)

## Completed Optimizations
- [x] Inline concatLists in OP_LIST_CONCAT (force+memcpy)
- [x] Inline scalar equality in OP_EQ/OP_NEQ (int/string/bool/null)
- [x] OP_INHERIT_FROM_INIT/SET for non-rec attrset inherit(expr)
- [x] levelOffset for Inherited bindings in compileLet
- [x] Lambda body offset reset
- [x] Thunk body offset preservation

## Future Optimizations
- [ ] Direct primop dispatch in OP_CALL_1 (check isPrimOp)
- [ ] Fast-path string coercion in vmStrConcat
- [ ] Remove redundant forceValue in arithmetic opcodes
- [ ] Constant folding, tail call optimization
- [ ] Superinstructions (GET_LOCAL_0_FORCE, etc.)
