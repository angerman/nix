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
    for (uint32_t i = 0; i < symbols.size(); ++i)
        if (symbols[i] == sym)
            return i;
    uint32_t idx = static_cast<uint32_t>(symbols.size());
    symbols.push_back(sym);
    return idx;
}

} // namespace nix::bytecode
