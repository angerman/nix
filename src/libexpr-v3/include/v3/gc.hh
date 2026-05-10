// Cheney scavenge for the v3 nursery.  Background and design:
// `lode/CHENEY_NURSERY_DESIGN.md`.
//
// Phase C entry point: walk live VM roots, copy live nursery objects
// (Thunk / Closure / ListVec) to the tenured arena via a side-table
// forwarding map, rewrite all encountered references, then reset the
// nursery's bump pointer so the buffer can be reused.
//
// Bindings are NOT in the nursery in Phase C: their entries[] hold
// Tag::Slot targets and `Thunk::cell` write-back pointers that must
// remain pointer-stable.  Phase D will revisit if Bindings turn out
// to dominate nursery pressure.
//
// Cells (`Value *` allocated via `Alloc::allocValue`) are tenured.  In
// Phase C v1 we DO NOT maintain a cell registry; instead we walk
// every tenured Bindings / ValuePair / Closure / Thunk / ListVec we
// reach from live roots with a visited-set, so any tenured-to-
// nursery reference is found through the live graph.  Phase D will
// switch to a remembered-set / cell-registry to bound walk cost.
//
// Trigger: `Nursery::maybeScavenge(vm)` is called between opcodes in
// the dispatch loop (when the nursery is past a fill threshold).
// After scavenge, the dispatcher MUST re-read its `closure` / `cu` /
// `stackBase` locals from `vm.frames.back()` because the frame's
// pointers may have been forwarded in place.
//
// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
// Input Output Group.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>

namespace nix::v3 {

class Nursery;
struct VMState;

/// Scavenge live nursery objects to tenured.  Walks roots from `vm`
/// (valueStack / withStack / frames / partialBindingsRegistry),
/// forwards nursery pointers through a side-table, then resets the
/// nursery's bump pointer.  Idempotent if nothing was allocated since
/// the last scavenge.
///
/// Caller invariants:
///   - The C-stack must not hold raw nursery pointers across this
///     call (other than what's reachable through `vm`).  Callers in
///     the dispatch loop must sync any per-frame locals before
///     entering scavenge and re-fetch them afterwards.
void scavengeNursery(Nursery & n, VMState & vm) noexcept;

} // namespace nix::v3
