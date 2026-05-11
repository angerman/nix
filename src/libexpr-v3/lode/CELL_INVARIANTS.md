# Cell ownership invariants (v3 STG-style cell mechanism)

**Date:** 2026-05-11
**Status:** Audit + invariants list; instrumentation pending

---

## Background

v3 thunks (`include/v3/closure.hh:Thunk`) carry an optional `Value * cell`
field.  When a thunk's body completes via `OP_RETURN` (CFF_THUNK_RETURN
frame), the handler at `vm.cc:3594-3597` writes the body's `retVal` to
`*cell` and clears `t->cell = nullptr`:

```cpp
if (Value * cell = fr.thunk->cell) {
    *cell = retVal;
    fr.thunk->cell = nullptr;
}
```

The cell is a heap-stable update target — typically an entry in a
`Bindings` slot — that lets multiple consumers observe the thunk's final
WHNF *without* walking the thunk chain.  It is v3's approximation of
STG's `Ind` (indirection) closure.

`shapeCell` is a related field: holds a sentinel `Tag::Thunk{t}` until
the body partial-publishes (via `OP_ATTRS_REC_INIT_TAIL`).  Used only
when `NIX_V3_CELL_EVERYWHERE=1`.  Default is opt-in / off.

This doc audits `cell` and `shapeCell` ownership and lists the
invariants that must hold for STG correctness.

## Setter sites (`->cell = <storage>`)

There are exactly **three** places that point a thunk's `cell` at heap
storage.  Each currently guards with `cell == nullptr` to prevent
double-setting on the same thunk.

### S1 — OP_THUNK_SET_LOCAL_THROUGH_CELL (`vm.cc:5666-5678`)

```cpp
Value * cell = Alloc::allocValue();
*cell = top;
if (top.payload.thunk->state == ThunkState::Suspended
    && top.payload.thunk->cell == nullptr) {
    top.payload.thunk->cell = cell;
}
```

**Storage:** freshly allocated via `Alloc::allocValue()` — guaranteed
unique (never aliases existing storage).
**Risk of aliasing:** ZERO. Each call allocates fresh storage.

### S2 — OP_ATTRS_REC_SET (`vm.cc:6732-6739`)

```cpp
if (v.isThunk() && v.payload.thunk
    && v.payload.thunk->state == ThunkState::Suspended
    && v.payload.thunk->cell == nullptr)
{
    v.payload.thunk->cell =
        &recAttrs.payload.bindings->entries[i].value;
}
```

**Storage:** `&bindings->entries[i].value` — a Bindings entry's value
slot.
**Risk of aliasing:** Potential. The same Bindings entry storage could
be targeted by another setter (S3 below) targeting the same Bindings.
The check `cell == nullptr` only prevents the SAME thunk being set
twice — NOT two different thunks pointing at the SAME storage.

### S3 — v3_hook.cc Bridge materialization (`v3_hook.cc:2749-2751`)

```cpp
if (bridge && bridge->cell == nullptr) {
    bridge->cell = &b->entries[i].value;
}
```

**Storage:** `&b->entries[i].value` — same shape as S2, but for a
Bindings being built by TW→v3 materialization (rec-build-cache path).
**Risk of aliasing:** Potential. Same shape as S2.

## Writer sites (`*cell = ...; t->cell = nullptr`)

There are **three** writers, each correctly clears the cell after use.

### W1 — OP_RETURN with CFF_THUNK_RETURN (`vm.cc:3585-3588, 3595-3596`)

The canonical writer.  Fires when a Suspended thunk's body completes.

```cpp
if (Value * cell = fr.thunk->cell) {
    *cell = retVal;
    fr.thunk->cell = nullptr;
}
```

### W2 — OP_FORCE on Bridge thunk (`vm.cc:4029-4032`)

When a Bridge thunk resolves (via `forceBridgeThunk`), it writes the
resolved TW value into its cell.

```cpp
if (Value * cell = t->cell) {
    *cell = resolved;
    t->cell = nullptr;
}
```

### W3 — public forceValue on Bridge thunk (`vm.cc:7978-7981`)

Mirror of W2 for the public forceValue entry.

## The aliasing risk

The single-owner guard at S2 / S3 (`cell == nullptr`) protects against
double-setting the *same thunk*.  It does NOT protect against:

**Multi-cell aliasing:** Two different thunks `t1`, `t2`, each with their own
`cell`, pointing at the **same storage address** `S`.  When `t1`'s body
returns, `*S = t1_retVal`.  When `t2`'s body returns, `*S = t2_retVal` —
overwriting `t1`'s update.

When can this happen?

- S2 wires `t->cell` to `&bindings->entries[i].value`.  The `bindings`
  is the `recAttrs.payload.bindings` at the top of stack when REC_SET
  fires.
- If two REC_SETs target the SAME `bindings` and SAME `i`, the second
  one's check `cell == nullptr` already protects against re-setting
  (because the entry's slot already has the first thunk, whose cell
  was just set).
- BUT: if the value at `bindings->entries[i]` is REPLACED (e.g., by
  `OP_APPLY_OVERRIDES`, which mutates the entry directly), the original
  thunk's cell still points at `&entries[i].value`.  Subsequent
  REC_SETs on a DIFFERENT bindings could write to a thunk whose cell
  happens to point at the SAME memory address (if Boehm/arena reuses
  the slot).

### Specifically dangerous shape — Bindings copy/share

If a Bindings B1 has entries[i] = T1 (thunk with cell = &B1.entries[i].value),
and code later constructs B2 by COPYING entries from B1 (e.g., via
`OP_ATTRS_UPDATE`'s `//` merge), B2.entries[i] = T1 (the same thunk
pointer).  T1's cell still points at &B1.entries[i].value.

When T1's body returns:
- *cell = retVal  → writes to B1's entry
- but T1 is REACHABLE via B2.entries[i]

Consumers reading B2.entries[i] see the thunk's `evaluated` cache (via
forceValue chase), which IS correct.  But consumers reading B1.entries[i]
also see the thunk's evaluated.  So far, both see the same value via
the thunk's own `evaluated` slot — no aliasing observed.

**The bug appears when a DIFFERENT thunk T2 also has cell = &B1.entries[i].value.**
For example, if S3 in the TW bridge materialises a NEW Bindings B3 whose
`cpu` entry uses the same heap-stable storage that S2 wired T1's cell
to.  Then T1 and T2 both believe they own &B1.entries[i].value.

## Invariants we want

For STG correctness:

**I-CELL-1 (Single ownership):** No two distinct thunks `t1`, `t2`
shall have `t1->cell == t2->cell != nullptr`.

**I-CELL-2 (Write-once):** Each non-nullptr cell is written via `*cell =
retVal` exactly once during its lifetime, then cleared (`t->cell =
nullptr`).

**I-CELL-3 (Storage-stability):** A thunk's cell pointer must remain
valid until the thunk completes its update.  In particular, the
underlying Bindings must not be deallocated or relocated.  Currently
satisfied by Bindings being arena-allocated (Boehm tenured).

**I-CELL-4 (No mid-eval rewiring):** Once `t->cell` is set, it is NOT
changed except via the writer paths (which clear to nullptr).  No
re-pointing of an existing cell.

**I-CELL-5 (Owner-only update):** The thunk whose cell points at
storage `S` is the ONLY entity that writes to `*S` via the cell-update
mechanism.  Other writes to `S` (e.g., OP_ATTRS_REC_SET filling the
entry initially, OP_APPLY_OVERRIDES mutating in place) are not via
`*cell` and are out-of-band.

## How to instrument

Add a thread-local hash map `cellOwnership : Value* → Thunk*` that:
- Records `cellOwnership[storage] = t` at every S1/S2/S3 setter.
- At each setter, if `cellOwnership[storage]` is already set to a
  DIFFERENT thunk, **assert / log** the invariant violation (I-CELL-1).
- At each W1/W2/W3 writer, verify `cellOwnership[t->cell] == t` (or
  `nullptr` if already cleared).  Clear `cellOwnership[t->cell]`.

Gate behind `NIX_V3_DBG_CELL_OWN=1`.  Zero hot-path cost when unset.

## Connection to `{family}` failure

Hypothesis P3 from `RCA_FAMILY_DIVERGENCE_DEEPER_2026-05-11.md`:
the cpuName slot in `tripleFromSystem`'s let-block is being written via
a cell-update from a thunk OTHER than the cpuName thunk itself.

If I-CELL-1 fires during the failing eval, this hypothesis is confirmed.
If it doesn't fire, P3 is refuted and we need to reopen the search
(e.g., look at the value-stack trajectory inside the cpuName thunk's
body more carefully, or audit OP_REC_BINDING_SLOT_REF / forceValue
return paths).

## Run result (2026-05-11)

`NIX_V3_DBG_CELL_OWN=1` on the failing `(import <nixpkgs> {})` workload
produced **ZERO cell-ownership violations**.  Phase A3's P3 hypothesis
is **REFUTED**: no two thunks share a cell pointer, and no cell write
fires from a thunk that isn't the recorded owner.

The bug is NOT cell aliasing.  Search reopens — see the follow-up doc
for the next set of hypotheses.

## Permanent invariants enforced

The tracker stays in the codebase as a permanent invariant check
(off by default, enabled via env var).  Future regressions involving
cell aliasing will fire visibly via I-CELL-1 / I-CELL-2 violations.
This is the load-bearing piece for catching this class of bug going
forward — exactly the structural infrastructure HONEST_ASSESSMENT.md
called out as missing.

---

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
