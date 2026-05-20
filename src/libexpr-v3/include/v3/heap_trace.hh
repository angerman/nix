/// @file
/// V3 in-process Boehm-heap sampler.  Emits periodic
/// `v3 heap-trace t_us=NNN heap=BBB free=CCC total=TTT` lines to
/// stderr while a v3-eval / nix v3-direct process runs.
///
/// **Why in-process**: Boehm's arena size is invisible from outside
/// the process — `psutil`'s RSS measures the whole resident set
/// (which mixes the Boehm arena with TW Values, mmap'd files, the
/// shared symbol table, etc.).  For honest GC-scan-time
/// attribution we need `GC_get_heap_size()`, which is only callable
/// in-process.
///
/// **Gate**: `NIX_V3_HEAP_TRACE` enables the sampler.  Optional
/// `NIX_V3_HEAP_TRACE_INTERVAL_MS` overrides the default 50 ms
/// cadence.  The pthread is a daemon; it exits cleanly when
/// `nix::v3::stopHeapTrace()` is called (or at process exit).
///
/// **Output format**: one tab-separated `key=val` line per sample,
/// to stderr, parseable by `perf-trace.py`:
///
///   v3 heap-trace t_us=12345678 heap=402653184 free=12345678 total=234567890
///
/// where:
///   - `t_us`  — monotonic microseconds since `startHeapTrace()`
///   - `heap`  — `GC_get_heap_size()` (current arena size in bytes)
///   - `free`  — `GC_get_free_bytes()` (unallocated within the arena)
///   - `total` — `GC_get_total_bytes()` (cumulative allocations,
///               monotonically increasing)
///
/// Per `PERF_TRACE_TOOL_DESIGN_2026-05-20.md` §"In-process Boehm
/// heap probe (~80 LoC, gated)".
///
/// Retirement criterion: when in-process GC stats are exposed via
/// a runtime API that external samplers can read without parsing
/// stderr (i.e. when Stage 3 nursery default-on lands and the GC
/// has a stable telemetry surface), this becomes redundant.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
/// Input Output Group.  SPDX-License-Identifier: Apache-2.0
#pragma once

namespace nix::v3 {

/// Start the sampler thread if `NIX_V3_HEAP_TRACE` is set in the
/// environment.  Idempotent: a second call is a no-op.  Safe to call
/// from main() before any v3 work happens.
///
/// Returns true if the sampler was started (env var present), false
/// otherwise (so callers can log "trace active").
bool startHeapTrace();

/// Signal the sampler to exit + join.  Best-effort; if the sampler
/// is in a sleep cycle it will exit on the next wakeup (max one
/// interval period).  Safe to call multiple times.
void stopHeapTrace();

} // namespace nix::v3
