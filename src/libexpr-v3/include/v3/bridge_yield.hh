#pragma once
/// @file
/// WC-18.2 yield protocol for the v3↔tree-walker bridge fiber.
///
/// A Mailbox is a single-slot channel between the v3 fiber and the
/// driver thread.  The fiber writes a YieldKind + payload, switches
/// to the driver, the driver reads, performs the action on its own
/// (large) stack, writes the result, switches back.
///
/// Forward-declares `nix::Value` and `nix::EvalState` so this header
/// has no transitive cost on the v3 hot path; users include
/// `<nix/expr/value.hh>` separately.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/fiber.hh"
#include "v3/value.hh"

#include <exception>

// Forward decls — no nix:: include here.
namespace nix {
class Value;
class EvalState;
class PosIdx;
}

namespace nix::v3 {

/// Yield reasons.  Anything that requires the caller's stack (e.g.
/// tree-walker forceValue, which recurses arbitrarily deep) becomes
/// a yield.
enum class YieldKind : uint8_t {
    None              = 0,
    /// Force a tree-walker Value to WHNF.  Driver runs ns.forceValue
    /// on its own stack and stores the (now-forced) value back.
    ForceTreeWalker   = 1,
    /// The fiber is done; result is in mailbox.resultV3 (or threw).
    FiberDone         = 2,
};

struct Mailbox {
    YieldKind kind = YieldKind::None;
    /// Tree-walker value to force (when kind == ForceTreeWalker).
    /// On resume the fiber sees the value forced in place.
    ::nix::Value * twValueToForce = nullptr;
    /// v3-side result (when kind == FiberDone).
    Value resultV3{};
    /// Exception thrown by the fiber, if any.  Driver rethrows.
    std::exception_ptr exc;
};

/// Yield "force this tree-walker Value" to the driver.  Returns
/// after the driver has forced the value.  The value is forced in
/// place; callers re-read it from the same pointer.  When NOT
/// inside a fiber (currentFiber == null), forces directly on the
/// caller's stack — same semantics, no yield overhead.
void yieldForceTreeWalker(::nix::EvalState & state, ::nix::Value & v);

/// Driver loop: spawn a fiber that runs `body`, repeatedly resume
/// it until done.  When the fiber yields ForceTreeWalker, force on
/// the driver's stack and resume.  Returns the fiber's final v3
/// Value (or rethrows the fiber's exception).
///
/// `stackSize` controls the fiber's stack (default: 16 MiB).  The
/// main-eval wrapper passes a larger value (~128 MiB) because deep
/// nixpkgs eval graphs (stdenv.mkDerivation × transitive deps) can
/// generate ~5000 recursive forceValue/callClosure C-frames at ~14
/// KiB each.  The fiber stack is dedicated and only allocated once
/// per fiber lifetime, so the cost is a one-time mmap (no per-page
/// faults during eval).
Value runInFiber(::nix::EvalState & state,
                 std::function<Value(Mailbox *)> body,
                 size_t stackSize = 0);  // 0 = use kDefaultFiberStack

/// Pointer to the active fiber's Mailbox, or null if not in a fiber.
extern thread_local Mailbox * currentMailbox;

/// WC-18.3: nested-fiber guard.  > 0 when at least one fiber driver
/// is active on this thread, even between yields.  Re-entrant
/// bridge calls consult this and skip fiber spawn — macOS arm64
/// ucontext doesn't reliably handle two simultaneous fibers.
extern thread_local int activeFiberDriverDepth;

} // namespace nix::v3
