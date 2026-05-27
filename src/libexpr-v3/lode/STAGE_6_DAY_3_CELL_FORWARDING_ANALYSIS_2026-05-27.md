# Stage 6 Day 3 — cell-forwarding analysis

**Date**: 2026-05-27 evening
**Status**: Day-3 planning artifact.  Documents the analytical work
to resolve the Day-2.2 KNOWN LIMITATION (Thunk::cell + shapeCell +
Tag::Slot referring to addresses inside active_ that move during
major scavenge).
**Owner**: next focused session executing Day 3 per
`STAGE_6_IMPLEMENTATION_GUIDE_2026-05-27.md`.

## The problem

When `MajorScavenger::fwdBindings` copies a Bindings from active_ to
backup_, the new Bindings has the SAME LAYOUT as the old.  But:

* `Thunk::cell` is a `Value *` that points at `&oldBindings->entries[i].value`.
* `Thunk::shapeCell` is a `Value *` that points at `&oldBindings->entries[j].value` OR at a standalone `allocValue()` cell.
* `Tag::Slot` Values have `payload.slot` = `Value *` pointing at a cell (same two cases).

After `swapRegions()` + `freeBackupBlocks()` (which frees the OLD
active blocks), all of these stale pointers dangle.  The next deref
SIGSEGVs.

## Cell address categories

A `Value *` cell pointer can refer to:

| Category | Location | Day-3 strategy |
|----------|----------|----------------|
| Bindings-resident | `&owningB->entries[i].value` for some Bindings `owningB` in active_ | **Offset-forward**: compute `offset = oldCellPtr - oldB`, then `newCellPtr = newB + offset` after `newB = fwdBindings(oldB)` |
| Standalone arena | Direct `allocValue()` result, separately tracked in `standaloneCellRoots()` | **Move**: copy to backup_, register old → new in forwarding table, update `standaloneCellRoots()` entry |
| Closure/Thunk-internal | `&someClosure->upvalues[i]` or `&someThunk->tail[i]` | **Offset-forward**: same pattern as Bindings-resident |
| External | Stack, libc, Boehm — not in arena | **Skip** (regionOf returns External) |

## Solution architecture

### Step 1 — standalone cell forwarding

Add to MajorScavenger:

```cpp
class MajorScavenger {
    // ...
    /// Standalone-cell forwarding: oldCellPtr → newCellPtr.
    /// Separate from forwarding_ (which keys on Closure/Thunk/
    /// Bindings/List/Pair pointers).  Walked separately because
    /// the per-cell size is fixed sizeof(Value) and the type
    /// dispatch is different.
    std::unordered_map<Value *, Value *> cellForwarding_;
};
```

Add to `runMajorScavenge` BEFORE `walkAllV3Roots`:

```cpp
// Move standalone cells first so subsequent slot lookups find them.
auto & roots = standaloneCellRoots();
for (size_t i = 0; i < roots.size(); ++i) {
    Value * oldCell = roots[i];
    if (!oldCell || !arena.inActive(oldCell)) continue;
    Value * newCell = static_cast<Value *>(
        arena.allocInBackup(sizeof(Value)));
    *newCell = *oldCell;
    cellForwarding_[oldCell] = newCell;
    roots[i] = newCell;
    // Recursive visit happens via worklist drain below.
    // We don't add a worklist entry here because cells aren't
    // typed objects; we walk via the payload of the moved
    // cell in step 4.
}
```

After standalone-cell move, drain pass also revisits each forwarded
cell's payload via `mv.visitValue(*newCell)` to forward any nested
arena pointers in its payload.

### Step 2 — Tag::Slot forwarding

Update `visitSlot`:

```cpp
void MajorScavenger::visitSlot(Value * & slot)
{
    if (!slot) return;

    // Case 1: standalone cell (in cellForwarding_).
    auto it = cellForwarding_.find(slot);
    if (it != cellForwarding_.end()) {
        slot = it->second;
        // Already walked by the standalone-cell pass; no
        // worklist entry needed.
        return;
    }

    // Case 2: Bindings-resident cell — slot points INSIDE an
    // active_ Bindings.  We need to find which Bindings and
    // forward via offset arithmetic.
    if (arena_.inActive(slot)) {
        // Search active blocks for the owning Bindings.  Linear
        // walk via blockRanges + a per-cell scan would be O(N²);
        // instead, defer to forwarding lookup AFTER the Bindings
        // is copied.  Strategy:
        //   - Record this slot in a pending-list
        //   - After drain, walk pending-list and resolve via
        //     forwarding[bindings] + offset
        pendingSlots_.push_back(&slot);
        return;
    }

    // Case 3: external — skip.
    // Case 4: backup — already forwarded; deref-walk via visitValue.
    if (cellsFollowed_.insert(slot).second) {
        ++stats_.slotsFollowed;
        visitValue(*slot);
    }
}
```

### Step 3 — pending-slot post-drain resolution

After `drain()`, walk `pendingSlots_` and resolve each:

```cpp
void MajorScavenger::resolvePendingSlots() noexcept
{
    for (Value ** slotPtr : pendingSlots_) {
        Value * oldCell = *slotPtr;
        if (!oldCell) continue;
        // Find which Bindings owns this cell.  Linear walk of
        // forwarding_ entries — O(forwarded_bindings_count).
        // For each forwarded Bindings, check if oldCell falls
        // within its entries[] array.
        for (auto & [oldP, newP] : forwarding_) {
            Bindings * oldB = static_cast<Bindings *>(oldP);
            // Discriminate Bindings from Closure/Thunk/etc:
            // need a typed forwarding table.  See Step 4.
            // ...
        }
    }
}
```

This raises an issue: `forwarding_` is type-erased
(`unordered_map<void*, void*>`).  Distinguishing Bindings from
Closure forwardings would require a separate typed map.

### Step 4 — typed forwarding tables

Refactor `forwarding_` into per-type maps:

```cpp
std::unordered_map<Closure *,   Closure *>   forwardingClosure_;
std::unordered_map<Thunk *,     Thunk *>     forwardingThunk_;
std::unordered_map<Bindings *,  Bindings *>  forwardingBindings_;
std::unordered_map<ListVec *,   ListVec *>   forwardingList_;
std::unordered_map<ValuePair *, ValuePair *> forwardingPair_;
std::unordered_map<Value *,     Value *>     forwardingCell_;
```

Each fwd* uses its typed map.  Step-3 pending-slot resolution
walks `forwardingBindings_` specifically.

### Step 5 — Thunk::cell + shapeCell forwarding

In `walkThunk`, after handling state-specific fields:

```cpp
if (t->cell) {
    if (t->cellContainer) {
        // Bindings-resident cell — offset-forward via cellContainer
        Bindings * oldB = t->cellContainer;
        auto it = forwardingBindings_.find(oldB);
        if (it != forwardingBindings_.end()) {
            Bindings * newB = it->second;
            ptrdiff_t offset = (char *)t->cell - (char *)oldB;
            t->cell = (Value *)((char *)newB + offset);
            t->cellContainer = newB;
        }
    } else {
        // Standalone cell — direct forwarding lookup
        auto it = forwardingCell_.find(t->cell);
        if (it != forwardingCell_.end())
            t->cell = it->second;
    }
}

// Same for t->shapeCell (NIX_V3_CELL_EVERYWHERE path).
```

### Step 6 — Closure/Thunk-internal cell pointers (rare)

`Closure::upvalues[i]` and `Thunk::tail[i]` are Values, not Value*s.
Tag::Slot may have `payload.slot` pointing AT another Closure's
upvalues (let-rec) — that's Bindings-style offset forwarding
against the closure's address.  Similar pattern to Step 5; needs
separate handling per pointer-source type.

For Day 3 MVP: focus on Bindings-resident + standalone cells (the
common case).  Closure/Thunk-internal slot pointers are rarer
(let-rec captures); flag as Day-4 follow-up.

## Day 3 implementation order

1. **Refactor forwarding into typed maps** (~30 LoC change in
   move_gc.cc) — Step 4.  Validate --quick still PASSes (no
   behaviour change; still caller-invoked only).
2. **Standalone cell forwarding** (Step 1) — `walkStandaloneCells`
   helper invoked from `runMajorScavenge` before `walkAllV3Roots`.
   Validate with smoke test (manually invoke; verify forwarding map
   populated).
3. **Tag::Slot forwarding** (Step 2 + Step 3) — split into
   immediate-resolve (standalone) + post-drain-resolve (Bindings).
4. **Thunk::cell + shapeCell forwarding** (Step 5) — wire in
   walkThunk after state-specific handling.
5. **Smoke test** — call `runMajorScavenge` on a constructed
   minimal scenario (1-2 cells); verify no crashes + addresses
   change.
6. **Real-workload test** — invoke at end of hello.drvPath eval
   under `NIX_V3_MAJOR_GC=1` env gate.  Expected: hello.drvPath
   output byte-identical to TW + arena freed.
7. **Dispatch-loop integration** — add `shouldMajorGC()` heuristic
   + trigger in vm.cc:2617 area alongside `nursery->maybeScavenge`.
   This is the actual production wiring.

Steps 1-4 are CORE Day 3 implementation (~3-4 hours focused).
Steps 5-7 are validation + integration (~2-3 hours).

## Day-3 SHIP gates (pre-committed per [[memory-first-class]])

* `all-v3-tests --quick` 6/6 + `--core` 15/15 PASS with
  `NIX_V3_MAJOR_GC=0` (default; production unaffected)
* `all-v3-tests --quick` 6/6 PASS with `NIX_V3_MAJOR_GC=1`
  (the integration mode)
* hello.drvPath byte-identical to TW under `NIX_V3_MAJOR_GC=1`
* HNE byte-identical to TW under `NIX_V3_MAJOR_GC=1`
* peak_rss measured reduction: ≥ 100 MB on hello.drvPath OR ≥ 300
  MB on HNE.  Stage 6 SHIP-GREEN claims ≥ 200 MB / ≥ 600 MB; Day-3
  is the FIRST measurement against that target.

If any gate fails: FALSIFY this commit; investigate via:
* V3_DBG_MAJOR_GC=1 (add as part of Day 3) — dumps forwarding
  table sizes + per-step stats
* `bench/arena-dereg-audit.sh` for External-tag re-verification
* Standalone test harness for reduced repro

## Risks documented

1. **Boehm interaction with backup_** — backup_ blocks get
   `GC_add_roots` on refillBackup (same gate as active_).  After
   swap, the OLD active blocks (now backup_) are FREED via
   `freeBackupBlocks` which calls `GC_remove_roots`.  Boehm sees
   no inconsistency BECAUSE the new active_ contains the live
   set, also registered.  Verify in smoke test.

2. **Concurrency** — single-threaded VM; no concurrent access to
   arena.  But the bridge_root_registry IS thread-local; each
   thread's MajorScavenger references its own threadArena().  Safe
   by construction.

3. **Forwarding table memory cost** — typed maps hold all live
   pointers during the scavenge.  At 797 MB freeable on HNE, the
   live set is ~617 MB → ~10M cells → ~150 MB of map overhead
   during the scavenge.  Drops to 0 after scavenge completes.
   Acceptable transient.

4. **Closure::cu / desc** — libc-allocated (in ImportCache::cus
   deque).  Day 2.2 walkClosure already skips these.  No action
   needed for Day 3.

5. **bridge_root_registry interaction** — bridgeSrc is a
   `nix::Value *` from TW.  Not an arena pointer; not forwarded.
   Day 2.2 walkThunk already skips it.  No action for Day 3.

## Cross-references

* `lode/STAGE_6_IMPLEMENTATION_GUIDE_2026-05-27.md` §"Day 3"
* `lode/STAGE_6_PRECISE_GC_DESIGN_2026-05-27.md` §6 risk list
* `move_gc.cc` Day 2.2 — current MajorScavenger framework
* `alloc.hh::Arena::regionOf` — Day 2.1 classification
* `gc.cc::Scavenger::fwdClosure` — the nursery analog
* `precise_root.cc::walkAllV3Roots` + `walkGlobalV3Roots` — root walk
* `standaloneCellRoots()` (alloc.hh) — the standalone cell registry
* `[[memory-first-class]]` — SHIP gate per workload
* `[[falsification-rule]]` — if Day-3 ship gate fails, FALSIFY
* `[[measure-twice-cut-once]]` — this doc IS the measure-twice for Day 3

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
  Input Output Group.
SPDX-License-Identifier: Apache-2.0
