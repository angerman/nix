// R1 page-release spike — POST_F4_GC_GOAL §4 cheap falsifier.
//
// Every one of the 11 prior v3 GC failures hit the arena page-release
// pin: they reclaimed logically but RSS never dropped, because arena
// blocks are `std::calloc`'d (alloc.hh kBlockSize=16 MB) and the only
// release path (`freeWholeBlock`) calls `std::free`.  Before the
// multi-week Nofl-Immix build (R2), this spike answers the deepest
// possible blocker empirically: on THIS platform, does releasing a
// known-dead region actually return its bytes to RSS?
//
// Tests each candidate mechanism in a FRESH process (argv-selected, so
// they cannot contaminate one another):
//   free          — std::free of 16 MB calloc blocks (the freeWholeBlock
//                   path today): does libc's large-alloc free munmap?
//   munmap        — mmap blocks + munmap half (the cleanest whole-block
//                   release; what R2 should use if free() doesn't return).
//   madv_dontneed — madvise(MADV_DONTNEED) whole blocks, keep mapping.
//   madv_free     — madvise(MADV_FREE) whole blocks (macOS-preferred,
//                   lazy under pressure).
//   subblock      — madvise(MADV_DONTNEED) the page-aligned MIDDLE 50% of
//                   each (still-mapped) block: the Immix recyclable-block
//                   / dead-line reclaim case (sub-block release).
//
// Pre-committed thresholds (POST_F4_GC_GOAL §4):
//   >= 80% of freed bytes returned to RSS  -> PASS (mechanism works)
//   <  30%                                  -> deepest blocker (rethink)
//
// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
//   Input Output Group.  SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/mman.h>

#if defined(__APPLE__)
#  include <mach/mach.h>
#endif

namespace {

// Current resident-set bytes — mirrors limits.cc::currentRssBytes().
uint64_t currentRssBytes()
{
#if defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return info.resident_size;
    return 0;
#else
    long pages = 0, dummy = 0;
    std::FILE * f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    int rc = std::fscanf(f, "%ld %ld", &dummy, &pages);
    std::fclose(f);
    if (rc < 2 || pages <= 0) return 0;
    long pgsize = sysconf(_SC_PAGESIZE);
    return static_cast<uint64_t>(pages) * static_cast<uint64_t>(pgsize);
#endif
}

constexpr size_t kBlock = size_t(16) << 20;  // 16 MB, == Arena::kBlockSize

double mb(uint64_t b) { return double(b) / (1u << 20); }

// Touch every page so the kernel commits it (RSS rises to full size).
void commit(char * p, size_t n)
{
    const size_t pg = size_t(sysconf(_SC_PAGESIZE));
    for (size_t i = 0; i < n; i += pg) p[i] = char(i);
    // also write the last byte
    if (n) p[n - 1] = 1;
}

} // namespace

int main(int argc, char ** argv)
{
    std::string mech = argc > 1 ? argv[1] : "free";
    size_t N = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 64;  // blocks
    size_t freeCount = N / 2;
    bool useMmap = (mech == "munmap" || mech == "madv_dontneed"
                    || mech == "madv_free");

    std::vector<char *> blocks(N, nullptr);
    for (size_t i = 0; i < N; ++i) {
        if (useMmap) {
            void * p = mmap(nullptr, kBlock, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON, -1, 0);
            blocks[i] = (p == MAP_FAILED) ? nullptr : static_cast<char *>(p);
        } else {
            blocks[i] = static_cast<char *>(std::calloc(1, kBlock));
        }
        if (!blocks[i]) { std::fprintf(stderr, "alloc %zu failed\n", i); return 2; }
        commit(blocks[i], kBlock);
    }

    uint64_t rssBefore = currentRssBytes();
    uint64_t freedBytes = freeCount * kBlock;

    // Release the first half via the selected mechanism.
    for (size_t i = 0; i < freeCount; ++i) {
        char * b = blocks[i];
        if (mech == "free") {
            std::free(b);
            blocks[i] = nullptr;
        } else if (mech == "munmap") {
            munmap(b, kBlock);
            blocks[i] = nullptr;
        } else if (mech == "madv_dontneed") {
            madvise(b, kBlock, MADV_DONTNEED);
        } else if (mech == "madv_free") {
#ifdef MADV_FREE
            madvise(b, kBlock, MADV_FREE);
#else
            madvise(b, kBlock, MADV_DONTNEED);
#endif
        } else if (mech == "subblock") {
            // Free the page-aligned middle 50% of each block, leaving the
            // first and last quarter "live" (still mapped + resident).
            const size_t pg = size_t(sysconf(_SC_PAGESIZE));
            size_t start = (kBlock / 4 + pg - 1) & ~(pg - 1);
            size_t end   = (kBlock * 3 / 4) & ~(pg - 1);
            madvise(b + start, end - start, MADV_DONTNEED);
        } else {
            std::fprintf(stderr, "unknown mechanism '%s'\n", mech.c_str());
            return 2;
        }
    }
    // subblock frees half of EACH of the N blocks, not the first N/2.
    if (mech == "subblock") freedBytes = N * (kBlock / 2);

    // Re-measure.  MADV_FREE is lazy on macOS (reclaimed under pressure),
    // so sample twice with a brief yield; report the lower.
    uint64_t rssAfter = currentRssBytes();
    for (int k = 0; k < 3; ++k) { usleep(50000); uint64_t r = currentRssBytes(); if (r < rssAfter) rssAfter = r; }

    int64_t returned = int64_t(rssBefore) - int64_t(rssAfter);
    double pct = freedBytes ? 100.0 * double(returned) / double(freedBytes) : 0.0;
    const char * verdict = pct >= 80.0 ? "PASS"
                         : pct >= 30.0 ? "PARTIAL"
                                       : "BLOCKER";

    std::printf("mech=%-13s N=%zu freed=%.0fMB  RSS %.0f->%.0f MB  "
                "returned=%.0f MB (%.1f%% of freed)  %s\n",
                mech.c_str(), N, mb(freedBytes), mb(rssBefore), mb(rssAfter),
                mb(returned > 0 ? uint64_t(returned) : 0), pct, verdict);

    // Keep the surviving half referenced so the compiler can't elide it.
    volatile char sink = 0;
    for (size_t i = freeCount; i < N; ++i) if (blocks[i]) sink ^= blocks[i][0];
    (void)sink;
    return 0;
}
