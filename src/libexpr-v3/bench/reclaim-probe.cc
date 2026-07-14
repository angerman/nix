// D3/WS-6 M1 — Linux reclaim mechanism falsifier.
// Mirrors the v3 arena's two block paths:
//   (a) calloc'd 16 MiB blocks freed via std::free  (freeHugeBlock, alloc.hh)
//   (b) mmap'd 16 MiB blocks released via munmap + MADV_DONTNEED (mapArenaBlock)
// The macOS R1 finding (alloc.hh:2758): (a) returns ~0% RSS (libmalloc magazines)
// and MADV_DONTNEED/FREE is a no-op. This probe measures the SAME on Linux glibc.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

static long rssMB() {
    // VmRSS from /proc/self/statm (field 2 = resident pages).
    FILE * f = std::fopen("/proc/self/statm", "r");
    if (!f) return -1;
    long total = 0, res = 0;
    if (std::fscanf(f, "%ld %ld", &total, &res) != 2) { std::fclose(f); return -1; }
    std::fclose(f);
    return (res * sysconf(_SC_PAGESIZE)) / (1024 * 1024);
}

static const size_t BLK = 16ull * 1024 * 1024;   // 16 MiB, matches kBlockSize
static const int N = 40;                          // 640 MiB total

int main() {
    printf("pagesize=%ld  block=16MiB  N=%d (%zu MiB)\n", sysconf(_SC_PAGESIZE), N, (BLK*N)>>20);
    printf("baseline RSS: %ld MB\n", rssMB());

    // (a) calloc + std::free
    {
        std::vector<void*> blks;
        for (int i = 0; i < N; ++i) {
            void * p = std::calloc(1, BLK);
            std::memset(p, 0xAB, BLK);            // fault every page resident
            blks.push_back(p);
        }
        long peak = rssMB();
        for (void * p : blks) std::free(p);
        long after = rssMB();
        printf("(a) calloc+free : peak=%ld MB  after_free=%ld MB  returned=%ld MB\n",
               peak, after, peak - after);
    }

    // (b) mmap + munmap
    {
        std::vector<void*> blks;
        for (int i = 0; i < N; ++i) {
            void * p = mmap(nullptr, BLK, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
            std::memset(p, 0xCD, BLK);
            blks.push_back(p);
        }
        long peak = rssMB();
        for (void * p : blks) munmap(p, BLK);
        long after = rssMB();
        printf("(b) mmap+munmap : peak=%ld MB  after_munmap=%ld MB  returned=%ld MB\n",
               peak, after, peak - after);
    }

    // (c) mmap + MADV_DONTNEED (release resident pages, keep mapping)
    {
        std::vector<void*> blks;
        for (int i = 0; i < N; ++i) {
            void * p = mmap(nullptr, BLK, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
            std::memset(p, 0xEF, BLK);
            blks.push_back(p);
        }
        long peak = rssMB();
        for (void * p : blks) madvise(p, BLK, MADV_DONTNEED);
        long after = rssMB();
        printf("(c) mmap+DONTNEED: peak=%ld MB  after_madv=%ld MB  returned=%ld MB\n",
               peak, after, peak - after);
        for (void * p : blks) munmap(p, BLK);
    }
    return 0;
}
