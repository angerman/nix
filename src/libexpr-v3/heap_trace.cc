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

#include <atomic>
#include <chrono>
#include <condition_variable>
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

        // Single fprintf for atomicity (no interleaving with other
        // stderr lines from concurrent threads).
        std::fprintf(stderr,
            "v3 heap-trace t_us=%lld heap=%zu free=%zu total=%zu\n",
            static_cast<long long>(t_us), heap, free, total);
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
