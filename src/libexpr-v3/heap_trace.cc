/// @file
/// V3 in-process Boehm-heap sampler.  See `include/v3/heap_trace.hh`
/// for design intent + protocol.
///
/// Implementation notes:
///   - Uses `std::thread` not raw pthread for portability; the
///     "pthread sampler" name in the design doc is shorthand.
///   - Uses `std::atomic<bool>` for the stop flag so the main thread
///     can signal without a mutex.
///   - Sleep cadence uses `std::condition_variable::wait_for` so the
///     stop signal interrupts the sleep cleanly (no up-to-50 ms
///     shutdown delay).
///   - The default 50 ms cadence matches `perf-trace.py`'s sampler
///     so the two streams align point-for-point.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
/// Input Output Group.  SPDX-License-Identifier: Apache-2.0

#include "v3/heap_trace.hh"

#include <gc/gc.h>

// Resident-RSS + CPU-time reads for the unified in-process time-series
// (observability, 2026-06-03).  Self-contained here (not a cross-TU call
// into limits.cc) to keep the sampler a single isolated translation unit;
// the platform code mirrors `limits.cc::currentRssBytes` exactly.
#include <sys/resource.h>   // getrusage (CPU user+sys)
#if defined(__APPLE__)
#  include <mach/mach.h>    // task_info / MACH_TASK_BASIC_INFO (current resident)
#else
#  include <unistd.h>       // sysconf(_SC_PAGESIZE) for /proc/self/statm
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace nix::v3 {

namespace {

std::atomic<bool> g_running{false};
std::atomic<bool> g_stop{false};
std::thread g_thread;
std::mutex g_cvMutex;
std::condition_variable g_cv;

/// Current RESIDENT set size in bytes (not peak).  A time-series wants the
/// live footprint at each tick, so this mirrors `limits.cc::currentRssBytes`
/// (macOS mach `resident_size`; Linux /proc/self/statm) — NOT `ru_maxrss`,
/// which is the run's high-water mark.  v3's real arena lives in the
/// malloc-backed `threadArena` (the "elsewhere" bucket invisible to Boehm),
/// so this is the only honest memory-over-time signal in-process.
size_t sampleRssBytes() noexcept
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

/// Cumulative process CPU time (user+sys) in milliseconds, all threads.
/// Emitted as a running total per tick so the consumer can differentiate
/// Δcpu_ms/Δwall_ms into a CPU% — this is the ONLY CPU-time signal v3 emits
/// (every other timer in the VM is wall-clock; see OBSERVABILITY_AUDIT).
long long sampleCpuMillis() noexcept
{
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) return -1;
    return static_cast<long long>(ru.ru_utime.tv_sec) * 1000
         + static_cast<long long>(ru.ru_utime.tv_usec) / 1000
         + static_cast<long long>(ru.ru_stime.tv_sec) * 1000
         + static_cast<long long>(ru.ru_stime.tv_usec) / 1000;
}

void samplerLoop(std::chrono::milliseconds interval)
{
    auto t0 = std::chrono::steady_clock::now();
    while (!g_stop.load(std::memory_order_acquire)) {
        auto now = std::chrono::steady_clock::now();
        auto t_us = std::chrono::duration_cast<std::chrono::microseconds>(now - t0).count();

        // Boehm GC stats — cheap, lock-free, safe from any thread.
        size_t heap = GC_get_heap_size();
        size_t free = GC_get_free_bytes();
        size_t total = GC_get_total_bytes();

        // Process resident + cumulative CPU — the unified-series fields.
        // Read here on the sampler thread; all three syscalls are
        // thread-safe and the sampler's own CPU is negligible (it sleeps).
        size_t rss = sampleRssBytes();
        long long cpu_ms = sampleCpuMillis();

        // Single fprintf for atomicity (no interleaving with other
        // stderr lines from concurrent threads).  The `rss=`/`cpu_ms=`
        // fields are APPENDED after `total=` so the historical
        // perf-trace.py regex (which captures the first four fields)
        // keeps matching unchanged.
        std::fprintf(stderr,
            "v3 heap-trace t_us=%lld heap=%zu free=%zu total=%zu "
            "rss=%zu cpu_ms=%lld\n",
            static_cast<long long>(t_us), heap, free, total,
            rss, cpu_ms);
        std::fflush(stderr);

        // Sleep with interruption-aware wait_for.  If g_stop is set
        // during the sleep, we wake immediately.
        std::unique_lock<std::mutex> lk(g_cvMutex);
        g_cv.wait_for(lk, interval, [] { return g_stop.load(); });
    }
}

} // namespace

bool startHeapTrace()
{
    if (!std::getenv("NIX_V3_HEAP_TRACE")) return false;

    // Idempotent: a second call no-ops.  Using compare_exchange so
    // there's no race between two concurrent startHeapTrace calls
    // (rare, but possible if main() calls it more than once).
    bool expected = false;
    if (!g_running.compare_exchange_strong(expected, true)) return false;

    // Optional cadence override.  Clamped to [10ms, 60s] to avoid
    // pathological values.
    std::chrono::milliseconds interval{50};
    if (const char * envInt = std::getenv("NIX_V3_HEAP_TRACE_INTERVAL_MS")) {
        long ms = std::atol(envInt);
        if (ms >= 10 && ms <= 60000)
            interval = std::chrono::milliseconds(ms);
    }

    std::fprintf(stderr,
        "v3 heap-trace start interval_ms=%lld\n",
        static_cast<long long>(interval.count()));
    std::fflush(stderr);

    g_stop.store(false, std::memory_order_release);
    g_thread = std::thread(samplerLoop, interval);

    // Detach is safer than join at process exit (no static-destruction
    // order hazard).  We rely on the daemon-thread pattern: the
    // process exit kills the thread.  Explicit stopHeapTrace() before
    // exit is still preferred for clean shutdown.
    g_thread.detach();
    return true;
}

void stopHeapTrace()
{
    if (!g_running.load()) return;
    {
        std::lock_guard<std::mutex> lk(g_cvMutex);
        g_stop.store(true, std::memory_order_release);
    }
    g_cv.notify_all();
    // No join — we detached.  The next sample tick (≤ interval) exits
    // the loop; the thread cleans itself up.
    g_running.store(false);
}

} // namespace nix::v3
