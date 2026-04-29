#pragma once
/// @file
/// WC-18 Fiber: lightweight cooperative coroutine for v3↔tree-walker
/// bridge isolation.
///
/// v3's bytecode dispatcher runs on its own stack (separate from the
/// caller's pthread stack).  Cross-VM transitions (forcing tree-walker
/// values, calling tree-walker primops) yield to a driver that
/// performs the action on the caller's stack and resumes the fiber.
/// Result: v3 bytecode evaluation depth is decoupled from the caller's
/// C stack; cross-CU eval-order divergences (WC-17 finding) no longer
/// blow up the C stack of the calling thread.
///
/// Implementation notes:
///   - macOS arm64 has `ucontext_t` available; deprecated since 10.6,
///     but still functional.  We wrap the deprecation warnings.
///   - Default stack size: 16 MiB, mmap'd.  A guard page below the
///     stack catches overflow.
///   - Fibers are NOT GC roots — Boehm scans the active thread's
///     pthread stack only.  WC-13 already registered the v3 arena
///     as a GC root; the only fiber-stack values that matter for GC
///     are tree-walker `nix::Value *` arguments to yields, which the
///     mailbox holds — and the mailbox lives on the driver's stack
///     (which IS scanned).  No additional GC integration needed.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>

// macOS arm64: ucontext.h is gated behind _XOPEN_SOURCE.  But
// _XOPEN_SOURCE alone hides Darwin extensions (e.g. MAP_ANON which
// fiber.cc's mmap'd-stack allocator needs).  Apple's solution:
// define BOTH so ucontext is exposed AND Darwin extensions stay
// visible.  The original WC-18 attempt set only _XOPEN_SOURCE,
// which broke MAP_ANON, made mmap return invalid memory, and
// caused the "macOS arm64 ucontext is unreliable" misdiagnosis.
#if defined(__APPLE__)
#  ifndef _XOPEN_SOURCE
#    define _XOPEN_SOURCE 600
#  endif
#  ifndef _DARWIN_C_SOURCE
#    define _DARWIN_C_SOURCE 1
#  endif
#endif
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <ucontext.h>
#pragma clang diagnostic pop

namespace nix::v3 {

/// Per-fiber state: own stack + ucontext.  Driver retains a Fiber
/// pointer; fiber retains a pointer to the parent context to switch
/// back to via `fiberYield`.
struct Fiber {
    ucontext_t  ctx;            // The fiber's own context.
    ucontext_t  parentCtx;      // The driver's context (set on switch-in).
    void *      stack = nullptr;
    size_t      stackSize = 0;
    void *      mailbox = nullptr;  // opaque; usually a Mailbox * cast.
    bool        done = false;
    std::exception_ptr exc;
    std::function<void(Fiber *)> entry;
};

constexpr size_t kDefaultFiberStack = 16ull * 1024 * 1024; // 16 MiB (closures can recurse deep)

/// Allocate a fiber with the given entry function and stack size.
/// `entry` runs once on the fiber's stack; when it returns, the
/// fiber sets done=true and yields back to the driver one final
/// time.  The caller is responsible for retrieving the result via
/// the mailbox.
Fiber * fiberCreate(std::function<void(Fiber *)> entry,
                    size_t stackSize = kDefaultFiberStack);

/// Start or resume the fiber, switching to its stack.  Returns when
/// the fiber yields (via fiberYield) or finishes.  Use
/// `fiber->done` to distinguish.  If the fiber threw, `fiber->exc`
/// holds the exception for the caller to rethrow.
void fiberResume(Fiber * fiber);

/// Yield control back to the driver.  Called from inside the fiber's
/// entry (or any function it calls).  When the driver later calls
/// fiberResume, execution continues right after this yield.
void fiberYield(Fiber * fiber);

/// Free the fiber's stack and the Fiber struct itself.
void fiberDestroy(Fiber * fiber);

/// Thread-local pointer to the currently-running fiber, or null
/// when not inside a fiber.  Yield points consult this to decide
/// whether to yield (fiber active) or fall through to a direct
/// call (no fiber).
extern thread_local Fiber * currentFiber;

} // namespace nix::v3
