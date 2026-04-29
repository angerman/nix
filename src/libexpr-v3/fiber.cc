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

#ifndef MAP_ANON
#error "MAP_ANON not defined — feature-test macros leaked"
#endif

namespace nix::v3 {

thread_local Fiber * currentFiber = nullptr;

namespace {

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
    auto * f = new Fiber{};
    f->entry = std::move(entry);
    f->stackSize = stackSize;
    // WC-18 follow-up: use posix_memalign (page-aligned malloc) for
    // the fiber stack.  Earlier attempt used mmap + PROT_NONE guard
    // page, which appeared to crash but actually succeeded; the
    // original SIGSEGV was unrelated (root cause: feature-test
    // macros hiding MAP_ANON).  Sticking with posix_memalign keeps
    // things simple and matches the verified-working test case.
    // No guard page — the 64 MB stack is large enough that overflow
    // is genuinely a bug worth chasing on its own; we'd just trade
    // one segfault for another.
    size_t pageSize = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
    void * mem = nullptr;
    if (::posix_memalign(&mem, pageSize, stackSize) != 0) {
        delete f;
        throw std::bad_alloc();
    }
    f->stack = mem;
    f->stackSize = stackSize;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (::getcontext(&f->ctx) == -1) {
        std::free(mem);
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
    Fiber * saved = currentFiber;
    // Hand the Fiber* to the trampoline via thread_local — see
    // fiberTrampoline.  Only matters on the FIRST resume (the
    // trampoline reads + clears on entry); subsequent resumes
    // continue from the post-yield swapcontext call.
    pendingFiber = fiber;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (::swapcontext(&fiber->parentCtx, &fiber->ctx) == -1)
        throw std::runtime_error("v3 fiber: swapcontext (resume) failed");
#pragma clang diagnostic pop
    currentFiber = saved;
}

void fiberYield(Fiber * fiber)
{
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
    if (fiber->stack) std::free(fiber->stack);
    delete fiber;
}

} // namespace nix::v3
