#pragma once
/// @file
/// Serialization format for bytecode CompilationUnits.
///
/// Used by the persistent CU cache (Phase 3.2) and any other tooling
/// that needs to round-trip a CU through bytes.
///
/// Format: magic + version + flat sections (code, symbols-as-strings,
/// constants, descriptors, positions, attrCaches).  Pointers into
/// process-local state (Symbol IDs, PosIdx values, AST Expr*,
/// Boehm-allocated Value*) are NEVER serialized directly — they are
/// either stored as their underlying content (strings for Symbols,
/// origin+offset for PosIdx, leaf payload for Value) or DROPPED at
/// serialize time and re-allocated on demand by the runtime.
///
/// Cacheability is enforced by bytecode::isCacheable(); attempting
/// to serialize an uncacheable CU throws a SerializationError.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "nix/expr/bytecode.hh"

#include <string>
#include <string_view>
#include <stdexcept>

namespace nix {
class EvalState;
}

namespace nix::bytecode {

/// Bumped whenever the on-disk schema changes (opcode encoding,
/// descriptor field layout, constant payload format, etc.).  A
/// mismatched version on load is an immediate failure — no migration.
constexpr uint32_t kBytecodeSerializeSchemaVersion = 1;

/// Magic prefix for cached CU blobs.  8 bytes including the schema
/// discriminator so format mismatches are detected up-front.
constexpr char kBytecodeMagic[8] = {'N','I','X','B','C','v','0','1'};

/// Thrown when a CU cannot be serialized or a blob cannot be parsed.
class SerializationError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/// Serialize a CompilationUnit to a binary blob.  The CU must be
/// cacheable (see bytecode::isCacheable); otherwise throws.
///
/// `state` is read-only here — used to look up Symbol strings via
/// state.symbols[sym] and PosIdx origins via state.positions.
std::string serializeCU(const CompilationUnit & unit, const EvalState & state);

/// Deserialize a binary blob into a fresh CompilationUnit allocated
/// against the GC heap (so traceable allocators work).  All Symbol
/// references are re-interned against state.symbols; all PosIdx
/// references are re-resolved via state.positions.addOrigin/add.
///
/// Throws SerializationError on magic/version mismatch or truncation.
/// Throws SerializationError on Symbol/PosIdx relocation failure.
CompilationUnit * deserializeCU(std::string_view blob, EvalState & state);

} // namespace nix::bytecode
