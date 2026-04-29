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

namespace nix::v3 {

thread_local Fiber * currentFiber = nullptr;

namespace {

// makecontext takes function arguments as ints (legacy API).  We split
// a Fiber* across two ints so it works on 64-bit systems.  Reassemble
// via static_cast<uint64_t>(unsigned)<<32 | unsigned.
[[noreturn]] static void fiberTrampoline(unsigned hi, unsigned lo)
{
    uintptr_t bits = (static_cast<uintptr_t>(hi) << 32) |
                     static_cast<uintptr_t>(lo);
    Fiber * f = reinterpret_cast<Fiber *>(bits);
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
    // mmap with PROT_READ|WRITE; add a guard page below to catch
    // overflow.  Total mapping = stackSize + page (guard).
    size_t pageSize = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
    size_t total = stackSize + pageSize;
    void * mem = ::mmap(nullptr, total, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mem == MAP_FAILED) {
        delete f;
        throw std::bad_alloc();
    }
    // First page = guard.  Make it PROT_NONE so any write into it
    // segfaults rather than silently corrupting whatever is below.
    ::mprotect(mem, pageSize, PROT_NONE);
    f->stack = static_cast<char *>(mem) + pageSize;
    f->stackSize = stackSize;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (::getcontext(&f->ctx) == -1) {
        ::munmap(mem, total);
        delete f;
        throw std::runtime_error("v3 fiber: getcontext failed");
    }
    f->ctx.uc_stack.ss_sp = f->stack;
    f->ctx.uc_stack.ss_size = stackSize;
    f->ctx.uc_link = nullptr;  // we explicitly setcontext(&parentCtx) on exit.
    uintptr_t bits = reinterpret_cast<uintptr_t>(f);
    unsigned hi = static_cast<unsigned>(bits >> 32);
    unsigned lo = static_cast<unsigned>(bits & 0xFFFFFFFFu);
    ::makecontext(&f->ctx, reinterpret_cast<void (*)()>(fiberTrampoline),
                  2, hi, lo);
#pragma clang diagnostic pop

    return f;
}

void fiberResume(Fiber * fiber)
{
    Fiber * saved = currentFiber;
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
    if (fiber->stack) {
        size_t pageSize = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
        void * base = static_cast<char *>(fiber->stack) - pageSize;
        ::munmap(base, fiber->stackSize + pageSize);
    }
    delete fiber;
}

} // namespace nix::v3
