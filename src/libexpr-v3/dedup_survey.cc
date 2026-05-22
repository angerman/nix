/// @file
/// #772 Stage 9 Phase L0 spike — bytecode-level dedup survey.
/// See `include/v3/dedup_survey.hh` for design rationale.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
/// Input Output Group.  SPDX-License-Identifier: Apache-2.0

#include "v3/dedup_survey.hh"
#include "v3/bytecode.hh"

#include <algorithm>
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
