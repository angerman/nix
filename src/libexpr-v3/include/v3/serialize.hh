#pragma once
/// @file
/// v3 CompilationUnit serialization for VM-4 (bytecode disk cache).
///
/// Format (binary, little-endian where applicable):
///
///   Magic (8 bytes):  "NIX3BC01"
///   Schema (4):       kSchemaVersion (bumped on layout change)
///   Code (4 + 4*N):   count + uint32_t instructions
///   IntConsts (4+8*N)
///   FloatConsts (4+8*N)
///   StringConsts:     count + (length + bytes)*
///   SymbolTable:      count + (length + bytes)*
///   Lambdas:          count + per-lambda:
///                       codeOffset, prologueOffset, nUpvalues, nLocals,
///                       arity, hasFormals, ellipsis, _pad,
///                       formalsCount + (name, hasDefault, pos)*
///   LambdaCodeOffsets: count + uint32_t*
///   Primops:          count + (name length + bytes)*  -- resolved by
///                     findPrimOp(name) at load time
///   AttrSelectCacheCount (4):  size of mutable IC cache (entries are
///                              zeroed on load)
///   EntryOffset (4)
///
/// Pointers into process-local state (PrimOp*, Bindings* in
/// AttrSelectIC) are NEVER serialized directly — primops are
/// re-resolved at load time, IC cache is zeroed.
///
/// Bumped whenever the on-disk schema changes (opcode encoding,
/// descriptor field layout, constant payload format, etc.).  A
/// mismatched version on load is an immediate failure — no migration.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/bytecode.hh"

#include <stdexcept>
#include <string>
#include <string_view>

namespace nix::v3::serialize {

/// Bumped whenever the on-disk format changes.  Mismatches at load
/// time are a hard failure — no migration logic.
///
/// Schema 3 (2026-05-05): added opcode-table fingerprint to the header
/// so opcode renumbering / addition / deletion can't produce silently-
/// mis-executing CUs from an older build's cache (REVIEW §1.4).
constexpr uint32_t kSchemaVersion = 3;

/// 8-byte magic prefix at the start of every serialized blob.
/// Includes a discriminator so format mismatches are detected early.
constexpr char kMagic[8] = {'N','I','X','3','B','C','0','1'};

/// Fingerprint of the opcode table that the running process knows
/// about.  Recomputed once on first call from the constexpr enum
/// values (so any change to bytecode.hh -- adding/removing/renumbering
/// an opcode -- changes this hash).  Embedded in serialized blobs;
/// loads from a CU with a different fingerprint are rejected.
uint64_t opcodeTableFingerprint();

/// Thrown when serialization or deserialization fails (truncated
/// input, magic/schema mismatch, unknown primop name, etc.).
class SerializationError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/// True if the CU can round-trip through serialize/deserialize
/// without losing semantics.  The current format handles every
/// CompilationUnit produced by the v3 lowerer + emitter, so this
/// always returns true today; the predicate exists for future
/// growth (e.g., if a CU embeds opaque host data, we'd skip).
bool isCacheable(const CompilationUnit & cu);

/// Serialize a CompilationUnit to a binary blob.  The CU must be
/// cacheable; otherwise throws.
std::string serializeCU(const CompilationUnit & cu);

/// Deserialize a binary blob into a fresh CompilationUnit.
/// Primop names in the blob are resolved via findPrimOp() at load
/// time; an unknown name throws SerializationError.
CompilationUnit deserializeCU(std::string_view blob);

} // namespace nix::v3::serialize
