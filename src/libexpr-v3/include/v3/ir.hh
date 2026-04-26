#pragma once
/// @file
/// v3 IR — minimal SSA for the bring-up subset (literals, arithmetic, let,
/// var, lambda, app).
///
/// This is INTENTIONALLY tiny.  The full v3 design (doc/v3-design/v3-design.md)
/// covers attrset, list, conditional, with, recursive let, formals, primops,
/// etc.  Each is added incrementally; the bring-up subset's job is to
/// validate the architecture.
///
/// Single-block-only for now — no CFG, no phi.  When conditionals land, we
/// add IRBlock + IRBranch.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <variant>
#include <vector>
#include <string>
#include <memory>

namespace nix::v3::ir {

/// SSA value identifier.  Index into the enclosing function's `bindings`
/// vector — a `Binding` defines exactly one VarId.
using VarId = uint32_t;
constexpr VarId kInvalid = 0;

// ---------------------------------------------------------------------------
// IR expression variants — each computes one Value from previous bindings.
// ---------------------------------------------------------------------------

struct LitInt    { int64_t value; };
struct VarRef    { VarId   var;   };
/// Literal lambda: when emitted, allocates a Closure with the captured
/// upvalues from `freeVars`.  `body` is a separate IRFunction.
struct Lambda    { std::vector<VarId> freeVars; uint32_t funcIdx; };
struct App       { VarId fun; VarId arg; };
struct Add       { VarId lhs; VarId rhs; };
struct Sub       { VarId lhs; VarId rhs; };
struct Mul       { VarId lhs; VarId rhs; };

/// IRExpr — the RHS of a binding.
using Expr = std::variant<LitInt, VarRef, Lambda, App, Add, Sub, Mul>;

/// One SSA binding inside an IRFunction's body.
struct Binding
{
    VarId      var;          // result name
    Expr       expr;
    uint32_t   useCount = 0;  // populated by analysis (DCE / strictness)
};

// ---------------------------------------------------------------------------
// IRFunction — one lambda or one top-level expression.
// ---------------------------------------------------------------------------

struct Function
{
    /// Bindings in evaluation order.  `var` field of each = its definition.
    std::vector<Binding> bindings;
    /// VarId returned from the function body.
    VarId returnVar = kInvalid;
    /// VarId for the parameter (single-arg lambdas only for now).  Top-level
    /// has no parameter (param == kInvalid).
    VarId param = kInvalid;
    /// Free vars referenced in this function's bindings, sorted ascending.
    /// Populated by computeFreeVars.  These become the closure's upvalues.
    std::vector<VarId> freeVars;
    /// Optional name (for diagnostics).
    std::string name;
};

// ---------------------------------------------------------------------------
// IRModule — the top-level function plus any nested lambdas.
// ---------------------------------------------------------------------------

struct Module
{
    /// functions[0] = top-level entry; functions[1..] = inner lambdas
    /// referenced by IRLambda::funcIdx.
    std::vector<Function> functions;
};

/// Compute free variables for every function.  Must be called after the
/// AST→IR lowering before emit.
void computeFreeVars(Module & m);

} // namespace nix::v3::ir
