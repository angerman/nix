# Iterative ForceValue Audit — 2026-05-18

Per action plan Phase 1.1. Pre-condition for the Phase 1.2 conversion
of the highest-priority recursive site.

## Premise

`forceValue(VMState&, Value)` (vm.cc:8571) is the v3 force entry point.
Its body is **internally iterative** for Tag::App / Tag::Slot / Tag::Thunk
chases (see the long `while (true)` loop). C-recursion concerns are
at **CALL SITES of forceValue from inside other code**, not the
function itself.

Each recursive forceValue call grows the C stack by roughly the size
of the visible callee frame plus the inner dispatchLoop frame (when the
forced thunk re-enters dispatch). The A8 series (commits f6bf3fe8d,
f82a2f725, 5d9909d8c, f5804ea05, 3d997fc7d) converted ~14 opcode
handlers to the `op_force_slow` + frame-retry pattern, which uses
**writeback to a frame-local slot** instead of returning the forced
value through a C-frame.

Phase 1 closes the remaining gap.

## Totals

```
src/libexpr-v3/vm.cc       : 62 forceValue call sites
src/libexpr-v3/primops.cc  : 114 forceValue call sites
                           ------
                           176 total
```

## Classification scheme

- **(a) ALREADY-ITERATIVE** — the call site is already inside an
  `op_force_slow`-style frame-retry loop OR is a one-shot leaf force
  whose result is consumed in the same C frame and never recurses.
  No conversion needed.

- **(b) NEEDS-WRITEBACK** — the call is inside a loop or recursive
  pattern that can grow the C stack proportionally to user data
  (list length, attrset size, recursion depth). Convert per the A8
  pattern.

- **(c) LEAF-BOUNDED** — primop leaf operation that forces a fixed
  number of arguments. C-stack growth bounded by primop arity (≤8).
  May stay recursive; explicit depth cap noted.

## Highest-priority NEEDS-WRITEBACK candidates

The audit's primary deliverable: identify the ≤5 call sites where
conversion gives the most C-stack budget. Phase 1.2 picks one.

### B1. `callClosure` primop-arg force loop (vm.cc:9501-9504)

```cpp
for (uint32_t i = 0; i < po->arity; ++i) {
    if (po->lazyArgs & (1u << i)) continue;
    buf[i] = forceValue(vm, buf[i]);
}
```

Per call: up to 8 forceValue invocations. Per recursion depth of
callClosure (which is itself called recursively for App spines and
__functor chains): up to 8 × N. For nixpkgs callPackage chains, N
can be hundreds. **Best target for Phase 1.2** — single conversion,
broad benefit.

Conversion approach: pre-force args before the OP_CALL_PRIMOP frame
push using `op_force_slow` + per-arg writeback slots (the
`CFF_FORCE_WB_PTR` mechanism is already in place for ListVec
element pre-force; extend to PrimOp arg buffer).

### B2. `OP_CALL` PrimOp arg force loop (vm.cc:2714)

```cpp
for (uint32_t i = 0; i < po->arity; ++i) {
    if (po->lazyArgs & (1u << i)) continue;
    buf[i] = forceValue(vm, buf[i]);
}
```

Sister to B1 — the OP_CALL primop branch. Same conversion applies.
Should land in the same Phase 1.2 commit.

### B3. `valueEqual` recursive list/attrset compare (vm.cc near 492)

```cpp
ai = forceValue(vm, ai);
bi = forceValue(vm, bi);
if (valueEqual(vm, ai, bi)) ...
```

`valueEqual` recurses on list elements and attrset entries. Each
recursion forces both sides. For deep nested structures (cardano-
node config attrsets are ~7-8 deep), the C stack grows in lockstep.
**Phase 1.2 secondary candidate** if the conversion is cheap; else
Phase 1 follow-up.

Conversion approach: iterative comparison with an explicit work
stack (`std::vector<std::pair<Value, Value>>`).

### B4. primops.cc `primFoldl` (primops.cc:986)

```cpp
for (uint32_t i = 0; i < src->size; ++i) {
    Value step1 = callClosure(*state.vm, op, acc);
    acc = callClosure(*state.vm, step1, src->elems[i]);
}
```

Each iteration's `callClosure` may force args via B1. For long lists
(`foldl' (a: b: a + b) 0 (genList id 5000)`), depth = list length.
Closing B1 closes B4 transitively.

### B5. primops.cc `primConcatLists` / `primConcatMap` element forces

```cpp
for (uint32_t i = 0; i < lst->size; ++i) {
    Value el = forceValue(*state.vm, lst->elems[i]);
    ...
}
```

Sequential element forces. Each force may recurse via the element's
own thunk. For `concatLists [[a] [b] ...]` where each element is
itself a thunked list, the recursion can compound.

Already partially mitigated via ListVec writeback hooks in some
sites; audit which call sites still use the raw C-return pattern.

## ALREADY-ITERATIVE call sites (a)

Confirmed by inspection:

- **forceValue's own Tag::App spine walk** (vm.cc:8924-8951). Walks
  `rights` vector via `v = v.payload.pair->left`; bottoms out the
  recursion at the leaf, then applies via `callClosure` in a loop.
  See A8 commit f6bf3fe8d.
- **OP_FORCE's chase loop** (vm.cc:4512-4561). Same pattern. Path
  compression writes back through the chase chain.
- **OP_LIST_CONCAT** (vm.cc:5086). Per A8 commit f82a2f725.
- **OP_ATTRS_UPDATE / OP_ATTRS_UPDATE_TAIL** — A8 commit 5d9909d8c.
- **OP_STR_CONCAT** — A8 commit f5804ea05.
- **OP_HEAD / OP_TAIL / OP_LENGTH / OP_ELEM_AT** — A8 commit
  3d997fc7d.

These ~14 opcode handlers form the proof-of-pattern for the writeback
approach.

## LEAF-BOUNDED call sites (c)

- **primops with arity ≤ 8**: most of the 114 primops.cc sites are
  the canonical "force args, do the math, return" shape. Bounded
  by `po->arity ≤ 8`.
- **forceValue's depth-bound guard** (vm.cc:8896-8902): the
  `kMaxCallDepth` (5000) check at the top of forceValue itself is
  the bound-of-last-resort. Any (c)-classified site relies on this
  guard.

## Estimate of Phase 1.2 scope

Converting **B1 + B2** is the highest-leverage single PR. It touches:
- `vm.cc` OP_CALL primop branch (~15 lines)
- `vm.cc` callClosure primop branch (~15 lines)
- An extension of the existing CFF_FORCE_WB_PTR helpers for arrays
  (currently for ListVec; widen to PrimOp arg buffer)

Estimated 1-2 days. Verification: the 5000-deep let-chain test
(Phase 1.3) should pass with both B1 and B2 converted.

If after B1+B2 the 5000-deep test still C-stack-overflows, the
remaining recursion lives in **B3 (valueEqual)** or **B5
(primConcatLists family)**. Triage at that point.

## Followups (NOT Phase 1)

- **B3 valueEqual** — convert to explicit-stack iteration. Independent
  effort.
- **B4 primFoldl** — closes via B1.
- **Phase 2 dependency**: Tag::App writeback inside `forceValue` is
  already path-compression-capable; making it write back to
  bindings entries (the memoization that primAttrValues breaks per
  #583) is a separate concern from C-stack iteration.

## Methodology note

Per-call-site classification of all 176 sites would take ~1 day
alone. The audit above does the architectural triage instead:
identify the high-leverage targets, leave the long tail to be
audited as Phase 1.2 closes each candidate. This matches the action
plan's "1 day" budget — full enumeration is for Phase 4 cleanup.

## Phase 1.2 progress log

Per action plan Rule 0 (every commit must answer "what hypothesis
does this kill?"): the work below incrementally falsifies the
"forceValue function call is paid on every primop arg" model.

### Step 1 — RESOLVED 2026-05-15 (commit 8df749725)

**Hypothesis killed**: "callClosure and OP_CALL share the same
primop-arg force fast path." (They didn't — OP_CALL had inline
WHNF skip from #558 Phase 2.3; callClosure did not.)

**Site**: `vm.cc` callClosure primop-arg force loop (formerly
B1 in the audit's priority list, lines ~9572-9575).

**Pattern applied**: inline tag check before forceValue, same as
OP_CALL's existing pattern. No iterative-via-writeback yet — the
fast-path is the WHNF-skip flavour.

**Verification**: regression tests pass; bench n=5 vs the morning's
baseline shows movement within ±3% noise (workloads in the corpus
don't heavily exercise recursive-primop chains).

### Step 2 — RESOLVED 2026-05-15 (commit 557d1fac8)

**Hypothesis killed**: "all hot primop list iterations have the
WHNF fast-path applied uniformly." (They didn't — primConcatMap /
primPartition / primAll / primAny / primMap had it; primConcatLists
and primConcatStringsSep didn't.)

**Sites**:
- `primops.cc` primConcatLists element-force loop (B5 family).
- `primops.cc` primConcatStringsSep element-force loop (B5 family).

**Pattern applied**: same inline WHNF-skip as step 1.

**Verification**: regression tests pass; bench within ±3% noise.

### Remaining Phase 1.2 candidates

The WHNF-skip pattern is the easy half. The remaining work is the
**iterative-via-writeback** conversion for sites where the
recursion isn't on a single value-check but on a deeply-nested
structure. From the audit:

- **B3 valueEqual** — recursive list/attrset comparison. Converting
  requires an explicit `std::vector<std::pair<Value, Value>>` work
  stack. Independent effort; 1-2 days. Not closed by step 1/2.
- **App-spine deep recursion** — surfaces in the
  `v3-iterative-force-depth` test's app-spine-5000 probe (still
  fails at the kMaxCallDepth=5000 guard, per the probe). Each `id
  (id (... 0))` application's body forces its arg, recursing into
  callClosure → forceValue. Fix requires making the inner force
  chain truly iterative — push frames onto vm.frames and let the
  outer dispatchLoop drive, instead of C-recursing through
  forceValue + callClosure. Architectural change; multi-day.

These are the remaining hypotheses the rest of Phase 1.2 will kill.
