/// @file
/// WC-18.2 yield-protocol implementation.  Driver loop + yield helper.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/bridge_yield.hh"

#include "nix/expr/eval.hh"
#include "nix/expr/value.hh"
#include "nix/util/pos-idx.hh"

namespace nix::v3 {

thread_local Mailbox * currentMailbox = nullptr;

/// WC-18.3: depth of active fiber drivers on this thread.  Set
/// even when execution is on the driver's pthread stack (between
/// yields).  Lets primV3CallBridge1 detect re-entrant bridge calls
/// from inside tree-walker forces and skip the fiber spawn (which
/// would be nested ucontext usage; macOS arm64 doesn't reliably
/// support that).
thread_local int activeFiberDriverDepth = 0;

void yieldForceTreeWalker(::nix::EvalState & state, ::nix::Value & v)
{
    Fiber * f = currentFiber;
    if (!f || !currentMailbox) {
        // Not in a fiber — force directly.  Same semantics.
        state.forceValue(v, ::nix::noPos);
        return;
    }
    Mailbox * mb = currentMailbox;
    mb->kind = YieldKind::ForceTreeWalker;
    mb->twValueToForce = &v;
    fiberYield(f);
    // On resume the value has been forced; mailbox kind reset.
}

Value runInFiber(::nix::EvalState & state,
                 std::function<Value(Mailbox *)> body,
                 size_t stackSize)
{
    Mailbox mb;
    auto entry = [&body, &mb](Fiber * /*self*/) {
        Mailbox * saved = currentMailbox;
        currentMailbox = &mb;
        try {
            mb.resultV3 = body(&mb);
        } catch (...) {
            mb.exc = std::current_exception();
        }
        mb.kind = YieldKind::FiberDone;
        currentMailbox = saved;
    };

    Fiber * fiber = stackSize > 0
        ? fiberCreate(entry, stackSize)
        : fiberCreate(entry);
    activeFiberDriverDepth++;
    try {
        for (;;) {
            fiberResume(fiber);
            if (fiber->done) break;
            switch (mb.kind) {
            case YieldKind::ForceTreeWalker: {
                if (mb.twValueToForce)
                    state.forceValue(*mb.twValueToForce, ::nix::noPos);
                mb.kind = YieldKind::None;
                mb.twValueToForce = nullptr;
                break;
            }
            case YieldKind::FiberDone:
                // Fiber set kind=FiberDone in its trampoline tail before
                // setcontext'ing back; the loop will exit via the
                // `fiber->done` check on the next iteration.  But the
                // trampoline already did setcontext; we shouldn't get
                // here.  Defensive break.
                break;
            case YieldKind::None:
                throw std::runtime_error(
                    "v3 fiber yielded with no kind (driver protocol bug)");
            }
        }
    } catch (...) {
        activeFiberDriverDepth--;
        fiberDestroy(fiber);
        throw;
    }
    activeFiberDriverDepth--;
    Value result = mb.resultV3;
    std::exception_ptr exc = mb.exc;
    fiberDestroy(fiber);
    if (exc) std::rethrow_exception(exc);
    return result;
}

} // namespace nix::v3
