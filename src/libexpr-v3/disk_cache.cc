/// @file
/// File-based disk cache for v3 CompilationUnit blobs.  See
/// include/v3/disk_cache.hh.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/disk_cache.hh"
#include "nix/util/hash.hh"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <pthread.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <unistd.h>

namespace nix::v3::disk_cache {

bool CacheKey::empty() const noexcept
{
    for (auto b : bytes) if (b) return false;
    return true;
}

std::string CacheKey::hex() const
{
    static const char hexChars[] = "0123456789abcdef";
    std::string s;
    s.resize(64);
    for (size_t i = 0; i < 32; ++i) {
        s[i * 2]     = hexChars[bytes[i] >> 4];
        s[i * 2 + 1] = hexChars[bytes[i] & 0xF];
    }
    return s;
}

namespace {

void sha256(std::string_view data, CacheKey & out)
{
    nix::Hash h = nix::hashString(nix::HashAlgorithm::SHA256, data);
    static_assert(sizeof(out.bytes) == 32, "CacheKey expects 32-byte SHA-256");
    std::memcpy(out.bytes, h.hash, 32);
}

/// Cache directory: $XDG_CACHE_HOME/nix/v3-bc-v1 or ~/.cache/nix/v3-bc-v1.
/// Computed once on first use; created if missing.  On creation
/// failure (read-only filesystem, etc.) the directory is left as
/// an empty string and lookups/inserts no-op.
std::string & cacheDir()
{
    static std::string dir = []{
        const char * env = std::getenv("NIX_V3_CACHE_DIR");
        std::string base;
        if (env && *env) {
            base = env;
        } else if (const char * xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) {
            base = std::string(xdg) + "/nix/v3-bc-v1";
        } else if (const char * home = std::getenv("HOME"); home && *home) {
            base = std::string(home) + "/.cache/nix/v3-bc-v1";
        } else {
            return std::string{};  // No reasonable place — disable cache.
        }
        // Best-effort mkdir -p.  Walk up the path, mkdir each segment.
        // Failure on the leaf is fatal; we simply leave dir empty.
        size_t pos = 0;
        while (pos < base.size()) {
            size_t next = base.find('/', pos + 1);
            std::string seg = base.substr(0, next == std::string::npos
                                            ? base.size() : next);
            if (!seg.empty())
                mkdir(seg.c_str(), 0755);  // ignore errors; rely on leaf check
            if (next == std::string::npos) break;
            pos = next;
        }
        struct stat st;
        if (stat(base.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
            return std::string{};
        return base;
    }();
    return dir;
}

} // namespace

CacheKey computeKeyForFile(const std::string & path)
{
    CacheKey k{};
    std::ifstream f(path, std::ios::binary);
    if (!f) return k;
    std::ostringstream oss;
    oss << f.rdbuf();
    std::string content = oss.str();
    sha256(content, k);
    return k;
}

CacheKey computeKeyForString(std::string_view content)
{
    CacheKey k{};
    sha256(content, k);
    return k;
}

std::optional<std::string> lookup(const CacheKey & key)
{
    auto & st = stats();
    if (key.empty()) {
        // Caller passed an unset key — no cache hit possible.
        return std::nullopt;
    }
    st.lookups++;
    auto & dir = cacheDir();
    if (dir.empty()) { st.misses++; return std::nullopt; }
    std::string path = dir + "/" + key.hex();
    std::ifstream f(path, std::ios::binary);
    if (!f) { st.misses++; return std::nullopt; }
    std::ostringstream oss;
    oss << f.rdbuf();
    std::string content = oss.str();
    if (content.empty()) { st.misses++; return std::nullopt; }
    st.hits++;
    return content;
}

void insert(const CacheKey & key, std::string_view blob)
{
    auto & st = stats();
    if (key.empty() || blob.empty()) return;
    auto & dir = cacheDir();
    if (dir.empty()) return;
    std::string finalPath = dir + "/" + key.hex();
    // REVIEW_2026-05-04 B-4 / §6.4: writer-unique temp path.  Previous
    // `finalPath + ".tmp"` collided when two concurrent writers (within
    // a process or across processes) hit the same content-hash key.
    // Same key → same blob converges harmlessly TODAY, but any schema
    // drift mid-process or partial write would produce torn data
    // visible to the rename winner.  Disambiguate by pid + thread id +
    // a process-local sequence so each writer has its own temp file.
    static std::atomic<uint64_t> seq{0};
    char buf[64];
    std::snprintf(buf, sizeof buf, ".tmp.%lld.%llu.%llu",
        (long long)::getpid(),
        (unsigned long long)pthread_self(),
        (unsigned long long)seq.fetch_add(1, std::memory_order_relaxed));
    std::string tempPath = finalPath + buf;
    {
        std::ofstream f(tempPath, std::ios::binary | std::ios::trunc);
        if (!f) { st.insertFailures++; return; }
        f.write(blob.data(), static_cast<std::streamsize>(blob.size()));
        if (!f) { st.insertFailures++; std::remove(tempPath.c_str()); return; }
    }
    if (std::rename(tempPath.c_str(), finalPath.c_str()) != 0) {
        // rename can fail if another writer already moved a file into
        // place between our open() and rename().  Same content-hash
        // key → same blob, so the winner's data is correct; we just
        // discard our temp.
        std::remove(tempPath.c_str());
        st.insertFailures++;
        return;
    }
    st.inserts++;
}

Stats & stats() noexcept
{
    static Stats s;
    return s;
}

} // namespace nix::v3::disk_cache
