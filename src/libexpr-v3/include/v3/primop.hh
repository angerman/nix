#pragma once
/// @file
/// v3 primop infrastructure.
///
/// PrimOp = a fixed-arity native function.  Each PrimOp has a name, an arity,
/// and a function pointer that takes (EvalState, args) and returns a Value.
///
/// PrimOps are registered in a thread-safe global registry at process start.
/// They appear as ordinary Closure-like Values (Tag::PrimOp) and participate
/// in OP_CALL via partial application: a PrimOp with arity > 1 wraps
/// successive calls in PrimOpApp pairs until all args are gathered.
///
/// Bring-up: only fully-applied PrimOps are supported (single-arg primops, or
/// multi-arg primops dispatched through OP_CALL_PRIMOP_N).  Partial
/// application TBD.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/value.hh"

#include <cstdint>
#include <string_view>
#include <vector>

namespace nix::v3 {

struct VMState;

/// Placeholder EvalState — the bring-up primops don't need any of its
/// fields, but we want a stable type for the function-pointer signature.
///
/// `vm` is set by the dispatch loop just before invoking a primop, and
/// can be used by callback primops (map, filter, foldl', genList) to
/// re-enter the VM via callClosure().
struct EvalState
{
    VMState * vm = nullptr;
};

/// Apply a closure (or single-arg primop) to one argument and return
/// the result, by re-entering the VM dispatch loop on the same VMState.
/// Used by callback primops.  Throws if `fun` is not callable.
Value callClosure(VMState & vm, Value fun, Value arg);

/// Force a thunk to WHNF.  Pass-through for non-thunk values.  Re-enters
/// the dispatch loop on the same VMState (used by primops like tryEval
/// that need to force from C++).
Value forceValue(VMState & vm, Value v);

/// Function pointer signature.  The primop is given a span of forced
/// argument Values (the dispatcher arranges forcing) and writes its
/// result into `out`.
using PrimOpFn = void (*)(EvalState & state, Value * args, Value & out);

struct PrimOp
{
    std::string_view name;
    uint8_t          arity;
    PrimOpFn         fn;
    std::string_view doc;        // optional
};

/// Look up a primop by name.  Returns nullptr if not registered.
const PrimOp * findPrimOp(std::string_view name);

/// Register a primop (or replace an existing one — last write wins).
void registerPrimOp(const PrimOp & op);

/// Register the bring-up subset of primops (length, head, tail, ...).
/// Idempotent; call during EvalState init.
void registerBuiltinPrimOps();

} // namespace nix::v3
