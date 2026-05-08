# STG-8 Cell-Update Mechanism Audit (2026-05-08)

Static read-only audit of every Thunk allocation site and the cell
field's set-and-fire behaviour.  Triggered by the observation that
under `NIX_V3_STG_KEEP_HOOKS=1`, `(import <nixpkgs> {}).lib` still
hangs on a cycle that the cell-update was supposed to break — sub-
thunks are evidently still chasing the OUTER thunk's Black state
instead of seeing the OUTER's Evaluated value via a single-deref
Tag::Slot read.

## Allocator surface

`include/v3/alloc.hh:317-348` initialises `cell = nullptr` for both
`allocThunkSuspended` and `allocBridgeThunk`.  Bridge thunks never
set `cell` after allocation — they resolve through `bridgeSrc`, not
OP_RETURN — so they're irrelevant to STG-8 by construction.

The ONLY cell-write site is `vm.cc:5174-5180` in
`OP_ATTRS_REC_SET`: when the entry value `v` is a Suspended thunk
with `cell == nullptr`, the cell is set to
`&recAttrs.payload.bindings->entries[i].value`.

The ONLY cell-read site is `vm.cc:3003-3006` in
`OP_RETURN`'s `CFF_THUNK_RETURN` handler: read-and-clear, writes
`*cell = retVal`.

## Thunk allocation site table

| File:line | Path | Reason | Cell set after alloc? | Risk if missing |
|-----------|------|--------|----------------------|-----------------|
| `vm.cc:1549` | OP_MAKE_THUNK | per-attr LetRec entries, hidden-from-expr thunks, free-standing thunks | **NO at alloc** — set at `vm.cc:5174` *only when followed by* OP_ATTRS_REC_SET | Hidden-from-expr thunks (`emit.cc:594-601`) and `OP_MAKE_THUNK + OP_SET_LOCAL` slot writes get **NO cell**; OUTER-thunk OP_RETURN can't propagate the body result back to the slot the inner thunk captured by reference. |
| `vm.cc:5149-5181` | OP_ATTRS_REC_SET | rec-attrset entry SET | **YES** (the only write site) | — |
| `vm.cc:1988` | OP_CALL Bridge tw-out | TW callFunction returned an nThunk | **NO** (Bridge thunk: bridgeSrc-resolved, not OP_RETURN-resolved) | N/A — Bridge thunks don't run an OP_RETURN. |
| `vm.cc:6553` | callClosure Bridge tw-out | analogous to OP_CALL Bridge | **NO** | N/A |
| `primops.cc:4141` | treeWalkerToV3 nFunction | wrap TW lambda as v3 Bridge | **NO** | N/A |
| `primops.cc:4150` | treeWalkerToV3 nExternal | wrap TW external | **NO** | N/A |
| `primops.cc:4169` | treeWalkerToV3 nThunk | wrap unforced TW thunk | **NO** | N/A |
| `primops.cc:4281` | treeWalkerToV3 shallow nAttrs entry | per-entry lazy-bridge | **NO** | N/A |
| `primops.cc:7174` | tryDispatchBridge1Direct | wrap TW arg as v3 Bridge | **NO** | N/A |
| `primops.cc:7285` | tryDispatchFormalsLambdaBridge | wrap TW arg as v3 Bridge | **NO** | N/A |
| `v3_hook.cc:2599` | prepHookUpvaluesAndWiths Direct | TW env value -> v3 Bridge upvalue | **NO** | N/A (Bridge) but **see Risk #1** below |
| `v3_hook.cc:2674` | prepHookUpvaluesAndWiths RecBuild | per-name TW env -> v3 Bridge | **NO** | N/A (Bridge) — the Bindings is heap-allocated and cached per `(env, names)`; cell-update wouldn't help because the entries are Bridge thunks that resolve to TW. |
| `v3_hook.cc:2750` | prepHookUpvaluesAndWiths outerWith | TW with-env -> v3 Bridge | **NO** | N/A |
| `v3_hook.cc:3703` | v3CallFunctionEntry arg wrap | TW arg -> v3 Bridge | **NO** | N/A |

`Closure` allocations (`vm.cc:1344, 3419, 5387, 5548, 5665, 6337`)
have no cell field and aren't part of STG-8.

## Where cell SHOULD be set but isn't (likely STG-12 contributors)

### Risk #1 — Hidden-from-expr thunks (HIGH, most likely cycle source)

`emit.cc:594-601` emits one `OP_MAKE_THUNK` per from-expr followed
by `OP_SET_LOCAL hiddenSlot`.  These thunks are written into the
let-rec frame's value-stack slot; they are NEVER passed to
`OP_ATTRS_REC_SET`, so their `cell` stays nullptr.  Per-attr
thunks that capture the hidden slot via `emitVarRef` see a
Tag::Thunk by value — when the hidden thunk is forced and goes
Black, those captures hold a stale Thunk pointer whose state is
Black.  When the hidden thunk's body completes via OP_RETURN, the
fix is supposed to be the cell-update writing the Evaluated value
through the captured slot reference — but no cell was ever set.

The hidden-thunk path is exactly what `inherit (X // Y) ...` in
nixpkgs `lib/systems/elaborate.nix` (the #495 / #498 pattern) and
`lib/fixed-points.nix` `prev // overlay final prev` lower into.
Under STG_KEEP_HOOKS the eval hook re-enters mid-construction;
without cell-update it sees only the Black state.

**Suggested fix:** in `emit.cc`'s LetRec path emit, immediately
after `OP_MAKE_THUNK` for hidden-from-expr, insert an opcode that
writes the heap-stable cell-pointer into the just-allocated
Thunk.  Two options: (a) add a new opcode `OP_THUNK_SET_CELL_LOCAL
hiddenSlot` that pokes `&vm.valueStack[stackBase + slot]` into
`top.payload.thunk->cell`; (b) heap-allocate a stable Value cell
(like `OP_REC_SLOT_PUBLISH` does) and store the thunk through that
cell.  Option (b) is safer because value-stack addresses are not
heap-stable across `valueStack.resize()`.

### Risk #2 — Per-attr thunks before OP_ATTRS_REC_SET (LOW)

Between `OP_MAKE_THUNK en.thunkBody` (`emit.cc:641`) and
`OP_ATTRS_REC_SET` (`emit.cc:643`) the freshly-allocated Thunk
sits transiently on the operand stack with `cell == nullptr`.
This is harmless in tree-walker-style use because nothing observes
the thunk before the SET fires, but if any nested re-entry occurred
through a TW callback during that window (e.g. via a call hook
during emitVarRef-driven force on an upvalue), the captured
reference would still see a cellless thunk.  The current emit
order pushes upvalues BEFORE OP_MAKE_THUNK so this window is
empty in practice.

### Risk #3 — Lambda-skip-translated TW lambdas (HIGH for STG_KEEP_HOOKS)

`v3_hook.cc:3958-4036` (`tryDispatchTWLambdaInV3`) and
`v3_hook.cc:3058-...` (`v3CallFunctionEntry`) call
`prepHookUpvaluesAndWiths` to materialise upvalues from a TW env.
When a freeVar resolves as `RecBuild`, `v3_hook.cc:2674` builds
per-name Bridge thunks pointing into the TW env.  These bridge
thunks have `cell == nullptr`.  Under STG_KEEP_HOOKS the v3 body
running on these upvalues never benefits from cell-update because
the bridge resolves through `bridgeSrc` (`forceBridgeThunk` in
vm.cc), but **the v3-side rec-attrset the body BUILDS** does pass
through OP_ATTRS_REC_SET — that part is fine.

The actual problem here is NOT a missing cell; it's that when the
v3 body's Suspended thunks are written into the rec-Bindings AND
those bindings are returned through the call back into TW, TW's
callers are still seeing the v3 Bindings entries by Tag::Slot
into `&entries[i].value` — and IF the OUTER thunk is itself the
ENCLOSING let-rec attrset that re-enters via `_internalCallByName`
or a fix-point overlay, the OUTER thunk's CFF_THUNK_RETURN never
gets the cell because the OUTER thunk was created via
`OP_MAKE_THUNK + OP_SET_LOCAL` (Risk #1 case) rather than
`OP_MAKE_THUNK + OP_ATTRS_REC_SET`.

So Risk #3 is really a corollary of Risk #1.

### Risk #4 — OP_REC_SLOT_PUBLISH heap slot (NO ACTION)

`vm.cc:4386-4390` allocates a heap Value, copies the Tag::Attrs
into it, and pushes a Tag::Slot pointing at it.  No Thunk is
allocated, so cell is N/A.  Callers reach the rec-Bindings via
the Slot deref, which already gives them the OP_ATTRS_REC_SET
mutations to entries[].

### Risk #5 — primops.cc treeWalkerToV3 entries are Bridges (NO ACTION)

All primops.cc allocBridgeThunk sites are bridge-to-TW. They
resolve via bridgeSrc through `forceBridgeThunk`, which uses
the address-stable TW Value pointer — TW's own in-place update
is the equivalent of cell-update on the TW side.  No v3 cell
needed.

## STG-12 (hello.name, lib) most-likely cycle path

Combining the risk analysis:

1.  Top-level `(import <nixpkgs> {}).lib` lowers `lib =
    fix (self: { ... })` (or similar) into a hidden-thunk
    pattern (Risk #1 territory: the synthesised hidden-from-expr
    thunks for `inherit (X // Y) ...` patterns).
2.  Under STG_KEEP_HOOKS the eval hook re-enters via TW.  v3
    creates the hidden Suspended thunk, transitions it Black,
    and starts running the body.
3.  The body re-references the same hidden thunk through a
    captured upvalue-slot (or via `self.X` → recovery from
    partialBindingsRegistry, which is **disabled under
    NIX_V3_STG=1** at `vm.cc:856`).
4.  Without partialBindings recovery AND without cell-update on
    the hidden thunk, the captured reference still points at a
    Black thunk; OP_FORCE on it throws BlackholeError → the
    let-rec retries → cycle.

## Recommendations (no code change in this audit)

1.  Add cell-set for hidden-from-expr thunks at emit time
    (`emit.cc:594-601`).  Allocate a heap-stable Value cell for
    each hidden-thunk slot and have OP_MAKE_THUNK + a new
    OP_SET_LOCAL_THROUGH_CELL store both the cell-ref and the
    thunk's `cell` pointer.

2.  Audit any non-LetRec `OP_MAKE_THUNK` emit site (currently
    none -- but if `ir::Let`/`ir::ListBuilder`/etc. ever start
    materialising lazy thunks, they need the same treatment).

3.  Document in `closure.hh:114-129` the invariant that **every
    Suspended thunk that may be observed via a captured slot
    MUST have its `cell` set before any other thread of control
    can observe it** — that's the missing invariant today.

4.  Consider folding the `partialBindingsRegistry` (which is
    disabled under STG) into a per-thunk `cell`-style mechanism:
    when a Black thunk is re-entered mid-construction, the
    cell could hold the partial Bindings and a sub-flag could
    indicate "in-progress, cell holds partial".  This would
    eliminate the side-table entirely while preserving the
    legitimate `rec { x = 1; y = self.x; }` use case.

## File index

- Cell field declaration: `src/libexpr-v3/include/v3/closure.hh:114-129`
- Allocator initialisation: `src/libexpr-v3/include/v3/alloc.hh:317-348`
- Cell write (only): `src/libexpr-v3/vm.cc:5148-5182` (OP_ATTRS_REC_SET)
- Cell read+clear (only): `src/libexpr-v3/vm.cc:2980-3006` (CFF_THUNK_RETURN)
- LetRec emit (hidden + per-attr thunks): `src/libexpr-v3/emit.cc:524-651`
- prepHookUpvaluesAndWiths: `src/libexpr-v3/v3_hook.cc:2527-2763`
- tryDispatchTWLambdaInV3: `src/libexpr-v3/v3_hook.cc:3956-4050`
- partialBindingsRegistry (legacy publish; disabled in STG): `src/libexpr-v3/vm.cc:807-932`
