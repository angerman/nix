/// @file
/// Live-fraction tracer — Stage 6 SPIKE for "ditch Boehm" project.
///
/// See include/v3/live_trace.hh for the API + purpose.  This file
/// implements the transitive mark-from-roots pass using the Stage 3
/// `walkAllV3Roots` infrastructure as the root-set provider.
///
/// The walker is intentionally a separate code path from gc.cc's
/// Scavenger (which is nursery-focused, has forwarding semantics,
/// and post-walk barriers).  This tracer:
///   - Does NOT mutate any heap state (no forwarding, no barriers).
///   - Walks the FULL transitive closure (not just nursery objects).
///   - Counts unique reached objects per type + bytes.
///
/// One-shot, end-of-run.  Cost is O(reachable) per type.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/live_trace.hh"
#include "v3/precise_root.hh"
#include "v3/alloc.hh"
#include "v3/value.hh"
#include "v3/closure.hh"
#include "v3/vm.hh"           // activeVMStack()
#include "v3/primop.hh"       // importCacheBytecodeBytes / CuCount / ResultCount
#include "v3/disk_cache.hh"   // disk_cache::approxResidentBytes (BC-cache bucket)

#include <gc/gc.h>            // GC_get_heap_size / GC_get_free_bytes (Boehm-live)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>           // getpid() for default periodic-out path
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Resident-RSS read for the memory-bucket report (decomposes RESIDENT
// RSS, never peak ru_maxrss).  Self-contained per the heap_trace.cc
// precedent (limits.cc::currentRssBytes is anon-namespace, not linkable).
#if defined(__APPLE__)
#  include <mach/mach.h>      // task_info / MACH_TASK_BASIC_INFO
#endif

namespace nix::v3 {

namespace {

/// Gray-list entry: a discovered pointer not yet walked.  Tagged
/// with the kind so the worklist drain knows which walk* function
/// to invoke.
enum GrayKind : uint8_t {
    GK_CLOSURE  = 0,
    GK_THUNK    = 1,
    GK_BINDINGS = 2,
    GK_LIST     = 3,
    GK_PAIR     = 4,
};

struct Gray { void * ptr; GrayKind kind; };

/// Per-type live counters.  Two-axis: COUNT (unique objects reached)
/// and BYTES (sum of object sizes, FAM-aware).
// DIAG-2 (2026-05-29 evening, per DIAGNOSTIC_AUDIT §6.2 +
// NIX_MEMORY_PROFILER_DESIGN §5.1): per-source-position live-bytes
// rollup.  Aggregates bytes by posHandle during walkBindings +
// walkThunk so we can answer "lib/fixed-points.nix:95 retains 85 MB"
// rather than the Tag-level "84 % Bindings".
struct LivePosEntry
{
    uint64_t bindingsBytes = 0;
    uint64_t thunksBytes = 0;
    uint32_t bindingsCount = 0;
    uint32_t thunksCount = 0;
};

struct LiveCounters
{
    size_t closures = 0,   bytesClosures = 0;
    size_t thunks   = 0,   bytesThunks   = 0;
    size_t bindings = 0,   bytesBindings = 0;
    size_t lists    = 0,   bytesLists    = 0;
    size_t pairs    = 0,   bytesPairs    = 0;
    size_t slotsDereffed = 0;  // edges followed via Tag::Slot
    // DIAG-2: per-posHandle aggregation (allocated only when
    // NIX_V3_LIVE_POS_ATTR=1 — empty in the default case).
    std::unordered_map<uint32_t, LivePosEntry> liveByPos;

    // Arena-dereg audit counters (per
    // ARENA_DEREGISTRATION_DESIGN_2026-05-27 §4 §4.1-4.2):
    // Tag::External / String / Path payloads in v3 cells are
    // candidate Boehm-managed pointers that arena dereg must
    // either register separately OR confirm absent.
    // Sampled at every visited Value during the live-trace walk.
    size_t externalCount = 0;
    size_t stringCount   = 0;
    size_t pathCount     = 0;
    // First N pointer addresses per non-zero tag for follow-up
    // audit; bounded so output stays small.
    static constexpr size_t kAuditSampleCap = 16;
    std::vector<void *> externalSamples;
    std::vector<const char *> stringSamples;
    std::vector<const char *> pathSamples;
};

/// Transitive mark-from-roots tracer.  Pull pointers from
/// walkAllV3Roots into a worklist; drain the worklist visiting each
/// pointer's outgoing edges; count unique reached objects.
class LiveTracer : public RootVisitor
{
public:
    LiveCounters counts;

    // DIAG-2: per-posHandle live-bytes aggregation gate.  Read once
    // per process via inline-const-bool (mirrors bindingsOriginEnabled
    // pattern at alloc.hh:2702 — magic-static-guard-free hot path).
    inline static const bool livePosAttrEnabled =
        std::getenv("NIX_V3_LIVE_POS_ATTR") != nullptr;

    void visitClosure(Closure   * & p) override { enqueue(p, GK_CLOSURE); }
    void visitThunk  (Thunk     * & p) override { enqueue(p, GK_THUNK); }
    void visitBindings(Bindings * & p) override { enqueue(p, GK_BINDINGS); }
    void visitList   (ListVec   * & p) override { enqueue(p, GK_LIST); }
    void visitPair   (ValuePair * & p) override { enqueue(p, GK_PAIR); }
    void visitSlot   (Value     * & p) override
    {
        if (!p) return;
        // Dedup: multiple Tag::Slot Values may alias the same cell.
        // The cell itself is NOT counted (it's a Value * into another
        // allocation that's already counted via its own type).  But
        // the Value at the cell may carry a payload we haven't seen.
        if (cellsWalked.insert(p).second) {
            ++counts.slotsDereffed;
            auditAndVisit(*p);
        }
    }

    /// Arena-dereg audit hook: count Tag::External / String / Path
    /// payloads in any Value reached during the walk.  Tally goes
    /// into LiveCounters.externalCount / stringCount / pathCount.
    /// First N addresses per non-zero tag are stored for follow-up
    /// investigation.
    ///
    /// Called from each walk* function instead of `visitValue`
    /// directly.  Drops back to the base `visitValue` for the
    /// pointer dispatch (the inspection is additive, not replacing).
    void auditAndVisit(Value & v) noexcept
    {
        // -Werror=switch-enum: explicit no-op for every other Tag.
        switch (v.tag()) {
        case Tag::External:
            ++counts.externalCount;
            if (counts.externalSamples.size() < LiveCounters::kAuditSampleCap)
                counts.externalSamples.push_back(v.payload.raw);
            break;
        case Tag::String:
            ++counts.stringCount;
            if (counts.stringSamples.size() < LiveCounters::kAuditSampleCap)
                counts.stringSamples.push_back(v.payload.str);
            break;
        case Tag::Path:
            ++counts.pathCount;
            if (counts.pathSamples.size() < LiveCounters::kAuditSampleCap)
                counts.pathSamples.push_back(v.payload.path);
            break;
        case Tag::Uninitialized:
        case Tag::Int:
        case Tag::Float:
        case Tag::Bool:
        case Tag::Null:
        case Tag::Attrs:
        case Tag::List:
        case Tag::Closure:
        case Tag::Thunk:
        case Tag::PrimOp:
        case Tag::PrimOpApp:
        case Tag::App:
        case Tag::App3:
        case Tag::Blackhole:
        case Tag::Slot:
            break;
        }
        visitValue(v);
    }

    /// Drain the worklist.  Each iteration pops one gray object and
    /// walks its outgoing edges (via visitValue).  visitValue enqueues
    /// newly-seen pointers; cycles are handled by `seen`.
    void drain()
    {
        while (!worklist.empty()) {
            Gray g = worklist.back();
            worklist.pop_back();
            switch (g.kind) {
            case GK_CLOSURE:  walkClosure (static_cast<Closure  *>(g.ptr)); break;
            case GK_THUNK:    walkThunk   (static_cast<Thunk    *>(g.ptr)); break;
            case GK_BINDINGS: walkBindings(static_cast<Bindings *>(g.ptr)); break;
            case GK_LIST:     walkList    (static_cast<ListVec  *>(g.ptr)); break;
            case GK_PAIR:     walkPair    (static_cast<ValuePair *>(g.ptr)); break;
            }
        }
    }

private:
    std::unordered_set<void *> seen;
    std::unordered_set<void *> cellsWalked;
    std::vector<Gray> worklist;

    template <typename P>
    void enqueue(P * p, GrayKind k)
    {
        if (!p) return;
        if (!seen.insert(p).second) return;  // already marked
        worklist.push_back({p, k});
    }

    /// Walk a Closure's outgoing edges.  Mirrors gc.cc Scavenger::
    /// walkClosure modulo the recordLiveTenured / forwarding calls
    /// (we just count + recurse).
    void walkClosure(Closure * c)
    {
        ++counts.closures;
        counts.bytesClosures += sizeof(Closure)
                              + size_t(c->nUpvalues) * sizeof(Value);
        if (c->capturedWiths)
            enqueue(c->capturedWiths, GK_LIST);
        for (uint16_t i = 0; i < c->nUpvalues; ++i)
            auditAndVisit(c->upvalues[i]);
    }

    /// Walk a Thunk.  State-dependent: Suspended/Native/Blackhole have
    /// nUpvalues tail values; Evaluated has the cached `evaluated`
    /// Value; Bridge has only the bridgeSrc (TW pointer, not v3 heap).
    void walkThunk(Thunk * t)
    {
        ++counts.thunks;
        size_t bytes;
        switch (t->state) {
        case ThunkState::Suspended:
        case ThunkState::Native:
        case ThunkState::Blackhole:
            bytes = sizeof(Thunk) + sizeof(Value) * t->nUpvalues;
            break;
        case ThunkState::Evaluated:
            bytes = sizeof(Thunk);
            break;
        }
        counts.bytesThunks += bytes;
        // DIAG-2: per-posHandle attribution.  Only Suspended/Blackhole
        // have a valid suspended.desc->posHandle; Evaluated/Native/Bridge
        // get attributed to posHandle=0 (unknown).
        if (livePosAttrEnabled) {
            uint32_t ph = 0;
            if ((t->state == ThunkState::Suspended
                 || t->state == ThunkState::Blackhole)
                && t->suspended.desc)
                ph = t->suspended.desc->posHandle;
            auto & e = counts.liveByPos[ph];
            e.thunksBytes += bytes;
            ++e.thunksCount;
        }

        // cell / shapeCell point INTO another allocation (Bindings
        // entry, standalone cell).  We don't count them here; the
        // owning Bindings is already counted (or will be) via its
        // own enqueue.  But we DO walk through the cell content
        // because it may reach objects not otherwise rooted.
        if (t->cell && cellsWalked.insert(t->cell).second)
            auditAndVisit(*t->cell);
        if (t->shapeCell && cellsWalked.insert(t->shapeCell).second)
            auditAndVisit(*t->shapeCell);

        switch (t->state) {
        case ThunkState::Suspended:
        case ThunkState::Blackhole:
            if (t->suspended.capturedWiths)
                enqueue(t->suspended.capturedWiths, GK_LIST);
            for (uint16_t i = 0; i < t->nUpvalues; ++i)
                auditAndVisit(t->tail[i]);
            break;
        case ThunkState::Evaluated:
            auditAndVisit(t->evaluated);
            break;
        case ThunkState::Native:
            for (uint16_t i = 0; i < t->nUpvalues; ++i)
                auditAndVisit(t->tail[i]);
            break;
        }
    }

    void walkBindings(Bindings * b)
    {
        ++counts.bindings;
        const size_t bytes = sizeof(Bindings)
                           + sizeof(Bindings::Entry) * b->size;
        counts.bytesBindings += bytes;
        // DIAG-2: per-posHandle attribution.  Look up the Bindings'
        // construction posHandle from bindingsOriginTable (already
        // populated by recordBindingsOrigin / bindingsAllocSiteRecord
        // at every Alloc::allocBindings under NIX_V3_BINDINGS_ATTR=1
        // OR NIX_V3_DBG_BINDINGS_ORIGIN=1 — the gate is set externally).
        //
        // Phase 2 (2026-05-29 evening): if origin table has posHandle=0
        // OR no entry, FALL BACK to the FIRST ENTRY's posHandle.
        // Per-entry pos is populated by emit.cc for every attrset
        // op (#752 inline-pos in Bindings::Entry); even unrecorded
        // mergeBindings allocs end up with per-entry positions from
        // the merged sources.  This converts the 82.8 % unknown
        // bucket into source-attributed bytes whenever entries[0]
        // has a non-zero pos.
        if (livePosAttrEnabled) {
            uint32_t ph = 0;
            auto & tbl = bindingsOriginTable();
            auto it = tbl.find(b);
            if (it != tbl.end()) ph = it->second.posHandle;
            // Fallback A: first entry's pos.
            if (ph == 0 && b->size > 0) ph = b->entries[0].pos;
            // Fallback B: scan entries for ANY non-zero pos.  Some
            // entries have pos=0 (e.g., compiler-generated names);
            // scan the first few to catch the first real source pos.
            if (ph == 0 && b->size > 0) {
                const uint32_t scanLimit =
                    b->size < 8 ? b->size : 8;
                for (uint32_t i = 1; i < scanLimit; ++i) {
                    if (b->entries[i].pos != 0) {
                        ph = b->entries[i].pos;
                        break;
                    }
                }
            }
            auto & e = counts.liveByPos[ph];
            e.bindingsBytes += bytes;
            ++e.bindingsCount;
        }
        for (uint32_t i = 0; i < b->size; ++i)
            auditAndVisit(b->entries[i].value);
        // Chain bindings: walk parent.  Each segment of the chain
        // contributes its own bytes (overlay-only `size`); the
        // chain head sees overlay + parent transitively.  Sorted
        // bindings always have parent == nullptr.
        if (b->parent)
            enqueue(const_cast<Bindings *>(b->parent), GK_BINDINGS);
    }

    void walkList(ListVec * l)
    {
        ++counts.lists;
        counts.bytesLists += sizeof(ListVec) + sizeof(Value) * l->size;
        for (uint32_t i = 0; i < l->size; ++i)
            auditAndVisit(l->elems[i]);
    }

    void walkPair(ValuePair * p)
    {
        ++counts.pairs;
        counts.bytesPairs += sizeof(ValuePair);
        auditAndVisit(p->left);
        auditAndVisit(p->right);
        auditAndVisit(p->evaluated);
        auditAndVisit(p->third);  // 2026-05-30 Tag::App3 arg2
    }
};

/// Pretty-print bytes with K/M/G suffix.  Mirrors NIX_VM_STATS output
/// conventions (no thousands separator, two decimals for fractional).
void fmtBytes(size_t bytes, char * out, size_t out_sz)
{
    if (bytes >= (1ULL << 30))
        std::snprintf(out, out_sz, "%.2f GB", double(bytes) / (1ULL << 30));
    else if (bytes >= (1ULL << 20))
        std::snprintf(out, out_sz, "%.2f MB", double(bytes) / (1ULL << 20));
    else if (bytes >= (1ULL << 10))
        std::snprintf(out, out_sz, "%.2f KB", double(bytes) / (1ULL << 10));
    else
        std::snprintf(out, out_sz, "%zu B", bytes);
}

/// Pretty-print a percent value, or "—" when divisor is zero.
void fmtPct(size_t live, size_t total, char * out, size_t out_sz)
{
    if (total == 0)
        std::snprintf(out, out_sz, "—");
    else
        std::snprintf(out, out_sz, "%.1f%%",
            100.0 * double(live) / double(total));
}

} // namespace

// (dumpV3BridgeRetention retired — TW_VALUE_ERADICATION F4, 2026-06-02.)

void dumpV3LiveFraction() noexcept
{
    static const bool s_enabled =
        std::getenv("NIX_V3_LIVE_TRACE") != nullptr;
    if (!s_enabled) return;
    // Fire exactly once per process.  run.cc may be re-entered via
    // bytecode-primop install passes (multi-runRootExpr); dumping
    // the trace per-pass produces N near-identical reports.  The
    // FINAL pass is the meaningful one (largest arena), but we
    // can't know in advance which pass is last.  Mitigation: dump
    // on every pass, but skip subsequent passes whose arena hasn't
    // grown vs the previous dump (so we still see the largest).
    // Implementation: track largest arena bytes seen so far + only
    // print when the current call exceeds it.
    static std::atomic<size_t> s_lastArena{0};
    const size_t curArena =
          allocStats().bytesClosures + allocStats().bytesThunks
        + allocStats().bytesBindings + allocStats().bytesLists
        + allocStats().bytesPairs;
    // Allow re-dump if arena grew by more than 64 KB since the
    // previous dump — filters out small primop-install passes
    // (~3 KB arena each) while letting the real workload report.
    if (curArena < s_lastArena.load(std::memory_order_relaxed) + (1 << 16))
        return;
    s_lastArena.store(curArena, std::memory_order_relaxed);

    LiveTracer tr;

    // Drive root walk from the active VMState if any; otherwise from
    // global roots only (same shape as dumpAllV3Roots).  End-of-run
    // measurement with VMState already torn down gives a LOWER BOUND
    // on live-fraction (only persistent global roots remain) — this
    // is honest and useful: it shows how much of arena is residual
    // (persistent, can't be freed even at end) vs transient (could
    // have been freed earlier during eval by a precise GC).
    bool walkedActiveVM = false;
    const auto & stack = activeVMStack();
    if (!stack.empty() && stack.back()) {
        walkAllV3Roots(*stack.back(), tr);
        walkedActiveVM = true;
    } else {
        walkGlobalV3Roots(tr);
    }

    // Drain the worklist.  Transitive closure of all reachable
    // pointers from the root set.
    tr.drain();

    const auto & stats = allocStats();
    char liveB[32], allocB[32], pctC[8];
    char buf1[32], buf2[32];

    std::fprintf(stderr,
        "\n"
        "=================== v3 LIVE-FRACTION TRACE ===================\n"
        "Reachable from precise roots (transitive closure).  Bytes\n"
        "ALLOCATED is the cumulative arena bump (allocStats counters);\n"
        "Bytes LIVE is the post-trace mark-from-roots count.  Their\n"
        "ratio bounds the achievable reclamation of a precise GC of\n"
        "the v3 arena.\n"
        "\n"
        "Root scope: %s\n"
        "\n"
        "                 LIVE-objs   LIVE-bytes     ALLOC-bytes    live%%\n",
        walkedActiveVM
            ? "active VMState + global (FULL coverage)"
            : "global only (VMState torn down — RESIDUAL lower-bound)"
    );

    auto row = [&](const char * label, size_t liveObjs,
                   size_t liveBytes, size_t allocBytes)
    {
        fmtBytes(liveBytes, buf1, sizeof(buf1));
        fmtBytes(allocBytes, buf2, sizeof(buf2));
        fmtPct(liveBytes, allocBytes, pctC, sizeof(pctC));
        std::fprintf(stderr,
            "  %-12s %10zu  %12s  %14s  %7s\n",
            label, liveObjs, buf1, buf2, pctC);
    };

    row("Closures",  tr.counts.closures,  tr.counts.bytesClosures,
                     stats.bytesClosures);
    row("Thunks",    tr.counts.thunks,    tr.counts.bytesThunks,
                     stats.bytesThunks);
    row("Bindings",  tr.counts.bindings,  tr.counts.bytesBindings,
                     stats.bytesBindings);
    row("Lists",     tr.counts.lists,     tr.counts.bytesLists,
                     stats.bytesLists);
    row("Pairs",     tr.counts.pairs,     tr.counts.bytesPairs,
                     stats.bytesPairs);

    size_t totalLive = tr.counts.bytesClosures + tr.counts.bytesThunks
                     + tr.counts.bytesBindings + tr.counts.bytesLists
                     + tr.counts.bytesPairs;
    size_t totalAlloc = stats.bytesClosures + stats.bytesThunks
                      + stats.bytesBindings + stats.bytesLists
                      + stats.bytesPairs;

    fmtBytes(totalLive, liveB, sizeof(liveB));
    fmtBytes(totalAlloc, allocB, sizeof(allocB));
    fmtPct(totalLive, totalAlloc, pctC, sizeof(pctC));

    std::fprintf(stderr,
        "  ----------------------------------------------------------\n"
        "  %-12s %10s  %12s  %14s  %7s\n",
        "TOTAL", "", liveB, allocB, pctC);

    size_t freeable = (totalAlloc > totalLive) ? (totalAlloc - totalLive) : 0;
    char freeableB[32];
    fmtBytes(freeable, freeableB, sizeof(freeableB));

    // Decision banner — pre-committed thresholds per
    // [[measure-twice-cut-once]].  ≥200 MB freeable arena bytes is
    // the SHIP threshold for the precise GC project; <50 MB is the
    // FALSIFICATION threshold (project pivots).
    const char * verdict;
    if (freeable >= (size_t(200) << 20))      verdict = "SHIP-GREEN";
    else if (freeable >= (size_t(50)  << 20)) verdict = "MARGINAL";
    else                                       verdict = "FALSIFIED";

    std::fprintf(stderr,
        "\n"
        "  Slots followed: %zu\n"
        "  Freeable arena bytes (alloc - live): %s\n"
        "  Verdict: %s (200 MB ship gate; <50 MB → pivot)\n",
        tr.counts.slotsDereffed, freeableB, verdict);

    // DIAG-2 (2026-05-29 evening, per DIAGNOSTIC_AUDIT §6.2 +
    // NIX_MEMORY_PROFILER_DESIGN §5.1): per-PosIdx live-bytes
    // top-N retainers.  Converts "84 % Bindings" from a Tag-level
    // pattern-match into "lib/fixed-points.nix:95 retains 85 MB"
    // (source-level).
    //
    // Sets the aggregator gate from `LiveTracer::livePosAttrEnabled`
    // (NIX_V3_LIVE_POS_ATTR=1); when unset, the map is empty and
    // this dump skips.
    if (!tr.counts.liveByPos.empty()) {
        // Pull entries into a sortable vector.
        struct Row {
            uint32_t posHandle;
            uint64_t totalBytes;
            uint64_t bindingsBytes;
            uint64_t thunksBytes;
            uint32_t bindingsCount;
            uint32_t thunksCount;
        };
        std::vector<Row> rows;
        rows.reserve(tr.counts.liveByPos.size());
        uint64_t grandTotal = 0;
        for (auto & [ph, e] : tr.counts.liveByPos) {
            const uint64_t tot = e.bindingsBytes + e.thunksBytes;
            grandTotal += tot;
            rows.push_back({ph, tot,
                e.bindingsBytes, e.thunksBytes,
                e.bindingsCount, e.thunksCount});
        }
        std::sort(rows.begin(), rows.end(),
            [](const Row & a, const Row & b) {
                return a.totalBytes > b.totalBytes;
            });
        const size_t N = std::min<size_t>(20, rows.size());
        std::fprintf(stderr,
            "\n"
            "  Per-PosIdx live-bytes top-%zu (of %zu unique posHandles, "
            "grandTotal=%.1f MB):\n"
            "                bytes   pct  binds(N)   thunks(N)  source\n",
            N, rows.size(), double(grandTotal) / 1e6);
        for (size_t i = 0; i < N; ++i) {
            const Row & r = rows[i];
            const double pct = grandTotal > 0
                ? 100.0 * double(r.totalBytes) / double(grandTotal)
                : 0.0;
            const PosSnapshot * ps = resolvePosSnapshot(r.posHandle);
            char buf[32];
            fmtBytes(r.totalBytes, buf, sizeof(buf));
            if (ps && !ps->file.empty()) {
                std::fprintf(stderr,
                    "    %12s %5.1f%%  %5u(%6u)  %5u(%6u)  %s:%u:%u\n",
                    buf, pct,
                    (unsigned)(r.bindingsBytes / 1024), r.bindingsCount,
                    (unsigned)(r.thunksBytes / 1024), r.thunksCount,
                    ps->file.c_str(), ps->line, ps->column);
            } else {
                std::fprintf(stderr,
                    "    %12s %5.1f%%  %5u(%6u)  %5u(%6u)  <posHandle=%u %s>\n",
                    buf, pct,
                    (unsigned)(r.bindingsBytes / 1024), r.bindingsCount,
                    (unsigned)(r.thunksBytes / 1024), r.thunksCount,
                    r.posHandle,
                    r.posHandle == 0 ? "unknown" : "no-snapshot");
            }
        }
    }

    // Arena-dereg audit report (per ARENA_DEREGISTRATION_DESIGN
    // §4.1 String/Path + §4.2 External audits).  These counts
    // tell the future Arena dereg session whether external
    // Boehm-managed pointers persist in v3 cells.  If all three
    // counters are 0, arena dereg is safe wrt these tags.
    std::fprintf(stderr,
        "\n"
        "  Arena-dereg audit (Tag classification of visited Values):\n"
        "    Tag::External : %12zu reached\n"
        "    Tag::String   : %12zu reached\n"
        "    Tag::Path     : %12zu reached\n",
        tr.counts.externalCount,
        tr.counts.stringCount,
        tr.counts.pathCount);
    if (tr.counts.externalCount > 0 && !tr.counts.externalSamples.empty()) {
        std::fprintf(stderr,
            "    External samples (first %zu):\n",
            tr.counts.externalSamples.size());
        for (void * p : tr.counts.externalSamples) {
            std::fprintf(stderr, "      payload.raw=%p\n", p);
        }
    }
    if (tr.counts.stringCount > 0 && !tr.counts.stringSamples.empty()) {
        // Strings + paths can be MASSIVELY duplicated (same `const
        // char *` shared across many Values).  Print sample addresses
        // + first ~24 chars for diagnostic.  De-duplicate addresses
        // before printing so the same pointer isn't shown 16 times.
        std::unordered_set<const char *> seenS;
        size_t shown = 0;
        std::fprintf(stderr,
            "    String samples (first up to %zu unique addrs):\n",
            tr.counts.stringSamples.size());
        for (const char * p : tr.counts.stringSamples) {
            if (!p || !seenS.insert(p).second) continue;
            char preview[28] = {0};
            std::snprintf(preview, sizeof(preview), "%s", p);
            // Truncate for readability — show first 24 chars.
            if (std::strlen(p) > 24) {
                preview[24] = '.'; preview[25] = '.'; preview[26] = '.';
                preview[27] = 0;
            }
            std::fprintf(stderr,
                "      payload.str=%p  '%s'\n", (void *)p, preview);
            if (++shown >= 8) break;
        }
    }
    if (tr.counts.pathCount > 0 && !tr.counts.pathSamples.empty()) {
        std::unordered_set<const char *> seenP;
        size_t shown = 0;
        std::fprintf(stderr,
            "    Path samples (first up to %zu unique addrs):\n",
            tr.counts.pathSamples.size());
        for (const char * p : tr.counts.pathSamples) {
            if (!p || !seenP.insert(p).second) continue;
            char preview[28] = {0};
            std::snprintf(preview, sizeof(preview), "%s", p);
            if (std::strlen(p) > 24) {
                preview[24] = '.'; preview[25] = '.'; preview[26] = '.';
                preview[27] = 0;
            }
            std::fprintf(stderr,
                "      payload.path=%p  '%s'\n", (void *)p, preview);
            if (++shown >= 8) break;
        }
    }
    std::fprintf(stderr,
        "============================================================\n");
}

// ============================================================================
// 2026-06-04: LIVE MEMORY BUCKETS — the honest, GHC-style decomposition.
//
// User directive: "GHC allocates 1 TB of virtual memory but counts LIVE
// bytes.  We must break this down into buckets: CU cache, BC cache,
// actual live bytes during eval, FFI live bytes."
//
// The headline `v3-direct memory:` line (run.cc) is misleading: it
// reports `v3_arena = bytesAllocated()` — the CUMULATIVE bump counter
// (every byte ever allocated, never decremented on cell death; ~1.5×
// resident on M5) — subtracted from PEAK `ru_maxrss`, with the
// remainder clamped at 0.  This report instead decomposes CURRENT
// RESIDENT RSS into LIVE buckets:
//
//   * EVAL working set  — arena cells reachable from the VM root set
//     (the genuine live graph; what a precise GC bounds the arena to).
//   * CU cache          — (a) cached eval-result Value graphs in the
//     arena, pinned by the import cache BEYOND the working set; plus
//     (b) the parsed bytecode (libc-malloc'd CompilationUnits).
//   * FFI / Boehm-live  — Boehm heap−free (TW-interop + FFI-leaf values).
//   * BC cache          — the bytecode SQLite connection's page cache.
//
// Attribution is EVAL-FIRST first-touch (see RootSource): a cell
// reachable from both eval and a cache counts as EVAL, so each cache
// shows only its MARGINAL retention.  Gated NIX_V3_MEM_BUCKETS=1.
//
// Retirement criterion: when the precise GC ships default-on and the
// arena's `bytesAllocated()` becomes a live-bytes proxy (whole-block
// free decrements it), fold this into NIX_VM_STATS and drop the gate.
// ============================================================================
namespace {

/// Current resident-set bytes (mach phys_footprint-class `resident_size`
/// on macOS; /proc/self/statm RSS on Linux).  NOT peak `ru_maxrss`, NOT
/// the arena's cumulative bump counter.  Mirrors limits.cc and
/// heap_trace.cc (both replicate this rather than cross-TU call).
size_t bucketCurrentRssBytes() noexcept
{
#if defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return static_cast<size_t>(info.resident_size);
    return 0;
#else
    long pages = 0, dummy = 0;
    std::FILE * f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    int rc = std::fscanf(f, "%ld %ld", &dummy, &pages);
    std::fclose(f);
    if (rc < 2 || pages <= 0) return 0;
    long pgsize = sysconf(_SC_PAGESIZE);
    return static_cast<size_t>(pages) * static_cast<size_t>(pgsize);
#endif
}

/// Eval-first bucketing tracer.  Same transitive-mark machinery as
/// LiveTracer, but it tracks the CURRENT root-source bucket and drains
/// the worklist at each source boundary under the OLD label (see
/// enterRootSource), so each first-touch-marked cell is attributed to
/// whichever bucket REACHED it first.  Because walkAllV3Roots announces
/// EVAL before CuCache, a cell shared by eval and a cache lands in EVAL
/// and the cache bucket reflects only marginal retention.
///
/// Strings/paths (Tag::String/Path arena buffers) are intentionally not
/// counted — ~1.2% of arena per STRINGS_ATTR_SPIKE_2026-05-29; folded
/// into the residual.  Standalone cells (16 B) are walked-through but
/// not size-counted (their pointee contents ARE counted via typed walks).
class BucketTracer : public RootVisitor
{
public:
    // Indexed by (int)RootSource: 0=Eval, 1=CuCache, 2=Ffi.
    size_t bytesBy[3] = {0, 0, 0};
    size_t objsBy [3] = {0, 0, 0};

    void enterRootSource(RootSource rs) noexcept override
    {
        drain();                          // finish previous phase, OLD label
        cur_ = static_cast<int>(rs);      // then switch buckets
    }

    void visitClosure (Closure   * & p) override { enqueue(p, GK_CLOSURE);  }
    void visitThunk   (Thunk     * & p) override { enqueue(p, GK_THUNK);    }
    void visitBindings(Bindings  * & p) override { enqueue(p, GK_BINDINGS); }
    void visitList    (ListVec   * & p) override { enqueue(p, GK_LIST);     }
    void visitPair    (ValuePair * & p) override { enqueue(p, GK_PAIR);     }
    void visitSlot    (Value     * & p) override
    {
        if (!p) return;
        if (cells_.insert(p).second) visitValue(*p);
    }

    void drain() noexcept
    {
        while (!work_.empty()) {
            Gray g = work_.back();
            work_.pop_back();
            switch (g.kind) {
            case GK_CLOSURE:  walkClosure (static_cast<Closure   *>(g.ptr)); break;
            case GK_THUNK:    walkThunk   (static_cast<Thunk     *>(g.ptr)); break;
            case GK_BINDINGS: walkBindings(static_cast<Bindings  *>(g.ptr)); break;
            case GK_LIST:     walkList    (static_cast<ListVec   *>(g.ptr)); break;
            case GK_PAIR:     walkPair    (static_cast<ValuePair *>(g.ptr)); break;
            }
        }
    }

private:
    int cur_ = 0;  // current bucket == RootSource::Eval until announced
    std::unordered_set<void *> seen_;
    std::unordered_set<void *> cells_;
    std::vector<Gray> work_;

    template <typename P> void enqueue(P * p, GrayKind k)
    {
        if (!p) return;
        if (!seen_.insert(p).second) return;  // first-touch dedup
        work_.push_back({static_cast<void *>(p), k});
    }

    void account(size_t bytes) noexcept { bytesBy[cur_] += bytes; ++objsBy[cur_]; }

    void walkClosure(Closure * c)
    {
        account(sizeof(Closure) + size_t(c->nUpvalues) * sizeof(Value));
        if (c->capturedWiths) enqueue(c->capturedWiths, GK_LIST);
        for (uint16_t i = 0; i < c->nUpvalues; ++i) visitValue(c->upvalues[i]);
    }
    void walkThunk(Thunk * t)
    {
        // Evaluated thunks dropped their tail; the rest keep nUpvalues.
        const size_t bytes = (t->state == ThunkState::Evaluated)
            ? sizeof(Thunk)
            : sizeof(Thunk) + sizeof(Value) * t->nUpvalues;
        account(bytes);
        if (t->cell && cells_.insert(t->cell).second) visitValue(*t->cell);
        if (t->shapeCell && cells_.insert(t->shapeCell).second) visitValue(*t->shapeCell);
        switch (t->state) {
        case ThunkState::Suspended:
        case ThunkState::Blackhole:
            if (t->suspended.capturedWiths) enqueue(t->suspended.capturedWiths, GK_LIST);
            for (uint16_t i = 0; i < t->nUpvalues; ++i) visitValue(t->tail[i]);
            break;
        case ThunkState::Evaluated:
            visitValue(t->evaluated);
            break;
        case ThunkState::Native:
            for (uint16_t i = 0; i < t->nUpvalues; ++i) visitValue(t->tail[i]);
            break;
        }
    }
    void walkBindings(Bindings * b)
    {
        account(sizeof(Bindings) + sizeof(Bindings::Entry) * b->size);
        for (uint32_t i = 0; i < b->size; ++i) visitValue(b->entries[i].value);
        if (b->parent) enqueue(const_cast<Bindings *>(b->parent), GK_BINDINGS);
    }
    void walkList(ListVec * l)
    {
        account(sizeof(ListVec) + sizeof(Value) * l->size);
        for (uint32_t i = 0; i < l->size; ++i) visitValue(l->elems[i]);
    }
    void walkPair(ValuePair * p)
    {
        account(sizeof(ValuePair));
        visitValue(p->left);
        visitValue(p->right);
        visitValue(p->evaluated);
        visitValue(p->third);  // Tag::App3 arg2 (2026-05-30)
    }
};

} // namespace

void dumpV3MemoryBuckets() noexcept
{
    static const bool s_enabled =
        std::getenv("NIX_V3_MEM_BUCKETS") != nullptr;
    if (!s_enabled) return;

    // Multi-pass guard: run.cc may re-enter via bytecode-primop install
    // passes (each a tiny eval).  Report only once allocation crosses a
    // floor + only when it has grown since the last report — so the
    // install passes (~tens of objects) are filtered and the real
    // workload pass prints.  Uses cumulative OBJECT COUNTS, NOT the
    // `bytes*` counters: those are gated (V3_STATS_BUMP) and read 0 in
    // default / NIX_VM_STATS builds, which is why the byte-based guard
    // dumpV3LiveFraction inherited never fires.  Counts are always
    // maintained under NIX_VM_STATS (the gate this report lives behind).
    static std::atomic<size_t> s_lastObjs{0};
    const auto & st = allocStats();
    const size_t curObjs = st.closuresAllocated + st.thunksAllocated
        + st.attrsetsAllocated + st.listsAllocated + st.pairsAllocated;
    if (curObjs < 1024) return;  // floor: skip trivial / install passes
    if (curObjs < s_lastObjs.load(std::memory_order_relaxed) + 1024)
        return;
    s_lastObjs.store(curObjs, std::memory_order_relaxed);

    BucketTracer tr;
    bool fullVm = false;
    const auto & stack = activeVMStack();
    if (!stack.empty() && stack.back()) {
        walkAllV3Roots(*stack.back(), tr);
        fullVm = true;
    } else {
        walkGlobalV3Roots(tr);
    }
    tr.drain();  // final phase (the EVAL infra announced last)

    // -- Arena live, split by bucket (eval-first first-touch) ----------
    const size_t evalArena    = tr.bytesBy[static_cast<int>(RootSource::Eval)];
    const size_t cuCacheArena = tr.bytesBy[static_cast<int>(RootSource::CuCache)];
    const size_t ffiArena     = tr.bytesBy[static_cast<int>(RootSource::Ffi)];
    const size_t arenaLive    = evalArena + cuCacheArena + ffiArena;

    // -- CU cache: parsed bytecode (libc-malloc'd, not in the arena) ---
    const size_t cuBytecode  = importCacheBytecodeBytes();
    const size_t cuCount     = importCacheCuCount();
    const size_t resultCount = importCacheResultCount();

    // -- FFI / Boehm-live: heap − free (TW interop + FFI-leaf values) --
    const size_t boehmHeap = GC_get_heap_size();
    const size_t boehmFree = GC_get_free_bytes();
    const size_t boehmLive = boehmHeap > boehmFree ? boehmHeap - boehmFree : 0;

    // -- BC cache: bytecode SQLite connection's page cache -------------
    const size_t bcCache = static_cast<size_t>(disk_cache::approxResidentBytes());

    // -- Base + residual -----------------------------------------------
    const size_t resident = bucketCurrentRssBytes();
    const size_t accounted = arenaLive + cuBytecode + boehmLive + bcCache;
    const size_t residual  = resident > accounted ? resident - accounted : 0;
    // The "1 TB virtual" analog: the arena's cumulative bump counter —
    // every byte ever bump-allocated (+ unused tail of reserved 16 MB
    // blocks), never decremented on cell death.  This is the OLD
    // `v3_arena` headline number; the overcount vs arena LIVE is the
    // counting-correction the user asked for.
    const size_t arenaCumulative = threadArena().bytesAllocated();
    const size_t overcount =
        arenaCumulative > arenaLive ? arenaCumulative - arenaLive : 0;

    char b1[32], b2[32], b3[32], b4[32], b5[32], b6[32], b7[32], b8[32];
    char bArenaLive[32], bCumul[32], bOver[32];
    fmtBytes(evalArena,    b1, sizeof(b1));
    fmtBytes(cuCacheArena, b2, sizeof(b2));
    fmtBytes(cuBytecode,   b3, sizeof(b3));
    fmtBytes(boehmLive,    b4, sizeof(b4));
    fmtBytes(bcCache,      b5, sizeof(b5));
    fmtBytes(accounted,    b6, sizeof(b6));
    fmtBytes(resident,     b7, sizeof(b7));
    fmtBytes(residual,     b8, sizeof(b8));
    fmtBytes(arenaLive,      bArenaLive, sizeof(bArenaLive));
    fmtBytes(arenaCumulative, bCumul, sizeof(bCumul));
    fmtBytes(overcount,      bOver,  sizeof(bOver));

    // Compact "pretty" layout: resident decomposition, no verbose
    // preamble / separator rules.  The scope line is kept (one line) —
    // it is load-bearing: under "residual" the EVAL bucket is a lower
    // bound (VMState already unwound), not the peak working set.
    std::fprintf(stderr,
        "\n"
        "=========== v3 LIVE MEMORY BUCKETS — resident decomposition ===========\n"
        "  eval-first first-touch · resident (not bump-cumulative, not peak); "
        "scope: %s\n"
        "\n"
        "  bucket                          LIVE bytes   detail\n"
        "  eval working set (arena)      %12s   %zu objs\n"
        "  CU cache — result graph (arena) %10s   pinned beyond eval (%zu results)\n"
        "  CU cache — bytecode (libc)    %12s   %zu CUs (parsed bytecode)\n"
        "  FFI / Boehm-live              %12s   heap-free\n"
        "  BC cache (SQLite page cache)  %12s   sqlite3_db_status\n",
        fullVm ? "FULL (active VMState + global)"
               : "RESIDUAL lower-bound (VMState torn down)",
        b1, tr.objsBy[static_cast<int>(RootSource::Eval)],
        b2, resultCount,
        b3, cuCount,
        b4,
        b5);

    if (ffiArena > 0) {
        char bf[32];
        fmtBytes(ffiArena, bf, sizeof(bf));
        std::fprintf(stderr,
            "  FFI bridge graph (arena)      %12s   %zu objs\n",
            bf, tr.objsBy[static_cast<int>(RootSource::Ffi)]);
    }

    std::fprintf(stderr,
        "  accounted live                %12s\n"
        "  resident RSS (now)            %12s\n"
        "  residual (RSS - accounted)    %12s   libstore/stdlib/AOT/unattributed\n"
        "\n"
        "  memo (the counting-correction):\n"
        "    arena LIVE  (eval + CU graph) %10s   <- honest working set\n"
        "    arena RESERVED (bump counter) %10s   <- the OLD v3_arena number\n"
        "    not-live    (reserved - live) %10s   <- dead cells + unused block tail\n"
        "=======================================================================\n",
        b6, b7, b8,
        bArenaLive, bCumul, bOver);
}

// ============================================================================
// Day 5 2026-05-28: Per-block live-bytes probe.
//
// Decision-quality data for Stage 6 generational tenured collector
// (per lode/STAGE_6_CHENEY_FALSIFIED_2026-05-27.md alt #2).
//
// Walks roots transitively (same shape as LiveTracer above), marks
// reached cells PER TYPE so we know cell sizes, then attributes each
// marked cell's bytes to the arena BLOCK containing it.  Reports:
//   * Total arena bytes, total live bytes, freeable bytes
//   * Per-block fill histogram (empty / <25% / <50% / <75% / 75%+)
//   * Fully-dead block count + bytes (block-aware sweep recoverable)
//
// Pre-committed SHIP threshold for generational mark+sweep:
//   ≥ 30% of arena recoverable via fully-dead block freeing.
//
// Below that, block-aware sweep doesn't justify the implementation
// cost; need mark-compact or a different layout.  Above, generational
// mark-sweep is viable.
// ============================================================================
namespace {

class BlockProbe : public RootVisitor
{
public:
    explicit BlockProbe(Arena & arena) noexcept : arena_(arena) {}

    void visitClosure (Closure   * & p) override
    {
        if (!p || !arena_.inActive(p)) return;
        if (!markedClosures_.insert(p).second) return;
        worklist_.push_back({p, GK_CLOSURE});
    }
    void visitThunk(Thunk * & p) override
    {
        if (!p || !arena_.inActive(p)) return;
        if (!markedThunks_.insert(p).second) return;
        worklist_.push_back({p, GK_THUNK});
    }
    void visitBindings(Bindings * & p) override
    {
        if (!p || !arena_.inActive(p)) return;
        if (!markedBindings_.insert(p).second) return;
        worklist_.push_back({p, GK_BINDINGS});
    }
    void visitList(ListVec * & p) override
    {
        if (!p || !arena_.inActive(p)) return;
        if (!markedLists_.insert(p).second) return;
        worklist_.push_back({p, GK_LIST});
    }
    void visitPair(ValuePair * & p) override
    {
        if (!p || !arena_.inActive(p)) return;
        if (!markedPairs_.insert(p).second) return;
        worklist_.push_back({p, GK_PAIR});
    }
    void visitSlot(Value * & p) override
    {
        if (!p) return;
        if (!markedCells_.insert(p).second) return;
        // Walk the cell's content; the slot's TARGET (a Value)
        // may carry pointers we haven't otherwise marked.
        visitValue(*p);
    }
    void visitString(const char * & s) noexcept override
    {
        if (!s) return;
        if (!arena_.inActive(const_cast<char *>(s))) return;
        markedChars_.insert(s);
    }
    void visitPath(const char * & s) noexcept override
    {
        if (!s) return;
        if (!arena_.inActive(const_cast<char *>(s))) return;
        markedChars_.insert(s);
    }

    void drain() noexcept
    {
        while (!worklist_.empty()) {
            Gray g = worklist_.back();
            worklist_.pop_back();
            switch (g.kind) {
            case GK_CLOSURE:  walkClosure (static_cast<Closure   *>(g.ptr)); break;
            case GK_THUNK:    walkThunk   (static_cast<Thunk     *>(g.ptr)); break;
            case GK_BINDINGS: walkBindings(static_cast<Bindings  *>(g.ptr)); break;
            case GK_LIST:     walkList    (static_cast<ListVec   *>(g.ptr)); break;
            case GK_PAIR:     walkPair    (static_cast<ValuePair *>(g.ptr)); break;
            }
        }
    }

    // Day 6: public accessors so dumpV3LiveBlockProbe can compute
    // live bytes for the sweep-cost projection (Falsifier #2).
    const std::unordered_set<Closure   *> & markedClosuresPub() const noexcept
        { return markedClosures_; }
    const std::unordered_set<Thunk     *> & markedThunksPub()   const noexcept
        { return markedThunks_; }
    const std::unordered_set<Bindings  *> & markedBindingsPub() const noexcept
        { return markedBindings_; }
    const std::unordered_set<ListVec   *> & markedListsPub()    const noexcept
        { return markedLists_; }
    const std::unordered_set<ValuePair *> & markedPairsPub()    const noexcept
        { return markedPairs_; }
    const std::unordered_set<Value     *> & markedCellsPub()    const noexcept
        { return markedCells_; }
    const std::unordered_set<const char *> & markedCharsPub()  const noexcept
        { return markedChars_; }

    void reportBlocks() noexcept
    {
        // Build per-block live-bytes map.  Block is identified by its
        // begin pointer (Arena::blockRanges convention).
        std::unordered_map<const char *, size_t> blockLive;
        const auto ranges = arena_.blockRanges();

        // Cache ranges sorted by begin for binary lookup.
        // arena.blockRanges builds the vector each call; the call is
        // O(N) and we credit M cells, so M log N total.
        auto findBlock = [&](const void * cellPtr) -> const char * {
            // Linear scan acceptable for first measurement (N ~ 100
            // blocks on HNE).  Future: sorted-binary search per
            // hypothesis ranking.
            for (const auto & r : ranges) {
                if (cellPtr >= (const void *)r.begin
                    && cellPtr < (const void *)r.end)
                    return r.begin;
            }
            return nullptr;  // External / huge-block / not in main blocks
        };

        size_t hugeBlockBytes = 0;
        size_t hugeBlockLive = 0;

        auto credit = [&](const void * cellPtr, size_t cellBytes) {
            const char * blk = findBlock(cellPtr);
            if (blk) {
                blockLive[blk] += cellBytes;
            } else {
                // Could be huge or external.  Approximate: charge to
                // hugeBlockLive (huge blocks aren't candidates for
                // sweep since each is its own allocation).
                hugeBlockLive += cellBytes;
            }
        };

        for (Closure   * c : markedClosures_)
            credit(c, sizeof(Closure) + sizeof(Value) * c->nUpvalues);
        for (Thunk     * t : markedThunks_) {
            size_t bytes = (t->state == ThunkState::Suspended
                         || t->state == ThunkState::Native
                         || t->state == ThunkState::Blackhole)
                ? sizeof(Thunk) + sizeof(Value) * t->nUpvalues
                : sizeof(Thunk);
            credit(t, bytes);
        }
        for (Bindings  * b : markedBindings_)
            credit(b, sizeof(Bindings) + sizeof(Bindings::Entry) * b->size);
        for (ListVec   * l : markedLists_)
            credit(l, sizeof(ListVec) + sizeof(Value) * l->size);
        for (ValuePair * p : markedPairs_)
            credit(p, sizeof(ValuePair));
        for (Value     * c : markedCells_)
            credit(c, sizeof(Value));  // standalone allocValue cells
        for (const char * s : markedChars_) {
            const size_t n = std::strlen(s) + 1;
            credit(s, n);
        }

        // Classify blocks by fill ratio.  kBlockSize is 16 MB
        // (alloc.hh:613).  Huge blocks are reported separately.
        constexpr size_t kBlockSize = Arena::kBlockSize;

        size_t totalBlocks = 0;
        size_t totalArenaBytes = 0;
        size_t totalLiveBytes = 0;
        size_t fullyDeadBlocks = 0;
        size_t fullyDeadBytes  = 0;

        // Histogram bins (live fraction within block).
        size_t binEmpty = 0, binVeryLow = 0, binLow = 0,
               binMid   = 0, binHigh   = 0, binFull = 0;

        // Use a set of "regular" block begins (non-huge) for the
        // classification.  Huge blocks are tracked separately.
        // Arena::blockRanges enumerates BOTH; we mimic the boundary
        // check by checking if the range's size equals kBlockSize.
        for (const auto & r : ranges) {
            const size_t rangeBytes =
                static_cast<size_t>(r.end - r.begin);
            const bool isHuge =
                rangeBytes != kBlockSize
                && r.end - r.begin != static_cast<ptrdiff_t>(kBlockSize);
            if (isHuge) {
                hugeBlockBytes += rangeBytes;
                continue;
            }
            ++totalBlocks;
            totalArenaBytes += rangeBytes;
            auto it = blockLive.find(r.begin);
            size_t live = (it == blockLive.end()) ? 0 : it->second;
            totalLiveBytes += live;
            if (live == 0) {
                ++fullyDeadBlocks;
                fullyDeadBytes += rangeBytes;
            }
            const double fill = double(live) / double(rangeBytes);
            if (fill < 0.01)      ++binEmpty;
            else if (fill < 0.10) ++binVeryLow;
            else if (fill < 0.25) ++binLow;
            else if (fill < 0.50) ++binMid;
            else if (fill < 0.75) ++binHigh;
            else                  ++binFull;
        }

        std::fprintf(stderr,
            "\n=================== v3 LIVE-BLOCK PROBE ===================\n"
            "Decision data for Stage 6 generational tenured collector\n"
            "(per lode/STAGE_6_CHENEY_FALSIFIED_2026-05-27.md alt #2).\n"
            "\n"
            "Reachable: closures=%zu thunks=%zu bindings=%zu lists=%zu\n"
            "           pairs=%zu cells=%zu chars=%zu\n",
            markedClosures_.size(), markedThunks_.size(),
            markedBindings_.size(), markedLists_.size(),
            markedPairs_.size(), markedCells_.size(),
            markedChars_.size());

        const double fillRatio = totalArenaBytes
            ? double(totalLiveBytes) / double(totalArenaBytes) : 0.0;
        const double sweepablePct = totalArenaBytes
            ? 100.0 * double(fullyDeadBytes) / double(totalArenaBytes) : 0.0;

        std::fprintf(stderr,
            "\n"
            "Regular blocks (kBlockSize=%zu MB):\n"
            "  total           %zu  (%.1f MB)\n"
            "  live-bytes-sum     %.1f MB  (avg fill = %.1f%%)\n"
            "  fully-dead         %zu  (%.1f%% of blocks)\n"
            "  fully-dead-bytes   %.1f MB  (%.1f%% of arena -- SWEEPABLE)\n"
            "\n"
            "Fill histogram (per-block live fraction):\n"
            "  empty (0%%)        %zu\n"
            "  very-low (<10%%)   %zu\n"
            "  low (<25%%)        %zu\n"
            "  mid (<50%%)        %zu\n"
            "  high (<75%%)       %zu\n"
            "  full (75%%+)       %zu\n"
            "\n"
            "Huge blocks (oversized allocations):\n"
            "  total-bytes        %.1f MB\n"
            "  live-bytes         %.1f MB  (live fraction = %.1f%%)\n",
            kBlockSize >> 20,
            totalBlocks, totalArenaBytes / 1e6,
            totalLiveBytes / 1e6, 100.0 * fillRatio,
            fullyDeadBlocks,
            100.0 * double(fullyDeadBlocks) /
                std::max<size_t>(1, totalBlocks),
            fullyDeadBytes / 1e6, sweepablePct,
            binEmpty, binVeryLow, binLow, binMid, binHigh, binFull,
            hugeBlockBytes / 1e6, hugeBlockLive / 1e6,
            hugeBlockBytes ?
                100.0 * double(hugeBlockLive) / double(hugeBlockBytes) :
                0.0);

        // Pre-committed SHIP threshold from STAGE_6_CHENEY_FALSIFIED:
        // ≥ 30% of arena recoverable via fully-dead-block sweep.
        const double shipThresholdPct = 30.0;
        std::fprintf(stderr,
            "\n"
            "VERDICT (pre-committed threshold: >=%.0f%% sweepable):\n"
            "  measured: %.1f%% sweepable -> %s\n"
            "============================================================\n",
            shipThresholdPct,
            sweepablePct,
            sweepablePct >= shipThresholdPct
                ? "PASS: generational mark+sweep is VIABLE"
                : "FAIL: blocks too uniformly populated; reconsider design");
    }

    // Day 6 (2026-05-28): Falsifier #2 from GC_DESIGN_POST_CHENEY §5.2.
    //
    // Question: does flat mark-sweep's wall cost dominate eval time
    // at v3's high live-fraction?
    //
    // Method: time the mark + scan phase using wall clocks.  Mark walks
    // every reachable cell from precise roots (drain phase).  Scan
    // walks the marked sets once for size accounting.  Together these
    // approximate the per-cycle cost of MS's "walk live cells, populate
    // free lists in their gaps" step (the closest analog without
    // actually implementing the sweep).
    //
    // Pre-committed threshold: <5% of eval wall on hello.drvPath ->
    // OK to ship.  5-10% -> marginal; revisit Immix.  >10% -> sweep
    // cost dominates; abort flat MS, switch to Immix or other.
    void reportSweepCost(double markMs, size_t totalLiveBytes) noexcept
    {
        // We extrapolate from the mark cost: in flat MS, sweep is
        // roughly walking the marked set in address order + computing
        // gap sizes between consecutive marked cells.  That's O(N_live)
        // per cycle.  Mark itself is also O(N_live) (transitive
        // closure of pointer follows).
        //
        // For HNE-scale workloads, expect mark + sweep per cycle to
        // approximate 2x mark cost.  Project across N major GCs per
        // eval at the default threshold cadence.
        const double markMsPerByte = totalLiveBytes
            ? markMs / double(totalLiveBytes) : 0.0;
        const double projectedSweepMs = markMs;  // O(N_live) too
        std::fprintf(stderr,
            "\n=== Mark+sweep cost projection ===\n"
            "  mark wall (this drain): %.2f ms\n"
            "  live bytes processed:   %.1f MB\n"
            "  mark cost / live byte:  %.3f ns\n"
            "  projected sweep wall:   %.2f ms (O(N_live) walk)\n"
            "  per-cycle total:        %.2f ms\n"
            "\n"
            "  At default trigger cadence (arena 256 MB -> reclaim live):\n"
            "  if eval runs 10 cycles, sweep budget = %.0f ms\n"
            "  if eval runs 50 cycles, sweep budget = %.0f ms\n",
            markMs,
            totalLiveBytes / 1e6,
            markMsPerByte * 1e6,
            projectedSweepMs,
            markMs + projectedSweepMs,
            10.0 * (markMs + projectedSweepMs),
            50.0 * (markMs + projectedSweepMs));
    }

    // Day 6 (2026-05-28): Falsifier #1 from GC_DESIGN_POST_CHENEY §5.1.
    //
    // Question: does Immix's mark-region bump-realloc deliver enough
    // space-savings vs flat mark-sweep to justify the +2 KLoC?
    //
    // Method: partition each arena block into LINES of `lineSize` bytes.
    // For each marked cell, mark every line its byte range touches.
    // After all cells marked, count lines with NO marked cell.
    //
    // Lines fully dead are the bytes Immix can reclaim via bump-realloc
    // within partially-live blocks (its main advantage over flat MS).
    // Lines partially live are pinned by Immix (a partially-live line
    // can't be bump-realloced into).
    //
    // Pre-committed threshold: >=30% lines fully dead per cycle ->
    // Immix path. <30% -> flat MS path.
    //
    // Reports at multiple line sizes (64, 128, 256, 512) so the
    // sensitivity to line granularity is visible.  4096 + 16384 added
    // 2026-06-01 to measure page-level madvise viability (#875 Path A
    // refined-granularity spike following whole-block falsification at
    // EXIT_PATH_A_FALSIFIED_2026-05-31).
    void reportLines() noexcept
    {
        // Default Immix line size = 128 B; sweep 64/128/256/512.
        // 4096 = OS page size on macOS aarch64; 16384 = 4-page bundle.
        for (size_t lineSize : {size_t(64), size_t(128),
                                size_t(256), size_t(512),
                                size_t(4096), size_t(16384)}) {
            reportLinesAtSize(lineSize);
        }
    }

private:
    void reportLinesAtSize(size_t lineSize) noexcept
    {
        // Pre-compute block-by-block layout.  We iterate
        // `arena_.blockRanges()` once and store the range list locally
        // since each `markRange` call would otherwise re-allocate.
        const auto ranges = arena_.blockRanges();

        // For each regular block, allocate a per-line bitmap.  Huge
        // blocks (single-allocation, non-kBlockSize) are tracked
        // separately as fully-alive (an Immix line-reclaim doesn't
        // apply since the whole huge block is one allocation).
        struct BlockEntry {
            const char * begin;
            const char * end;
            std::vector<uint8_t> lineMarked;  // bool per line
        };
        std::vector<BlockEntry> blocks;
        size_t hugeBytes = 0;
        size_t hugeLiveBytes = 0;
        for (const auto & r : ranges) {
            const size_t rangeBytes =
                static_cast<size_t>(r.end - r.begin);
            if (rangeBytes != Arena::kBlockSize) {
                hugeBytes += rangeBytes;
                continue;
            }
            BlockEntry e;
            e.begin = r.begin;
            e.end = r.end;
            const size_t nLines =
                (rangeBytes + lineSize - 1) / lineSize;
            e.lineMarked.assign(nLines, 0);
            blocks.push_back(std::move(e));
        }

        // Sort blocks by begin for binary lookup.
        std::sort(blocks.begin(), blocks.end(),
            [](const BlockEntry & a, const BlockEntry & b) {
                return a.begin < b.begin;
            });

        auto findBlock = [&](const void * cellPtr) -> BlockEntry * {
            // Binary search by begin; check end after.
            auto it = std::upper_bound(blocks.begin(), blocks.end(),
                cellPtr,
                [](const void * cp, const BlockEntry & b) {
                    return cp < (const void *)b.begin;
                });
            if (it == blocks.begin()) return nullptr;
            --it;
            if (cellPtr >= (const void *)it->begin
                && cellPtr < (const void *)it->end)
                return &*it;
            return nullptr;
        };

        auto markRange = [&](const void * cellPtr, size_t cellBytes) {
            BlockEntry * blk = findBlock(cellPtr);
            if (!blk) {
                // Huge block (or external).  Charge as huge-live.
                hugeLiveBytes += cellBytes;
                return;
            }
            size_t offset = (const char *)cellPtr - blk->begin;
            if (offset + cellBytes > Arena::kBlockSize) {
                // Cell straddles end-of-block (shouldn't normally
                // happen since allocator refills on overflow, but
                // defensive).
                cellBytes = Arena::kBlockSize - offset;
            }
            size_t firstLine = offset / lineSize;
            size_t lastLine  = (offset + cellBytes - 1) / lineSize;
            for (size_t i = firstLine;
                 i <= lastLine && i < blk->lineMarked.size(); ++i)
            {
                blk->lineMarked[i] = 1;
            }
        };

        // Walk every marked set; mark lines.
        for (Closure * c : markedClosures_)
            markRange(c,
                sizeof(Closure) + sizeof(Value) * c->nUpvalues);
        for (Thunk * t : markedThunks_) {
            size_t bytes = (t->state == ThunkState::Suspended
                         || t->state == ThunkState::Native
                         || t->state == ThunkState::Blackhole)
                ? sizeof(Thunk) + sizeof(Value) * t->nUpvalues
                : sizeof(Thunk);
            markRange(t, bytes);
        }
        for (Bindings * b : markedBindings_)
            markRange(b,
                sizeof(Bindings) + sizeof(Bindings::Entry) * b->size);
        for (ListVec * l : markedLists_)
            markRange(l, sizeof(ListVec) + sizeof(Value) * l->size);
        for (ValuePair * p : markedPairs_)
            markRange(p, sizeof(ValuePair));
        for (Value * c : markedCells_)
            markRange(c, sizeof(Value));
        for (const char * s : markedChars_)
            markRange(s, std::strlen(s) + 1);

        // Aggregate per-block + global.
        size_t totalLines = 0, deadLines = 0;
        size_t pinnedBytes = 0;   // lines with >=1 marked cell
        size_t reclaimedBytes = 0; // lines fully dead
        // Per-block dead-line histogram (for grouping detection).
        size_t blocksAllDead = 0, blocksMostlyDead = 0,
               blocksMixed = 0,  blocksMostlyLive = 0,
               blocksAllLive = 0;
        size_t maxLinesAnyBlock = 0;
        for (auto & blk : blocks) {
            size_t blockTotal = blk.lineMarked.size();
            maxLinesAnyBlock = std::max(maxLinesAnyBlock, blockTotal);
            size_t blockMarked = 0;
            for (uint8_t b : blk.lineMarked)
                if (b) ++blockMarked;
            size_t blockDead = blockTotal - blockMarked;
            totalLines += blockTotal;
            deadLines  += blockDead;
            pinnedBytes    += blockMarked * lineSize;
            reclaimedBytes += blockDead   * lineSize;
            double deadFrac = double(blockDead) / double(blockTotal);
            if (deadFrac > 0.99)      ++blocksAllDead;
            else if (deadFrac > 0.75) ++blocksMostlyDead;
            else if (deadFrac > 0.25) ++blocksMixed;
            else if (deadFrac > 0.01) ++blocksMostlyLive;
            else                      ++blocksAllLive;
        }

        const double deadLinePct = totalLines
            ? 100.0 * double(deadLines) / double(totalLines) : 0.0;
        const double arenaBytes =
            double(totalLines * lineSize);
        const double reclaimablePct = arenaBytes
            ? 100.0 * double(reclaimedBytes) / arenaBytes : 0.0;

        // Pre-committed threshold per GC_DESIGN_POST_CHENEY §5.1.
        const double immixThresholdPct = 30.0;

        std::fprintf(stderr,
            "\n=== Immix line-occupancy probe @ line_size=%zu B ===\n"
            "  blocks            %zu (block_size=16 MB)\n"
            "  total lines       %zu (= %.1f MB)\n"
            "  pinned (live)     %zu lines  (= %.1f MB)\n"
            "  fully-dead        %zu lines  (= %.1f%% of lines, %.1f MB)\n"
            "  bump-realloc-recoverable: %.1f%% of arena bytes\n"
            "  blocks all-dead=%zu mostly-dead=%zu mixed=%zu "
            "mostly-live=%zu all-live=%zu\n"
            "  VERDICT (>=%.0f%% lines dead -> Immix viable):\n"
            "    %s\n",
            lineSize,
            blocks.size(),
            totalLines, arenaBytes / 1e6,
            totalLines - deadLines, pinnedBytes / 1e6,
            deadLines, deadLinePct, reclaimedBytes / 1e6,
            reclaimablePct,
            blocksAllDead, blocksMostlyDead, blocksMixed,
            blocksMostlyLive, blocksAllLive,
            immixThresholdPct,
            deadLinePct >= immixThresholdPct
                ? "PASS: Immix bump-realloc worth the +2 KLoC complexity"
                : "FAIL: too many lines partially-live; flat MS dominates");

        if (hugeBytes) {
            std::fprintf(stderr,
                "  (huge blocks: %.1f MB total, %.1f MB live)\n",
                hugeBytes / 1e6, hugeLiveBytes / 1e6);
        }
    }

public:
    // Day 5 follow-up: per-alloc-site live-ratio report.  Requires
    // NIX_V3_BINDINGS_ORIGIN=1 + NIX_V3_THUNKS_ATTR=1 to populate the
    // origin tables.  Tests the hypothesis: are there alloc sites
    // whose live-ratio is so low that ROUTING THOSE SITES to a
    // dedicated "young" allocator region would produce free-on-return
    // dead blocks naturally?
    //
    // Reports top-N sites by allocated bytes; per site shows allocated
    // bytes, live bytes, and live fraction.  Sites with live fraction
    // <10% are candidates for ephemeral-routing.
    void reportAllocSites() noexcept
    {
        struct Site {
            std::string label;
            size_t allocBytes = 0;
            size_t liveBytes = 0;
            uint64_t allocCount = 0;
            uint64_t liveCount = 0;
        };
        std::unordered_map<std::string, Site> sites;

        // -- Bindings (NIX_V3_BINDINGS_ORIGIN=1) ----------------------
        {
            auto & tbl = bindingsOriginTable();
            for (const auto & [b, origin] : tbl) {
                std::string key;
                if (origin.source) key = origin.source;
                key += ":pos=" + std::to_string(origin.posHandle);
                auto & s = sites[key];
                s.label = key + " [Bindings]";
                size_t bytes = sizeof(Bindings)
                    + sizeof(Bindings::Entry) * origin.allocN;
                s.allocBytes += bytes;
                ++s.allocCount;
                if (markedBindings_.count(const_cast<Bindings *>(b))) {
                    s.liveBytes += sizeof(Bindings)
                        + sizeof(Bindings::Entry) *
                          const_cast<Bindings *>(b)->size;
                    ++s.liveCount;
                }
            }
        }
        // -- Thunks (NIX_V3_THUNKS_ATTR=1) ----------------------------
        {
            auto & tbl = thunkOriginTable();
            for (const auto & [t, origin] : tbl) {
                std::string key;
                if (origin.file) key = origin.file;
                key += ":line=" + std::to_string(origin.line);
                auto & s = sites[key];
                s.label = key + " [Thunk]";
                size_t bytes = sizeof(Thunk)
                    + sizeof(Value) * origin.nUpvalues;
                s.allocBytes += bytes;
                ++s.allocCount;
                if (markedThunks_.count(const_cast<Thunk *>(t))) {
                    Thunk * tnc = const_cast<Thunk *>(t);
                    size_t lbytes = (tnc->state == ThunkState::Suspended
                                  || tnc->state == ThunkState::Native
                                  || tnc->state == ThunkState::Blackhole)
                        ? sizeof(Thunk) + sizeof(Value) * tnc->nUpvalues
                        : sizeof(Thunk);
                    s.liveBytes += lbytes;
                    ++s.liveCount;
                }
            }
        }

        if (sites.empty()) {
            std::fprintf(stderr,
                "\n[alloc-site probe: origin tables empty -- need\n"
                " NIX_V3_BINDINGS_ORIGIN=1 + NIX_V3_THUNKS_ATTR=1 to\n"
                " populate them at allocation time]\n");
            return;
        }

        // Sort sites by allocBytes desc.
        std::vector<Site> sorted;
        sorted.reserve(sites.size());
        for (auto & kv : sites) sorted.push_back(std::move(kv.second));
        std::sort(sorted.begin(), sorted.end(),
            [](const Site & a, const Site & b) {
                return a.allocBytes > b.allocBytes;
            });

        // Bucket by live-ratio band.
        size_t ephemeralCount = 0;  // <10% live
        size_t midCount       = 0;  // 10-50% live
        size_t persistentCount = 0; // >=50% live
        size_t ephemeralAllocBytes = 0;
        size_t totalAllocBytes = 0;
        for (const auto & s : sorted) {
            totalAllocBytes += s.allocBytes;
            double liveRatio = s.allocBytes
                ? double(s.liveBytes) / double(s.allocBytes) : 0.0;
            if (liveRatio < 0.10) {
                ++ephemeralCount;
                ephemeralAllocBytes += s.allocBytes;
            } else if (liveRatio < 0.50) {
                ++midCount;
            } else {
                ++persistentCount;
            }
        }

        std::fprintf(stderr,
            "\n=================== v3 ALLOC-SITE PROBE ===================\n"
            "Per-call-site live-vs-allocated ratio (Bindings+Thunks).\n"
            "Tests hypothesis: would routing low-live-ratio sites to a\n"
            "dedicated young region produce dead-block-friendly\n"
            "distributions at end of primop calls?\n"
            "\n"
            "Sites tracked: %zu\n"
            "  ephemeral (<10%% live): %zu sites, %.1f MB allocated\n"
            "  mid       (10-50%% live): %zu sites\n"
            "  persistent (>=50%% live): %zu sites\n"
            "\n"
            "Routing-candidate ratio: %.1f%% of allocated bytes come from\n"
            "ephemeral sites (-> would land in a young allocator region).\n",
            sorted.size(),
            ephemeralCount,
            ephemeralAllocBytes / 1e6,
            midCount,
            persistentCount,
            totalAllocBytes
                ? 100.0 * double(ephemeralAllocBytes)
                          / double(totalAllocBytes)
                : 0.0);

        std::fprintf(stderr,
            "\nTop-15 sites by allocated bytes:\n"
            "%-72s %10s %10s %6s %10s\n",
            "site", "alloc_MB", "live_MB", "live%%", "alloc_n");
        for (size_t i = 0; i < std::min<size_t>(15, sorted.size()); ++i) {
            const auto & s = sorted[i];
            double liveRatio = s.allocBytes
                ? 100.0 * double(s.liveBytes) / double(s.allocBytes) : 0.0;
            std::fprintf(stderr,
                "  %-70s %10.1f %10.1f %6.1f %10llu\n",
                s.label.c_str(),
                s.allocBytes / 1e6,
                s.liveBytes / 1e6,
                liveRatio,
                (unsigned long long)s.allocCount);
        }

        // Pre-committed: if >=50% of allocated bytes are ephemeral
        // (<10% live), allocation-time routing IS viable.  Else,
        // routing alone won't solve the distribution problem.
        const double routingShipPct = 50.0;
        double measuredPct = totalAllocBytes
            ? 100.0 * double(ephemeralAllocBytes) / double(totalAllocBytes)
            : 0.0;
        std::fprintf(stderr,
            "\n"
            "VERDICT (pre-committed: >=%.0f%% ephemeral-site bytes):\n"
            "  measured %.1f%% -> %s\n"
            "============================================================\n",
            routingShipPct,
            measuredPct,
            measuredPct >= routingShipPct
                ? "PASS: alloc-time routing is VIABLE"
                : "FAIL: not enough ephemeral concentration; need runtime compaction");
    }

private:
    Arena & arena_;
    enum GrayKindLocal : uint8_t {
        GK_CLOSURE = 0, GK_THUNK, GK_BINDINGS, GK_LIST, GK_PAIR
    };
    struct Gray { void * ptr; GrayKindLocal kind; };
    std::vector<Gray> worklist_;

    std::unordered_set<Closure   *> markedClosures_;
    std::unordered_set<Thunk     *> markedThunks_;
    std::unordered_set<Bindings  *> markedBindings_;
    std::unordered_set<ListVec   *> markedLists_;
    std::unordered_set<ValuePair *> markedPairs_;
    std::unordered_set<Value     *> markedCells_;
    std::unordered_set<const char *> markedChars_;

    void walkClosure(Closure * c) noexcept
    {
        if (c->capturedWiths) visitList(c->capturedWiths);
        for (uint16_t i = 0; i < c->nUpvalues; ++i)
            visitValue(c->upvalues[i]);
    }
    void walkThunk(Thunk * t) noexcept
    {
        if (t->cell)      visitSlot(t->cell);
        if (t->shapeCell) visitSlot(t->shapeCell);
        switch (t->state) {
        case ThunkState::Suspended:
        case ThunkState::Blackhole:
            if (t->suspended.capturedWiths)
                visitList(t->suspended.capturedWiths);
            for (uint16_t i = 0; i < t->nUpvalues; ++i)
                visitValue(t->tail[i]);
            break;
        case ThunkState::Evaluated:
            visitValue(t->evaluated);
            break;
        case ThunkState::Native:
            for (uint16_t i = 0; i < t->nUpvalues; ++i)
                visitValue(t->tail[i]);
            break;
        }
    }
    void walkBindings(Bindings * b) noexcept
    {
        for (uint32_t i = 0; i < b->size; ++i)
            visitValue(b->entries[i].value);
        if (b->parent)
            visitBindings(const_cast<Bindings *&>(b->parent));
    }
    void walkList(ListVec * l) noexcept
    {
        for (uint32_t i = 0; i < l->size; ++i)
            visitValue(l->elems[i]);
    }
    void walkPair(ValuePair * p) noexcept
    {
        visitValue(p->left);
        visitValue(p->right);
        visitValue(p->evaluated);
        visitValue(p->third);  // 2026-05-30 Tag::App3 arg2
    }
};

} // namespace

void dumpV3LiveBlockProbe() noexcept
{
    static const bool s_enabled =
        std::getenv("NIX_V3_BLOCK_PROBE") != nullptr;
    if (!s_enabled) return;

    // Fire once per process (mirrors dumpV3LiveFraction's gate).
    static std::atomic<size_t> s_lastArena{0};
    Arena & arena = threadArena();
    const size_t curArena = arena.bytesAllocated();
    if (curArena < s_lastArena.load(std::memory_order_relaxed) + (1 << 16))
        return;
    s_lastArena.store(curArena, std::memory_order_relaxed);

    BlockProbe pr(arena);

    // Day 6: time the mark phase for Falsifier #2 (sweep cost projection).
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();

    bool walkedActiveVM = false;
    const auto & stack = activeVMStack();
    if (!stack.empty() && stack.back()) {
        walkAllV3Roots(*stack.back(), pr);
        walkedActiveVM = true;
    } else {
        walkGlobalV3Roots(pr);
    }
    pr.drain();

    const auto t1 = clock::now();
    const double markMs =
        std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (!walkedActiveVM) {
        std::fprintf(stderr,
            "\n[NIX_V3_BLOCK_PROBE: VMState already torn down — "
            "results are a LOWER BOUND (only global persistent roots "
            "reachable)]\n");
    }
    pr.reportBlocks();
    pr.reportLines();
    // Approximate live bytes from the per-block live-bytes computed
    // during reportBlocks; we just re-aggregate from sets here cheaply.
    size_t liveBytesApprox = 0;
    for (Closure * c : pr.markedClosuresPub())
        liveBytesApprox += sizeof(Closure) + sizeof(Value) * c->nUpvalues;
    for (Thunk * t : pr.markedThunksPub()) {
        size_t b = (t->state == ThunkState::Suspended
                 || t->state == ThunkState::Native
                 || t->state == ThunkState::Blackhole)
            ? sizeof(Thunk) + sizeof(Value) * t->nUpvalues
            : sizeof(Thunk);
        liveBytesApprox += b;
    }
    for (Bindings * b : pr.markedBindingsPub())
        liveBytesApprox += sizeof(Bindings) + sizeof(Bindings::Entry) * b->size;
    for (ListVec * l : pr.markedListsPub())
        liveBytesApprox += sizeof(ListVec) + sizeof(Value) * l->size;
    for ([[maybe_unused]] ValuePair * p : pr.markedPairsPub())
        liveBytesApprox += sizeof(ValuePair);
    for ([[maybe_unused]] Value * c : pr.markedCellsPub())
        liveBytesApprox += sizeof(Value);
    for (const char * s : pr.markedCharsPub())
        liveBytesApprox += std::strlen(s) + 1;
    pr.reportSweepCost(markMs, liveBytesApprox);
    pr.reportAllocSites();
}

// ============================================================================
// Periodic L(t) live-fraction trace (Step 4 of post-Phase-3.8 plan).
//
// Per `lode/L_MEASUREMENT_GAP_2026-05-28.md` §5: extend dumpV3LiveFraction
// with periodic sampling so we get L(t) across the eval, not just L_end.
// Fires every K MB of arena allocation from the dispatch-loop safepoint.
// Each sample = one full transitive walk; records CSV row; flushes file
// at end-of-run.
//
// Gate: NIX_V3_LIVE_TRACE_PERIODIC=<K>      (K in MB; default 64)
//       NIX_V3_LIVE_TRACE_PERIODIC_OUT=<f>  (CSV path; default
//                                            /tmp/v3-live-periodic-<pid>.csv)
//
// Retirement: when the L(t) metric is integrated into the bench harness
// as a default-OFF metric, remove the env-gate per Rule 0 §2.  Tracked
// in L_TIME_SERIES_DATA_2026-05-29 follow-up.
// ============================================================================

namespace {

struct PeriodicCsvRow {
    double alloc_offset_mb;   // arena.bytesAllocated() at sample time
    double resident_mb;       // = alloc_offset_mb (current arena footprint)
    double live_mb;           // transitive-walk live bytes
    double l_resident;        // live / resident
    double l_cumulative;      // live / cumulative-allocated
    double wall_ms;           // ms since eval start
    // DIAG-3 (2026-05-29 evening, per DIAGNOSTIC_AUDIT §6.3):
    // per-Tag live-bytes breakdown.  Same liveBytes sum as `live_mb`,
    // decomposed.  LiveTracer already separates per-Tag internally
    // (counts.bytesClosures, bytesThunks, etc.); just plumb through.
    double live_closures_mb;
    double live_thunks_mb;
    double live_bindings_mb;
    double live_lists_mb;
    double live_pairs_mb;
    // 2026-05-29 evening (DIAG bridge analysis): bridge-table sizes.
    // Entry counts at sample time; each entry is 24 B vector
    // storage + transitive v3-heap retention (the load-bearing
    // portion, much larger than 24 B).
    size_t bridge_closures = 0;
    size_t bridge_attrs    = 0;
    size_t bridge_lists    = 0;
};

/// thread_local state for the periodic trace.  Thread-local because v3
/// has per-thread arenas (`threadArena()`) and per-thread VMState.
struct PeriodicState {
    std::vector<PeriodicCsvRow> rows;
    size_t nextThresholdBytes = 0;
    std::chrono::steady_clock::time_point evalStart;
    bool evalStartSet = false;
};

PeriodicState & periodicState() noexcept {
    static thread_local PeriodicState s;
    return s;
}

/// Gate cache + parse env once at startup.
bool periodicEnabledImpl() noexcept {
    static const bool s_enabled =
        std::getenv("NIX_V3_LIVE_TRACE_PERIODIC") != nullptr;
    return s_enabled;
}

size_t periodicThresholdBytes() noexcept {
    static const size_t s_thresholdBytes = [] {
        const char * v = std::getenv("NIX_V3_LIVE_TRACE_PERIODIC");
        long mb = 64;  // default
        if (v && *v) {
            long parsed = std::strtol(v, nullptr, 10);
            // Clamp 1 MB ... 16 GB.  K=0 is meaningless (infinite samples);
            // K=64 MB is the default.  K too large means few samples
            // (the eval ends before crossing); user's risk.
            if (parsed >= 1 && parsed <= 16384) mb = parsed;
        }
        return static_cast<size_t>(mb) << 20;
    }();
    return s_thresholdBytes;
}

const char * periodicOutPath() noexcept {
    static const std::string s_outPath = [] {
        const char * v = std::getenv("NIX_V3_LIVE_TRACE_PERIODIC_OUT");
        if (v && *v) return std::string(v);
        // Default: /tmp/v3-live-periodic-<pid>.csv
        char buf[64];
        std::snprintf(buf, sizeof(buf),
            "/tmp/v3-live-periodic-%d.csv", static_cast<int>(getpid()));
        return std::string(buf);
    }();
    return s_outPath.c_str();
}

} // namespace

bool periodicLiveTraceEnabled() noexcept {
    return periodicEnabledImpl();
}

void maybeSamplePeriodicLiveFraction(VMState & vm) noexcept
{
    if (!periodicEnabledImpl()) return;

    auto & st = periodicState();

    // Lazy-initialise eval-start clock on first call.
    if (!st.evalStartSet) {
        st.evalStart = std::chrono::steady_clock::now();
        st.evalStartSet = true;
        st.nextThresholdBytes = periodicThresholdBytes();
    }

    Arena & arena = threadArena();
    const size_t curBytes = arena.bytesAllocated();
    if (curBytes < st.nextThresholdBytes) return;

    const size_t K = periodicThresholdBytes();

    // Wall time since eval start (ms) — captured BEFORE the walk
    // so wall_ms reflects when this K-crossing was observed.
    const auto now = std::chrono::steady_clock::now();
    const double wall_ms = std::chrono::duration<double, std::milli>(
        now - st.evalStart).count();

    // Single transitive live-walk per safepoint visit.  No catch-up
    // duplication: even if multiple K-multiples were crossed between
    // safepoints, we emit ONE row.  This honestly reports observable
    // L values — between safepoints L is unknown.  Advance threshold
    // PAST the current bytes so the next sample fires K MB later.
    LiveTracer tr;
    walkAllV3Roots(vm, tr);
    tr.drain();

    const size_t liveBytes =
          tr.counts.bytesClosures + tr.counts.bytesThunks
        + tr.counts.bytesBindings + tr.counts.bytesLists
        + tr.counts.bytesPairs;

    const auto & stats = allocStats();
    const size_t cumulativeBytes =
          stats.bytesClosures + stats.bytesThunks
        + stats.bytesBindings + stats.bytesLists
        + stats.bytesPairs;

    PeriodicCsvRow row;
    row.alloc_offset_mb = double(curBytes) / (1ULL << 20);
    row.resident_mb     = double(curBytes) / (1ULL << 20);
    row.live_mb         = double(liveBytes) / (1ULL << 20);
    row.l_resident      = curBytes > 0
        ? double(liveBytes) / double(curBytes) : 0.0;
    row.l_cumulative    = cumulativeBytes > 0
        ? double(liveBytes) / double(cumulativeBytes) : 0.0;
    row.wall_ms         = wall_ms;
    // DIAG-3 per-Tag plumbing.
    constexpr double MB = 1.0 / double(1ULL << 20);
    row.live_closures_mb = double(tr.counts.bytesClosures) * MB;
    row.live_thunks_mb   = double(tr.counts.bytesThunks)   * MB;
    row.live_bindings_mb = double(tr.counts.bytesBindings) * MB;
    row.live_lists_mb    = double(tr.counts.bytesLists)    * MB;
    row.live_pairs_mb    = double(tr.counts.bytesPairs)    * MB;
        // (bridge-table L(t) sampling retired — TW_VALUE_ERADICATION F4.)
    st.rows.push_back(row);

    // Advance threshold past current bytes by the next K-multiple
    // (so duplicate sampling at the same safepoint is avoided, while
    // forward progress is guaranteed).
    st.nextThresholdBytes = curBytes + K;
}

void flushPeriodicLiveTraceCsv() noexcept
{
    if (!periodicEnabledImpl()) return;
    auto & st = periodicState();
    if (st.rows.empty()) return;  // silent: many threads may flush with 0 rows

    const char * outPath = periodicOutPath();

    // Multi-runRootExpr / multi-thread coordination: each
    // `runRootExpr` invocation flushes from its own thread_local state.
    // To accumulate samples across all phases (primop-install passes +
    // main eval) we APPEND to the CSV; the header is written exactly
    // once across the process via a process-wide atomic flag.
    static std::atomic<bool> s_headerWritten{false};
    bool needsHeader = !s_headerWritten.exchange(true,
        std::memory_order_acq_rel);
    if (needsHeader) {
        // Truncate file on first write so reruns start fresh.
        std::FILE * trunc = std::fopen(outPath, "w");
        if (trunc) {
            std::fprintf(trunc,
                "alloc_offset_mb,resident_mb,live_mb,"
                "L_resident,L_cumulative,wall_ms,"
                "live_closures_mb,live_thunks_mb,"
                "live_bindings_mb,live_lists_mb,live_pairs_mb,"
                "bridge_closures,bridge_attrs,bridge_lists\n");
            std::fclose(trunc);
        }
    }

    std::FILE * f = std::fopen(outPath, "a");
    if (!f) {
        std::fprintf(stderr,
            "[v3-live-periodic] ERROR: cannot open output '%s' for append.\n",
            outPath);
        return;
    }
    for (const auto & r : st.rows) {
        std::fprintf(f,
            "%.2f,%.2f,%.2f,%.4f,%.4f,%.1f,"
            "%.2f,%.2f,%.2f,%.2f,%.2f,"
            "%zu,%zu,%zu\n",
            r.alloc_offset_mb, r.resident_mb, r.live_mb,
            r.l_resident, r.l_cumulative, r.wall_ms,
            r.live_closures_mb, r.live_thunks_mb,
            r.live_bindings_mb, r.live_lists_mb, r.live_pairs_mb,
            r.bridge_closures, r.bridge_attrs, r.bridge_lists);
    }
    std::fclose(f);

    // Clear state so subsequent flushes on same thread don't duplicate.
    const size_t nThisFlush = st.rows.size();
    st.rows.clear();

    // Only emit a banner from the flush with enough samples to be
    // meaningful (≥2 — needed to compute min/max + variance).  Threads
    // that crossed the threshold only once during a brief
    // primop-install pass would otherwise spam the stderr.
    if (nThisFlush < 2) return;

    std::vector<double> Ls;
    Ls.reserve(nThisFlush);
    for (size_t i = 0; i < nThisFlush; ++i) {
        // Banner re-reads from the file is overkill; reconstruct from
        // the local rows BEFORE clear.  We cleared already, so the
        // banner reports the count + path only.  Full distribution
        // stats are in the CSV.
    }
    std::fprintf(stderr,
        "\n"
        "================= v3 LIVE-FRACTION PERIODIC =================\n"
        "  CSV samples written this flush: %zu (appended)\n"
        "  Output: %s\n"
        "  (run `python3 -c 'import csv; ...'` on CSV for L distribution)\n"
        "============================================================\n",
        nThisFlush, outPath);
}

} // namespace nix::v3
