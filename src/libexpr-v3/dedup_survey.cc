/// @file
/// #772 Stage 9 Phase L0 spike — bytecode-level dedup survey.
/// See `include/v3/dedup_survey.hh` for design rationale.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
/// Input Output Group.  SPDX-License-Identifier: Apache-2.0

#include "v3/dedup_survey.hh"
#include "v3/bytecode.hh"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>
#include <utility>
#include <vector>

namespace nix::v3 {

DedupSurvey & dedupSurvey() noexcept
{
    static DedupSurvey s;
    return s;
}

bool dedupSurveyEnabled() noexcept
{
    static const bool e = std::getenv("NIX_V3_DEDUP_SURVEY") != nullptr;
    return e;
}

/// Emit the FINAL cumulative survey (all CUs observed process-wide, incl.
/// the hundreds of imported nixpkgs CUs).  The per-runRootExpr report at
/// run.cc:933 only fires for the builtins-install evals (it is not reached by
/// the user's top-level eval, whose imports run re-entrantly via `run()` — not
/// `runRootExpr`), so it printed a mid-eval snapshot (≈131 fns) that MASSIVELY
/// under-counted.  Registering the report at process exit reads the same
/// process-wide accumulator AFTER every import has been surveyed.  Reads the
/// dedupSurvey() singleton in THIS TU — identical instance the survey writes.
static void reportFinalDedupSurveyAtExit() noexcept
{
    const auto & sur = dedupSurvey();
    if (sur.totalFunctions == 0) return;
    double fnRatio = sur.uniqueHashes > 0
        ? (double)sur.totalFunctions / (double)sur.uniqueHashes : 0.0;
    double byteRatio = sur.uniqueBytes > 0
        ? (double)sur.totalBytes / (double)sur.uniqueBytes : 0.0;
    std::fprintf(stderr,
        "v3-direct dedup_survey [FINAL]: totalFunctions=%llu uniqueHashes=%llu "
        "fn_dedup_lb=%.2fx totalBytes=%.1fKB uniqueBytes=%.1fKB byte_dedup_lb=%.2fx\n",
        (unsigned long long)sur.totalFunctions,
        (unsigned long long)sur.uniqueHashes, fnRatio,
        sur.totalBytes / 1024.0, sur.uniqueBytes / 1024.0, byteRatio);
}

namespace {

/// Thread-local set of observed bytecode hashes.  The set lifetime
/// matches the process; this is intentional — the survey is process-
/// wide.  We use uint64_t (FNV-1a) over the bytecode words; the
/// false-positive rate is negligible for the scale of any single
/// `nix eval` run (~10 K unique functions; collision probability
/// ~10⁻⁵ on the birthday bound).  If the survey gates a real
/// architectural decision the user can rerun with BLAKE3.
std::unordered_set<uint64_t> & seenHashes() noexcept
{
    static std::unordered_set<uint64_t> s;
    return s;
}

/// FNV-1a over a slice of bytecode words.  The bytecode is uint32_t
/// per opcode/operand; we hash the underlying bytes.
[[gnu::always_inline]] inline uint64_t fnv1a(const uint32_t * data,
                                              size_t nWords) noexcept
{
    uint64_t h = 14695981039346656037ULL; // FNV offset basis
    const uint8_t * p = reinterpret_cast<const uint8_t *>(data);
    const size_t nBytes = nWords * sizeof(uint32_t);
    for (size_t i = 0; i < nBytes; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL; // FNV prime
    }
    return h;
}

} // anonymous namespace

void surveyCUBytecodeDedup(const CompilationUnit & cu)
{
    if (!dedupSurveyEnabled()) return;

    // Register the process-exit final report ONCE (reads the same accumulator
    // this function writes; the per-eval report at run.cc:933 under-counts).
    static const bool s_atexitRegistered = [] {
        std::atexit(reportFinalDedupSurveyAtExit);
        return true;
    }();
    (void)s_atexitRegistered;

    auto & sur = dedupSurvey();
    auto & seen = seenHashes();

    const size_t nLambdas = cu.lambdas.size();
    if (nLambdas == 0) return;

    // Bytecode end for the LAST lambda is cu.code.size(); for any
    // earlier lambda it's the next lambda's codeOffset.  The
    // codeOffsets are NOT guaranteed monotonic — emit.cc emits
    // inner functions first (see emit.cc:emitAll) — so we need to
    // pair (start, end) by SORTING codeOffsets and taking adjacent
    // pairs; or, more robustly, derive each lambda's range from
    // (codeOffset, next-larger codeOffset, code.size()).
    //
    // Cheaper: copy the codeOffsets, sort, then each sorted offset
    // is followed by the next sorted offset (or code.size()).  But
    // we need to associate the sorted slot with the original lambda
    // index to attribute uniqueness correctly.  Simpler still:
    // build a sorted vector of (codeOffset, lambdaIdx), iterate
    // pairwise, hash each slice.
    std::vector<std::pair<uint32_t, size_t>> ranges;
    ranges.reserve(nLambdas);
    for (size_t i = 0; i < nLambdas; ++i)
        ranges.emplace_back(cu.lambdas[i].codeOffset, i);
    std::sort(ranges.begin(), ranges.end());

    for (size_t r = 0; r < ranges.size(); ++r) {
        const uint32_t start = ranges[r].first;
        const uint32_t end = (r + 1 < ranges.size())
            ? ranges[r + 1].first
            : static_cast<uint32_t>(cu.code.size());
        if (end <= start) continue;
        const size_t nWords = end - start;

        const uint64_t h = fnv1a(cu.code.data() + start, nWords);
        sur.totalFunctions++;
        sur.totalBytes += nWords * sizeof(uint32_t);
        auto [it, inserted] = seen.insert(h);
        if (inserted) {
            sur.uniqueHashes++;
            sur.uniqueBytes += nWords * sizeof(uint32_t);
        }
    }
}

} // namespace nix::v3
