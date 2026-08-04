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

} // namespace nix::v3
