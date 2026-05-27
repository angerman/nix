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

#include <cstdio>
#include <cstdlib>
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
            // The cell pointer itself is tenured (allocValue), but its
            // CONTENT may carry a payload to visit.  Walk through.
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

    // -- TODO: FFI bridge tables (v3BridgeLists / v3BridgeAttrsets)
    // These hold v3 Value handles that TW indexes into.  Currently
    // kept alive via Boehm conservative scan.  Subsequent Stage-3
    // sub-commit will expose the iterator surface in primops.cc and
    // wire it here.

    // -- TODO: cellOwnerTable --------------------------------------
    // alloc.hh keeps a Value*→Thunk* map for cell ownership.  Some
    // entries are reachable from the dirty-list / frames already
    // walked; others (e.g., owned by code that's exited the frame
    // but whose cell is still live) need explicit walking.  Audit
    // pending.

    // -- TODO: drvHashCacheMap -------------------------------------
    // In-memory Value cache for derivation eval results.  Survives
    // across primDerivationStrict calls within a process.
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
    // singleton standalone cells survive across eval scopes and are
    // a real component of the root set the future precise GC must
    // cover.
    if (!walkedVm) {
        for (Value * cell : standaloneCellRoots()) {
            if (cell) dv.visitValue(*cell);
        }
    }

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
