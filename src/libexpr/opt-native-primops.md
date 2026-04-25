# VM-Native Primop Continuations

## Status: Design Complete, Implementation Pending

## Problem

Primops like `map`, `filter`, `genList`, `foldl'`, `sort`, `any`, `all`,
`mapAttrs` are implemented as tree-walker C++ functions (`prim_map`, etc.
in `primops.cc`). When called from the v2 bytecode VM, the call path is:

```
VM dispatch loop (vmExec)
  → OP_CALL_1 / OP_CALL_PRIMOP
    → prim_map(state, pos, args, result)       ← tree-walker C++ code
      → for each element:
          state.allocValue()->mkApp(f, elem)   ← creates lazy App thunk
      → return list of App thunks
  ← back in VM

Later, when list elements are forced:
  VM dispatch loop
    → OP_FORCE
      → state.forceValue(App(f, elem))         ← tree-walker forceValue
        → state.callFunction(f, elem, result)  ← tree-walker callFunction
          → detects isBytecodeProxy
            → vmExec(...)                      ← NEW nested vmExec!
```

### Consequences

1. **Frame depth exhaustion**: Each nested `vmExec` pushes frames onto
   the shared `VMState.frames` stack. Deep overlay chains (old nixpkgs
   23.11, haskell.nix) hit the 65536 frame limit through:
   `genList → N lazy thunks → force each → vmExec → force more → ...`

2. **Extra allocations**: `prim_map` creates N `mkApp` thunks (N
   allocValues), each containing a function pointer + arg. These are
   intermediate values that exist only to be forced immediately.

3. **vmExec re-entry overhead**: Each element force creates a new
   `vmExec` invocation with its own `entryFrameDepth`, `DepthGuard`,
   local variable setup. The `vmExecDepth` counter (limit 10000) can
   also be hit for large lists.

4. **No tail-call across primop boundary**: A `map f (map g list)` chain
   creates nested vmExec calls that can't be tail-call optimized.

## Solution: Continuation-Based Primop Execution

Instead of calling the tree-walker `prim_map` implementation, the VM
detects the primop at `OP_CALL_1` time and pushes a **continuation
frame** that drives the iteration directly within the dispatch loop.

### Architecture

```
VM dispatch loop
  → OP_CALL_1: fun = builtins.map, arg = f
    → OP_CALL_1: fun = PrimOpApp(map, f), arg = list
      → Detect: saturated map(f, list)
      → Force list
      → Allocate result array (N elements)
      → Push ContState::Map continuation frame:
          { kind=Map, func=f, index=0, count=N,
            inputElems=list.elems, results=resultArray }
      → Call f(list[0]) by pushing a normal call frame
      → DISPATCH (enters f's bytecoded body)

  ... f's body executes, hits OP_RETURN ...

  → OP_RETURN:
    → Pop call frame, write result
    → Check parent frame: has ContState::Map continuation!
    → Store result in results[index]
    → index++
    → if index < count:
        → Call f(list[index]) by pushing another call frame
        → DISPATCH (stays in same vmExec, no re-entry!)
    → else:
        → Build final list Value from results array
        → Pop continuation frame
        → Push result, DISPATCH
```

### Key Properties

- **Single vmExec invocation**: The entire map/filter/genList runs within
  one vmExec call. No nested re-entry, no vmExecDepth accumulation.

- **No intermediate App thunks**: Elements are called directly via the
  VM's call mechanism, not through `mkApp` + lazy forcing.

- **Frame depth = call depth**: Each element application uses one call
  frame (pushed and popped within the loop). The continuation frame
  itself is on the parent frame. Total frame depth for `map f list`
  with N elements is: parent + 1 continuation + 1 active call = 3.
  Compare to current: parent + N nested vmExec entries.

- **Works with v2 closures AND tree-walker lambdas**: The call inside
  the continuation uses the same OP_CALL_1 dispatch as normal calls.
  If `f` is a v2 closure, it trampolines. If it's a tree-walker
  lambda, it falls back to callFunction (but only for one element at
  a time, not the whole list).

## ContState (already defined in vm.hh)

```cpp
enum class ContKind : uint8_t {
    None = 0,
    Map,            // builtins.map: apply f to each element
    Filter,         // builtins.filter: apply pred, collect matches
    AllAny,         // builtins.all/any: apply pred, short-circuit
    FoldlStrict,    // builtins.foldl': accumulate with strict eval
    Sort,           // builtins.sort: comparison-based sort
    MapAttrs,       // builtins.mapAttrs: apply f to each attr
    GenList,        // builtins.genList: generate list by index
};

struct ContState {
    ContKind kind = ContKind::None;
    uint32_t index = 0;       // Current iteration index
    uint32_t count = 0;       // Total elements
    Value * func = nullptr;    // The closure being applied
    Value * list = nullptr;    // The input list/attrset
    Value ** results = nullptr; // GC-traced result array
    Value ** inputElems = nullptr;
    uint32_t nResults = 0;    // For filter: matches collected
    Value * accumulator = nullptr; // For foldl'
    bool isAll = true;        // For all/any distinction
};
```

## Implementation Plan

### Phase 1: builtins.map (highest impact)

`builtins.map` is the most frequently called iterating primop in nixpkgs.

**Detection in OP_CALL_1** (vm.cc):
When OP_CALL_1 sees a saturated `PrimOpApp(map, f)` with `arg = list`:
```cpp
if (fun->isPrimOpApp()) {
    Value * root = fun->primOpApp().left;
    if (root->isPrimOp() && root->primOp()->name == "map") {
        Value * f = fun->primOpApp().right;
        // Force the list argument
        state.forceList(*arg, pos, "");
        auto listSize = arg->listSize();
        auto * listElems = arg->listElems();
        // Allocate result array
        auto * results = state.mem.allocBoehmArray<Value *>(listSize);
        // Push continuation frame
        vm.frames.back().cont = ContState{
            .kind = ContKind::Map,
            .index = 0,
            .count = static_cast<uint32_t>(listSize),
            .func = f,
            .results = results,
            .inputElems = listElems,
        };
        // Call f(list[0]) — push call frame for first element
        if (listSize > 0) {
            // ... push f + listElems[0] and dispatch to OP_CALL_1 logic
        } else {
            // Empty list: push empty result immediately
        }
    }
}
```

**Continuation in OP_RETURN** (vm.cc):
After popping a call frame and restoring the parent:
```cpp
auto & parentCont = vm.frames.back().cont;
if (parentCont.kind == ContKind::Map) {
    // Store the result from the just-completed call
    parentCont.results[parentCont.index] = resultSlot;
    parentCont.index++;
    if (parentCont.index < parentCont.count) {
        // More elements: call f(list[next])
        Value * f = parentCont.func;
        Value * nextElem = parentCont.inputElems[parentCont.index];
        // Push f and nextElem, dispatch to call logic
        // (reuse the same continuation frame)
    } else {
        // Done: build list Value from results array
        auto * listVal = state.allocValue();
        listVal->mkList(parentCont.count, parentCont.results);
        parentCont.kind = ContKind::None; // clear continuation
        vm.push(listVal);
        DISPATCH();
    }
}
```

### Phase 2: builtins.filter

Similar to map but with conditional collection:
- Call `pred(elem)` for each element
- On return: if result is true, copy `elem` to results[nResults++]
- After all elements: build list from results[0..nResults-1]

### Phase 3: builtins.genList

Simpler than map — no input list, just index:
- Call `f(0)`, `f(1)`, ..., `f(n-1)`
- On return: store result in results[index]
- After all: build list

### Phase 4: builtins.foldl'

Accumulator pattern:
- Call `op(acc, elem[0])`, force result (strict!)
- On return: `acc = result`, call `op(acc, elem[1])`
- After all: push final acc

### Phase 5: builtins.all / builtins.any

Short-circuit pattern:
- Call `pred(elem[0])`
- On return: if `all` and result is false → short-circuit, push false
- If `any` and result is true → short-circuit, push true
- Otherwise continue to next element

### Phase 6: builtins.sort

Comparison-based sort — more complex:
- Needs to call `comparator(a, b)` during sort algorithm
- Could use insertion sort for small lists, merge sort for large
- Each comparison is a continuation call
- State machine tracks sort progress between comparisons

### Phase 7: builtins.mapAttrs

Like map but over attrset entries:
- Call `f(name, value)` for each attribute
- Collect results as new attrset

## Detection Strategy

The continuation path must be triggered at the right point. Options:

### Option A: Detect in OP_CALL_1 PrimOpApp handler (recommended)
When OP_CALL_1 saturates a PrimOpApp and the root primop is one of the
known iterating primops, enter the continuation path instead of calling
the C++ impl. This is the most natural insertion point.

### Option B: New opcode OP_CALL_PRIMOP_CONT
The emitter detects saturated calls to known primops and emits a special
opcode. Requires compile-time primop detection (already partially done).

### Option C: Primop flag
Add a `isIterating` flag to PrimOp descriptors. OP_CALL_1 checks the
flag and routes to the continuation path. Most general but requires
touching the primop registration.

**Recommendation**: Option A for initial implementation (minimal changes,
works with runtime detection). Add Option C later for cleanliness.

## Expected Impact

### Frame depth
- Current: `map f (100-element list)` → 100 nested vmExec re-entries
- After: `map f (100-element list)` → 1 vmExec, 1 continuation, 100
  sequential call frames (max depth 3 at any time)

### Old nixpkgs / cardano-node
- The frame depth 65536 guard that blocks old nixpkgs 23.11 evaluation
  is hit because genList/reverseList/imap1 create deep nested vmExec
  chains. With continuations, the iteration is flat (depth 3), so the
  guard is never hit.
- **This is the most likely fix for the old nixpkgs correctness issue.**

### Performance
- Eliminates N allocValues per map/filter/genList call (no mkApp thunks)
- Eliminates N vmExec re-entries (no DepthGuard, no entryFrameDepth)
- Eliminates N callFunction dispatches (direct VM call mechanism)
- Estimated: 5-15% overall speedup, more on list-heavy code

### Memory
- No intermediate App thunks (saves 2 allocValues per element)
- For `map f list` with N elements: saves 2N allocations
- With ~180K primop calls in nixpkgs eval, many iterating over lists
  of 10-100 elements, this is potentially 500K-2M fewer allocations

## Files to Modify

| File | Change |
|------|--------|
| `src/libexpr/vm.cc` OP_CALL_1 | Detect saturated iterating primops, push ContState |
| `src/libexpr/vm.cc` OP_RETURN | Check parent frame ContState, advance iteration |
| `src/libexpr/include/nix/expr/vm.hh` | ContState already defined, may need tweaks |
| `src/libexpr/include/nix/expr/eval.hh` | Add `isIteratingPrimop(PrimOp*)` helper |

## Risks

1. **Primop semantics**: The C++ `prim_map` has error handling, position
   tracking, and edge cases (empty list, non-function arg, etc.) that
   the continuation must replicate exactly.

2. **Exception handling**: If `f(elem)` throws, the continuation must
   clean up properly (pop the continuation frame, restore state).

3. **Lazy vs eager**: `prim_map` creates LAZY App thunks. The
   continuation calls `f` EAGERLY for each element. For `builtins.map`,
   this changes evaluation order — elements are forced left-to-right
   instead of on-demand. This is semantically different! For `genList`
   and `filter`, the same issue applies.

   **Mitigation**: Only use continuations when the result list is
   immediately consumed (e.g., followed by `builtins.length`, `head`,
   `elemAt`, or another `map`). For the general case, fall back to
   the lazy App thunk approach. Alternatively, create the result list
   lazily but use the continuation to force elements on demand (more
   complex but semantically correct).

   **Alternative**: Create the result as a list of thunks, where each
   thunk's body is a continuation call. But this defeats the purpose.

   **Recommended**: For `foldl'` (which is strict by definition) and
   `filter` (which must evaluate the predicate), continuations are
   semantically correct. For `map` and `genList`, the result must
   remain lazy — so the continuation only helps when the result is
   immediately forced (which is common in nixpkgs but not guaranteed).

4. **sort comparator**: The sort algorithm needs to call the comparator
   multiple times in a non-linear order. A simple index-based
   continuation doesn't work. Need a state machine for the sort
   algorithm itself.
