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

/// Placeholder EvalState — the bring-up primops don't need any of its
/// fields, but we want a stable type for the function-pointer signature.
/// As we add primops that need a symbol table, file loader, etc., this
/// fills out.
struct EvalState
{
    // Reserved for: symbol table, store, fetcher, etc.
};

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
