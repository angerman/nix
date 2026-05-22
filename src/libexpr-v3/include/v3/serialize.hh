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
///
/// Schema 7 (#548c, 2026-05-10): non-rec attrset emit switched from
/// OP_ATTRS_INIT (compute-then-allocate) to the OP_ATTRS_REC_INIT +
/// OP_ATTRS_REC_SET pattern (allocate-first-fill-later).  This is
/// the STG-style early-alloc for `{ a = ...; b = ...; }` literals
/// so withLookup can peek at the partial Bindings during entry
/// computation; closes the v3-direct nixpkgs `cycle while resolving
/// 'libsForQt5'` failure.  Old caches must reload because the same
/// IR (ir::AttrSet) now produces different bytecode.
///
/// 8: #558 (2026-05-10) introduce OP_ATTRS_UPDATE_TAIL (0x88).  IR
/// `Update::isFunctionReturn` flag.  Old caches must reload because
/// tail-position // expressions now emit a different opcode.
///
/// 9: #781b (2026-05-23) sparse symbolTable.  Previously each CU
/// serialised a copy of the ENTIRE global symbol table (~50 K
/// entries by the 269th import on hello.drvPath), and deserialize
/// interned every entry — 296 ms of 304 ms total deserialize cost.
/// Schema 9 walks the CU's bytecode + formals at serialize time
/// to collect just the SymbolIds actually referenced; writes
/// (origId, name) pairs sorted by origId.  On load, deserialize
/// builds a sparse remap (vector<uint32_t> sized maxOrigId+1).
/// Bytecode operands still carry the same global IDs from
/// serialize time; only unreferenced slots are dropped.
///
/// 10: #779 (2026-05-23) OP_REC_BINDING_SLOT_REF gains a 1-word
/// IC follow-up indexing CompilationUnit::recSlotCache.  Mirror of
/// OP_ATTRS_SELECT's IC mechanism for the LetRec slot-ref hot
/// path (9.05 % of dispatch on hello.drvPath; 1.4 M calls).
/// Cache size is serialised; entries are zeroed on load.
constexpr uint32_t kSchemaVersion = 10;

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

/// #777b (2026-05-23) per-section timing breakdown for
/// deserializeCU.  Only populated when V3_DBG_DESERIALIZE=1 is
/// set (gated to avoid clock_gettime overhead in steady-state).
/// Use the breakdown to identify which section dominates the
/// deserialize budget; informs the next optimisation lever.
struct DeserializeBreakdownSnapshot {
    uint64_t headerNs;
    uint64_t codeNs;
    uint64_t intConstantsNs;
    uint64_t floatConstantsNs;
    uint64_t stringConstantsNs;
    uint64_t symbolTableNs;
    uint64_t lambdasNs;
    uint64_t lambdaCodeOffsetsNs;
    uint64_t primopsNs;
    uint64_t miscNs;
    uint64_t remapNs;
    uint64_t calls;
};

DeserializeBreakdownSnapshot deserializeBreakdown();
bool deserializeBreakdownEnabled();

} // namespace nix::v3::serialize
