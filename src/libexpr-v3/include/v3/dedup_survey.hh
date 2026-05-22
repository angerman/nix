#pragma once
/// @file
/// #772 Stage 9 Phase L0 spike (2026-05-22) — bytecode-level
/// dedup survey.
///
/// The full Stage 9 design (LINKING_DESIGN_2026-05-17.md) calls
/// for IR-node `structuralHash()` modulo alpha-equivalent VarId
/// renumbering.  That's a 1-week task with ~400 LoC of careful
/// recursive walks across 40+ Expr variants.  Before committing
/// to that, this spike falsifies the underlying hypothesis with
/// a cheaper proxy: hash each Function's *bytecode slice* (post-
/// compile, post-symbol-remap-to-global) and survey the
/// duplicate ratio across all compiled CUs in a run.
///
/// Bytecode-level hashing is a LOWER BOUND on cell dedup.  Two
/// alpha-equivalent functions will hash differently if their
/// VarId numbering differs (the compiler's local-slot assignment
/// is order-sensitive).  So the bytecode-survey number is the
/// floor; the real (IR-level) dedup ratio can only be higher.
///
/// If even the LOWER BOUND shows ≥5 × dedup on nixpkgs, Stage 9
/// is justified.  If it shows <2 ×, Stage 9 is killed (and the
/// per-file disk cache is sufficient).
///
/// Gate: NIX_V3_DEDUP_SURVEY=1.  Zero overhead when unset.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
/// Input Output Group.  SPDX-License-Identifier: Apache-2.0

#include <cstdint>

namespace nix::v3 {

struct CompilationUnit;

struct DedupSurvey
{
    /// Total Function bodies surveyed (across all CompilationUnits
    /// produced during this process — top-level + every primImport).
    uint64_t totalFunctions = 0;
    /// Distinct hashes observed.  totalFunctions / uniqueHashes is
    /// the "lower bound dedup ratio."
    uint64_t uniqueHashes   = 0;
    /// Bytes of bytecode hashed across all functions.  Useful for
    /// the "if we cached at cell granularity, how much memory is
    /// duplicate" question.
    uint64_t totalBytes     = 0;
    /// Bytes attributable to UNIQUE cells (each unique-hash counted
    /// once at the FIRST encounter, with the size that first
    /// encounter had).  totalBytes / uniqueBytes is the byte-level
    /// dedup ratio.
    uint64_t uniqueBytes    = 0;
};

DedupSurvey & dedupSurvey() noexcept;

bool dedupSurveyEnabled() noexcept;

/// Walk every Function in `cu` and bump the survey counters.  Called
/// from primops.cc primImport AFTER `compile()` returns.  Hashing is
/// content-addressed over the bytecode slice [codeOffset .. next
/// lambda's codeOffset).
void surveyCUBytecodeDedup(const CompilationUnit & cu);

} // namespace nix::v3
