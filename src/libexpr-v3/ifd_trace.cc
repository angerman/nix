/// @file
/// Per-site IFD tracing instrument — implementation.
///
/// See ifd_trace.hh for the five signals, the byte-id-neutral correctness
/// contract, and the Rule-0 retirement criterion (delete once the Phase-1
/// IFD routing decision — GO-2 / GO-3 / KILL — is recorded).
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/ifd_trace.hh"

#include "v3/vm.hh"        // currentDispatchVM, VMState, CallFrame
#include "v3/closure.hh"   // Closure, LambdaDescriptor
#include "v3/alloc.hh"     // resolvePosSnapshot / PosSnapshot
#include "v3/bytecode.hh"  // ifdProbeKindName / IfdProbeKind

#include <cstdio>
#include <cstdlib>
#include <exception>   // std::uncaught_exceptions
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nix::v3::ifdtrace {

namespace {

/// One trace record per realise. Plain POD-ish; process-global, append-only,
/// never reset (cumulative across the re-entrant installer sub-evals + the
/// user eval — the last report per process is authoritative). v3 eval is
/// single-threaded; no synchronisation.
struct Rec
{
    uint64_t    seq;
    uint8_t     kind;
    uint32_t    depthAtEntry;   // 0 = independent-candidate; >0 = nested (must-sequence)
    uint64_t    parentSeq;      // seq of the enclosing realise (0 if depth 0)
    uint64_t    nanos;          // per-realise wall-time
    bool        threw;          // the realise threw (build failed / no builder — sandbox)
    std::string arg;            // derivation / store-path string realised
    std::string site;           // nearest source file:line:col of the triggering scope
};

std::vector<Rec> g_records;

/// Monotonic realise sequence + current realise NESTING depth. Incremented on
/// RealiseScope entry, decremented on exit — a realise begun while depth>0 is
/// nested inside (data-dependent on) the realise(s) already in flight.
uint64_t              g_seq   = 0;
thread_local uint32_t g_depth = 0;
/// Stack of in-flight realise seqs so a nested record can name its parent.
thread_local std::vector<uint64_t> g_inflight;

/// Nearest attribution of the currently-executing scope: walk the VM frame
/// stack from the top down, returning the first frame whose closure descriptor
/// carries a non-null posHandle, formatted "file:line:col". IFD realises are
/// frequently reached from a primop / derivationStrict continuation whose top
/// frame is not a user-source lambda (posHandle 0), so as a fallback we return
/// the nearest descriptor NAME ("fn:<name>"). Empty string only if no VM /
/// no attributable frame at all (the realise arg then remains the identifier).
std::string captureSite()
{
    VMState * vm = currentDispatchVM();
    if (!vm) return {};
    std::string nameFallback;
    for (size_t i = vm->frames.size(); i-- > 0;) {
        const Closure * c = vm->frames[i].closure;
        if (!c || !c->desc) continue;
        const PosSnapshot * ps = resolvePosSnapshot(c->desc->posHandle);
        if (ps && !ps->file.empty()) {
            std::string s = ps->file;
            s += ':';
            s += std::to_string((unsigned long long) ps->line);
            s += ':';
            s += std::to_string((unsigned long long) ps->column);
            return s;
        }
        if (nameFallback.empty()) {
            std::string_view nm = c->desc->contextualName;
            if (nm.empty()) nm = c->desc->name;
            if (!nm.empty()) nameFallback = "fn:" + std::string(nm);
        }
    }
    return nameFallback;
}

}  // namespace

bool enabled() noexcept
{
    // Retirement criterion (see ifd_trace.hh): delete this gate + the module
    // once the Phase-1 IFD routing verdict is recorded. Diagnostic-only,
    // byte-id neutral — never affects eval results. lint:allow-getenv (cold:
    // static-const init, one read per process).
    static const bool s_on = std::getenv("NIX_V3_IFD_TRACE") != nullptr;
    return s_on;
}

RealiseScope::RealiseScope(uint8_t kind, std::string arg)
    : seq_(++g_seq)
    , kind_(kind)
    , depthAtEntry_(g_depth)
    , parentSeq_(g_inflight.empty() ? 0 : g_inflight.back())
    , arg_(std::move(arg))
    , site_(captureSite())
    , t0_(std::chrono::steady_clock::now())
    , excBase_(std::uncaught_exceptions())
{
    ++g_depth;
    g_inflight.push_back(seq_);
}

RealiseScope::~RealiseScope()
{
    if (g_depth > 0) --g_depth;
    if (!g_inflight.empty()) g_inflight.pop_back();
    // Never let record-keeping throw during stack unwind (the realise may be
    // throwing right now — a build failure in the sandbox is the common case).
    try {
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now() - t0_)
                      .count();
        g_records.push_back(Rec{
            seq_, kind_, depthAtEntry_, parentSeq_, (uint64_t) ns,
            std::uncaught_exceptions() > excBase_, std::move(arg_),
            std::move(site_)});
    } catch (...) {
        // out-of-memory recording a diagnostic — drop it silently.
    }
}

void dumpReport() noexcept
{
    if (__builtin_expect(!enabled(), 1)) return;
    if (g_records.empty()) return;

    // ---- summary aggregates -------------------------------------------------
    uint64_t nTotal = g_records.size();
    uint64_t nIndep = 0, nNested = 0;      // depth-0 (candidate) vs depth>0
    uint64_t nThrew = 0;
    uint64_t nsIndep = 0, nsNested = 0;    // wall-time by class
    uint64_t nsTotal = 0;
    // per-kind counts + per-kind independent-candidate wall
    uint64_t kindCount[16] = {};
    uint64_t kindIndepNs[16] = {};
    // repeat detection: distinct realise args, and count of realises whose arg
    // recurs (>1 realise of the SAME arg within this process = Phase-3 target).
    std::unordered_map<std::string, uint64_t> argCount;

    for (const auto & r : g_records) {
        nsTotal += r.nanos;
        if (r.threw) ++nThrew;
        if (r.depthAtEntry == 0) { ++nIndep;  nsIndep  += r.nanos; }
        else                     { ++nNested; nsNested += r.nanos; }
        uint8_t k = r.kind < 16 ? r.kind : 0;
        ++kindCount[k];
        if (r.depthAtEntry == 0) kindIndepNs[k] += r.nanos;
        ++argCount[r.arg];
    }
    uint64_t distinctArgs = argCount.size();
    uint64_t repeatRealises = 0;   // realises beyond the first for each arg
    for (const auto & [a, c] : argCount) { (void) a; if (c > 1) repeatRealises += (c - 1); }

    auto ms = [](uint64_t ns) { return (double) ns / 1e6; };
    auto pct = [](uint64_t a, uint64_t b) { return b ? 100.0 * (double) a / (double) b : 0.0; };

    std::fprintf(stderr,
        "\n=== v3 IFD TRACE (NIX_V3_IFD_TRACE) — Phase-1 routing instrument ===\n"
        "  byte-id neutral: drvPath ON == OFF (reads/counts only).\n"
        "  one line per realise; independence = realise NESTING depth at entry.\n"
        "  depth 0        = independent-candidate (no realise in flight; Phase-2 overlap target)\n"
        "  depth>0 nested = data-dependent on its parent realise (MUST sequence)\n"
        "  cache/build: ms is the built-vs-served proxy (fast=served/substituted/cached,\n"
        "    slow=real build); definitive split needs the deploy store /\n"
        "    --option profile-import-from-derivation true. This run should be cache-OFF.\n"
        "\n"
        "  seq  kind           depth   ms       threw  site  ::  arg\n");

    for (const auto & r : g_records) {
        std::fprintf(stderr,
            "  %-4llu %-14s %s%-4u %8.2f  %-5s  %s  ::  %s\n",
            (unsigned long long) r.seq,
            ifdProbeKindName(r.kind),
            r.depthAtEntry == 0 ? "top " : "nest",
            r.depthAtEntry,
            ms(r.nanos),
            r.threw ? "THREW" : "-",
            r.site.empty() ? "<no-pos>" : r.site.c_str(),
            r.arg.empty() ? "<none>" : r.arg.c_str());
        if (r.depthAtEntry > 0)
            std::fprintf(stderr, "         └─ nested under realise seq=%llu\n",
                (unsigned long long) r.parentSeq);
    }

    std::fprintf(stderr,
        "\n  --- SUMMARY (the Phase-1 routing numbers) ---\n"
        "  realises total                         : %llu  (threw=%llu — build unavailable)\n"
        "  wall in realise (sum, per-realise)     : %.2f ms\n"
        "  INDEPENDENT-CANDIDATES (depth 0)       : %llu  (%.1f%%)   wall %.2f ms (%.1f%% of realise wall)\n"
        "  MUST-SEQUENCE (nested, depth>0)        : %llu  (%.1f%%)   wall %.2f ms (%.1f%% of realise wall)\n"
        "     [Phase-2 upper bound = independent-candidate wall; a per-realise\n"
        "      background build could hide it IF the candidates are truly\n"
        "      independent — depth-0 is an upper bound, sequential depth-0\n"
        "      realises may still depend through ordinary eval between them.]\n"
        "  distinct realise args                  : %llu\n"
        "  repeat realises (same arg, >1×)        : %llu  (%.1f%% of total)  [Phase-3 within-run target]\n"
        "     [Phase-3 cross-eval repeat rate = diff this arg set against a\n"
        "      second process's trace — see the git-noted deploy recipe.]\n"
        "  per-kind (count | independent-candidate wall ms):\n",
        (unsigned long long) nTotal, (unsigned long long) nThrew,
        ms(nsTotal),
        (unsigned long long) nIndep, pct(nIndep, nTotal), ms(nsIndep), pct(nsIndep, nsTotal),
        (unsigned long long) nNested, pct(nNested, nTotal), ms(nsNested), pct(nsNested, nsTotal),
        (unsigned long long) distinctArgs,
        (unsigned long long) repeatRealises, pct(repeatRealises, nTotal));

    for (int k = 1; k < (int) kIfdProbeKindCount; ++k)
        if (kindCount[k] > 0)
            std::fprintf(stderr, "    %-14s %llu | %.2f ms\n",
                ifdProbeKindName(static_cast<uint8_t>(k)),
                (unsigned long long) kindCount[k], ms(kindIndepNs[k]));

    std::fprintf(stderr, "=== end v3 IFD trace ===\n\n");
    std::fflush(stderr);
}

}  // namespace nix::v3::ifdtrace
