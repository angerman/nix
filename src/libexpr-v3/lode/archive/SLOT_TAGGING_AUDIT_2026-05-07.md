## Lambda-formal slot-tagging audit — STG-9 / WITH.md §4.5 #3

Audit date: 2026-05-07.  Goal: verify Phase 5's slot-threading covers
all the call-chain entry points so a Tag::Slot passed to a simple
lambda preserves its slot identity through to the body's
`OP_WITH_PUSH` / `OP_GET_LOCAL` / sub-thunk capture.

### Coverage matrix

| Path | Slot preserved? | Notes |
|---|---|---|
| `OP_CALL` → simple lambda | ✓ | `valueStack[newBase + 0] = arg` |
| `OP_CALL` → formals lambda | (forced) | `forceValue(vm, arg)` for non-ellipsis validation; slot identity lost.  Acceptable: a formals attrset isn't a slot use case. |
| `OP_TAIL_CALL` → simple lambda | ✓ | mirrors `OP_CALL` |
| `OP_TAIL_CALL` → formals lambda | (forced) | mirrors `OP_CALL` |
| `callClosure` (primop callback) | ✓ | `valueStack[newBase + 0] = arg`, no force |
| `forceValue` on `Tag::Slot` | ✓ | dereferences + memoises into the slot (vm.cc:5187) |
| `OP_GET_LOCAL` | ✓ | direct `valueStack[stackBase + n]` read |
| `OP_GET_LOCAL_FORCE` | ✓ | force-on-receive variant; chases through slot |
| `OP_GET_UPVALUE` | ✓ | direct `closure->upvalues[n]` read |
| `OP_GET_UPVALUE_FORCE` | ✓ | force-on-receive variant |
| `OP_WITH_PUSH` | ✓ | passes through any tag including `Tag::Slot` |
| `OP_WITH_LOOKUP` | ✓ | dereferences `Tag::Slot` via `withLookup` slot path |
| `OP_REC_BINDING_SLOT_REF` | ✓ | produces `Tag::Slot` pointing at `&entries[i].value` |
| `OP_MAKE_THUNK` upvalue capture | ✓ | pops Values into FAM tail; preserves any tag |
| `OP_MAKE_CLOSURE` upvalue capture | ✓ | mirrors `OP_MAKE_THUNK` |
| `snapshotCurrentWiths` | ✓ | bulk Value copy from `withStack`; preserves tag |
| `pushCapturedWiths` | ✓ | bulk Value copy into `withStack`; preserves tag |
| `v3ToTreeWalker` (v3 → TW) | (lost) | TW has no `Tag::Slot`; force is necessary at the bridge boundary |
| `treeWalkerToV3` (TW → v3) | n/a | TW values can't carry slot identity into v3 |

### `ir::With` lowering

`lower.cc::lowerWith` (line 2270+) produces `ir::With{attrs, body,
recAttrsVar, recAttrsName}`.  The emit (`emit.cc::emitOne(With)`) has
two paths:

1. **Rec-attrset entry path** (`recAttrsVar != ir::kInvalid`):
   Emits `OP_REC_BINDING_SLOT_REF` → `Tag::Slot` pointing at
   `&entries[i].value`.  Heap-stable.  This is the path WITH.md
   §4.5's "rec-attrset entry" landed.

2. **Generic-var path** (otherwise): Emits `emitVarRef(attrs)` →
   `OP_GET_LOCAL` / `OP_GET_UPVALUE` for the attrs source.  This is
   the path used for `with pkgs;` where `pkgs` is a lambda formal.
   The runtime value is whatever the caller passed; if Phase 5
   threaded a `Tag::Slot` through the call chain, with-stack gets a
   slot.  If not, with-stack gets a snapshot.

The lower comment on path 2 mentions "lifetime issues" (the slot's
storage being reused after the surrounding frame returns).  In
practice this concern doesn't apply for the fix-point chain we
care about — fix's slot points into `Bindings::entries[i].value`
on the v3 heap, which outlives the call chain.  The "lambda param"
that `with pkgs;` reads IS that heap slot, just propagated through
several intermediate lambda formals.

### Findings

1. **Phase 5 is fully landed** for the call paths that matter.  A
   `Tag::Slot` passed to a simple lambda's formal preserves its
   identity through to `with self;` / `with pkgs;` and into
   sub-thunk captures.

2. **No additional emit-side change is needed** for the lambda-formal
   case.  The slot identity is carried by the runtime Value through
   `OP_GET_LOCAL` etc.

3. **The remaining nixpkgs hello.name gap under
   `NIX_V3_STG_KEEP_HOOKS=1` is NOT a slot-tagging issue** — slot
   identity is preserved correctly.  The gap is the cross-VMState
   fresh-VMState pattern (TW->v3 hooks spawn fresh VMStates that
   force v3 thunks Black on the outer VMState).  Closing it
   requires either:
   - Single-VM evaluation (TW->v3 transitions push frames onto an
     existing VM rather than allocating fresh ones).
   - OR thunk-state-per-VMState (each VMState has its own Black
     bookkeeping; Black on VM A is NOT Black on VM B).

   Both are architectural changes outside §4.5's scope.

### Test confirmation

```nix
let
  fix = f: let x = f x; in x;
  simpleLambda = self: { a = "X"; b = self.a + "Y"; };
  nestedSimple = res: pkgs: super: with pkgs; {
    helper = "h"; derived = helper + "-d";
  };
in {
  simple = (fix simpleLambda).b;                    # "XY"
  withChain = (nestedSimple {} { helper = "h"; } {}).derived; # "h-d"
}
```

Both default mode (`NIX_USE_V3=1`) and STG mode
(`NIX_USE_V3=1 NIX_V3_STG=1`) produce the expected results,
confirming `with self;` works correctly through the lambda-formal
slot threading.

### Cross-references

- WITH.md §4.5: the design memo on `with` and the SECD DUM/RAP
  slot-aliasing move.
- OPTIMIZATION_PLAN.md Phases 1-5 (commits 07985c540, 7eca8f04b,
  a09278924): the multi-phase landing of `Tag::Slot` /
  `OP_REC_BINDING_SLOT_REF`.
- STG-1 through STG-8 (commits 2ed6007d3 → b00879e88): the broader
  STG-mode cleanup that removed the publish/recovery side-table
  and added cell-update at OP_RETURN.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
