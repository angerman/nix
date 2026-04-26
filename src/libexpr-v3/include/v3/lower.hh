#pragma once
/// @file
/// Lower a nix::Expr* AST into a v3::ir::Module.
///
/// Prerequisites: the AST must have been through Expr::bindVars(), so each
/// ExprVar carries `level / displ / fromWith` annotations.
///
/// Behavior on unsupported AST shapes: throws std::runtime_error.  As the
/// lowerer's coverage expands the failure surface shrinks.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/ir.hh"

namespace nix {
struct Expr;
class SymbolTable;
}

namespace nix::v3 {

/// Lower a top-level Nix expression into a fresh v3 IR module.  The
/// returned module's functions[0] is the entry function.
ir::Module lowerNixExpr(nix::Expr * e, const nix::SymbolTable & symbols);

} // namespace nix::v3
