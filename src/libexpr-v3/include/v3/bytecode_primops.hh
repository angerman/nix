#pragma once
/// @file
/// Bytecode primop infrastructure (T0 — A12b architectural refactor).
///
/// The C primops in primops.cc that take user lambdas as arguments
/// (map / filter / foldl' / concatMap / mapAttrs / etc.) invoke those
/// lambdas via `callClosure(vm, op, arg)`, which C-recurses into
/// dispatchLoop.  On deep eval graphs (stdenv.mkDerivation × transitive
/// dependencies), this is the dominant C-stack consumer and the root
/// cause of the depth=5000 SIGSEGV documented in
/// `memory/project_a12b_depth5000.md`.
///
/// The fix: replace those C primops with hand-written Nix-source
/// templates that use ONLY non-callback primops (length, elemAt,
/// arithmetic) for their bodies plus OP_CALL on the user lambda.
/// At runtime, the inner OP_CALL dispatches through dispatchLoop
/// iteratively (per commit 7f5a392f4) — no C-recursion.  The
/// recursive helper inside each template is rewritten to OP_TAIL_CALL
/// by emit.cc's tail-call peephole, so the loop runs in O(1)
/// vm.frames depth regardless of input size.
///
/// `installBytecodePrimop` compiles a Nix-source primop body at v3
/// init time and replaces the corresponding entry in TW's builtins
/// attrset with a v3-Closure-wrapped-as-TW-value (via
/// `v3ToTreeWalkerPublic`).  Both v3-direct and TW dispatch through
/// the bridged value; v3-direct unwraps it and runs the inner
/// closure on the current VM (`tryUnwrapBridge1Closure` shortcut in
/// OP_CALL, vm.cc:2920).
///
/// Property tests in `test/property/property_tests.py` ensure that
/// each replacement preserves TW's exact semantics on randomized
/// inputs; lang-test parity covers the static corpus.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
/// Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/value.hh"
#include <functional>
#include <string>

namespace nix {
class EvalState;
}

namespace nix::v3 {

struct PrimOp;

/// #705 (2026-05-20): walk the primop-replacement map as a scavenger
/// root.  Each Value in the map can carry a nursery Closure*; without
/// this walk, scavenge frees the closure and the next
/// `lookupPrimopReplacement` returns a stale pointer.  Caller supplies
/// a visitor that forwards each Value's payload.
void walkBytecodePrimopRoots(const std::function<void(Value &)> & visit);

/// #705 (2026-05-20): walk the static `vBuiltins` Value as a
/// scavenger root.  `vBuiltins` is a process-wide singleton built by
/// `getBuiltinsValue()`; the bytecode-primop install path patches its
/// Bindings entries in-place to point at fresh bytecode closures,
/// which may live in the nursery.  Without this walk, scavenge frees
/// those closures and the next OP_LIT_BUILTINS push hands the dispatch
/// a stale pointer.  No-op if `vBuiltins` hasn't been materialised
/// yet.
void walkBuiltinsRoot(const std::function<void(Value &)> & visit);

/// Returns the bytecode-closure replacement Value for `po`, or
/// nullptr if no replacement has been installed.  Used by:
///   - `vm.cc` OP_LIT_PRIMOP to push the replacement Closure
///     instead of a Tag::PrimOp Value.
///   - `lower.cc` `lowerCall` to skip the static PrimOpCall path
///     for replaced primops (forcing the call through the generic
///     App-chain → OP_CALL emit path, which then sees the
///     replacement Closure via the OP_LIT_PRIMOP hook above).
///
/// Cheap (one unordered_map lookup).  Safe to call before
/// `installAllBytecodePrimops` — returns nullptr until populated.
const Value * lookupPrimopReplacement(const PrimOp * po) noexcept;


/// Compile `nixSource` (which must be a lambda expression like
/// `op: nul: list: <body>`) via the v3 lowering pipeline and replace
/// the entry for `primopName` in TW's `builtins` attrset with the
/// resulting closure.
///
/// `nixSource` should reference only non-callback primops in its body
/// — `builtins.length`, `builtins.elemAt`, arithmetic, comparisons,
/// and the lambda's own params.  References to callback primops
/// (foldl', map, etc.) are allowed IF those primops are themselves
/// installed FIRST (handled by dependency-order in
/// `installAllBytecodePrimops`).
///
/// Side effects:
///   - Adds the produced CompilationUnit to a process-global
///     keep-alive vector (the Value's string/path payloads reference
///     it).
///   - Mutates `state.getBuiltin(primopName)` in place.
///
/// Throws on any pipeline error (parse, bind-vars, compile, run).
/// Idempotent: re-calling with the same name skips the work.
void installBytecodePrimop(
    nix::EvalState & state,
    const std::string & primopName,
    const std::string & nixSource);

/// Install ALL the bytecode-primop replacements known to v3.  Called
/// once from `runRootExpr` (guarded by std::call_once + recursion
/// flag, since the install itself uses the v3 pipeline).
void installAllBytecodePrimops(nix::EvalState & state);

} // namespace nix::v3
