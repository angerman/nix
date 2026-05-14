#pragma once
/// @file
/// Cross-evaluator eval-order trace.
///
/// Both the tree-walker (libexpr) and v3 (libexpr-v3) emit identically
/// formatted lines to a per-evaluator log when `NIX_TRACE_EVAL` is set,
/// so the two traces can be diff'd to find eval-order divergences.
///
/// Output schema (one event per line):
///
///   F <depth> <pos>          — about to force a thunk / expr
///   W <depth> <pos> -> <ty>  — force returned WHNF of type <ty>
///   B <depth> <pos>          — force hit a blackhole / cycle
///
/// where
///
///   <depth> is the active force-depth (thread-local counter, incremented
///           on every traced FORCE entry, decremented on WHNF/BLACK exit).
///           Both TW and v3 maintain it via the same RAII helper below.
///   <pos>   is the source position, normalized so the `/nix/store/<hash>-`
///           prefix is stripped (`/nix/store/abc-source/lib/foo.nix:10:5`
///           → `<store>/source/lib/foo.nix:10:5`).  Positions that have no
///           source attached become `<no-pos>`.
///   <ty>    is one of: Int, Float, Bool, Null, String, Path, List(N),
///           Attrs(N), Lambda, PrimOp, PrimOpApp, External, Thunk,
///           Blackhole.  N is the element/attr count when cheap to read.
///
/// Routing:
///
///   NIX_TRACE_EVAL=1              -> stderr
///   NIX_TRACE_EVAL=/path/to/file  -> append to file (truncated at process
///                                    start when the same path is seen for
///                                    the first time this run)
///
/// The driver script (`maintainers/eval-trace-diff.sh`) runs the two
/// evaluators on the same expression with distinct output files and runs
/// `diff` after path-normalization.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
/// Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <exception>
#include <string>
#include <string_view>

namespace nix::evalTrace {

/// Return the FILE* the trace writes to (stderr or a per-path-opened
/// FILE*), or nullptr when `NIX_TRACE_EVAL` is unset.  Cached after first
/// call; safe to use on the hot path.
inline FILE * sink() noexcept
{
    static FILE * cached = []() -> FILE * {
        const char * v = std::getenv("NIX_TRACE_EVAL");
        if (!v || !*v) return nullptr;
        if (std::strcmp(v, "1") == 0) return stderr;
        // Truncate-on-first-open per process so two invocations of the
        // same binary in a row don't accumulate stale lines.  fopen "w"
        // achieves that; subsequent traffic in this process appends via
        // the same FILE*.
        FILE * f = std::fopen(v, "w");
        if (!f) {
            std::fprintf(stderr,
                "NIX_TRACE_EVAL: failed to open '%s' for writing; "
                "falling back to stderr\n", v);
            return stderr;
        }
        // Line-buffered so partial traces survive a crash mid-eval.
        std::setvbuf(f, nullptr, _IOLBF, 0);
        return f;
    }();
    return cached;
}

inline bool enabled() noexcept { return sink() != nullptr; }

/// Thread-local force-depth counter.  Symmetric between TW and v3; the
/// RAII Scope below increments on entry / decrements on exit so depth
/// reflects the *currently in-flight* force chain on this thread, not
/// the C-stack depth (which differs between evaluators).
inline uint32_t & depthRef() noexcept
{
    thread_local uint32_t d = 0;
    return d;
}

/// Strip `/nix/store/<hash>-` prefix down to `<store>/<rest>` so the
/// trace is independent of the absolute store path.  Two roots:
///
///   /nix/store/abc123-source/lib/foo.nix  ->  <store>/source/lib/foo.nix
///   /nix/store/abc123-source             ->  <store>/source
///
/// Anything not under `/nix/store/<hash>-` is returned verbatim.
inline std::string normalizePath(std::string_view path)
{
    constexpr std::string_view prefix = "/nix/store/";
    if (path.substr(0, prefix.size()) != prefix)
        return std::string(path);
    auto rest = path.substr(prefix.size());
    auto dash = rest.find('-');
    if (dash == std::string_view::npos)
        return std::string(path);
    auto after = rest.substr(dash + 1);
    std::string out;
    out.reserve(out.size() + 8 + after.size());
    out.append("<store>/");
    out.append(after.data(), after.size());
    return out;
}

/// Compose a "<file>:<line>:<col>" string, or "<no-pos>" when the
/// caller has nothing.
inline std::string formatPos(std::string_view file, unsigned line, unsigned col)
{
    if (file.empty()) return "<no-pos>";
    auto norm = normalizePath(file);
    char buf[32];
    std::snprintf(buf, sizeof buf, ":%u:%u", line, col);
    norm.append(buf);
    return norm;
}

/// Emit a "F" line and increment depth.
inline void enterForce(std::string_view pos) noexcept
{
    FILE * f = sink();
    if (!f) return;
    std::fprintf(f, "F %u %.*s\n",
        depthRef(), (int)pos.size(), pos.data());
    ++depthRef();
}

/// Emit a "W" line and decrement depth.  `ty` is the WHNF type string
/// (e.g. "Attrs(11)").
inline void leaveWhnf(std::string_view pos, std::string_view ty) noexcept
{
    FILE * f = sink();
    if (!f) return;
    if (depthRef() > 0) --depthRef();
    std::fprintf(f, "W %u %.*s -> %.*s\n",
        depthRef(),
        (int)pos.size(), pos.data(),
        (int)ty.size(), ty.data());
}

/// Emit a "B" line and decrement depth.
inline void leaveBlack(std::string_view pos) noexcept
{
    FILE * f = sink();
    if (!f) return;
    if (depthRef() > 0) --depthRef();
    std::fprintf(f, "B %u %.*s\n",
        depthRef(), (int)pos.size(), pos.data());
}

/// Emit an "M" (marker) line, no depth change.  Used by callers to
/// tag the trace with the name of an external forceValue / forceDeep
/// call site so we can attribute top-level force entries to their
/// origin (e.g. nix CLI eval.cc lines).  Cheap when disabled.
inline void mark(std::string_view tag) noexcept
{
    FILE * f = sink();
    if (!f) return;
    std::fprintf(f, "M %u %.*s\n",
        depthRef(), (int)tag.size(), tag.data());
}

/// RAII helper that emits F on construction and matching W (or B if
/// destroyed during stack unwinding) on destruction.  Uses
/// `std::uncaught_exceptions()` to distinguish normal exit from
/// throw, so callers never need explicit try/catch around the eval.
/// Skipped entirely when tracing is disabled.
struct Scope
{
    std::string pos;
    std::string ty;
    int initialUncaught;
    bool armed;

    explicit Scope(std::string p)
        : pos(std::move(p))
        , initialUncaught(0)
        , armed(enabled())
    {
        if (armed) {
            initialUncaught = std::uncaught_exceptions();
            enterForce(pos);
        }
    }

    /// Record the WHNF type of the result.  If not called, the W line
    /// will report "Unknown".
    void recordWhnf(std::string t)
    {
        if (armed) ty = std::move(t);
    }

    ~Scope()
    {
        if (!armed) return;
        if (std::uncaught_exceptions() > initialUncaught)
            leaveBlack(pos);
        else
            leaveWhnf(pos, ty.empty() ? std::string_view("Unknown") : std::string_view(ty));
    }

    Scope(const Scope &) = delete;
    Scope & operator=(const Scope &) = delete;
};

} // namespace nix::evalTrace
