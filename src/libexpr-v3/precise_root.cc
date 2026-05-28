/// @file
/// Precise-root enumeration — `walkAllV3Roots` implementation.
///
/// See include/v3/precise_root.hh for the API + scope.  This file
/// walks the well-defined root sources for Stage 3 of the precise-
/// root foundation (lode/GC_PRECISE_ROOT_FOUNDATION_2026-05-27.md).
///
/// Sources NOT YET covered here (subsequent Stage-3 sub-commits):
///   - v3BridgeLists / v3BridgeAttrsets (need primops.cc API exposure)
///   - cellOwnerTable (need alloc.hh accessor in the walk shape)
///   - drvHashCacheMap (need value_serialize.cc API exposure)
///
/// Each TODO is bounded — a few-hour follow-up per source.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/precise_root.hh"
#include "v3/vm.hh"
#include "v3/barrier.hh"
#include "v3/primop.hh"   // walkV3BridgeRoots, walkImportCacheRoots
#include "v3/gc_root.hh"  // Stage 5: walkCppStackRoots

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <unordered_set>

namespace nix::v3 {

namespace {

/// Walk one VMState's roots: stacks + frames.  Pulled out so the
/// main `walkAllV3Roots` can reuse it for the primary vm + each
/// secondary vm in activeVMStack().
inline void walkOneVMState(VMState & vm, RootVisitor & visitor) noexcept
{
    // 1. Operand + local-variable stack.
    for (Value & v : vm.valueStack) visitor.visitValue(v);

    // 2. `with`-expression scope chain.
    for (Value & v : vm.withStack) visitor.visitValue(v);

    // 3. Call frames.  CallFrame::closure is `const Closure *` for
    // documentation hygiene; the const is not a GC-safety constraint.
    // For visitClosure to receive a mutable slot we cast away const
    // (mirrors the scavenger's existing pattern in gc.cc).
    for (CallFrame & f : vm.frames) {
        if (f.closure) {
            Closure * c = const_cast<Closure *>(f.closure);
            visitor.visitClosure(c);
            f.closure = c;  // visitor may have rewritten (moving GC)
        }
        if (f.thunk) {
            visitor.visitThunk(f.thunk);
        }
        if (f.forceWriteTarget) {
            // Stage 6 Day 3 Step 7: forward the cell-pointer ITSELF
            // first (via visitSlot).  If the cell is a standalone
            // allocValue registered in standaloneCellRoots() and the
            // scavenger's walkStandaloneCells has already moved it,
            // visitSlot rewrites f.forceWriteTarget to the new
            // backup-resident address.  Then walk through to the
            // cell's content payload.
            //
            // For nursery scavenges (gc.cc has its own walker — does
            // not call walkOneVMState) tenured cells aren't moved, so
            // this is a no-op there.  For non-moving visitors
            // (NoOp default + auditor) visitSlot is also a no-op.
            visitor.visitSlot(f.forceWriteTarget);
            if (f.forceWriteTarget)
                visitor.visitValue(*f.forceWriteTarget);
        }
    }
}

} // namespace

void walkAllV3Roots(VMState & vm, RootVisitor & visitor) noexcept
{
    // -- Primary VMState --------------------------------------------
    walkOneVMState(vm, visitor);

    // -- Secondary VMStates (nested runFunctionWithUpvalues etc.) ---
    // The nursery is shared across VMStates on the same thread, so
    // a precise walk fired from any vm must cover ALL active vms.
    // Dedup against the primary; the scavenger's dedup against
    // already-visited objects is handled by the visitor itself.
    std::unordered_set<VMState *> walkedVms{&vm};
    for (VMState * other : activeVMStack()) {
        if (!other || !walkedVms.insert(other).second) continue;
        walkOneVMState(*other, visitor);
    }

    // -- Standalone cell roots --------------------------------------
    // Registered global Values (singletons, transient cells).  The
    // cell pointers themselves are tenured; we walk through to their
    // contents.
    for (Value * cell : standaloneCellRoots()) {
        if (cell) visitor.visitValue(*cell);
    }

    // -- Singleton closure registry (Phase 3.7, 2026-05-28) ---------
    // Each entry is the address of a LambdaDescriptor::
    // cachedSingletonClosure field (libc-resident slot holding an
    // arena Closure pointer).  Mark walks each slot so the cached
    // closure survives Phase 3 mark+sweep cycles.  Without this, the
    // libc→arena cross-boundary pointer is invisible to the walker;
    // the closure gets swept; subsequent lambda calls dereference a
    // stale cached pointer.
    for (Closure ** slot : singletonClosureRegistry()) {
        if (slot && *slot) visitor.visitClosure(*slot);
    }

    // -- FFI bridge tables (Stage 3 sub-source 6) -------------------
    // v3BridgeClosures / v3BridgeAttrs / v3BridgeLists hold v3 Value
    // handles that TW indexes into via the bridge primops.  Each
    // entry stores a Value at a stable address; the entry can carry
    // any payload tag, so we route via visitValue's tag dispatch.
    //
    // primops.cc already exposes `walkV3BridgeRoots` (declared in
    // primop.hh) — used by the nursery scavenger and the auditor.
    // Reuse it here with a std::function adapter so the Stage 3
    // walker doesn't duplicate the table-iteration logic.
    {
        std::function<void(Value &)> adapter =
            [&visitor](Value & v) { visitor.visitValue(v); };
        walkV3BridgeRoots(adapter);
    }

    // -- Import cache roots (Stage 3 sub-source 7) -----------------
    // primImport caches results in an in-memory map.  Entries hold
    // v3 Value payloads (typically Tag::Attrs for imported nixpkgs
    // modules) that survive across primImport calls.  Same adapter
    // pattern as the bridge tables.
    {
        std::function<void(Value &)> adapter =
            [&visitor](Value & v) { visitor.visitValue(v); };
        walkImportCacheRoots(adapter);
    }

    // -- Stage 5: C++-stack roots (sub-source 8) -------------------
    // Values held in C++ helper frames + registered via the GcRoot
    // RAII helper.  Forward-looking for Stage 6 production precise
    // GC where the safe-point model permits mid-primop collection.
    // Today's nursery scavenger (Phase D/E) does NOT fire from
    // primop bodies, so this list is typically empty in normal
    // operation — non-zero entries appear only during the brief
    // window between `GcRoot` construct + destruct.
    walkCppStackRoots(visitor);

    // (Note: global root sources walked above are also reachable
    //  via the standalone helper `walkGlobalV3Roots`, used by
    //  end-of-run diagnostics that fire after VMState teardown.)

    // -- NOTE: cellOwnerTable ---------------------------------------
    // alloc.hh::cellOwnerTable() maps cells→owning-Thunk*.  Audit
    // concluded this is METADATA, not a unique root source: a Thunk
    // present in cellOwnerTable is always also reachable via the
    // owning cell (which IS walked through standalone roots / dirty
    // list / frames).  Walking the table separately would
    // double-count.  Left out by design — comment kept so future
    // audits don't reopen this question.

    // -- NOTE: drvHashCacheMap --------------------------------------
    // value_serialize.cc::drvHashCacheMap() is a `std::string →
    // std::string` map of drvPath → SERIALIZED-BYTES.  The values
    // are BYTES (a v3 Value blob's wire format), not live v3 Value
    // pointers.  C++ std::string manages the bytes' lifetime; no
    // v3-heap pointer reaches into this cache.  NOT a precise-root
    // source.  Comment kept for the same reason as cellOwnerTable.
}

void walkGlobalV3Roots(RootVisitor & visitor) noexcept
{
    // Standalone cell roots — singletons + registered transient cells.
    for (Value * cell : standaloneCellRoots()) {
        if (cell) visitor.visitValue(*cell);
    }
    // Stage 5: C++-stack roots (RAII-registered via `GcRoot`).
    walkCppStackRoots(visitor);
    // FFI bridge tables — v3 Value handles indexed by TW.
    {
        std::function<void(Value &)> adapter =
            [&visitor](Value & v) { visitor.visitValue(v); };
        walkV3BridgeRoots(adapter);
    }
    // primImport result cache.
    {
        std::function<void(Value &)> adapter =
            [&visitor](Value & v) { visitor.visitValue(v); };
        walkImportCacheRoots(adapter);
    }
}

namespace {

/// Diagnostic visitor: counts visited pointers per type + dumps a
/// histogram to stderr.  Used by `dumpAllV3Roots` under
/// V3_DBG_ROOT_DUMP=1.
struct DumpVisitor : RootVisitor
{
    size_t closures = 0;
    size_t thunks   = 0;
    size_t bindings = 0;
    size_t lists    = 0;
    size_t pairs    = 0;
    size_t slots    = 0;
    size_t nullSlots = 0;

    void visitClosure  (Closure   * & p) override
    {
        if (p) ++closures; else ++nullSlots;
    }
    void visitThunk    (Thunk     * & p) override
    {
        if (p) ++thunks;   else ++nullSlots;
    }
    void visitBindings (Bindings  * & p) override
    {
        if (p) ++bindings; else ++nullSlots;
    }
    void visitList     (ListVec   * & p) override
    {
        if (p) ++lists;    else ++nullSlots;
    }
    void visitPair     (ValuePair * & p) override
    {
        if (p) ++pairs;    else ++nullSlots;
    }
    void visitSlot     (Value     * & p) override
    {
        if (p) ++slots;    else ++nullSlots;
    }
};

} // namespace

void dumpAllV3Roots() noexcept
{
    static const bool s_enabled =
        std::getenv("V3_DBG_ROOT_DUMP") != nullptr;
    if (!s_enabled) return;

    DumpVisitor dv;

    // Walk active VMStates if any.  At end-of-run (the typical wire-
    // point) all frames are unwound and activeVMStack is empty —
    // that's correct GC-wise (no live thread roots) but means the
    // VM-specific counts will be zero.  Global roots
    // (standaloneCellRoots, etc.) are walked regardless.
    const auto & stack = activeVMStack();
    bool walkedVm = false;
    if (!stack.empty() && stack.back()) {
        walkAllV3Roots(*stack.back(), dv);
        walkedVm = true;
    }
    // Even when no VMState is active, walk the global roots: the
    // singleton standalone cells + FFI bridge tables survive across
    // eval scopes and are a real component of the root set the
    // future precise GC must cover.
    if (!walkedVm) walkGlobalV3Roots(dv);

    std::fprintf(stderr,
        "v3-direct precise-root dump (vm-active=%s):\n"
        "  closures = %zu\n"
        "  thunks   = %zu\n"
        "  bindings = %zu\n"
        "  lists    = %zu\n"
        "  pairs    = %zu\n"
        "  slots    = %zu\n"
        "  null     = %zu (slots holding null; not heap pointers)\n"
        "  TOTAL    = %zu non-null root pointers visited\n",
        walkedVm ? "yes" : "no (only global roots walked)",
        dv.closures, dv.thunks, dv.bindings,
        dv.lists, dv.pairs, dv.slots, dv.nullSlots,
        dv.closures + dv.thunks + dv.bindings +
        dv.lists + dv.pairs + dv.slots);
}

} // namespace nix::v3
