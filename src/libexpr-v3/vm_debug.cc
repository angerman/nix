/// @file
/// v3 VM diagnostic / trace helpers — extracted from vm.cc (step 1 of the
/// vm.cc split).  PURE MOVE: these definitions are byte-for-byte the ones
/// that used to live in vm.cc's anonymous namespace; only their linkage
/// changed (internal → external, declared in v3/vm_internal.hh) so the
/// dispatch loop + forceValue in vm.cc can call them across the TU boundary.
///
/// All four are COLD paths:
///   - v3ValueTypeName / v3ThunkTracePos: NIX_TRACE_EVAL formatting.
///   - dbgLogForceInsideX: V3_DBG_FORCE_INSIDE_X (lib.fix eval-order RCA).
///   - dbgLogForceSite:    V3_DBG_FORCE_SITE (per-force emit-site trace).
///
/// The hot-path gates that share this diagnostic character but sit on the
/// dispatch hot path (dbgForceStatsActive @ OP_MAKE_THUNK, hotForceCheck @
/// thunk-force) deliberately stay `inline` in vm.cc — out-of-lining them
/// would insert a call on the hottest paths.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/vm_internal.hh"
#include "v3/vm.hh"          // VMState, CFF_THUNK_RETURN, Value/Thunk/CompilationUnit
#include "v3/alloc.hh"       // resolvePosSnapshot, PosSnapshot
#include "v3/cu_registry.hh" // cuForDesc
#include "v3/ffi.hh"         // nix::evalTrace::formatPos

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace nix::v3 {

/// NIX_TRACE_EVAL helpers used by OP_FORCE / OP_RETURN to emit
/// F / W events whenever a CFF_THUNK_RETURN frame is pushed or
/// popped.  Definitions live here so both dispatchLoop (which
/// contains OP_FORCE / OP_RETURN) and forceValue in vm.cc
/// can call them with consistent formatting.
std::string v3ValueTypeName(Value v)
{
    switch (v.tag()) {
    case Tag::Int:       return "Int";
    case Tag::Float:     return "Float";
    case Tag::Bool:      return "Bool";
    case Tag::Null:      return "Null";
    case Tag::String:    return "String";
    case Tag::Path:      return "Path";
    case Tag::List: {
        char b[32];
        std::snprintf(b, sizeof b, "List(%zu)",
            v.asList() ? (size_t)v.asList()->size : (size_t)0);
        return b;
    }
    case Tag::Attrs: {
        char b[32];
        std::snprintf(b, sizeof b, "Attrs(%zu)",
            v.asAttrs() ? (size_t)v.asAttrs()->size : (size_t)0);
        return b;
    }
    case Tag::Closure:   return "Lambda";
    case Tag::PrimOp:    return "Lambda";
    case Tag::PrimOpApp: return "Lambda";
    case Tag::Thunk:     return "Thunk";
    case Tag::App:       return "App";
    case Tag::App3:      return "App3";
    case Tag::Blackhole: return "Blackhole";
    case Tag::External:  return "External";
    case Tag::Slot:      return "Slot";
    case Tag::Uninitialized: return "Uninitialized";
    }
    return "?";
}

std::string v3ThunkTracePos(const Thunk * t)
{
    if (!t) return "<no-pos>";
    // 2026-05-18: disambiguate <no-pos> cases for cc-wrapper bisection.
    // Pre-fix the kind annotation when the thunk lacks position info so
    // the cross-evaluator NIX_TRACE_EVAL diff makes the source of the
    // mystery thunk visible.  Gated on NIX_TRACE_EVAL_VERBOSE_NOPOS so
    // normal trace stays clean for fixture comparisons.
    static const bool s_verboseNoPos =
        std::getenv("NIX_TRACE_EVAL_VERBOSE_NOPOS") != nullptr;
    auto formatNoPos = [t](const char * label) -> std::string {
        if (!s_verboseNoPos) return "<no-pos>";
        char buf[96];
        std::snprintf(buf, sizeof buf,
            "<no-pos:%s t=%p st=%d>", label, (void *)t, (int)t->state);
        return std::string(buf);
    };
    if (t->state == ThunkState::Evaluated) return formatNoPos("evald");
    if (t->state == ThunkState::Native) return formatNoPos("native");
    const LambdaDescriptor * d = t->suspended.desc;
    if (!d) return formatNoPos("nodesc");
    const PosSnapshot * ps = resolvePosSnapshot(d->posHandle);
    if (!ps) return formatNoPos("noresol");
    if (ps->file.empty()) return formatNoPos("emptyfile");
    return nix::evalTrace::formatPos(ps->file, ps->line, ps->column);
}

// T2 (LIST_ITERATION_FIX_PLAN_2026-06-08) — these force-trace gates are
// read on the per-element OP_FORCE *slow* path (every non-WHNF force, i.e.
// once per lazy list element in a fold/map).  Caching them at NAMESPACE
// scope makes each read a plain global load; a function-local `static const`
// instead carries a guard-variable check (acquire-load + branch) on every
// call.  Behaviour is identical (same env var, same enabled/disabled).
// Retirement: fold into one V3_DBG_* dispatch flag if the trace surface
// grows.  (lint-no-inline-getenv: `static const` keyword present.)
static const bool g_dbgForceInsideX =
    std::getenv("V3_DBG_FORCE_INSIDE_X") != nullptr;
static const bool g_dbgForceSite =
    std::getenv("V3_DBG_FORCE_SITE") != nullptr;

/// V3_DBG_FORCE_INSIDE_X — tightly scoped force tracer for the v3-direct
/// nixpkgs eval-order RCA.  Fires only when there's a Black thunk
/// named "x" anywhere on the frame stack (== lib.fix's x_thunk being
/// forced).  Logs the forced thunk's name + codeOffset, the forcing
/// site's bytecode IP, and the immediate enclosing thunk/closure
/// frame.  Capped at 200 entries so it doesn't flood.  Useful for
/// finding the v3-specific eager force that has no TW analog —
/// compare two traces (one v3-direct + STG, one a synthetic that
/// works) and the divergent line is the smoking gun.
[[gnu::cold]]
void dbgLogForceInsideX(VMState & vm, const Value * forcing)
{
    if (__builtin_expect(!g_dbgForceInsideX, 1)) return;
    static thread_local int s_logged = 0;
    if (s_logged >= 2000) return;
    bool insideX = false;
    for (const auto & f : vm.frames) {
        if (!(f.flags & CFF_THUNK_RETURN)) continue;
        if (!f.thunk) continue;
        if (f.thunk->state != ThunkState::Blackhole) continue;
        const auto * d = f.thunk->suspended.desc;
        if (d && d->name == "x") { insideX = true; break; }
    }
    if (!insideX) return;
    // Filter: only log Thunk-shaped values (where the force actually
    // does work).  WHNF values (Int/Bool/Attrs/etc.) are no-ops and
    // would flood the log.  We DO want Tag::Slot since that's how
    // captured rec / lambda-param refs reach us.
    if (!forcing) return;
    Value chased = *forcing;
    if (chased.tag() == Tag::Slot && chased.asSlot())
        chased = *chased.asSlot();
    if (chased.tag() != Tag::Thunk && !chased.isAppLike())
        return;
    // Filter: only log Suspended thunks (the FIRST force that flips
    // state to Blackhole).  Already-Evaluated thunks are harmless
    // and just flood the log.  Bridge/Blackhole are also informative.
    if (chased.tag() == Tag::Thunk && chased.asThunk()
        && chased.asThunk()->state == ThunkState::Evaluated)
        return;
    // Identify the forcing site: innermost frame's name + ip.
    const char * outerName = "?";
    uint32_t outerCodeOff = 0;
    uint32_t outerIp = 0;
    if (!vm.frames.empty()) {
        const auto & f = vm.frames.back();
        const LambdaDescriptor * d = nullptr;
        if (f.closure) d = f.closure->desc;
        else if (f.thunk) d = f.thunk->suspended.desc;
        if (d && !d->name.empty()) {
            outerName = d->name.c_str();
            outerCodeOff = d->codeOffset;
        }
        outerIp = f.ip;
    }
    // Identify forcee (the thunk we're about to force).
    const char * forcedName = "?";
    uint32_t forcedCodeOff = 0;
    void * forcedThunk = nullptr;
    int forcedState = -1;
    if (chased.tag() == Tag::Thunk && chased.asThunk()) {
        forcedThunk = (void *)chased.asThunk();
        forcedState = (int)chased.asThunk()->state;
        if (chased.asThunk()->state == ThunkState::Suspended
            && chased.asThunk()->suspended.desc) {
            const auto * d = chased.asThunk()->suspended.desc;
            if (!d->name.empty()) forcedName = d->name.c_str();
            forcedCodeOff = d->codeOffset;
        }
    }
    std::fprintf(stderr,
        "FORCE-IN-X[%d] outer=%s@codeOff=%u ip=%u forced=%s thunk=%p codeOff=%u state=%d frames=%zu\n",
        s_logged++,
        outerName, (unsigned)outerCodeOff, (unsigned)outerIp,
        forcedName, forcedThunk, (unsigned)forcedCodeOff,
        forcedState, vm.frames.size());
}

/// V3_DBG_FORCE_SITE diagnostic: log "OP_FORCE@ip=N site=lower.cc:LINE"
/// for each force-flavoured opcode dispatched.  Reads the side-table
/// `cu->forceEmitSites` populated by emit.cc.  The env-var check is
/// done exactly once (static-once-init) so when the var is unset the
/// branch predictor will skip this entirely — no runtime cost in the
/// default build.
///
/// `instrIp` is the bytecode offset of the force opcode itself (i.e.
/// `ip - 1` at the OP_FORCE / OP_GET_LOCAL_FORCE / OP_GET_UPVALUE_FORCE
/// entry, before any further increments).  Lookup is via std::lower_bound
/// on the (already-sorted) side-table — O(log N) where N is the number
/// of force emit sites in the CU.
[[gnu::cold]]
void dbgLogForceSite(const CompilationUnit * cu, uint32_t instrIp,
                     const Value * forcing)
{
    if (__builtin_expect(!g_dbgForceSite, 1)) return;   // T2: hoisted gate
    if (!cu) return;
    const auto & tbl = cu->forceEmitSites;
    // lower_bound finds the first entry with offset >= instrIp; since
    // entries are unique per offset the equality case is what we want.
    auto it = std::lower_bound(
        tbl.begin(), tbl.end(), instrIp,
        [](const std::pair<uint32_t, const char *> & e, uint32_t v) {
            return e.first < v;
        });
    const char * site = (it != tbl.end() && it->first == instrIp)
        ? it->second
        : "<unknown>";
    // Option-1 enrichment: log the thunk pointer + creation codeOffset
    // when forcing a Thunk-shape value, so post-processing can trace
    // back which Nix expression's thunk is being forced.  pkgs.X
    // thunks created via `inherit (rec {...}) X` have stable codeOffsets
    // identifiable by name in the disasm.
    //
    // Also chase one level through Tag::Slot to surface the underlying
    // thunk pointer — useful because OP_GET_LOCAL_FORCE on a let-rec
    // slot reads through Tag::Slot first.
    const Value * v = forcing;
    Value chased{};
    if (v && v->tag() == Tag::Slot && v->asSlot()) {
        chased = *v->asSlot();
        v = &chased;
    }
    if (v && v->tag() == Tag::Thunk && v->asThunk()) {
        const Thunk * t = v->asThunk();
        uint32_t codeOff = 0;
        const char * tname = "?";
        const void * thunkCu = nullptr;
        if (t->state == ThunkState::Suspended && t->suspended.desc) {
            auto * d = t->suspended.desc;
            codeOff = d->codeOffset;
            if (!d->name.empty()) tname = d->name.c_str();
            thunkCu = (const void *)cuForDesc(d);  // WS5-D1: was d->cu
        }
        std::fprintf(stderr,
            "OP_FORCE@ip=%u site=%s thunk=%p name=%s codeOff=%u state=%d cu=%p caller_cu=%p\n",
            (unsigned)instrIp, site, (const void *)t, tname,
            (unsigned)codeOff, (int)t->state, thunkCu, (const void *)cu);
        return;
    }
    std::fprintf(stderr, "OP_FORCE@ip=%u site=%s\n",
                 (unsigned)instrIp, site);
}

} // namespace nix::v3
