/// @file
/// Per-creation-site force-rate histogram instrument — implementation.
///
/// See forcerate_trace.hh for the site model and the byte-id-neutral
/// correctness contract. Retirement: delete once the cheap-eagerness /
/// optimistic-eval GO/NO-GO is decided (LITERATURE_SWEEP_2026-07-07 §2).
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/forcerate_trace.hh"

#include "v3/bytecode.hh"   // CompilationUnit, decodeOp, Op, LambdaDescriptor
#include "v3/closure.hh"    // LambdaDescriptor (full decl)
#include "v3/alloc.hh"      // resolvePosSnapshot / PosSnapshot

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

namespace nix::v3::forcerate {

namespace {

/// Per-site accumulators. Keyed by the LambdaDescriptor* stamped on the
/// thunk at creation — a stable O(1) site key (a given (CU, funcIdx) maps
/// to exactly one descriptor). The instrument is single-threaded (v3 eval
/// is single-threaded); no atomics.
///
/// Kept as two maps rather than one struct-map so the two hot hooks each
/// touch a single map. A descriptor may appear in `g_forced` but NOT
/// `g_created` if a thunk stamped with it was made somewhere other than
/// OP_MAKE_THUNK (App-based lazies, formal wrappers, bridge thunks) — the
/// report surfaces that as a separate diagnostic rather than folding it in.
std::unordered_map<const LambdaDescriptor *, uint64_t> g_created;
std::unordered_map<const LambdaDescriptor *, uint64_t> g_forced;
/// The owning CU per descriptor (captured at creation; == desc->cu). Only
/// used at report time for the RHS-cheapness body scan.
std::unordered_map<const LambdaDescriptor *, const CompilationUnit *> g_cuOf;

}  // namespace

bool enabled() noexcept
{
    // Retirement criterion (see forcerate_trace.hh): delete this gate once
    // the cheap-eagerness / optimistic-eval lever is decided. Diagnostic-
    // only, byte-id neutral — never affects eval results.
    static const bool s_on = std::getenv("NIX_V3_FORCERATE_TRACE") != nullptr;
    return s_on;
}

void created(const LambdaDescriptor * desc,
             const CompilationUnit * cu) noexcept
{
    if (__builtin_expect(!enabled(), 1)) return;
    if (!desc) return;
    ++g_created[desc];
    if (cu) g_cuOf[desc] = cu;
}

void forcedFirst(const LambdaDescriptor * desc) noexcept
{
    if (__builtin_expect(!enabled(), 1)) return;
    if (!desc) return;
    ++g_forced[desc];
}

namespace {

/// Coarse RHS-cheapness classifier for a creation site's thunk body.
///
/// Walks the descriptor's bytecode from `codeOffset` to the first body
/// terminator (RETURN / TAIL_CALL / HALT), counting code-WORDS (the same
/// word-granular scan the retired TT-1 body-size instrument used — it
/// treats data words as words too, an intentional over-count so the bound
/// is conservative). A body is "cheap / eagerly-evaluable" iff:
///   (1) it is short (≤ kCheapWords words), AND
///   (2) it contains NO nested thunk/closure creation and NO call/apply
///       (which could allocate, recurse, or run unbounded work).
///
/// (2) is the load-bearing test: an eagerly-evaluated RHS must have
/// BOUNDED cost and no laziness of its own. Because the scan is word-
/// granular, a data word can occasionally alias a call/make opcode — that
/// only makes us classify MORE bodies as heavy (never fewer), the safe
/// direction for "is this eagerly-evaluable" (we never over-claim cheap).
struct Cheapness { bool cheap; uint32_t words; };

constexpr uint32_t kCheapWords = 8;  // ≤ 8 words = "small" per the sweep's coarse bound

Cheapness classifyBody(const LambdaDescriptor * desc,
                       const CompilationUnit * cu) noexcept
{
    if (!cu || desc->codeOffset >= cu->code.size())
        return { false, 0 };
    uint32_t words = 0;
    bool heavy = false;
    for (uint32_t p = desc->codeOffset;
         p < cu->code.size() && words < 100000; ++p) {
        Op o = decodeOp(cu->code[p]);
        ++words;
        // Nested laziness / calls / allocation of aggregates ⇒ heavy.
        // Anything that creates a thunk/closure, calls, or applies is
        // NOT a bounded-cost eager RHS.  (if-chain, not a switch, to avoid
        // -Werror=switch-enum's all-86-opcodes requirement.)
        if (o == OP_MAKE_THUNK || o == OP_MAKE_CLOSURE
            || o == OP_CALL || o == OP_CALL_N
            || o == OP_TAIL_CALL || o == OP_TAIL_CALL_N)
            heavy = true;
        if (o == OP_RETURN || o == OP_R_RETURN || o == OP_HALT
            || o == OP_TAIL_CALL || o == OP_TAIL_CALL_N)
            break;
    }
    bool cheap = !heavy && words <= kCheapWords;
    return { cheap, words };
}

/// Map a force-rate in [0,1] to one of 12 buckets:
///   0 = exactly 0%   (never forced)
///   1..10 = (0%,10%], (10%,20%], … , (90%,100%)
///   11 = exactly 100% (always forced)
int rateBucket(double rate) noexcept
{
    if (rate <= 0.0) return 0;
    if (rate >= 1.0) return 11;
    int b = 1 + (int)(rate * 10.0);   // (0,0.1]→1 … (0.9,1.0)→10
    if (b < 1) b = 1;
    if (b > 10) b = 10;
    return b;
}

const char * bucketLabel(int b) noexcept
{
    static const char * labels[12] = {
        "  0%   (never)", "  0-10%", " 10-20%", " 20-30%", " 30-40%",
        " 40-50%", " 50-60%", " 60-70%", " 70-80%", " 80-90%",
        " 90-100%", "100%   (always)"
    };
    return labels[b];
}

}  // namespace

void dumpReport() noexcept
{
    if (__builtin_expect(!enabled(), 1)) return;
    // Self-gate on activity (mirrors partrace::dumpReport): runRootExpr is
    // called recursively by primop-bytecode installers, so this can fire on
    // every module exit. Counters are process-global + never reset, so the
    // LAST printed report per process is authoritative. Suppress empty ones.
    if (g_created.empty() && g_forced.empty()) return;

    // Aggregate over all sites that CREATED at least one thunk. Sites that
    // only appear in g_forced (thunk not made via OP_MAKE_THUNK) are
    // reported separately as a diagnostic and excluded from the per-site
    // force-rate distribution (their creation count is unknown here).
    uint64_t totalCreated = 0, totalForced = 0;
    uint64_t createdAtCreationSites = 0;     // created-count over sites w/ created>0
    uint64_t forcedAtCreationSites = 0;      // first-forces attributable to those sites

    // Histogram: created-weighted mass per force-rate bucket.
    uint64_t bucketCreated[12] = {};
    uint64_t bucketSites[12]   = {};

    // Aggregate A: of all NEVER-forced thunks (created - forced per site,
    // summed), how many are at LOW-force-rate sites (site rate < 10%, a
    // per-site classifier would correctly keep them lazy) vs MIXED sites
    // (rate 10-90%, ambiguous) vs HIGH sites (rate ≥ 90%, would wrongly
    // speculate — the "wasted speculation on the tail" mass)?
    uint64_t neverForcedTotal   = 0;   // Σ max(created-forced, 0)
    uint64_t neverAtLowRate     = 0;   // never-forced mass at sites rate < 10%
    uint64_t neverAtMixedRate   = 0;   // 10% ≤ rate < 90%
    uint64_t neverAtHighRate    = 0;   // rate ≥ 90%

    // Aggregate B: of the ALWAYS-forced mass (created at sites rate > 90%),
    // how much has a STATICALLY-CHEAP (bounded, non-lazy) RHS — the safely
    // eager-evaluable fraction — vs a heavy RHS?
    uint64_t alwaysForcedMass   = 0;   // created at sites rate > 90%
    uint64_t alwaysCheapMass    = 0;   // …of which RHS is cheap
    uint64_t alwaysHeavyMass    = 0;

    for (const auto & [desc, created] : g_created) {
        uint64_t forced = 0;
        if (auto it = g_forced.find(desc); it != g_forced.end())
            forced = it->second;
        // Clamp: a site can (rarely) show forced > created if a descriptor
        // is shared by thunks made both via OP_MAKE_THUNK and elsewhere.
        // Cap the rate at 1.0 so the bucketing stays well-defined.
        double rate = created > 0
            ? (double)forced / (double)created : 0.0;
        if (rate > 1.0) rate = 1.0;

        createdAtCreationSites += created;
        forcedAtCreationSites  += std::min(forced, created);

        int b = rateBucket(rate);
        bucketCreated[b] += created;
        bucketSites[b]   += 1;

        uint64_t never = created > forced ? created - forced : 0;
        neverForcedTotal += never;
        if (rate < 0.10)      neverAtLowRate  += never;
        else if (rate < 0.90) neverAtMixedRate += never;
        else                  neverAtHighRate += never;

        if (rate > 0.90) {
            alwaysForcedMass += created;
            const CompilationUnit * cu = nullptr;
            if (auto it = g_cuOf.find(desc); it != g_cuOf.end()) cu = it->second;
            Cheapness c = classifyBody(desc, cu);
            if (c.cheap) alwaysCheapMass += created;
            else         alwaysHeavyMass += created;
        }
    }

    // Grand totals (include the elsewhere-created diagnostic).
    for (const auto & [d, c] : g_created) { (void)d; totalCreated += c; }
    for (const auto & [d, f] : g_forced)  { (void)d; totalForced  += f; }

    // Sites forced but never seen created here (made outside OP_MAKE_THUNK).
    uint64_t elsewhereForced = 0;
    size_t elsewhereSites = 0;
    for (const auto & [desc, forced] : g_forced) {
        if (g_created.find(desc) == g_created.end()) {
            elsewhereForced += forced;
            ++elsewhereSites;
        }
    }

    std::fprintf(stderr,
        "\n=== v3 PER-SITE FORCE-RATE HISTOGRAM (NIX_V3_FORCERATE_TRACE) ===\n"
        "  site = OP_MAKE_THUNK creation location (LambdaDescriptor*).\n"
        "  force-rate = first-forces / thunks-created, per site.\n"
        "  byte-id neutral: drvPath ON == OFF (counters only).\n"
        "\n"
        "  TOTALS (cross-check the 62-68%%-never-forced aggregate):\n"
        "    OP_MAKE_THUNK creation sites            : %zu\n"
        "    thunks created (at OP_MAKE_THUNK sites) : %llu\n"
        "    first-forces attributable to those      : %llu\n"
        "    aggregate force-rate                    : %.4f  (never-forced = %.2f%%)\n"
        "    [all descriptors] total created=%llu forced=%llu\n"
        "    [forced-but-not-created-here] sites=%zu forces=%llu\n"
        "      (thunks made outside OP_MAKE_THUNK: App-lazies / formal-\n"
        "       wrappers / bridge thunks — excluded from the per-site dist)\n"
        "\n"
        "  PER-SITE FORCE-RATE HISTOGRAM (created-weighted):\n"
        "    bucket            sites        created    %%of-created\n",
        g_created.size(),
        (unsigned long long)createdAtCreationSites,
        (unsigned long long)forcedAtCreationSites,
        createdAtCreationSites > 0
            ? (double)forcedAtCreationSites / (double)createdAtCreationSites : 0.0,
        createdAtCreationSites > 0
            ? 100.0 * (double)(createdAtCreationSites - forcedAtCreationSites)
                    / (double)createdAtCreationSites : 0.0,
        (unsigned long long)totalCreated,
        (unsigned long long)totalForced,
        elsewhereSites,
        (unsigned long long)elsewhereForced);

    for (int b = 0; b < 12; ++b) {
        double pct = createdAtCreationSites > 0
            ? 100.0 * (double)bucketCreated[b] / (double)createdAtCreationSites : 0.0;
        // A crude inline bar (each '#' ≈ 2%).
        char bar[52];
        int nb = (int)(pct / 2.0);
        if (nb > 50) nb = 50;
        for (int i = 0; i < nb; ++i) bar[i] = '#';
        bar[nb] = '\0';
        std::fprintf(stderr,
            "   %-16s %8llu  %13llu   %6.2f%%  %s\n",
            bucketLabel(b),
            (unsigned long long)bucketSites[b],
            (unsigned long long)bucketCreated[b],
            pct, bar);
    }

    // Bimodality read: mass concentrated at the two extreme buckets
    // (0% and 100%) vs spread across the middle. Report the split.
    uint64_t extremeMass = bucketCreated[0] + bucketCreated[11];
    uint64_t middleMass  = createdAtCreationSites - extremeMass;
    std::fprintf(stderr,
        "\n  BIMODALITY (created-weighted mass):\n"
        "    at extremes (0%% never + 100%% always) : %llu  (%.2f%%)\n"
        "    in the middle (any 0-100%% bucket)     : %llu  (%.2f%%)\n"
        "    mass at exactly 0%%   (never)          : %llu  (%.2f%%)\n"
        "    mass at exactly 100%% (always)         : %llu  (%.2f%%)\n",
        (unsigned long long)extremeMass,
        createdAtCreationSites > 0 ? 100.0*(double)extremeMass/(double)createdAtCreationSites : 0.0,
        (unsigned long long)middleMass,
        createdAtCreationSites > 0 ? 100.0*(double)middleMass/(double)createdAtCreationSites : 0.0,
        (unsigned long long)bucketCreated[0],
        createdAtCreationSites > 0 ? 100.0*(double)bucketCreated[0]/(double)createdAtCreationSites : 0.0,
        (unsigned long long)bucketCreated[11],
        createdAtCreationSites > 0 ? 100.0*(double)bucketCreated[11]/(double)createdAtCreationSites : 0.0);

    // Aggregate A: never-forced mass by the site's force-rate class.
    std::fprintf(stderr,
        "\n  NEVER-FORCED MASS by site force-rate (would a per-site "
        "classifier keep it lazy?):\n"
        "    total never-forced thunks             : %llu\n"
        "    at LOW-rate sites (<10%%, keep lazy ✓) : %llu  (%.2f%% of never)\n"
        "    at MIXED sites (10-90%%, ambiguous)    : %llu  (%.2f%% of never)\n"
        "    at HIGH-rate sites (≥90%%, mis-spec ✗) : %llu  (%.2f%% of never)\n",
        (unsigned long long)neverForcedTotal,
        (unsigned long long)neverAtLowRate,
        neverForcedTotal > 0 ? 100.0*(double)neverAtLowRate/(double)neverForcedTotal : 0.0,
        (unsigned long long)neverAtMixedRate,
        neverForcedTotal > 0 ? 100.0*(double)neverAtMixedRate/(double)neverForcedTotal : 0.0,
        (unsigned long long)neverAtHighRate,
        neverForcedTotal > 0 ? 100.0*(double)neverAtHighRate/(double)neverForcedTotal : 0.0);

    // Aggregate B: always-forced mass RHS-cheapness.
    std::fprintf(stderr,
        "\n  ALWAYS-FORCED MASS (sites rate >90%%) RHS static cheapness "
        "(coarse: ≤%u words, no nested MAKE/CALL):\n"
        "    always-forced created mass            : %llu\n"
        "    of which CHEAP RHS (safely eager)     : %llu  (%.2f%%)\n"
        "    of which HEAVY RHS                    : %llu  (%.2f%%)\n"
        "=== end per-site force-rate histogram ===\n\n",
        kCheapWords,
        (unsigned long long)alwaysForcedMass,
        (unsigned long long)alwaysCheapMass,
        alwaysForcedMass > 0 ? 100.0*(double)alwaysCheapMass/(double)alwaysForcedMass : 0.0,
        (unsigned long long)alwaysHeavyMass,
        alwaysForcedMass > 0 ? 100.0*(double)alwaysHeavyMass/(double)alwaysForcedMass : 0.0);

    std::fflush(stderr);
}

}  // namespace nix::v3::forcerate
