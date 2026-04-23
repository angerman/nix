# Bytecode VM TODO List

## Current Status
- 60+ commits, 152 bytecode tests + 452 other tests = 604 total
- All 604 tests pass
- nixpkgs hello.name evaluates correctly
- Desugared inherit(expr) for let, non-rec attrsets, AND rec attrsets
- Forward reference fix for recursive let bindings (level=0 thunk wrapping)
- SELECT_FORCE for desugared thunks (force after attribute selection)
- Inline concatLists + scalar equality in VM

## Performance (debug build -O0)
- nixpkgs hello: ~20% overhead (3.75s vs 3.1s tree-walker)
  - 558ms (15%) spent in bytecode compilation alone
  - Bytecoded execution is FASTER than tree-walker after compilation
- VM stats:
  - 163,912 bytecoded instructions
  - 13,597 bytecoded thunk forces
  - 3,666 trampolined lambda calls
  - 14,692 OP_CALL_1 tree-walker calls (primops)
  - 71 OP_FORCE tree-walker calls
  - 15 OP_EVAL_EXPR fallbacks
  - 251 compilation units (all cold)

## Remaining 15 OP_EVAL_EXPR Fallbacks

### Dynamic ExprSelect (14 hits)
`(attrset)."${expr}"` patterns in lib/systems/parse.nix.
Requires runtime string-to-symbol conversion via `state.symbols.create()`.
- [ ] Add OP_ATTR_SELECT_DYN that pops a string name + attrset, does dynamic lookup

### Non-rec attrset with special bindings (1 hit)
`{ description = "..." ... }` in modules — has non-plain bindings.
- [ ] Investigate: likely an `inherit` binding without `(expr)`

## Inherent Tree-Walker Dependencies
- Primop calls (14,692 OP_CALL_1 fallbacks — C++ builtins)
- state.coerceToString (__toString functor)
- state.eqValues for compound types (recursive)
- App values (partial primop application)

## Optimization Opportunities
- [ ] Direct primop dispatch in OP_CALL_1 (avoid callFunction overhead)
- [ ] OP_ATTR_SELECT_DYN for dynamic attribute names
- [ ] Reduce level=0 thunk wrapping overhead (only wrap when forward ref is possible)
- [ ] -O2 build to reduce compilation overhead
- [ ] Compilation caching across evaluations
- [ ] Constant folding, tail call optimization
