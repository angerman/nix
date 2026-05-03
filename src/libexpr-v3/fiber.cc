/// @file
/// WC-18 Fiber implementation.  See include/v3/fiber.hh for design.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/fiber.hh"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <new>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>
#include <signal.h>

#ifndef MAP_ANON
#error "MAP_ANON not defined — feature-test macros leaked"
#endif

namespace nix::v3 {

thread_local Fiber * currentFiber = nullptr;

namespace {

/// Dump faulting register state to stderr.  Installed only when
/// V3_DBG_FIBER_SEGV is set.  Helps diagnose ucontext-switch crashes
/// where the standard backtrace tool sees only one frame because PC
/// has been redirected to garbage.
[[noreturn]] static void fiberSegvHandler(int sig, siginfo_t * info, void * uctx)
{
    auto * uc = static_cast<ucontext_t *>(uctx);
    std::fprintf(stderr,
        "\n=== FIBER SEGV HANDLER ===\n"
        "sig=%d code=%d faultAddr=%p\n",
        sig, info ? info->si_code : -1,
        info ? info->si_addr : (void *)0);
#if defined(__APPLE__) && defined(__aarch64__)
    if (uc) {
        auto * mc = uc->uc_mcontext;
        if (mc) {
            std::fprintf(stderr,
                "  pc =0x%016llx\n"
                "  sp =0x%016llx\n"
                "  fp =0x%016llx\n"
                "  lr =0x%016llx\n"
                "  cpsr=0x%08x\n",
                (unsigned long long)mc->__ss.__pc,
                (unsigned long long)mc->__ss.__sp,
                (unsigned long long)mc->__ss.__fp,
                (unsigned long long)mc->__ss.__lr,
                (unsigned)mc->__ss.__cpsr);
            for (int i = 0; i < 29; ++i) {
                std::fprintf(stderr, "  x%-2d=0x%016llx%s",
                    i, (unsigned long long)mc->__ss.__x[i],
                    (i % 2 == 1 || i == 28) ? "\n" : "  ");
            }
        }
    }
#endif
    std::fprintf(stderr,
        "currentFiber=%p\n", (void *)currentFiber);
    if (currentFiber) {
        std::fprintf(stderr,
            "  fiber stack=[%p..%p) size=%zu\n",
            currentFiber->stack,
            (char *)currentFiber->stack + currentFiber->stackSize,
            currentFiber->stackSize);
    }
    std::fflush(stderr);
    // Re-raise default to get the core/abort.
    signal(sig, SIG_DFL);
    raise(sig);
    _exit(128 + sig);
}

static void maybeInstallSegvHandler()
{
    static bool installed = false;
    if (installed) return;
    if (!std::getenv("V3_DBG_FIBER_SEGV")) return;
    struct sigaction sa{};
    sa.sa_sigaction = fiberSegvHandler;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS,  &sa, nullptr);
    installed = true;
    std::fprintf(stderr, "fiber: SEGV handler installed\n");
}

/// Holds the Fiber* for the next trampoline invocation.  makecontext
/// arg-passing on macOS arm64 has reliability issues with multi-arg
/// trampolines; passing via thread_local avoids it entirely.  Set
/// just before swapcontext-into-fiber; the trampoline reads + clears
/// on entry.
thread_local Fiber * pendingFiber = nullptr;

[[noreturn]] static void fiberTrampoline()
{
    Fiber * f = pendingFiber;
    pendingFiber = nullptr;
    static const bool dbg = std::getenv("V3_DBG_FIBER") != nullptr;
    if (dbg) std::fprintf(stderr,
        "fiber: trampoline enter f=%p stack=[%p..%p)\n",
        (void*)f, f->stack, (char*)f->stack + f->stackSize);
    Fiber * prev = currentFiber;
    currentFiber = f;
    try {
        f->entry(f);
    } catch (...) {
        f->exc = std::current_exception();
    }
    f->done = true;
    currentFiber = prev;
    // Switch back to driver one last time.  Driver's loop sees
    // fiber->done and exits.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    setcontext(&f->parentCtx);
#pragma clang diagnostic pop
    // setcontext does not return.
    std::abort();
}

}  // namespace

Fiber * fiberCreate(std::function<void(Fiber *)> entry, size_t stackSize)
{
    maybeInstallSegvHandler();
    auto * f = new Fiber{};
    f->entry = std::move(entry);
    f->stackSize = stackSize;
    // REVIEW MED-15: mmap + PROT_NONE guard page below the stack.
    // Stack grows DOWN on every supported arch (x86_64, aarch64), so
    // overflow runs into the low-address guard page and triggers
    // SIGSEGV synchronously instead of corrupting whatever lives
    // immediately below in the heap (typically arena blocks).
    //
    // Layout:
    //   [guardPage : pageSize bytes (PROT_NONE)]
    //   [usableStack : stackSize bytes (PROT_READ|PROT_WRITE)]
    // ss_sp is the usable-stack base; ucontext picks ss_sp+ss_size as
    // the initial SP for downward-growing stacks.
    //
    // The earlier attempt at this guard page was abandoned because of
    // unrelated MAP_ANON visibility issues (feature-test macros) --
    // those are fixed via #include <sys/mman.h>.
    size_t pageSize = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
    size_t totalSize = stackSize + pageSize;
    void * region = ::mmap(nullptr, totalSize, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANON, -1, 0);
    if (region == MAP_FAILED) {
        delete f;
        throw std::bad_alloc();
    }
    if (::mprotect(region, pageSize, PROT_NONE) != 0) {
        ::munmap(region, totalSize);
        delete f;
        throw std::runtime_error("v3 fiber: mprotect guard page failed");
    }
    void * mem = static_cast<char *>(region) + pageSize;
    f->stack = mem;
    f->stackSize = stackSize;
    // Track the full mmap'd region so fiberDestroy can munmap it.
    // We re-derive (region, totalSize) from (stack, stackSize, pageSize).

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (::getcontext(&f->ctx) == -1) {
        ::munmap(static_cast<char *>(f->stack) - pageSize, totalSize);
        delete f;
        throw std::runtime_error("v3 fiber: getcontext failed");
    }
    f->ctx.uc_stack.ss_sp = f->stack;
    f->ctx.uc_stack.ss_size = stackSize;
    f->ctx.uc_link = nullptr;  // we explicitly setcontext(&parentCtx) on exit.
    ::makecontext(&f->ctx, fiberTrampoline, 0);
#pragma clang diagnostic pop

    return f;
}

void fiberResume(Fiber * fiber)
{
    static const bool dbg = std::getenv("V3_DBG_FIBER") != nullptr;
    if (dbg) {
        char marker;
        std::fprintf(stderr,
            "fiber: resume f=%p stack=[%p..%p) (driver sp~=%p)\n",
            (void*)fiber, fiber->stack,
            (char*)fiber->stack + fiber->stackSize, (void*)&marker);
        std::fflush(stderr);
    }
    Fiber * saved = currentFiber;
    pendingFiber = fiber;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (::swapcontext(&fiber->parentCtx, &fiber->ctx) == -1)
        throw std::runtime_error("v3 fiber: swapcontext (resume) failed");
#pragma clang diagnostic pop
    currentFiber = saved;
    if (dbg) {
        char m2;
        std::fprintf(stderr,
            "fiber: resume returned f=%p done=%d sp~=%p\n",
            (void*)fiber, (int)fiber->done, (void*)&m2);
        std::fflush(stderr);
    }
}

void fiberYield(Fiber * fiber)
{
    static const bool dbg = std::getenv("V3_DBG_FIBER") != nullptr;
    if (dbg) {
        char marker;
        std::fprintf(stderr,
            "fiber: yield f=%p (fiber sp~=%p)\n",
            (void*)fiber, (void*)&marker);
    }
    Fiber * saved = currentFiber;
    currentFiber = nullptr;  // driver runs without a fiber context.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (::swapcontext(&fiber->ctx, &fiber->parentCtx) == -1) {
        currentFiber = saved;
        throw std::runtime_error("v3 fiber: swapcontext (yield) failed");
    }
#pragma clang diagnostic pop
    currentFiber = saved;
}

void fiberDestroy(Fiber * fiber)
{
    if (!fiber) return;
    if (fiber->stack) {
        // Reverse the layout established in fiberCreate: usableStack
        // sits one page above the mmap'd region, and the total mapping
        // is stackSize + pageSize.
        size_t pageSize = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
        void * region = static_cast<char *>(fiber->stack) - pageSize;
        ::munmap(region, fiber->stackSize + pageSize);
    }
    delete fiber;
}

} // namespace nix::v3
