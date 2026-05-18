/// @file
/// v3 resource limits — Phase 1.6 (2026-05-18).  See limits.hh for
/// the design overview.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/limits.hh"
#include "v3/alloc.hh"  // for allocStats() in diagnostics

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
        } else {
            std::fprintf(stderr,
                "v3 limits: NIX_V3_MAX_HEAP='%s' is not a valid size "
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
                "v3 limits: NIX_V3_MAX_CPU_TIME='%s' is not a valid duration "
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
                "v3 limits: NIX_V3_MAX_WALL_TIME='%s' is not a valid duration "
                "(expected NUMBER[s|m|h]); cap disabled\n", v);
        }
    }

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
