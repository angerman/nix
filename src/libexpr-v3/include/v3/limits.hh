#pragma once
/// @file
/// v3 evaluator resource limits — Phase 1.6 (2026-05-18).
///
/// Three caps, all opt-in via env var:
///   NIX_V3_MAX_HEAP=2G      — Boehm heap ceiling.  OOM-handler sets an
///                             atomic flag; dispatch loop polls + throws
///                             nix::v3::OutOfMemoryError.
///   NIX_V3_MAX_CPU_TIME=60s — CPU time (user+sys via getrusage).  Polled
///                             every kPollInterval (10k) opcodes in
///                             dispatchLoop; throws CpuTimeExceededError.
///   NIX_V3_MAX_WALL_TIME=5m — wall time via std::chrono::steady_clock.
///                             Same poll cadence; throws
///                             WallTimeExceededError.
///
/// Sizes accept G/M/K (1024-base) suffixes; durations accept s/m/h.
///
/// Design rationale lives in
/// `lode/ACTION_PLAN_2026-05-15.md` Phase 1.6 (lines 94-139).
///
/// The polling overhead is zero when all three env vars are unset
/// (an upfront check elides the entire poll site at the dispatch
/// loop's hot path).  When caps ARE set, a thread-local poll counter
/// gates the getrusage / steady_clock / atomic-flag reads to once
/// per kPollInterval opcodes.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace nix::v3 {

// ---------------------------------------------------------------------------
// Exception hierarchy.  All three are v3-internal std::runtime_error
// subclasses (matching the rest of vm.cc's throw idiom).  The
// `ResourceLimitExceeded` base lets downstream catchers
// (`v3-eval.cc`, `nix eval`'s integration site) match all three at
// once when classifying for exit codes / diagnostic format.
// ---------------------------------------------------------------------------

class ResourceLimitExceeded : public std::runtime_error
{
public:
    explicit ResourceLimitExceeded(const std::string & msg)
        : std::runtime_error(msg) {}
};

class OutOfMemoryError : public ResourceLimitExceeded
{
public:
    explicit OutOfMemoryError(const std::string & msg)
        : ResourceLimitExceeded(msg) {}
};

class CpuTimeExceededError : public ResourceLimitExceeded
{
public:
    explicit CpuTimeExceededError(const std::string & msg)
        : ResourceLimitExceeded(msg) {}
};

class WallTimeExceededError : public ResourceLimitExceeded
{
public:
    explicit WallTimeExceededError(const std::string & msg)
        : ResourceLimitExceeded(msg) {}
};

// ---------------------------------------------------------------------------
// Env-var parsers.  Internal use; exposed for unit-testability.
// ---------------------------------------------------------------------------

/// Parse a size string like "2G" / "512M" / "64K" (1024-base) or a
/// bare decimal number (bytes).  Returns std::nullopt on parse
/// failure or overflow.  Empty string yields nullopt.
std::optional<uint64_t> parseSize(std::string_view s);

/// Parse a duration string like "60s" / "5m" / "1h" or a bare
/// decimal number (seconds).  Returns std::nullopt on parse
/// failure.  Empty string yields nullopt.
std::optional<std::chrono::seconds> parseDuration(std::string_view s);

// ---------------------------------------------------------------------------
// Initialisation + polling.
// ---------------------------------------------------------------------------

/// Initialise limits from environment.  Idempotent (called once per
/// process; subsequent calls are no-ops).  Reads NIX_V3_MAX_HEAP /
/// NIX_V3_MAX_CPU_TIME / NIX_V3_MAX_WALL_TIME, installs the Boehm
/// OOM handler if heap cap is set, captures the eval start time.
///
/// If no env vars are set, this function still runs (cheap) but
/// `limitsActive()` returns false — the dispatch loop's poll
/// branch then has zero per-opcode cost.
void initLimits();

/// Internal: backing global for `limitsActive()`.  Set by
/// `initLimits()` once.  Exposed in the header so the dispatch
/// loop's poll site can read it inline (one load + one branch,
/// no function call).
extern bool _limitsActiveGlobal;

/// True when at least one cap is set (any of heap / CPU / wall).
/// Inlined into vm.cc's hot dispatch loop: one load + one branch
/// when no caps are set.
inline bool limitsActive() noexcept
{
    return _limitsActiveGlobal;
}

/// Polling cadence.  Every `kPollInterval` opcodes the dispatch
/// loop calls checkLimits() which reads getrusage + steady_clock
/// + the OOM atomic flag.  Tuned so per-opcode amortised cost
/// stays under 2ns (the Phase 1.6 exit criterion).
constexpr uint32_t kPollInterval = 10000;

/// Polled by the dispatch loop every kPollInterval opcodes (the
/// outer `if (limitsActive())` is its own guard).  Throws the
/// appropriate typed exception when ANY cap is exceeded.
void checkLimits();

/// Force a check immediately — used by the OOM handler path which
/// runs OUTSIDE the dispatch loop and needs to signal cleanly.
/// Sets the OOM atomic; the next checkLimits() call will throw.
void signalOutOfMemory();

} // namespace nix::v3
