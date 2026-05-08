## Slot mechanism audit under STG_KEEP_HOOKS hook re-entry — 2026-05-08

Audit date: 2026-05-08.  Prereqs: STG-1..3 removed publish/recovery,
STG-7 introduced `Tag::Slot` Values and STG-8 cell-update at
`OP_RETURN` (vm.cc:3003).  Bisect: cycle on nixpkgs hello.name needs
both `NIX_V3_STG=1` AND `NIX_V3_STG_KEEP_HOOKS=1`; either alone passes.
This memo enumerates the gaps the hook-active configuration exposes.

### Re-verification of the existing coverage matrix (read-only)

All sixteen rows in `SLOT_TAGGING_AUDIT_2026-05-07.md` are still
correct in isolation (single-VMState).  Spot-checked sites:

| Path | File:line | Note |
|---|---|---|
| `OP_REC_SLOT_PUBLISH` | vm.cc:4359-4391 | allocs heap-stable cell; pushes `Tag::Slot` on top of stack. |
| `OP_REC_BINDING_SLOT_REF` | vm.cc:4393-4550 | binary-search → `mkSlot(&entries[i].value)`. |
| `OP_MAKE_THUNK` upvalue capture | vm.cc:1546-1557 | FAM tail copy; tag preserved. |
| `OP_MAKE_CLOSURE` upvalue capture | vm.cc:1341-1349 | mirrors thunk capture. |
| `snapshotCurrentWiths` / `pushCapturedWiths` | vm.cc:721-742 | bulk copy; tag preserved. |
| `forceValue` `Tag::Slot` chase + memo | vm.cc:5986-5996, 6428-6429 | `*memoSlot = v` writes WHNF back into slot storage. |
| `OP_GET_LOCAL` / `OP_GET_UPVALUE` (and `_FORCE`) | vm.cc:1076-1331 | direct reads, force chases through Slot. |
| `OP_CALL` simple-arg / `OP_TAIL_CALL` | vm.cc:2496-2498 | `valueStack[newBase + 0] = arg`, no force. |
| `OP_CALL` formals lambda | vm.cc:2406-2483 | `forceValue(arg)` is called — slot identity LOST at this site. |
| `callClosure` simple arg | vm.cc:6433-6700 | passes arg through; preserves Slot. |
| `OP_ATTRS_REC_SET` cell wire-up | vm.cc:5174-5180 | `v.payload.thunk->cell = &entries[i].value` — only when entry is a fresh Suspended thunk. |

Cell-update / slot-deref together implement the publish equivalence
in single-VMState mode.  The remaining gaps fall into three buckets:
**bridge boundary**, **cell-update preconditions**, and **invariant
loss**.  All three become observable only when the call/eval hooks
fire, because hook fire is what creates the cross-VMState topology.

---

### Gap 1 — `v3CallFunctionEntry` arg bridging drops slot identity

File: `src/libexpr-v3/v3_hook.cc:3676-3707` (after `if (!arg) return false;`).

Failure mode: TW reaches `EvalState::callFunction(fun, arg, ...)` with
`arg = nix::Value *`.  `arg` is a TW value — by construction it cannot
carry a v3 `Tag::Slot`.  The hook wraps it as a Bridge thunk
(`allocBridgeThunk(arg)`) regardless of whether `*arg` is a TW thunk
that internally references a v3 cell.  Cell-published v3 Values that
TW imported via `v3ToTreeWalker` have already lost slot identity at
that earlier bridge step (Gap 4 below); on the way back into v3 the
Bridge thunk wraps the TW Value, so a v3 body's `OP_GET_LOCAL 0` reads
a Tag::Thunk Bridge — not the original Tag::Slot.

This is fundamental to the hook's contract (the tree-walker is the
caller; it has no Tag::Slot to hand us), but it means **any rec-attrset
cell threaded through a TW lambda call boundary becomes ineligible for
cell-update / late-publish**.  The call-hook body that captures the arg
in a sub-thunk captures a Bridge whose `evaluated` is a SNAPSHOT once
forced, with no slot back-edge.

### Gap 2 — `prepHookUpvaluesAndWiths` builds RecBuildSlot pointing at a fresh v3 Bindings disjoint from any outer v3 frame's slot

File: `src/libexpr-v3/v3_hook.cc:2696-2717`.

Failure mode: Kind::RecBuildSlot synthesises a fresh `Bindings*`,
fills its entries with **Bridge thunks** wrapping each TW env value,
allocates a fresh `heapSlot = allocValue(); *heapSlot = Tag::Attrs(b)`,
and pushes Tag::Slot pointing at heapSlot.

The body's `OP_GET_UPVALUE_FORCE` deref of this Slot returns the v3
Bindings — but the entries are Bridge thunks, NOT Tag::Slot values
into the rec-attrset's eventual cells.  `OP_REC_BINDING_SLOT_REF`
elsewhere in the body would yield a Tag::Slot to `&entries[i].value`,
which is one of the Bridge thunks; forcing it returns a snapshot of
the TW value at that moment.  No backward path exists from v3 cell
update to the TW-side cell.

Equivalence to the publish path that this replaces: the legacy publish
side-table associated a publishing Tag::Thunk with the Bindings being
built INCREMENTALLY, so an outer v3 frame's `OP_ATTRS_REC_SET` was
visible to inner sub-thunks.  Under STG-8, the cell-update at
OP_RETURN delivers that visibility — but the materialisation site
above creates a Bindings the OUTER v3 frame does not own; the outer
frame has its own RecBuildSlot for the SAME Bindings name(s), pointing
at a DIFFERENT heap location.  Cell update on one is invisible to the
other.  This is the per-VMState-Bindings divergence noted in `#466
rec-attrset capture refusal` (v3_hook.cc:3584-3598), which currently
mitigates by refusing.

### Gap 3 — Cell-update precondition fails for re-entered RecBuildSlot

File: `src/libexpr-v3/vm.cc:5174-5180` (`OP_ATTRS_REC_SET` cell wire-up).

The cell is set ONLY when the entry's value is a fresh Suspended thunk.
Two re-entry paths violate this:

(a) When `prepHookUpvaluesAndWiths` materialises a RecBuildSlot for a
TW env that has ALREADY entered evaluation, the entries are Bridge
thunks whose state is Bridge — not Suspended.  The conditional at
vm.cc:5175 (`state == ThunkState::Suspended`) skips them.  Sub-thunks
captured-with this Bindings later read whatever the Bridge resolved
to; once resolved, the Bridge becomes Evaluated and the cell is never
set.  No cell-update, no late publish.

(b) Within a single v3 LetRec emit, if an entry is itself an already-
forced Value (e.g., from a literal), `v.isThunk()` is false and cell
is not assigned.  Inner sub-thunks reading via OP_REC_BINDING_SLOT_REF
correctly see the literal — fine.  But if a sub-thunk on the body of
that entry forces a different rec entry that's still a Suspended
thunk owned by the OUTER frame, AND the outer thunk is Black on a
foreign vm (hook re-entry), the cell-update at THAT thunk's OP_RETURN
fires on its own VMState, while the consumer reads through Tag::Slot
from a DIFFERENT VMState.  Read-through-slot returns the stale value
(or worse, the Black thunk itself if the cell hasn't been written
yet).

### Gap 4 — `v3ToTreeWalker(Tag::Slot)` deep-forces and snapshots

File: `src/libexpr-v3/primops.cc:3589-3635, 3754-` (`Tag::Attrs` and
fall-through cases) and `primops.cc:4018` (default branch handles
`Tag::Slot` as `mkNull`).

Failure mode: the `Tag::Slot` case is in the default fall-through and
returns `mkNull()`.  More importantly, the function's prelude calls
`forceValue(*state.vm, v)` (line 3616) which CHASES Slot to WHNF and
memo-writes WHNF back into the slot.  Any consumer that reads the
Tag::Slot AFTER this bridge sees the resolved WHNF, not the slot
indirection.  This is necessary for TW correctness (TW has no Slot)
but means **a v3 closure result returned across the bridge has all
its sub-Slot upvalues collapsed to WHNF at bridge time**, killing any
late-publish potential.

When the closure is then bridged back via `__v3_call_bridge_1`, its
captured upvalues remain v3 Tag::Slot internally (the closure is a v3
Closure, just wrapped as a TW PrimOpApp), so this gap mainly fires
when v3 returns a Tag::Attrs / Tag::List that traversed Slot chains.

### Gap 5 — `tryDispatchTWLambdaInV3` keeps v3Arg slot but TW upvalues are Bridge-snapshots

File: `src/libexpr-v3/v3_hook.cc:3956-4037`.

This shortcut (STG-14a, default-on under STG_KEEP_HOOKS) preserves the
v3 Tag::Slot arg unchanged into the body — that part is correct and
is the explicit fix for STG-12.

But upvalues are still materialised via `prepHookUpvaluesAndWiths(*envPtr, ent, ..., envBaseLevel=1)`
from the **TW env**.  Same as Gap 2: any RecBuildSlot upvalue points
at a fresh v3 Bindings constructed for THIS call only, disjoint from
whatever outer-frame state the TW caller is mid-building.  The arg is
slot-identity-preserved but the upvalues that the body ALSO needs to
share a slot with are not.

A body that uses (a) the slot-identity-preserved arg and (b) a
RecBuildSlot upvalue for what should be the SAME rec-attrset will see
two divergent views.

### Gap 6 — `forceBridgeThunk` snapshots the bridge result into `t->evaluated`

File: `src/libexpr-v3/vm.cc:6307-6309`, helper `primops.cc:7374-7445`.

After the first force, the Bridge's `state = Evaluated` and
`evaluated = v` (a snapshot of what `treeWalkerToV3Public` returned at
that moment).  Subsequent forces of the SAME Bridge thunk return the
snapshot.  The slot cells inside whatever TW value the Bridge wrapped
are never re-read.  Under hook re-entry this means: TW's cell
mutation (its own in-place `forceValue` updating a Bindings entry from
thunk to attrset) is visible to a v3 Bridge force ONLY on the FIRST
force; later forces in the same v3 evaluation return the stale
snapshot.  The opposite direction — v3 cell-update visible to TW — is
even more lossy because TW Value's are written through `*outTwHeap = ...`
in OP_CALL Bridge handler (vm.cc:1984-1985), not via Slot indirection.

### Gap 7 — `OP_CALL` formals-lambda forces the arg pre-call, dropping arg slot identity

File: `src/libexpr-v3/vm.cc:2406-2483`.

Already noted in 2026-05-07 audit as "(forced) — acceptable: a formals
attrset isn't a slot use case".  Re-flagging because under
STG_KEEP_HOOKS the call hook's arg materialisation often pushes a
formals lambda whose body USES the slot via `with arg;`.  The eager
force at line 2465 chases through any Slot, memos-writes WHNF, and
hands a non-Slot Value to `valueStack[newBase + 0] = arg` (line 2498).
The body's `OP_WITH_PUSH 0` then puts a non-Slot into the with-stack.
A sub-thunk captured under that `with` sees a snapshot.  Cell update
on the original cell is invisible.

### Gap 8 — Invariant loss: "every Black v3 thunk on the frame stack has a slot pointing at its eventual evaluated Value"

This is the publish-era invariant explicitly stated in
SLOT_TAGGING_AUDIT_2026-05-07.md §"why this matters".  Under publish,
every Black thunk announced its destination cell via the side-table
so foreign-vm forces could recover.  Under STG-8, the same information
is encoded in `t->cell` — but only for thunks that were the value
parameter of `OP_ATTRS_REC_SET` AND that were Suspended at SET time.

The invariant DOES hold for:
- LetRec emit's per-attr Suspended thunks (the common case).

It DOES NOT hold for:
- Call-hook-materialised RecBuild/RecBuildSlot Bindings (Gap 2-3).
- Bridge thunks of any kind (`t->state == Bridge`).
- Closure-result thunks (no `OP_ATTRS_REC_SET` ever ran).
- Thunks force-published by `forceValue`'s `*memoSlot = v` write
  (vm.cc:6428) — the slot gets mutated to WHNF, but no `cell`
  back-pointer was ever set on the underlying Thunk so cross-VMState
  observers searching by `t->cell` find nothing.

The publish side-table did not depend on these distinctions; cell-
update does.

---

### Prioritised likelihood: which gap is the STG_KEEP_HOOKS cycle source?

**Most likely (P1): Gap 2 + Gap 3 together** — `prepHookUpvaluesAndWiths`'s
RecBuildSlot synthesises a parallel-but-divergent Bindings whose entries
never get cell-update.  When the hook fires on a lambda body that holds
a fix-point self-reference, the body's `with self;` lookups read through
the disjoint Bindings; sub-thunks captured under that with-stack see
TW Bridge thunks that resolve to the OLD value (pre-publish), not the
in-progress fix-point.  This was previously masked by the publish
side-table writing a recoverable partial Bindings keyed by Black
Thunk*; STG-3 removed that recovery and there is no v3-side equivalent
for the externally-materialised Bindings.  This matches the
`#466 rec-attrset capture refusal` (v3_hook.cc:3584-3598) which
currently mitigates by refusing — but the mitigation is OFF by default
under STG_KEEP_HOOKS because the env-var gate is `s_refuseRecCapture =
std::getenv("NIX_V3_NO_REFUSE_REC_CAPTURE_LAMBDA") == nullptr`, i.e.,
default-on for a refusal-style mitigation, but the call hook itself is
also gated off by the `!STG_KEEP_HOOKS` check at
v3CallFunctionEntry:3084-3086 — so under STG_KEEP_HOOKS the rec-capture
refusal still applies, EXCEPT for the bridge1 / TW-lambda shortcuts at
v3_hook.cc:3126-3144 which bypass this refusal entirely.  These
shortcuts expose Gap 2/3 directly.

**Likely (P2): Gap 1 + Gap 7** — the formals-lambda path's arg force.
nixpkgs hello.name's pkg-set construction puts every callPackage
through a formals lambda; the eager force collapses any rec-attrset
slot the caller threaded through.  Combined with cell-update's
reliance on Suspended-thunk shape, the next force sees a stale
snapshot.

**Less likely (P3): Gap 5** — only fires when the TW-lambda-in-v3
shortcut applies; recently-added (#509/#515) and gated.  Worth
checking whether disabling `NIX_V3_NO_TW_LAMBDA_INV3=1` removes the
cycle.

**Least likely (P4): Gap 4 + Gap 6** — these affect what TW sees from
v3, not v3 internal cell update.  The cycle fails on a v3-side force
chain, so the TW-bound direction is downstream of the actual bug.

---

### Suggested next steps (NOT implemented; for the implementer)

1. Run with `NIX_V3_NO_REFUSE_REC_CAPTURE_LAMBDA=1` toggled OFF (keep
   default refusal) and `NIX_V3_NO_TW_LAMBDA_INV3=1` to disable the
   STG-14a shortcut.  If the cycle disappears, Gap 5 / shortcut path
   is the trigger.

2. Run with `V3_DBG_OPCALL_FORCE=1` + `STG_KEEP_HOOKS=1` to confirm
   the formals-lambda eager-force fires on a chase that hits a Black
   thunk pre-cycle (Gap 7 verification).

3. Look at whether `prepHookUpvaluesAndWiths` could MEMOISE the
   constructed RecBuildSlot Bindings keyed by `(env, names)` AND
   register cells on each Bridge entry pointing at the eventual TW
   Value, so cell-update on TW side propagates through.  Cache hit
   exists (`recBuildCache` in v3_hook.cc:2616-2625) but slot/cell
   wiring is absent.

4. Consider whether `v3CallFunctionEntry`'s arg bridge should — when
   `arg` already wraps a Bridge that wraps a TW Value pointing at
   what was originally a v3 cell — short-circuit the wrap and pass
   the original v3 Tag::Slot through.  Requires `bridgeSrc` to track
   provenance.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
