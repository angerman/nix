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
///
/// 11: #803 (2026-05-24) LambdaDescriptor now serialises `name`,
/// `contextualName`, and `posHandle` (resolved file/line/col via
/// posSnapshotPool look-up).  Pre-11 cached CUs lose these
/// diagnostic fields on load → "anonymous lambda" + src=?:0:0
/// in errors.  Required to diagnose the haskell.nix
/// 'unexpected argument git' divergence (see lode/
/// V3_TRUE_NATIVE_RCA_2026-05-24.md).
///
/// 12: #814 (2026-05-25) LambdaDescriptor now serialises
/// `selectorSym` (uint32) + `identityLambda` (uint8) emit-time
/// peephole flags.  These are NOT derivable from the bytecode
/// alone at load time; they are eval-affecting because vm.cc's
/// OP_CALL fast path takes selectorSym/identityLambda branches
/// that SKIP body execution (and therefore skip the body's
/// OP_GET_LOCAL_FORCE).  Without serialisation, cached CUs lose
/// these flags, fall through to the slow path which forces the
/// arg via OP_GET_LOCAL_FORCE, propagating string context that
/// the fast path would have kept lazy.  Side effects on
/// derivation inputDrvs surface as wrong drvPath for
/// overlay-heavy workloads (firefox.drvPath was the reproducer;
/// see RCA_DESERIALIZE_RT_2026-05-25.md).
///
/// 13: #814 follow-up (2026-05-25) deserialise now: (a) includes
/// `selectorSym` in the sparse symbol-table collection, (b)
/// remaps selectorSym at load via that sparse table, and (c)
/// re-sorts each LambdaDescriptor.formals by post-remap name to
/// restore the OP_CALL formals-validation pass's sorted
/// invariant (writer's SymbolId order ≠ reader's after cross-
/// process remap).  Existing schema-12 entries lack the
/// selectorSym sparse-table marker; deserialising them under the
/// new code path produces an unmapped selectorSym → fast-path
/// "missing attr" on haskell.nix-class overlay workloads.
/// Schema bump invalidates them.
///
/// 14: R1 trigger fix (2026-05-26) — adds sparse PosIdx remap to
/// close the positional-only DIFF class identified by
/// `V3_DBG_DESERIALIZE_VERIFY` (commit dcfbae871).  Schema 13
/// stored in-bytecode PosIdx values (second word of each
/// OP_ATTRS_(LET|REC)_INIT name/pos pair, Formal::pos) as raw
/// integers indexed into the writer's `posSnapshotPool` — not
/// portable across processes / compile orders.  Schema 14 adds a
/// new sparse posTable section between symbolTable and lambdas,
/// emitting `(origId, file, line, column)` for every PosIdx
/// referenced in the bytecode (gathered by
/// `collectReferencedPositions`).  Deserialise builds a remap by
/// calling `recordPosSnapshot` for each entry and applies it via
/// `remapPositionsInBytecode` + formals walk, mirroring the
/// existing SymbolId remap.  Existing schema-13 entries lack the
/// posTable section; schema bump invalidates them.
///
/// Schema 15 (2026-05-29, EXIT_GC_SPIRAL Day 9-11): introduces
/// Tag::App3 (= 17) as a 3-arg App variant for mapAttrs /
/// zipAttrsWith.  Old caches that serialized values with the legacy
/// 2-pair App-chain encoding remain decodable, but cache entries
/// produced by the new mapAttrs/zipAttrsWith use the App3 layout —
/// invalidate the disk cache to force re-emit.
/// 16 (2026-06-01): the import/CLI path now lowers natively (v3-native
/// parser → lowerV3Ast), which numbers VarIds / orders bindings
/// differently than the retired lower.cc nix::Expr path.  Native CUs are
/// bytecode-incompatible with old (lower.cc) cache entries — bump to
/// invalidate them.  OPERATING RULE: bump on any incompatible native-
/// lowering change (the rule that previously covered lower.cc edits).
///
/// 17 (2026-06-19): LambdaDescriptor serialises
/// `secondArgIdentityLambda` for the arity-2 `name: value: value`
/// peephole used by mapAttrs/callClosure2.  Cache-loaded CUs must not
/// silently lose the flag and fall back to per-entry App3 allocation.
///
/// 18 (2026-07-04): LambdaDescriptor serialises `usesDefEnv` + `envSlotCount`
/// (NIX_V3_ENV_CAPTURE Track E W2b).  Eval-affecting: they drive the frame-entry
/// defEnv install + OP_MAKE_ENV allocation size, so a cache-loaded CU must not
/// silently lose them.  false/0 for CUs compiled without the feature.
constexpr uint32_t kSchemaVersion = 18;

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
