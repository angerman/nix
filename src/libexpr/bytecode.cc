/// @file
/// Bytecode compilation unit implementation.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "nix/expr/bytecode.hh"
#include "nix/expr/symbol-table.hh"

namespace nix::bytecode {

uint32_t CompilationUnit::addSymbol(Symbol sym)
{
    auto [it, inserted] = symbolIndex.emplace(sym, static_cast<uint32_t>(symbols.size()));
    if (inserted)
        symbols.push_back(sym);
    return it->second;
}

} // namespace nix::bytecode
