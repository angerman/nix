/// @file
/// v3 resource limits — Phase 1.6 (2026-05-18).  See limits.hh for
/// the design overview.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/limits.hh"
#include "v3/alloc.hh"  // for allocStats() in diagnostics
#include "v3/primop.hh"  // for activeV3VM()
#include "v3/vm.hh"      // for VMState, CallFrame
#include "v3/disasm.hh"  // for disassembleWindow

#include <gc/gc.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <sys/time.h>
#include <signal.h>
#include <unistd.h>
#include <cerrno>

#if defined(__APPLE__)
# include <mach/mach.h>
# include <mach/task.h>
#endif

namespace nix::v3 {

namespace {

// ---------------------------------------------------------------------------
// State: cached limits + dispatch poll counter + OOM atomic flag.
// All initialised once by `initLimits()`.
// ---------------------------------------------------------------------------

struct LimitsState {
    bool initialised = false;

    // Configured caps.  Zero = unset (no cap on that dimension).
    uint64_t maxHeapBytes = 0;
    std::chrono::seconds maxCpuTime{0};
    std::chrono::seconds maxWallTime{0};

    // Captured eval-start clock for wall-time delta.
    std::chrono::steady_clock::time_point evalStart;

    // OOM flag — set by Boehm's oom_fn handler (which runs from
    // allocation paths and cannot itself throw).  Polled by
    // checkLimits().
    std::atomic<bool> oomFlag{false};
};

LimitsState & state()
{
    static LimitsState s;
    return s;
}

std::mutex & initMutex()
{
    static std::mutex m;
    return m;
}

} // anonymous namespace

// Defined here at nix::v3 scope so the inline `limitsActive()` in
// limits.hh can read it via the linker.  Set by `initLimits()`.
bool _limitsActiveGlobal = false;

namespace {  // re-open anonymous

// ---------------------------------------------------------------------------
// Helpers.
// ---------------------------------------------------------------------------

#if defined(__APPLE__)
uint64_t currentRssBytes()
{
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return info.resident_size;
    return 0;
}
#else
uint64_t currentRssBytes()
{
    // Linux: /proc/self/statm "rss" column in pages × page size.
    long pages = 0, dummy = 0;
    std::FILE * f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    int rc = std::fscanf(f, "%ld %ld", &dummy, &pages);
    std::fclose(f);
    if (rc < 2 || pages <= 0) return 0;
    long pgsize = sysconf(_SC_PAGESIZE);
    return static_cast<uint64_t>(pages) * static_cast<uint64_t>(pgsize);
}
#endif

/// Format byte count as human-friendly "1.2 GB" etc.
std::string fmtBytes(uint64_t b)
{
    char buf[32];
    if (b >= (1ull << 30))
        std::snprintf(buf, sizeof(buf), "%.2f GB", double(b) / double(1ull << 30));
    else if (b >= (1ull << 20))
        std::snprintf(buf, sizeof(buf), "%.2f MB", double(b) / double(1ull << 20));
    else if (b >= (1ull << 10))
        std::snprintf(buf, sizeof(buf), "%.2f KB", double(b) / double(1ull << 10));
    else
        std::snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)b);
    return buf;
}

/// Format seconds as "12.34s" / "1m12s" / "1h2m3s".
std::string fmtDuration(double s)
{
    char buf[32];
    if (s < 60)
        std::snprintf(buf, sizeof(buf), "%.2fs", s);
    else if (s < 3600)
        std::snprintf(buf, sizeof(buf), "%dm%ds",
            int(s / 60), int(s) % 60);
    else
        std::snprintf(buf, sizeof(buf), "%dh%dm%ds",
            int(s / 3600), int(s / 60) % 60, int(s) % 60);
    return buf;
}

/// Brief diagnostic snapshot for cap-exceeded messages.
std::string snapshot()
{
    const auto & a = allocStats();
    std::ostringstream o;
    o << "alloc: closures=" << a.closuresAllocated
      << " thunks=" << a.thunksAllocated
      << " lists=" << a.listsAllocated
      << " attrsets=" << a.attrsetsAllocated;
    uint64_t rss = currentRssBytes();
    if (rss > 0)
        o << " rss=" << fmtBytes(rss);
    // Boehm heap is only meaningfully queriable when GC has been
    // initialised; the call is cheap when it has.
    size_t heap = GC_get_heap_size();
    if (heap > 0)
        o << " boehm_heap=" << fmtBytes(heap);
    return o.str();
}

/// OOM handler — installed via GC_set_oom_fn when the heap cap is
/// active.  Boehm calls this from inside an allocation that would
/// exceed `GC_set_max_heap_size`.  We must NOT throw from here
/// (Boehm is C code with in-flight allocation state).  Instead:
/// set an atomic flag + return NULL.  The dispatch loop polls the
/// flag at the next safe boundary and throws OutOfMemoryError.
///
/// Returning NULL signals "allocation failed" to Boehm's caller.
/// Most Boehm clients (including v3) don't currently null-check
/// alloc results, but our policy is: the very next opcode dispatch
/// (≤ ~10us under normal load) will see the flag and tear down via
/// throw.  In the narrow window between OOM and check, an alloc
/// might dereference NULL and segfault — but only if the alloc
/// path that received NULL writes the result without checking
/// FIRST.  Most v3 alloc paths immediately store + use; the
/// segfault is acceptable as a fail-safe.
void * oomHandler(size_t /*bytes*/)
{
    state().oomFlag.store(true, std::memory_order_release);
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Parsers.
// ---------------------------------------------------------------------------

std::optional<uint64_t> parseSize(std::string_view s)
{
    if (s.empty()) return std::nullopt;
    // Trim ASCII whitespace.
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))  s.remove_suffix(1);
    if (s.empty()) return std::nullopt;

    // Detect trailing unit.  We accept G/M/K (case-insensitive),
    // with or without a 'B' suffix ("2G", "2GB", "2g" all mean 2 GiB).
    uint64_t multiplier = 1;
    if (!s.empty()) {
        char last = static_cast<char>(std::toupper(static_cast<unsigned char>(s.back())));
        if (last == 'B' && s.size() >= 2) {
            char penult = static_cast<char>(std::toupper(static_cast<unsigned char>(s[s.size()-2])));
            if (penult == 'G' || penult == 'M' || penult == 'K') {
                last = penult;
                s.remove_suffix(1);
            }
        }
        switch (last) {
            case 'G': multiplier = 1ull << 30; s.remove_suffix(1); break;
            case 'M': multiplier = 1ull << 20; s.remove_suffix(1); break;
            case 'K': multiplier = 1ull << 10; s.remove_suffix(1); break;
            default: break;  // bare decimal — bytes.
        }
    }
    // Parse remaining decimal.  Use a manual loop to avoid locale
    // issues with std::stoull and to detect overflow explicitly.
    uint64_t v = 0;
    if (s.empty()) return std::nullopt;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
        uint64_t d = static_cast<uint64_t>(c - '0');
        if (v > (UINT64_MAX - d) / 10) return std::nullopt;  // overflow
        v = v * 10 + d;
    }
    // Multiplier overflow check.
    if (multiplier > 1 && v > UINT64_MAX / multiplier) return std::nullopt;
    return v * multiplier;
}

std::optional<std::chrono::seconds> parseDuration(std::string_view s)
{
    if (s.empty()) return std::nullopt;
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))  s.remove_suffix(1);
    if (s.empty()) return std::nullopt;

    uint64_t multiplier = 1;  // default: seconds
    char last = static_cast<char>(std::tolower(static_cast<unsigned char>(s.back())));
    switch (last) {
        case 's': multiplier = 1;    s.remove_suffix(1); break;
        case 'm': multiplier = 60;   s.remove_suffix(1); break;
        case 'h': multiplier = 3600; s.remove_suffix(1); break;
        default: break;
    }
    if (s.empty()) return std::nullopt;
    uint64_t v = 0;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
        uint64_t d = static_cast<uint64_t>(c - '0');
        if (v > (UINT64_MAX - d) / 10) return std::nullopt;
        v = v * 10 + d;
    }
    if (multiplier > 1 && v > UINT64_MAX / multiplier) return std::nullopt;
    return std::chrono::seconds(v * multiplier);
}

namespace {

// ---------------------------------------------------------------------------
// #753 RSS watchdog — total-process memory enforcement.
//
// Background: NIX_V3_MAX_HEAP only caps Boehm GC's heap
// (GC_set_max_heap_size).  The v3 threadArena (raw malloc), the
// nursery (calloc), and every C++ STL container (libc malloc) live
// OUTSIDE Boehm and are unbounded by the cap.  A 2026-05-21
// cardano-node M5 attempt consumed 100+ GB with MAX_HEAP=8G —
// because the runaway allocation happened in TW-side / libexpr
// std::vector / std::unordered_map growth, none of which Boehm
// sees.
//
// This watchdog enforces an HONEST whole-process RSS cap.  Three
// defensive layers:
//   1. Boehm OOM handler (existing): typed OutOfMemoryError when
//      Boehm-only allocs hit the cap.  Fires for v3-arena-less
//      paths within libexpr's Boehm-managed code.
//   2. setrlimit(RLIMIT_AS, cap + 256 MB): Linux kernel-enforced
//      virtual-memory cap with headroom for stacks/libs/Boehm
//      metadata.  Causes malloc/mmap to return ENOMEM cleanly.
//      No-op on macOS (Apple's setrlimit ignores RLIMIT_AS).
//   3. SIGALRM itimer watchdog (100 ms polling): reads RSS via
//      mach task_info on macOS / getrusage on Linux.  When RSS
//      exceeds the cap, writes a banner + _exit(137).  This is the
//      only mechanism that fires while v3 is suspended in a deep
//      TW/libexpr callback — the dispatch-loop limit poll cannot
//      run in that state.  Async-signal-safe: only write(2) and
//      _exit(2) inside the handler.
//
// Plus a RSS check at the start of checkLimits() (below) — for the
// in-dispatch case, this throws a clean typed error rather than
// _exit so the eval frame can unwind.
// ---------------------------------------------------------------------------

inline uint64_t getProcessRssBytes() noexcept
{
#if defined(__APPLE__)
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  (task_info_t)&info, &count) == KERN_SUCCESS)
        return static_cast<uint64_t>(info.resident_size);
    return 0;
#else
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0)
        // Linux: ru_maxrss is in KB.
        return static_cast<uint64_t>(ru.ru_maxrss) * 1024;
    return 0;
#endif
}

// Global RSS cap for the SIGALRM handler.  Relaxed atomic so the
// handler is async-signal-safe (no lock acquisition).  Written
// exactly once by initLimits.
std::atomic<uint64_t> g_rssCap{0};

// Async-signal-safe integer-to-ASCII conversion.  Writes the
// decimal representation of `v` into `out` (advancing `out`).
// `out` must have at least 24 bytes of room (enough for uint64_t).
inline void aSafeAppendU64(char *& out, uint64_t v) noexcept
{
    if (v == 0) { *out++ = '0'; return; }
    char tmp[24];
    int n = 0;
    while (v > 0) { tmp[n++] = char('0' + (v % 10)); v /= 10; }
    while (n > 0) *out++ = tmp[--n];
}

inline void aSafeAppendLit(char *& out, const char * s) noexcept
{
    while (*s) *out++ = *s++;
}

inline void aSafeAppendBytes(char *& out, uint64_t v) noexcept
{
    if (v < (1ull << 20)) {
        aSafeAppendU64(out, v);
        aSafeAppendLit(out, "B");
    } else if (v < (1ull << 30)) {
        aSafeAppendU64(out, v / (1ull << 20));
        aSafeAppendLit(out, "MB");
    } else {
        // GB with one decimal.
        const uint64_t gb_tenths = (v * 10) >> 30;
        aSafeAppendU64(out, gb_tenths / 10);
        *out++ = '.';
        aSafeAppendU64(out, gb_tenths % 10);
        aSafeAppendLit(out, "GB");
    }
}

extern "C" void rssCapTimerHandler(int /*signo*/) noexcept
{
    const uint64_t cap = g_rssCap.load(std::memory_order_relaxed);
    if (cap == 0) return;
    const uint64_t rss = getProcessRssBytes();
    if (rss < cap) return;
    // Async-signal-safe forensic dump.  No fprintf, no malloc, no
    // locks.  Reads atomic-ish allocStats() counters (uint64_t
    // writes are atomic on aarch64/x86_64 for normal-aligned
    // accesses) and the static opName() table.
    //
    // Format:
    //   v3 SAFETY watchdog (137): RSS=<rss> cap=<cap>
    //     alloc: closures=N thunks=N attrsets=N lists=N pairs=N
    //            insns=N forced=N bridge=N
    //     bytes: closures=NMB thunks=NMB bindings=NMB lists=NMB ...
    //     top opcodes (with NIX_VM_OPCOUNTS=1):
    //       OP_xxx (n)
    //       OP_yyy (m)
    //       ...
    //   exit 137
    char buf[3072];
    char * p = buf;
    aSafeAppendLit(p,
        "\nv3 SAFETY watchdog: process exit (137); RSS=");
    aSafeAppendBytes(p, rss);
    aSafeAppendLit(p, " cap=");
    aSafeAppendBytes(p, cap);
    aSafeAppendLit(p, "\n");

    const auto & a = allocStats();
    aSafeAppendLit(p,
        "  alloc: closures=");   aSafeAppendU64(p, a.closuresAllocated);
    aSafeAppendLit(p, " thunks=");  aSafeAppendU64(p, a.thunksAllocated);
    aSafeAppendLit(p, " attrsets="); aSafeAppendU64(p, a.attrsetsAllocated);
    aSafeAppendLit(p, " lists=");    aSafeAppendU64(p, a.listsAllocated);
    aSafeAppendLit(p, " pairs=");    aSafeAppendU64(p, a.pairsAllocated);
    aSafeAppendLit(p, " values=");   aSafeAppendU64(p, a.valuesAllocated);
    aSafeAppendLit(p, "\n         insns=");
    aSafeAppendU64(p, a.bytecodeInstructions);
    aSafeAppendLit(p, " forced=");   aSafeAppendU64(p, a.thunksForced);
    aSafeAppendLit(p, " bridgeForced=");
    aSafeAppendU64(p, a.bridgeThunksForced);
    aSafeAppendLit(p, "\n");

    aSafeAppendLit(p, "  bytes: closures=");
    aSafeAppendBytes(p, a.bytesClosures);
    aSafeAppendLit(p, " thunks=");  aSafeAppendBytes(p, a.bytesThunks);
    aSafeAppendLit(p, " bindings="); aSafeAppendBytes(p, a.bytesBindings);
    aSafeAppendLit(p, " lists=");    aSafeAppendBytes(p, a.bytesLists);
    aSafeAppendLit(p, " pairs=");    aSafeAppendBytes(p, a.bytesPairs);
    aSafeAppendLit(p, " chars=");    aSafeAppendBytes(p, a.bytesChars);
    const uint64_t v3Total =
          a.bytesValues + a.bytesClosures + a.bytesThunks + a.bytesEnvs
        + a.bytesLists  + a.bytesBindings + a.bytesPairs + a.bytesChars;
    aSafeAppendLit(p, "\n         v3_total=");
    aSafeAppendBytes(p, v3Total);
    // Boehm heap size at this instant.  GC_get_heap_size is
    // documented async-signal-safe in Boehm 8.0+ (used by other
    // GC-aware runtime watchdogs).
    const uint64_t boehmHeap = (uint64_t) GC_get_heap_size();
    aSafeAppendLit(p, " boehm=");
    aSafeAppendBytes(p, boehmHeap);
    // "Elsewhere" = RSS - boehm - v3_arena (libc malloc, mmap,
    // bridge tables, etc.).  When v3-native callFlake's runaway
    // happens here, the bug is in code that v3 doesn't track.
    const uint64_t elsewhere =
        (rss > boehmHeap + v3Total)
          ? (rss - boehmHeap - v3Total)
          : 0;
    aSafeAppendLit(p, " elsewhere=");
    aSafeAppendBytes(p, elsewhere);
    aSafeAppendLit(p, "\n");

    // Top-N opcodes by count.  Find them via a single linear scan
    // selecting the highest unseen entry each iteration — O(N * 256)
    // total work, no allocation, no sort.
    const auto & oc = a.opcodeCounts;
    uint64_t totalOps = 0;
    for (int i = 0; i < 256; ++i) totalOps += oc[i];
    if (totalOps > 0) {
        aSafeAppendLit(p, "  top opcodes (NIX_VM_OPCOUNTS):\n");
        bool seen[256] = {};
        const int topN = 10;
        for (int k = 0; k < topN; ++k) {
            int bestIdx = -1;
            uint64_t bestCnt = 0;
            for (int i = 0; i < 256; ++i) {
                if (seen[i]) continue;
                if (oc[i] > bestCnt) { bestCnt = oc[i]; bestIdx = i; }
            }
            if (bestIdx < 0 || bestCnt == 0) break;
            seen[bestIdx] = true;
            aSafeAppendLit(p, "    ");
            const char * nm = opName(static_cast<Op>(bestIdx));
            aSafeAppendLit(p, nm ? nm : "OP_?");
            aSafeAppendLit(p, " ");
            aSafeAppendU64(p, bestCnt);
            // % of total
            aSafeAppendLit(p, " (");
            aSafeAppendU64(p, totalOps > 0
                ? (bestCnt * 100) / totalOps : 0);
            aSafeAppendLit(p, "%)\n");
            // Safety check on buffer space.
            if (p - buf > int(sizeof buf) - 256) break;
        }
    } else {
        aSafeAppendLit(p,
            "  (opcode breakdown unavailable; rerun with NIX_VM_OPCOUNTS=1)\n");
    }

    (void) !write(STDERR_FILENO, buf, size_t(p - buf));
    _exit(137);
}

void installRssWatchdog(uint64_t capBytes) noexcept
{
    g_rssCap.store(capBytes, std::memory_order_relaxed);

    struct sigaction sa{};
    sa.sa_handler = &rssCapTimerHandler;
    sigemptyset(&sa.sa_mask);
    // SA_RESTART so v3's read/write/etc. don't return EINTR when
    // the timer fires during a syscall.  Without this, every 100 ms
    // could spuriously interrupt long IFD reads.
    sa.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &sa, nullptr);

    struct itimerval itv{};
    itv.it_interval.tv_sec = 0;
    itv.it_interval.tv_usec = 100 * 1000;  // 100 ms poll
    itv.it_value = itv.it_interval;
    setitimer(ITIMER_REAL, &itv, nullptr);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// initLimits.  Idempotent; reads env vars; installs Boehm OOM handler.
// ---------------------------------------------------------------------------

void initLimits()
{
    std::lock_guard<std::mutex> lock(initMutex());
    auto & st = state();
    if (st.initialised) return;
    st.initialised = true;

    // gate: NIX_V3_MAX_HEAP — Boehm heap ceiling for v3-direct.
    // Retire when v3-direct has bounded-memory guarantees by
    // construction (Stage 3 nursery default-on + write barriers
    // closed); until then, default-off escape hatch for users
    // running on memory-constrained systems / CI / cardano-node.
    if (const char * v = std::getenv("NIX_V3_MAX_HEAP")) {
        if (auto bytes = parseSize(v); bytes && *bytes > 0) {
            st.maxHeapBytes = *bytes;
            GC_set_max_heap_size(static_cast<size_t>(*bytes));
            GC_set_oom_fn(&oomHandler);
            // #753 belt: kernel-enforced virtual-memory cap with
            // headroom for stacks, libc data, and Boehm/system
            // mmap regions outside the heap.  Linux enforces;
            // macOS silently ignores (setrlimit returns 0 but the
            // kernel doesn't act on RLIMIT_AS).  When this fires
            // on Linux, malloc/mmap returns ENOMEM cleanly.
            const uint64_t headroom = uint64_t(256) * 1024 * 1024;
            struct rlimit rl{};
            rl.rlim_cur = static_cast<rlim_t>(*bytes + headroom);
            rl.rlim_max = rl.rlim_cur;
            if (setrlimit(RLIMIT_AS, &rl) != 0) {
                // Not fatal — macOS often returns success but
                // doesn't enforce, or may return EPERM if the
                // requested cap exceeds the inherited hard limit.
                // The SIGALRM watchdog below covers both cases.
                std::fprintf(stderr,
                    "warning: v3 limits: setrlimit(RLIMIT_AS, %llu) failed (%s); "
                    "relying on SIGALRM watchdog\n",
                    (unsigned long long)rl.rlim_cur,
                    std::strerror(errno));
            }
            // #753 suspenders: 100 ms-polled RSS watchdog that
            // calls _exit(137) if the cap is exceeded.  Covers the
            // case where v3 is in a deep TW/libexpr callback and
            // the dispatch-loop's checkLimits() poll cannot run.
            installRssWatchdog(*bytes);
        } else {
            std::fprintf(stderr,
                "warning: v3 limits: NIX_V3_MAX_HEAP='%s' is not a valid size "
                "(expected NUMBER[K|M|G][B]); cap disabled\n", v);
        }
    }

    // gate: NIX_V3_MAX_CPU_TIME — CPU-time ceiling (user+sys via
    // getrusage).  Retire when v3-direct evaluations have provable
    // termination on all supported inputs; until then, used for
    // hang-detection in bench + tests.
    if (const char * v = std::getenv("NIX_V3_MAX_CPU_TIME")) {
        if (auto d = parseDuration(v); d && d->count() > 0) {
            st.maxCpuTime = *d;
        } else {
            std::fprintf(stderr,
                "warning: v3 limits: NIX_V3_MAX_CPU_TIME='%s' is not a valid duration "
                "(expected NUMBER[s|m|h]); cap disabled\n", v);
        }
    }

    // gate: NIX_V3_MAX_WALL_TIME — wall-clock ceiling.  Retire when
    // v3-direct has provable IFD-bounded wall time; until then,
    // user-facing kill-switch for slow IFD / stuck remote builds.
    if (const char * v = std::getenv("NIX_V3_MAX_WALL_TIME")) {
        if (auto d = parseDuration(v); d && d->count() > 0) {
            st.maxWallTime = *d;
        } else {
            std::fprintf(stderr,
                "warning: v3 limits: NIX_V3_MAX_WALL_TIME='%s' is not a valid duration "
                "(expected NUMBER[s|m|h]); cap disabled\n", v);
        }
    }

    // gate: NIX_V3_BOEHM_FREE_DIV — Boehm's free-space divisor.
    // Boehm aims for ≥ 1/N of heap free; default 3 (target 33 % free).
    // hello.drvPath observation: boehm_heap=403 MB with 99.9 % free,
    // suggesting Boehm is over-conservative on its watermark — large
    // working-set spike during eval then heap stays at peak forever.
    // Higher N → smaller free target → more aggressive GC + smaller
    // arena watermark.
    //
    // Suggested values per IDEAL_GC_DESIGN_2026-05-26.md §6.2:
    //   N=3  (default)  — current behaviour
    //   N=10            — moderate aggressive (~10 % free target)
    //   N=30            — aggressive (~3 % free target)
    //   N=100           — very aggressive; may cost wall time
    //
    // Retirement criterion: when v3 owns its own arena/nursery
    // allocator end-to-end (precise GC of v3 cells per Stage 6) and
    // Boehm is downscoped to FFI-only objects, Boehm's heap
    // watermark stops mattering — drop the gate.
    if (const char * v = std::getenv("NIX_V3_BOEHM_FREE_DIV")) {
        char * endp = nullptr;
        long n = std::strtol(v, &endp, 10);
        if (endp && *endp == '\0' && n > 0 && n <= 1000) {
            GC_set_free_space_divisor(static_cast<GC_word>(n));
            std::fprintf(stderr,
                "v3 boehm-tune: GC_set_free_space_divisor(%ld) — "
                "target ≈ 1/%ld free\n", n, n);
        } else {
            std::fprintf(stderr,
                "warning: v3 limits: NIX_V3_BOEHM_FREE_DIV='%s' is not a "
                "valid integer in [1, 1000]; gate ignored\n", v);
        }
    }

    // (Removed: NIX_V3_BOEHM_UNMAP_THRESHOLD — Boehm 8.2.8's public
    //  API doesn't expose `GC_set_unmap_threshold` as a setter.  The
    //  build-time `GC_UNMAP_THRESHOLD` macro controls this in the
    //  collector itself; runtime tuning would need a Boehm patch.
    //  The Boehm-internal default of 6 is in effect.  If the
    //  measurement spike shows headroom from forced unmap, we can
    //  call `GC_gcollect_and_unmap()` periodically from checkLimits
    //  — but that's a separate gate.  Removing the unsupported gate
    //  per [[falsification-rule]] — don't add gates that don't fire.)

    st.evalStart = std::chrono::steady_clock::now();

    _limitsActiveGlobal = (st.maxHeapBytes > 0)
                       || (st.maxCpuTime.count() > 0)
                       || (st.maxWallTime.count() > 0);
}

void signalOutOfMemory()
{
    state().oomFlag.store(true, std::memory_order_release);
}

void checkLimits()
{
    auto & st = state();

    // OOM flag fires first — the cheapest check.  If Boehm has
    // signalled OOM, the rest of the eval is in jeopardy (next
    // alloc may dereference NULL); throw immediately.
    if (st.oomFlag.load(std::memory_order_acquire)) {
        std::string msg = "v3 OutOfMemoryError: NIX_V3_MAX_HEAP="
            + fmtBytes(st.maxHeapBytes) + " exceeded ("
            + snapshot() + ")";
        // Reset the flag so subsequent re-entry through a catch
        // doesn't re-trigger.  Subsequent allocations against the
        // same cap will set the flag again.
        st.oomFlag.store(false, std::memory_order_release);
        throw OutOfMemoryError(msg);
    }

    // #753 in-dispatch RSS check.  The SIGALRM watchdog calls
    // _exit() when RSS exceeds the cap from ANY thread of
    // execution (TW callbacks, libexpr code, etc.), but doing so
    // skips destructor / atexit chains.  When the cap is reached
    // WHILE v3's dispatch loop happens to be polling, we can
    // throw a clean typed error instead and unwind the eval
    // frame normally — preserving stats output and any user
    // try-catch around the eval.  Only fires when MAX_HEAP cap
    // is active.
    if (st.maxHeapBytes > 0) {
        const uint64_t rss = getProcessRssBytes();
        if (rss >= st.maxHeapBytes) {
            std::string msg = "v3 OutOfMemoryError: NIX_V3_MAX_HEAP="
                + fmtBytes(st.maxHeapBytes) + " exceeded; RSS="
                + fmtBytes(rss) + " (" + snapshot() + ")";
            throw OutOfMemoryError(msg);
        }
    }

    // Wall time — cheap (no syscall on most platforms; vDSO).
    if (st.maxWallTime.count() > 0) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - st.evalStart);
        if (elapsed >= std::chrono::duration_cast<std::chrono::milliseconds>(
                st.maxWallTime)) {
            double secs = double(elapsed.count()) / 1000.0;
            std::string msg = "v3 WallTimeExceededError: NIX_V3_MAX_WALL_TIME="
                + fmtDuration(double(st.maxWallTime.count()))
                + " exceeded after " + fmtDuration(secs)
                + " (" + snapshot() + ")";
            // V3_DBG_TRAP_ON_LIMIT — dump active VMState's frame
            // stack + recent bytecode around each frame's IP before
            // throwing.  Diagnoses "where is v3 stuck" when an
            // infinite loop trips the wall-time cap with low
            // allocation counts (the alloc snapshot in the message
            // above already gives the macro-level state; this gives
            // the micro-level instruction context).
            static const bool s_dbgTrap =
                std::getenv("V3_DBG_TRAP_ON_LIMIT") != nullptr;
            if (s_dbgTrap) {
                // Prefer currentDispatchVM (set at dispatchLoop entry/exit)
                // over activeV3VM (only set at bridge boundaries).
                VMState * vm = currentDispatchVM();
                if (!vm) vm = activeV3VM();
                if (vm) {
                    std::fprintf(stderr,
                        "v3 trap-on-limit: VMState=%p frames=%zu  "
                        "valueStack=%zu  withStack=%zu\n",
                        (const void *)vm,
                        vm->frames.size(),
                        vm->valueStack.size(),
                        vm->withStack.size());
                    size_t n = vm->frames.size();
                    size_t maxFrames = 12;
                    size_t start = n > maxFrames ? n - maxFrames : 0;
                    for (size_t i = start; i < n; ++i) {
                        const auto & fr = vm->frames[i];
                        std::fprintf(stderr,
                            "  frame[%zu]: cu=%p ip=%u thunk=%p closure=%p "
                            "flags=0x%x\n",
                            i, (const void *)fr.cu, fr.ip,
                            (const void *)fr.thunk, (const void *)fr.closure,
                            unsigned(fr.flags));
                        if (fr.cu) {
                            uint32_t lo = fr.ip > 24 ? fr.ip - 24 : 0;
                            uint32_t hi = fr.ip + 12;
                            disassembleWindow(stderr, *fr.cu, lo, hi);
                        }
                    }
                } else {
                    std::fprintf(stderr, "v3 trap-on-limit: no active VMState\n");
                }
                std::fflush(stderr);
            }
            throw WallTimeExceededError(msg);
        }
    }

    // CPU time — one getrusage syscall, ~500ns on macOS.
    if (st.maxCpuTime.count() > 0) {
        struct rusage ru;
        if (getrusage(RUSAGE_SELF, &ru) == 0) {
            double cpuSecs = double(ru.ru_utime.tv_sec)
                + double(ru.ru_utime.tv_usec) / 1e6
                + double(ru.ru_stime.tv_sec)
                + double(ru.ru_stime.tv_usec) / 1e6;
            if (cpuSecs >= double(st.maxCpuTime.count())) {
                std::string msg = "v3 CpuTimeExceededError: NIX_V3_MAX_CPU_TIME="
                    + fmtDuration(double(st.maxCpuTime.count()))
                    + " exceeded; CPU=" + fmtDuration(cpuSecs)
                    + " (user=" + fmtDuration(double(ru.ru_utime.tv_sec)
                        + double(ru.ru_utime.tv_usec) / 1e6)
                    + " sys=" + fmtDuration(double(ru.ru_stime.tv_sec)
                        + double(ru.ru_stime.tv_usec) / 1e6) + "); "
                    + snapshot();
                throw CpuTimeExceededError(msg);
            }
        }
    }
}

} // namespace nix::v3
