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
///
/// Schema 4 (2026-05-08): #495/#509 STG-13d -- added per-LambdaDescriptor
/// intrinsicKind (uint8) + intrinsicVar0/1/2 (int8) for native dispatch
/// of the inner Extends/Compose lambdas.  Without this, cache-loaded CUs
/// for nixpkgs lib/fixed-points.nix have intrinsicKind=None and the
/// recogniseIntrinsic-driven native dispatch never fires on cached
/// loads (the dominant case in production workloads).
///
/// Schema 5 (2026-05-08): #530 lexical-with chain -- OP_MAKE_THUNK and
/// OP_MAKE_CLOSURE now carry a SECOND data word (`nWithTargets`) after
/// the existing `nUpvalues` data word, and LambdaDescriptor gains a
/// `nWithTargets` (uint16) field.  At MAKE time the runtime pops
/// nUpvalues followed by nWithTargets values; the with-target block
/// is materialised into the resulting Closure / Thunk's
/// `capturedWiths` ListVec.  Replaces `snapshotCurrentWiths` as the
/// source of truth for the lexical with-chain.
///
/// Schema 6 (2026-05-09): #546 v3-direct callPackage with-scope fix --
/// new OP_ATTRS_LET_REC_INIT (0x86) opcode emitted in lieu of
/// OP_ATTRS_REC_INIT for `let ... in body` shapes (lowerLet, hasBody=
/// true).  Bytecode-identical (same trailing data, same following
/// REC_SETs) but the runtime skips publishToNearestBlackThunkFrame --
/// the recAttrs is intermediate state, not the surrounding thunk's
/// eventual return value.  Old caches must reload because they used
/// OP_ATTRS_REC_INIT for both shapes; the opcode-table fingerprint
/// catches the difference but bumping the schema makes the rejection
/// crisp.  See lode/CALLPACKAGE_BUG_2026-05-09.md.
constexpr uint32_t kSchemaVersion = 6;

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
