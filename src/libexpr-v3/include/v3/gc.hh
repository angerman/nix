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
#include <cstdint>
#include <functional>

namespace nix::v3 {

class Nursery;
struct VMState;
struct Value;

/// #34 (2026-07-06): true iff a raw arena word at byte-offset `off` within a
/// tenured cell of CellType `cellType` (the CellType enum value) lies in a
/// provably-NON-POINTER scalar/metadata slot — a Bindings entry's packed
/// {SymbolId,PosIdx32} word, a header kind/size/count, closure {nUpvalues,pad},
/// etc.  The conservative post-scavenge BRUTE raw-word scan must SKIP these:
/// a scalar word's 64-bit value can coincidentally land in the nursery's
/// ASLR-varying address range (a ~0.07%/run false-positive → the "brute-audit"
/// flake, RCA 2026-07-06), but a pointer never lives in a scalar slot, so
/// skipping cannot hide a real missed root (the AUDIT deep-walk stays the
/// precise reachability check).  Offsets encode the CURRENT cell layouts and
/// MUST be updated on any header change (e.g. P1a Bindings 24->16B, P1b Closure
/// 40->32B).  Unit-tested in test/smoke.cc to guard against layout drift.
bool bruteScanSlotIsScalar(uint8_t cellType, size_t off) noexcept;

/// #705 (2026-05-21): expose v3_call_flake's `g_cachedCallFlake`
/// closureValue as a scavenger root.  The cached closure (produced
/// by running call-flake.nix bytecode at process init) may have
/// been allocated through the nursery; without this walk a scavenge
/// after the first getFlake call leaves the cache holding a stale
/// closure pointer.
void walkCallFlakeRoot(const std::function<void(Value &)> & visit);

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
