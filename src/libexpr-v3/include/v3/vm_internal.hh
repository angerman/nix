#pragma once
/// @file
/// v3/vm_internal.hh — shared internal declarations for the vm.cc TU family.
///
/// Step 1 of the vm.cc split (16 KLoC god-file): the pure diagnostic/trace
/// helpers that vm.cc's dispatch loop + forceValue still call, but whose
/// bodies now live in vm_debug.cc.  These are all COLD paths (NIX_TRACE_EVAL
/// / V3_DBG_FORCE_SITE / V3_DBG_FORCE_INSIDE_X), so moving them out-of-line
/// carries no hot-path cost.  Hot-path gates (dbgForceStatsActive,
/// hotForceCheck) deliberately stay `inline` in vm.cc.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/vm.hh"  // Value, Thunk (closure.hh), CompilationUnit (bytecode.hh), VMState

#include <cstdint>
#include <string>

namespace nix::v3 {

/// NIX_TRACE_EVAL helper: human-readable Tag name for `v` (e.g. "Attrs(3)").
/// Used by OP_RETURN's leaveWhnf trace.  (moved from vm.cc — vm_debug.cc)
std::string v3ValueTypeName(Value v);

/// NIX_TRACE_EVAL helper: source position of thunk `t`, or a `<no-pos:...>`
/// tag (verbose form gated on NIX_TRACE_EVAL_VERBOSE_NOPOS).  Used by the
/// enterForce trace.  (moved from vm.cc — vm_debug.cc)
std::string v3ThunkTracePos(const Thunk * t);

/// V3_DBG_FORCE_INSIDE_X tracer — logs Suspended-thunk forces that occur
/// while a Black thunk named "x" is on the frame stack (lib.fix RCA).
/// No-op unless the env var is set.  (moved from vm.cc — vm_debug.cc)
[[gnu::cold]] void dbgLogForceInsideX(VMState & vm, const Value * forcing);

/// V3_DBG_FORCE_SITE tracer — logs "OP_FORCE@ip=N site=..." for each
/// force-flavoured opcode dispatched, resolving the emit-site side-table.
/// No-op unless the env var is set.  (moved from vm.cc — vm_debug.cc)
[[gnu::cold]] void dbgLogForceSite(const CompilationUnit * cu, uint32_t instrIp,
                                   const Value * forcing = nullptr);

// --- Interning subsystems (vm_interning.cc, step 2 of the vm.cc split) ------
//
// Env-tuple interning + captured-withs singleton interning were extracted from
// vm.cc.  The entry points below were file-local (`static` / anonymous-namespace
// `inline`) but are called from vm.cc's dispatch / creation paths and its
// gen-major safepoint (clear*), so the move promotes them to external linkage.
// The GC-facing stats + scavenge-refresh wrappers (getCapWithsHits / Misses /
// Evicts + refreshCapWithsCacheAfterScavenge, called from run.cc + gc.cc) are
// declared in v3/vm.hh, unchanged.  pushCapturedWiths stays `inline` in vm.cc
// (hot call-path loop, deliberately NOT extracted).

/// Env-tuple interning: share a byte-identical nUp-slot upvalue Env off the top
/// of vm.valueStack.  DEFAULT-DISABLED (shareAfter returns UINT32_MAX ⇒ nullptr
/// ⇒ caller allocates the Env inline).  (moved from vm.cc — vm_interning.cc)
Env * maybeInternUpvalueEnvFromStack(VMState & vm, uint16_t nUp);

/// Clear the Env-tuple intern table.  Called at the major-GC safepoint before
/// mark/sweep so no stale Env* survives a collection.  (vm_interning.cc)
void clearEnvInternTable() noexcept;

/// Clear the captured-withs singleton cache (a weak cache — major GC must not
/// keep cache-only ListVecs alive).  Called at the major-GC safepoint.
/// (vm_interning.cc)
void clearCapWithsCache() noexcept;

/// Intern-or-allocate a 1-element capturedWiths ListVec for `v` (O(1) hit).
/// Called on the closure/thunk creation path.  (vm_interning.cc)
ListVec * internOrAllocSingletonCapWiths(const Value & v) noexcept;

/// Snapshot the current frame's visible with-stack into a fresh ListVec (or the
/// interned singleton for the 1-element case); nullptr when no withs are in
/// scope.  Called on the closure/thunk creation path.  (vm_interning.cc)
ListVec * snapshotCurrentWiths(VMState & vm);

// --- Applied-import RESULT cache — decision half (vm_applied_cache.cc) -------
//
// Step 3 of the vm.cc split.  The LEVER-1 applied-import result cache's policy /
// key-building / probe / shadow-validation cluster moved out of vm.cc.  These
// five entry points were file-local (anonymous namespace) but are called from
// vm.cc's OP_CALL / OP_TAIL_CALL / callClosure apply paths + OP_RETURN
// (shadow-compare / insert), so the move promotes them to external linkage.
// The file-local helpers (AppliedCacheProbeStats / appliedCacheProbeStats /
// kAppliedKeyBudget / appliedProbeBoundedKey / appliedKeyPrecheck /
// appliedShadowCompareOne) stay `static`/anonymous in vm_applied_cache.cc.  The
// cache STORAGE half (appliedCacheLookup / Insert / LookupPeek / Note* /
// StatsDump / RecordImportResult / IsImportResultDesc) lives in primops.cc and
// is declared in v3/primop.hh, unchanged.

/// Gate: is the applied-import result cache active for reuse?  DEFAULT-ON
/// (NIX_V3_APPLIED_CACHE unset / "1" / "shadow" / other → true; "0"/"off"/
/// "probe"/"count" → false).  (vm_applied_cache.cc)
bool appliedCacheOn() noexcept;

/// True iff NIX_V3_APPLIED_CACHE=shadow — arm+insert but never short-circuit a
/// would-HIT; OP_RETURN lockstep-compares fresh vs cached instead.
/// (vm_applied_cache.cc)
bool appliedCacheShadowMode() noexcept;

/// Build the memo key (callee LambdaDescriptor pointer + canonical args digest)
/// for an application, or return false when the arg is unhashable (non-forcing
/// structural pre-check + canonicalHash).  Called on the apply hot path when the
/// callee is an import-result closure.  (vm_applied_cache.cc)
bool appliedCacheTryKey(const Closure * callee, const Value & arg, std::string & out);

/// Shadow-mode entry (OP_RETURN): structurally compare `fresh` against the cached
/// entry for `key` over WHNF-vs-WHNF nodes only; records the verdict.  No-op when
/// the entry was evicted.  (vm_applied_cache.cc)
void appliedShadowCompare(const std::string & key, const Value & fresh) noexcept;

/// NIX_V3_APPLIED_CACHE=probe/count instrumentation: observe an eligible
/// application (site 0=OP_CALL 1=OP_TAIL_CALL 2=callClosure) and, in probe mode,
/// bounded-force its arg into a per-process key to measure the in-process hit
/// ceiling.  No-op cost unless the probe/count mode is armed.  (vm_applied_cache.cc)
void appliedCacheProbeObserve(VMState & vm, const Closure * callee, const Value & arg,
                              int site, bool hasFormals) noexcept;

} // namespace nix::v3
