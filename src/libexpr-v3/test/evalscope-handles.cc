/// @file
/// #485: behavioural unit tests for EvalScope handle issuance + scope-
/// bound invalidation.
///
/// Each test exercises a specific aspect of the FFI handle lifetime:
///   1. alloc returns a handle that's valid + resolves to the payload.
///   2. handle stays valid until the enclosing EvalScope dtor runs.
///   3. nested scopes: outer survives inner-scope dtor.
///   4. inner-scope handles are invalidated when inner scope ends.
///   5. crashes-aren't-undefined: random uint64_t inputs to isValid /
///      lookupClosureHandle return false / nullptr safely.
///   6. ABA-defence: a recycled scope generation doesn't accidentally
///      match an old handle (we use a 32-bit generation counter, so
///      ABA is in principle possible after 4 billion scopes -- the
///      test asserts that consecutive scopes get fresh generations).
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/ffi.hh"
#include "v3/value.hh"

#include "nix/util/source-path.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/posix-source-accessor.hh"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <unistd.h>

using namespace nix::v3;

namespace nix::v3 {
class Evaluator
{
public:
    Evaluator() = default;
    Evaluator(const Evaluator &) = delete;
    Evaluator & operator=(const Evaluator &) = delete;
};
}

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond) do { \
    if (cond) { ++g_passed; } \
    else { \
        ++g_failed; \
        std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

static void test_alloc_basic()
{
    Evaluator ev;
    EvalScope sc(ev);

    int payload_a = 42;
    auto h = allocClosureHandle(sc, &payload_a);

    CHECK(isValid(h));
    CHECK(lookupClosureHandle(h) == &payload_a);
}

static void test_invalidation_on_scope_dtor()
{
    Evaluator ev;
    ClosureHandle escaped{0};
    int payload = 7;

    {
        EvalScope sc(ev);
        escaped = allocClosureHandle(sc, &payload);
        CHECK(isValid(escaped));
    }
    // Scope destroyed.  Handle is now invalid.
    CHECK(!isValid(escaped));
    CHECK(lookupClosureHandle(escaped) == nullptr);
}

static void test_nested_scopes()
{
    Evaluator ev;
    int outer_payload = 1, inner_payload = 2;
    ClosureHandle outerH, innerH;

    EvalScope outerScope(ev);
    outerH = allocClosureHandle(outerScope, &outer_payload);
    {
        EvalScope innerScope(ev);
        innerH = allocClosureHandle(innerScope, &inner_payload);
        CHECK(isValid(outerH));
        CHECK(isValid(innerH));
        CHECK(lookupClosureHandle(outerH) == &outer_payload);
        CHECK(lookupClosureHandle(innerH) == &inner_payload);
    }
    // Inner scope destroyed; outer still alive.
    CHECK(isValid(outerH));
    CHECK(!isValid(innerH));
    CHECK(lookupClosureHandle(outerH) == &outer_payload);
    CHECK(lookupClosureHandle(innerH) == nullptr);
}

static void test_random_inputs_are_safe()
{
    // A bogus 64-bit value MUST resolve to invalid / nullptr without
    // crashing -- never trust opaque inputs from FFI consumers.
    ClosureHandle bogus[]{
        ClosureHandle{0},                        // generation 0 (reserved)
        ClosureHandle{0xDEADBEEFCAFEBABEULL},    // arbitrary
        ClosureHandle{uint64_t(-1)},             // all-ones
        ClosureHandle{0x00000001FFFFFFFFULL},    // gen=1 (likely live somewhere) but huge slot
    };
    for (auto h : bogus) {
        CHECK(!isValid(h));
        CHECK(lookupClosureHandle(h) == nullptr);
    }
}

static void test_consecutive_scopes_get_fresh_generations()
{
    // Without a fresh generation per scope, ABA could yield false
    // positives.  Two consecutive scopes' handles MUST not collide.
    Evaluator ev;
    int p1 = 1, p2 = 2;
    ClosureHandle h1, h2;
    {
        EvalScope sc(ev);
        h1 = allocClosureHandle(sc, &p1);
    }
    {
        EvalScope sc(ev);
        h2 = allocClosureHandle(sc, &p2);
        CHECK(isValid(h2));
        CHECK(!isValid(h1));   // h1's scope is dead.
    }
    CHECK(!isValid(h1));
    CHECK(!isValid(h2));
    // The two handles MUST encode different generations.
    CHECK((h1.opaque >> 32) != (h2.opaque >> 32));
}

static void test_multi_alloc_in_scope()
{
    Evaluator ev;
    EvalScope sc(ev);
    int a = 1, b = 2, c = 3;
    auto h1 = allocClosureHandle(sc, &a);
    auto h2 = allocClosureHandle(sc, &b);
    auto h3 = allocClosureHandle(sc, &c);
    CHECK(isValid(h1) && isValid(h2) && isValid(h3));
    CHECK(lookupClosureHandle(h1) == &a);
    CHECK(lookupClosureHandle(h2) == &b);
    CHECK(lookupClosureHandle(h3) == &c);
    // Same scope generation, distinct slot indices.
    CHECK((h1.opaque >> 32) == (h2.opaque >> 32));
    CHECK(uint32_t(h1.opaque) != uint32_t(h2.opaque));
    CHECK(uint32_t(h2.opaque) != uint32_t(h3.opaque));
}

// Sprint priority 1: applyClosure proof-of-life.  These tests don't
// build a real v3 closure (that requires the full lower→compile→run
// pipeline); they verify the FFI surface — invalid-handle path, error
// translation, scope-aware payload retrieval — works end-to-end.

static void test_apply_closure_invalid_handle()
{
    // A handle that's never been allocated must yield Fallible-error
    // without crashing.
    Evaluator ev;
    EvalScope sc(ev);
    ClosureHandle bogus{0xDEADBEEFCAFEBABEULL};
    Value dummyArg{};  // tag=0 / payload=0; never read on this path.
    auto result = applyClosure(sc, bogus, dummyArg);
    CHECK(!result.ok());
    if (!result.ok()) {
        const auto & err = result.error();
        CHECK(!err.msg.empty());
        CHECK(err.msg.find("invalid handle") != std::string::npos);
    }
}

static void test_apply_closure_handle_after_scope_death()
{
    // Allocate a handle in an inner scope, exit the scope, then call
    // applyClosure.  The handle's scope is dead → invalid-handle path.
    Evaluator ev;
    ClosureHandle escaped{0};
    int placeholder = 0;
    {
        EvalScope inner(ev);
        escaped = allocClosureHandle(inner, &placeholder);
        CHECK(isValid(escaped));
    }
    CHECK(!isValid(escaped));

    EvalScope outer(ev);
    Value dummyArg{};
    auto result = applyClosure(outer, escaped, dummyArg);
    CHECK(!result.ok());
}

// Sprint priority 2: Category C Filesystem I/O proof-of-life.

static nix::SourcePath spOfPath(const std::filesystem::path & p)
{
    // Canonicalise to defeat macOS /tmp -> /private/tmp symlink
    // (PosixSourceAccessor::readFile asserts no symlinks in the path).
    std::error_code ec;
    auto resolved = std::filesystem::weakly_canonical(p, ec);
    if (ec) resolved = p;
    return nix::PosixSourceAccessor::createAtRoot(resolved);
}

static void test_readFile_happy()
{
    // Create a temp file with known content; readFile must return it.
    auto tmp = std::filesystem::temp_directory_path()
             / std::filesystem::path("v3-ffi-readFile-XXXXXX");
    std::string tmpStr = tmp.string();
    int fd = mkstemp(tmpStr.data());
    CHECK(fd >= 0);
    const char * content = "hello v3 ffi";
    write(fd, content, std::strlen(content));
    close(fd);

    auto result = readFile(spOfPath(tmpStr));
    CHECK(result.ok());
    if (result.ok()) {
        CHECK(result.unwrap() == content);
    }
    std::filesystem::remove(tmpStr);
}

static void test_readFile_missing_path_errors()
{
    auto bogus = std::filesystem::temp_directory_path()
               / "v3-ffi-no-such-path-please-do-not-exist";
    auto result = readFile(spOfPath(bogus));
    CHECK(!result.ok());
    if (!result.ok()) {
        CHECK(!result.error().msg.empty());
    }
}

static void test_pathExists_round_trip()
{
    // Tempdir always exists.
    auto tmp = std::filesystem::temp_directory_path();
    CHECK(pathExists(spOfPath(tmp)));

    auto bogus = tmp / "v3-ffi-no-such-path-please-do-not-exist";
    CHECK(!pathExists(spOfPath(bogus)));
}

static void test_readDir_happy()
{
    // Create a temp dir with two known entries; readDir must list them.
    auto tmp = std::filesystem::temp_directory_path()
             / std::filesystem::path("v3-ffi-readDir-XXXXXX");
    std::string tmpStr = tmp.string();
    char buf[1024];
    std::strncpy(buf, tmpStr.c_str(), sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char * mk = mkdtemp(buf);
    CHECK(mk != nullptr);
    if (mk) {
        std::filesystem::create_directories(std::filesystem::path(mk) / "subdir");
        {
            FILE * f = std::fopen((std::filesystem::path(mk) / "afile").c_str(), "w");
            std::fputs("x", f);
            std::fclose(f);
        }

        auto result = readDir(spOfPath(mk));
        CHECK(result.ok());
        if (result.ok()) {
            const auto & entries = result.unwrap();
            CHECK(entries.find("afile") != entries.end());
            CHECK(entries.find("subdir") != entries.end());
            // Type strings should be one of the expected enumeration.
            for (auto & [n, t] : entries) {
                CHECK(t == "regular" || t == "directory"
                   || t == "symlink" || t == "unknown");
            }
        }
        std::filesystem::remove_all(mk);
    }
}

int main()
{
    test_alloc_basic();
    test_invalidation_on_scope_dtor();
    test_nested_scopes();
    test_random_inputs_are_safe();
    test_consecutive_scopes_get_fresh_generations();
    test_multi_alloc_in_scope();
    test_apply_closure_invalid_handle();
    test_apply_closure_handle_after_scope_death();
    test_readFile_happy();
    test_readFile_missing_path_errors();
    test_pathExists_round_trip();
    test_readDir_happy();

    std::fprintf(stderr, "evalscope-handles: passed=%d failed=%d\n",
                 g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
