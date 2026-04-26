/// @file
/// Bytecode compilation unit implementation.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "nix/expr/bytecode.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/expr/value.hh"
#include "nix/expr/nixexpr.hh"
#include "nix/expr/ir.hh"

namespace nix::bytecode {

CompilationUnit::CompilationUnit() = default;
CompilationUnit::~CompilationUnit() = default;

uint32_t CompilationUnit::addSymbol(Symbol sym)
{
    auto [it, inserted] = symbolIndex.emplace(sym, static_cast<uint32_t>(symbols.size()));
    if (inserted)
        symbols.push_back(sym);
    return it->second;
}

// ---------------------------------------------------------------------------
// Cacheability predicate (Phase 3.2-1)
//
// A CU is cacheable iff every persistent reference can be re-materialized
// from a binary blob in a fresh EvalState.  Pointer-chasing structures
// (attrsets, lists, lambdas, etc.) cannot — they would require recursively
// serializing the heap.  We refuse cleanly rather than heuristically copy.
// ---------------------------------------------------------------------------

UncacheableReason cacheabilityCheck(const CompilationUnit & unit)
{
    // Most common rejection first.
    if (!unit.exprPool.empty())
        return UncacheableReason::HasExprPool;

    // Constants must round-trip through (kind, payload).  Allowed:
    // anything that converts to one of the leaf nValue types.
    // Disallowed: anything that holds Value* pointers or attrset/list
    // contents.
    //
    // We use the public `type()` method which returns the broader
    // ValueType (nInt/nBool/...), since getInternalType() is protected.
    // This is slightly less precise (e.g. tListSmall and tListN both
    // map to nList), but for cacheability we treat all list/attrset/
    // function/external/thunk values as non-leaf regardless of the
    // exact internal layout.
    for (const Value * v : unit.constants) {
        if (!v) continue;  // defensive — empty slot
        switch (v->type()) {
            case nInt:
            case nBool:
            case nNull:
            case nFloat:
            case nString:
            case nPath:
                continue;
            // nFunction may be tPrimOp (cacheable by name) or
            // tLambda (uncacheable — would need full closure capture).
            // Distinguish via Value::isPrimOp() vs isLambda().
            case nFunction:
                if (v->isPrimOp())
                    continue;
                return UncacheableReason::NonLeafConstant;
            case nFailed:
                continue;  // runtime sentinel; defensive
            // All non-leaf types: refuse.
            case nThunk:
            case nAttrs:
            case nList:
            case nExternal:
                return UncacheableReason::NonLeafConstant;
        }
    }

    // Lambdas with formals (pattern-match parameter list) hold a
    // Formals* into the AST BumpMemoryResource.  Until we serialize
    // those, refuse.
    for (const auto & lam : unit.lambdas) {
        if (lam.formals != nullptr)
            return UncacheableReason::HasFormalsLambda;
    }

    return UncacheableReason::Cacheable;
}

const char * uncacheableReasonName(UncacheableReason r)
{
    switch (r) {
        case UncacheableReason::Cacheable:         return "cacheable";
        case UncacheableReason::HasExprPool:       return "has-expr-pool";
        case UncacheableReason::NonLeafConstant:   return "non-leaf-constant";
        case UncacheableReason::HasFormalsLambda:  return "has-formals-lambda";
        case UncacheableReason::UnsupportedFeature: return "unsupported-feature";
    }
    return "unknown";
}

} // namespace nix::bytecode
