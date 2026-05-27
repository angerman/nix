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

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include <vector>

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
struct LiveCounters
{
    size_t closures = 0,   bytesClosures = 0;
    size_t thunks   = 0,   bytesThunks   = 0;
    size_t bindings = 0,   bytesBindings = 0;
    size_t lists    = 0,   bytesLists    = 0;
    size_t pairs    = 0,   bytesPairs    = 0;
    size_t slotsDereffed = 0;  // edges followed via Tag::Slot

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
        case ThunkState::Bridge:
            bytes = sizeof(Thunk);
            break;
        }
        counts.bytesThunks += bytes;

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
        case ThunkState::Bridge:
            // bridgeSrc is a TW nix::Value*, not v3 heap.  Skip.
            break;
        }
    }

    void walkBindings(Bindings * b)
    {
        ++counts.bindings;
        counts.bytesBindings += sizeof(Bindings)
                              + sizeof(Bindings::Entry) * b->size;
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
        case ThunkState::Bridge:
            // bridgeSrc is TW nix::Value*; not v3 heap.
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

    bool walkedActiveVM = false;
    const auto & stack = activeVMStack();
    if (!stack.empty() && stack.back()) {
        walkAllV3Roots(*stack.back(), pr);
        walkedActiveVM = true;
    } else {
        walkGlobalV3Roots(pr);
    }
    pr.drain();

    if (!walkedActiveVM) {
        std::fprintf(stderr,
            "\n[NIX_V3_BLOCK_PROBE: VMState already torn down — "
            "results are a LOWER BOUND (only global persistent roots "
            "reachable)]\n");
    }
    pr.reportBlocks();
}

} // namespace nix::v3
