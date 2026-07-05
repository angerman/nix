#pragma once
/// @file
/// Immediate-demand helpers for unrealized MapAttrs entries.
///
/// `builtins.mapAttrs (n: v: f n v) src` stores its entries UNREALIZED: the
/// entry's `value` is the source value `v` and a flag bit
/// (`kMapAttrsUnrealizedPosBit`) marks that the mapper `f` has not yet been
/// applied.  Consumers that are going to force/deeply-demand an attr value must
/// apply the mapper to get the MAPPED value (firing any side effects in `f`,
/// e.g. `builtins.trace`), NOT compare/force the stored source — otherwise v3
/// diverges from the tree-walker's strictness (see test 583 POS-3).
///
/// These helpers were originally file-local in primops.cc (used by deepSeq +
/// derivation env iteration).  They are hoisted here so `valueEqual` (vm.cc) and
/// any other cross-TU consumer can route through the SAME immediate-demand path
/// — the single source of truth for "force the mapped value, not the source".
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/alloc.hh"   // Bindings, Bindings::Entry, kMapAttrsUnrealizedPosBit
#include "v3/primop.hh"  // VMState, Value, callClosure2, forceValue

namespace nix::v3 {

/// True iff `e` is an entry of a MapAttrs Bindings whose mapper has not yet been
/// applied (the stored value is the source, not the mapped result).
[[gnu::always_inline]] inline bool isUnrealizedMapAttrsEntry(
    const Bindings * owner, const Bindings::Entry & e) noexcept
{
    return owner && owner->isMapAttrs()
        && (e.pos & Bindings::kMapAttrsUnrealizedPosBit) != 0;
}

/// Value an immediate consumer should see for `e`: for an unrealized MapAttrs
/// entry, apply the mapper `*owner->mapAttrsAux()` to (name, source) so the MAPPED value
/// (with its side effects) is produced; otherwise the stored value unchanged.
inline Value entryValueForImmediateDemand(
    VMState & vm, const Bindings * owner, const Bindings::Entry & e)
{
    if (!isUnrealizedMapAttrsEntry(owner, e))
        return e.value;

    auto * mutOwner = const_cast<Bindings *>(owner);
    auto * mutEntry = const_cast<Bindings::Entry *>(&e);
    Value nameStr = Bindings::makeMapAttrsNameValue(e.name);
    Value src = mutOwner->mapAttrsEntrySource(mutEntry);
    return callClosure2(vm, *mutOwner->mapAttrsAux(), nameStr, src);
}

/// `entryValueForImmediateDemand` then force to WHNF.
inline Value forceEntryForImmediateDemand(
    VMState & vm, const Bindings * owner, const Bindings::Entry & e)
{
    return forceValue(vm, entryValueForImmediateDemand(vm, owner, e));
}

} // namespace nix::v3
