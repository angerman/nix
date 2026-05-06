#pragma once
/// @file
/// #458 step B.2 -- v3::Value canonicalization, primop signature plumbing.
///
/// Today the codebase has two primop signatures:
///
///   TW:  void(nix::EvalState&, const nix::PosIdx, nix::Value** args, nix::Value& v)
///   v3:  void(nix::v3::EvalState&, nix::v3::Value* args, nix::v3::Value& out)
///
/// Step 4 of the v3-primary inversion plan (#458) replaces nix::Value with
/// nix::v3::Value as the canonical evaluator type.  The audit at
/// `lode/V3VALUE_CANON_AUDIT.md` shows ~117 sites in libexpr-v3 plus 594 in
/// libexpr; external CLI consumers barely touch nix::Value directly.
///
/// This header is the small concrete piece (B.2) that lets primop authors
/// migrate one family at a time.  It does NOT commit the codebase to the
/// migration -- nothing in v3's existing dispatch loop changes here.  When
/// a future canonicalization session begins, the adapters below let:
///
///   1. A canonical-Value primop (signature: `CanonPrimOpFn`) be wrapped
///      into the existing v3 PrimOp registry without rewriting the registry.
///   2. A canonical-Value primop also be exposed as a TW PrimOp via a
///      thin shim that bridges nix::Value <-> v3::Value at the entry edge.
///
/// **Status:** declarations only.  The shims are intentionally not wired
/// into either registry yet; doing so requires the broader Phase 1
/// migration work documented in the audit.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/value.hh"
#include "v3/primop.hh"

#include <cstddef>

namespace nix { struct EvalState; struct Value; class PosIdx; }

namespace nix::v3 {

/// CANONICAL primop signature.  This is the target shape after step 4:
/// every primop reads + writes nix::v3::Value, no per-call PosIdx
/// (errors carry source position via `EvalState`).
///
///   - `state` carries thread-local context (vm, nixEvalState bridge
///     to TW for the migration window, optional source position).
///   - `args` is an array of N forced argument Values; N == primop arity.
///   - `out` receives the result.  Caller pre-zeros to Tag::Uninitialized.
///
/// Identical to the existing `nix::v3::PrimOpFn` typedef in primop.hh;
/// re-exposed here under a more explicit name to make the
/// canonicalization intent explicit at call sites.
using CanonPrimOpFn = void (*)(EvalState & state, Value * args, Value & out);

/// Adapter: wrap a TW-shaped primop (the existing
/// `void(nix::EvalState&, nix::PosIdx, nix::Value**, nix::Value&)`
/// signature) so it can be registered in the v3 PrimOp registry
/// during the migration.  Bridges args TW->v3, runs the TW body,
/// bridges the result back v3<-TW.
///
/// Signature:
///   typedef void TwPrimOpFn(nix::EvalState&, const nix::PosIdx,
///                            nix::Value** args, nix::Value& v);
///   CanonPrimOpFn adaptedFromTw(TwPrimOpFn * tw, uint8_t arity);
///
/// Returns nullptr if `tw` is nullptr.  Otherwise returns a stable
/// thunk that the v3 registry can register as `PrimOp::fn`.
///
/// Implementation lives in primop_canon.cc (when it exists).  Today
/// this header just declares the contract -- the adapter isn't yet
/// implemented because no primop has been migrated.
using TwPrimOpFn = void (
    nix::EvalState & state,
    const nix::PosIdx pos,
    nix::Value ** args,
    nix::Value & v);
CanonPrimOpFn adaptFromTw(TwPrimOpFn * tw, uint8_t arity);

/// Reverse adapter: take a canonical primop (CanonPrimOpFn) and
/// expose it as a TW PrimOp body.  Used during the migration so a
/// CanonPrimOpFn-shaped implementation can serve both TW callers
/// (which read nix::Value) and v3 callers (which read v3::Value)
/// without duplicating logic.
///
/// Returns nullptr if `canon` is nullptr.  Caller is responsible for
/// allocating the returned trampoline lifetime; typically the trampoline
/// is a `static thread_local` per-primop closure object that the
/// registration site keeps alive.
TwPrimOpFn * adaptToTw(CanonPrimOpFn canon, uint8_t arity);

/// Concrete migration helper for a single primop family.  When the
/// first primop is migrated (B.3), this header gets a paired
/// `migrate_<family>.hh` with the canonical implementation, and the
/// existing TW + v3 registrations are replaced by registrations
/// against the canonical body.
///
/// The recommended starter family is the integer-arithmetic primops
/// (`builtins.add`, `sub`, `mul`, `div`, `bitAnd`, `bitOr`, `bitXor`)
/// because:
///   - they're pure scalar -> scalar; no Bindings traversal, no
///     thunk-state interaction (no shape-mismatch shim needed);
///   - they have direct semantic equivalents in both signatures;
///   - parity is mechanically verifiable (numeric output identity).

} // namespace nix::v3
