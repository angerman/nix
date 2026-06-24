/// @file
/// v3 starter primop set — implementations against v3::Value.
///
/// Implements a curated subset that covers the most common Nix patterns
/// without requiring store / fetcher / parser plumbing:
///
///   length, head, tail, elemAt
///   attrNames, attrValues, hasAttr, getAttr
///   isAttrs, isList, isFunction, isString, isInt, isBool, isNull,
///   isFloat, isPath
///   toString, typeOf
///   add, sub, mul, div  (numeric — same as the inline arith ops; useful
///                        when invoked indirectly via builtins.<op>)
///   stringLength
///   throw (terminates)
///
/// Bigger primops (import, derivationStrict, fetch*, exec) interface with
/// substantial C++ infrastructure and are deferred to AST integration.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/primop.hh"
#include "v3/alloc.hh"
#include "v3/import_timing.hh"  // #769 per-import phase timing
#include "v3/dedup_survey.hh"   // #772 Stage 9 L0 spike
#include "v3/barrier.hh"  // Phase D write-barrier helpers
// PARSER_PROJECT_PLAN §5.3: native parse + lower for the import path,
// behind NIX_V3_NATIVE_PARSER=1 (+ NIX_V3_NATIVE_LOWER=1), with a
// per-file canLowerV3 fallback to the proven bridge / TW path.
#include "v3-parse-api.hh"   // nix::v3::parser::parseString
#include "lower_v3.hh"       // canLowerV3 + lowerV3Ast (native AST→IR)
#include "v3/tw_baseenv.hh"  // twBaseEnvGlobals (free-name resolution)
#include "v3/vm.hh"
#include "v3/mapattrs_demand.hh"  // unrealized-MapAttrs immediate-demand helpers
#include "v3/ffi.hh"  // FFI plan migration step 1: surface declarations.
#include "v3/errors.hh"
#include "v3/disasm.hh"  // #815 RCA: cached-vs-fresh disassembly
#include "v3/limits.hh"

#include <chrono>

#include "nix/expr/eval.hh"

#include <sys/resource.h>
#if defined(__APPLE__)
# include <mach/mach.h>
# include <mach/task.h>
#endif
#include "v3/print.hh"  // #760: v3 printNixValue for toStringCoerceCtx error text
#include "v3/value_serialize.hh"  // #741 Phase 1: derivation-result round-trip test
#include "nix/expr/value/context.hh"
// v3-native primGetFlake's flake-loading FFI leaf (parseFlakeRef / lockFlake
// / LockFlags / flake::Settings) now lives behind ffi::lockFlakeAndRead, so
// the flake headers are no longer needed here (audit Phase 4).
#include "nix/util/canon-path.hh"
// experimental-features.hh + hash.hh are re-exported by v3/ffi.hh (Layer-0
// shared domain types) — no direct include needed (audit Phase 0/2).

#include <nlohmann/json.hpp>
#include <toml.hpp>

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <array>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <list>
#include <limits>
#include <mutex>
#include <sys/stat.h>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <set>  // C-23: value-based genericClosure key dedup

#include "nix/util/memory-source-accessor.hh"
#include "nix/store/store-api.hh"
#include "nix/store/derived-path.hh"
#include "nix/store/derivations.hh"  // hashPlaceholder
#include "nix/store/content-address.hh"  // ContentAddressMethod
#include "v3/serialize.hh"
#include "v3/disk_cache.hh"
#include "v3/cache_probe.hh"  // #827 / A3 per-call-site cache-hook
#include "v3/ir.hh"
#include "v3/ir_dump.hh"  // R1 trigger trace: V3_DBG_DUMP_IR_PATH
#include "v3/bytecode.hh"


#include <boost/unordered/concurrent_flat_map.hpp>

namespace nix::v3 {

// Hook-removal 2026-05-13: populateSubExprCachePublic and
// registerContentCachePublic are gone with v3_hook.cc.  The sub-Expr
// cache existed only for the force-hook's per-Expr* lookup; the
// content cache mapped sourceContentHash → CU so v3EvalEntry could
// short-circuit a fresh-parse.  Neither is reachable from v3-direct.
// primImport's call sites below are no-ops.

// (ScopedBridgeFallbackExpr + tlBridgeFallbackExpr retired — TW_VALUE_ERADICATION F4, 2026-06-02.)

namespace {

std::unordered_map<std::string, PrimOp> & registry()
{
    static std::unordered_map<std::string, PrimOp> r;
    return r;
}

thread_local nix::EvalState * tlNixEvalState = nullptr;

/// #466 active-v3-vm tracking — defined out-of-line in primop.hh.
///
/// When v3 executes a Bridge-out call (OP_CALL Bridge handler that
/// goes through ns->callFunction → TW → potentially v3 hooks), this
/// thread_local pointer is set to the OUTER v3 VMState.  Lets the
/// call-hook detect "we're being re-entered from inside an outer v3
/// force chain" and refuse early — preventing the cross-VMState
/// BlackHole cycle that's at the heart of the lambda-skip cycle.
inline VMState *& tlActiveV3VMRef()
{
    thread_local VMState * p = nullptr;
    return p;
}

// (bridge-primop depth-guard machinery retired — TW_VALUE_ERADICATION F4, 2026-06-02.)

// ---------------------------------------------------------------------------
// #466 / #479 Phase 1: cross-primop force-chain cycle detector.
// ---------------------------------------------------------------------------
//
// See `ForceChainGuard` doc in primop.hh.  Storage helpers live inside
// the surrounding anonymous namespace so the unordered_set/hash machinery
// doesn't leak.  The class methods are defined out-of-line in the
// `nix::v3` namespace below — needs a temporary close+reopen of the
// enclosing anon ns since out-of-line method definitions cannot live
// inside an anonymous namespace.

struct ForceChainKey {
    ForceChainOp op;
    uint64_t     a;
    uint64_t     b;
    bool operator==(const ForceChainKey & o) const noexcept {
        return op == o.op && a == o.a && b == o.b;
    }
};
struct ForceChainKeyHash {
    size_t operator()(const ForceChainKey & k) const noexcept {
        // Mix tagged op with payloads via FNV-style multiply.  Cheap;
        // collision quality matters less than per-call latency since
        // the set rarely exceeds a few dozen entries in practice.
        uint64_t h = 1469598103934665603ull;
        h ^= static_cast<uint64_t>(k.op); h *= 1099511628211ull;
        h ^= k.a;                          h *= 1099511628211ull;
        h ^= k.b;                          h *= 1099511628211ull;
        return static_cast<size_t>(h);
    }
};
using ForceChainSet = std::unordered_set<ForceChainKey, ForceChainKeyHash>;

inline ForceChainSet & forceChainSet()
{
    thread_local ForceChainSet s;
    return s;
}

inline size_t forceChainMaxDepth()
{
    static const size_t k = []{
        if (const char * v = std::getenv("NIX_V3_FORCE_CHAIN_DEPTH"))
            return static_cast<size_t>(std::max(0, std::atoi(v)));
        return static_cast<size_t>(256);
    }();
    return k;
}

/// #493 step 3c: force-chain detector relaxed from binary set to
/// N-reentry counter, mirroring the bridge1 per-handle counter (step
/// 3b).  Pre-step-3c: first re-entry on the same (op, keyA, keyB)
/// fired BlackholeError; too eager for legitimate structural recursion
/// (nixpkgs overlay chains apply the SAME bridge1 handle across many
/// layers as the fix-point propagates).  TW handles such cases via
/// blackhole-on-thunk-slot which is per-thunk-instance, not per-key.
///
/// Tunable via NIX_V3_FORCE_CHAIN_REENTRY_MAX (default 8).  Same
/// rationale as kBridge1ReentryMax: bound runaway recursion without
/// firing on structurally legitimate re-entries.  The chain depth
/// ceiling (NIX_V3_FORCE_CHAIN_DEPTH, default 256) remains as the
/// outer bound.
inline size_t forceChainReentryMax()
{
    static const size_t k = []{
        if (const char * v = std::getenv("NIX_V3_FORCE_CHAIN_REENTRY_MAX"))
            return static_cast<size_t>(std::max(1, std::atoi(v)));
        return static_cast<size_t>(8);
    }();
    return k;
}

inline std::unordered_map<ForceChainKey, int, ForceChainKeyHash> &
forceChainCounters()
{
    thread_local std::unordered_map<ForceChainKey, int, ForceChainKeyHash> m;
    return m;
}

} // close enclosing anonymous ns (line 108) for ForceChainGuard methods

ForceChainGuard::ForceChainGuard(ForceChainOp op_, uint64_t a_, uint64_t b_)
    : m_op(op_), m_keyA(a_), m_keyB(b_)
{
    auto & chain = forceChainSet();
    size_t maxDepth = forceChainMaxDepth();
    if (maxDepth > 0 && chain.size() >= maxDepth) {
        m_overDepth = true;
        return;
    }
    // #493 step 3c: counter, not binary set.  Same key may re-enter up
    // to forceChainReentryMax() times before declaring cycle.
    ForceChainKey k{m_op, m_keyA, m_keyB};
    auto & counters = forceChainCounters();
    int & cnt = counters[k];
    if (cnt >= static_cast<int>(forceChainReentryMax())) {
        m_overReentry = true;
        return;
    }
    if (cnt == 0)
        chain.insert(k); // membership for set-based callers
    ++cnt;
    m_inserted = true;
}

ForceChainGuard::~ForceChainGuard()
{
    if (m_inserted) {
        ForceChainKey k{m_op, m_keyA, m_keyB};
        auto & counters = forceChainCounters();
        auto it = counters.find(k);
        if (it != counters.end()) {
            if (--it->second <= 0) {
                counters.erase(it);
                forceChainSet().erase(k);
            }
        }
    }
}

namespace { // re-open enclosing anonymous ns (matches close at line 2722)

/// REVIEW MED-13: scoped guard for tlNixEvalState.  Bridge entries
/// installed only on null (`if (!tlNixEvalState) tlNixEvalState = &ns`)
/// would silently use a stale pointer if a different EvalState later
/// re-entered v3 -- e.g., a library consumer (Hydra, LSP, test
/// harness) that creates and destroys multiple EvalStates on the
/// same thread.  Use this RAII guard at every bridge entry to push
/// the current EvalState and restore the previous on exit.
struct ScopedNixEvalState {
    nix::EvalState * prev;
    ScopedNixEvalState(nix::EvalState * cur) : prev(tlNixEvalState) { tlNixEvalState = cur; }
    ~ScopedNixEvalState() { tlNixEvalState = prev; }
};

/// C-6/C-7 (CODEBASE_REVIEW_2026-06-11): NIX_V3_ALLOW_FAKE_STORE=1 is a
/// debug-only escape hatch that re-enables the legacy /v3-fake-store/ path
/// synthesis — in derivationStrict (store wired but native path skipped) and
/// in builtins.path (no store wired at all).  By DEFAULT v3 refuses to
/// fabricate store paths that would silently diverge from a real store and
/// instead surfaces the failure as an error: a fabricated drvPath/store path
/// is the error-masking class that cost weeks on the drvPath divergence.
/// Retirement criterion: delete this gate once no workload or embedding needs
/// fake-store synthesis (the gate exists purely as a debug/embedding escape
/// hatch; it should be unused in all real configurations).
static bool allowFakeStore() {
    static const bool v = std::getenv("NIX_V3_ALLOW_FAKE_STORE") != nullptr;
    return v;
}

// String-context side-table is in alloc.hh — entries are encoded
// strings (`<path>` Opaque, `=<drvPath>` DrvDeep, `!<output>!<drvPath>`
// Built).  Helpers below convert to/from nix::NixStringContext.

/// C-7(d) (CODEBASE_REVIEW_2026-06-11): every side-table context token is
/// produced by v3 itself (encodeStringContext ∘ NixStringContextElem::to_string),
/// so it MUST round-trip back through ::parse.  A parse failure is therefore a
/// v3 invariant violation — corruption (e.g. the M-2 arena char* aliasing
/// class) — and silently skipping it deletes a dependency edge from any drv
/// that consumed the string, producing a wrong drv hash with no error.  Throw
/// loudly, naming the offending token, so the corruption surfaces at its
/// source instead of as a mysterious drvPath divergence downstream.
static void v3InsertContextToken(nix::NixStringContext & ctx,
                                 const std::string & token,
                                 const char * site)
{
    // PLAN_BEAT_TW Phase 0.4 falsifier (2026-06-12): the SAME context token
    // (one dependency edge) is re-parsed through NixStringContextElem::parse
    // at EVERY consuming drv (decodeStringContext runs per lookup).  This
    // thread_local memo bounds the CPU spent re-parsing identical tokens, so
    // we can decide whether the full interned-context lever (1.1) is worth
    // funding (firefox CPU drop ≥3% ⇒ parse-back is material; <3% ⇒ the cost
    // is in the COPIES and 1.1 narrows to span-sharing).  Gate:
    // V3_DBG_CTX_PARSE_MEMO=1.  Retirement (Rule 0): DELETE this whole block
    // once the 1.1 scope is decided — it keeps no production behaviour.
    static const bool s_parseMemo =
        std::getenv("V3_DBG_CTX_PARSE_MEMO") != nullptr;
    if (s_parseMemo) {
        static thread_local std::unordered_map<std::string,
            nix::NixStringContextElem> s_cache;
        auto it = s_cache.find(token);
        if (it != s_cache.end()) { ctx.insert(it->second); return; }
        try {
            auto elem = nix::NixStringContextElem::parse(token);
            ctx.insert(elem);
            s_cache.emplace(token, std::move(elem));
            return;
        } catch (const std::exception & e) {
            throw std::runtime_error(
                std::string("v3 string-context corruption at ") + site +
                ": un-parseable context token '" + token + "' (" + e.what() +
                "). Every v3 side-table token must round-trip through "
                "NixStringContextElem::parse; a dropped token would silently "
                "delete a derivation dependency edge.");
        }
    }
    try {
        ctx.insert(nix::NixStringContextElem::parse(token));
    } catch (const std::exception & e) {
        throw std::runtime_error(
            std::string("v3 string-context corruption at ") + site +
            ": un-parseable context token '" + token + "' (" + e.what() +
            "). Every v3 side-table token must round-trip through "
            "NixStringContextElem::parse; a dropped token would silently "
            "delete a derivation dependency edge.");
    }
}

static nix::NixStringContext decodeStringContext(const std::vector<std::string> & entries)
{
    nix::NixStringContext out;
    for (auto & e : entries)
        v3InsertContextToken(out, e, "decodeStringContext");
    return out;
}

static std::vector<std::string> encodeStringContext(const nix::NixStringContext & ctx)
{
    std::vector<std::string> out;
    out.reserve(ctx.size());
    for (auto & e : ctx) out.push_back(e.to_string());
    return out;
}

static const nix::NixStringContext lookupStringContext(const char * buf)
{
    if (auto * raw = lookupStringContextEntries(buf))
        return decodeStringContext(*raw);
    return {};
}

static void setStringContext(const char * buf, const nix::NixStringContext & ctx)
{
    if (ctx.empty()) return;
    setStringContextEntries(buf, encodeStringContext(ctx));
}

/// Throw `the string '%s' is not allowed to refer to a store path` if the
/// argument carries any string context.  Mirrors TW's `forceStringNoCtx`
/// (eval.cc:2826).  Used by primops that take a string and must reject
/// contexted inputs: parseDrvName / splitVersion / getEnv /
/// compareVersions / etc.  Caller has already verified
/// `args[0].isString()`.
///
/// Format matches TW exactly: `(such as '<displayed-context-elem>')`
/// where `<displayed-context-elem>` is the elem's `display(*store)`
/// form (e.g. `/nix/store/<hash>-<name>.drv^out` for a Built entry).
/// Without the display-format conversion, callers that pattern-match
/// the error string would see v3's internal encoding instead.
static void requireNoStringContext(EvalState & state, const Value & v,
                                    std::string_view primopName)
{
    if (!v.isString()) return;
    auto * raw = lookupStringContextEntries(v.asString());
    if (!raw || raw->empty()) return;
    std::string display;
    if (state.nixEvalState) {
        try {
            auto elem = nix::NixStringContextElem::parse(raw->front());
            // `store` is `ref<Store>` so always non-null; deref directly.
            display = elem.display(*state.nixEvalState->store);
        } catch (...) {
            display = raw->front();
        }
    } else {
        display = raw->front();
    }
    // Match TW's wording byte-for-byte (eval.cc:2832) so callers that
    // pattern-match the error string don't need a v3-specific branch.
    // V3_DBG_NOCTX_SITE: identify which primop's forceStringNoCtx
    // call rejected the context.  Cold path — only fires immediately
    // before throwing.  Used to bisect v3-vs-TW force-order
    // divergences (#757c).
    static const bool s_dbgNoCtxSite =
        std::getenv("V3_DBG_NOCTX_SITE") != nullptr;
    if (__builtin_expect(s_dbgNoCtxSite, 0))
        std::fprintf(stderr, "v3 NOCTX-SITE: requireNoStringContext primop=%.*s\n",
                     (int)primopName.size(), primopName.data());
    std::string buf = "the string '";
    if (v.asString()) buf.append(v.asString());
    buf += "' is not allowed to refer to a store path (such as '";
    buf += display;
    buf += "')";
    throw std::runtime_error(std::move(buf));
}

std::mutex & registryMutex()
{
    static std::mutex m;
    return m;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

[[noreturn]] inline void typeError(std::string_view op, std::string_view expected)
{
    throw std::runtime_error("v3 primop " + std::string(op) + ": expected " + std::string(expected));
}

// #693 — forward-decl of expectedTypeButFound (defined later, near
// primRemoveAttrs where it was first introduced).  Used by primops
// throughout the file to emit TW's `expected a <T> but found <T>:
// <value>` phrasing in place of the v3-internal typeError.
static std::string expectedTypeButFound(const char * expected, const Value & v);

template <typename F>
inline void forEachEntryNoMapAttrsRealize(const Bindings * b, F && f)
{
    if (!b) return;
    if (b->isChain()) {
        Bindings::Cursor c(b, false);
        while (const Bindings::Entry * e = c.next()) f(*e);
        return;
    }
    if (b->isHamt()) {   // Change-2 #149: sorted (SymbolId) Entry-view iteration
        std::vector<const HamtNode::Slot *> ss; ss.reserve(b->size);
        hamtCollectSlots(b->hamtRoot(), ss);
        std::sort(ss.begin(), ss.end(),
                  [](const HamtNode::Slot * x, const HamtNode::Slot * y) { return x->key < y->key; });
        for (const HamtNode::Slot * s : ss) f(*reinterpret_cast<const Bindings::Entry *>(s));
        return;
    }
    for (uint32_t i = 0; i < b->size; ++i) f(b->entries[i]);
}

template <typename F>
inline void forEachEntryRefNoMapAttrsRealize(const Bindings * b, F && f)
{
    if (!b) return;
    if (b->isChain()) {
        Bindings::Cursor c(b, false);
        while (const Bindings::Entry * e = c.next())
            f(c.lastOwner(), *e);
        return;
    }
    if (b->isHamt()) {   // Change-2 #149: sorted (SymbolId) Entry-view iteration
        std::vector<const HamtNode::Slot *> ss; ss.reserve(b->size);
        hamtCollectSlots(b->hamtRoot(), ss);
        std::sort(ss.begin(), ss.end(),
                  [](const HamtNode::Slot * x, const HamtNode::Slot * y) { return x->key < y->key; });
        for (const HamtNode::Slot * s : ss) f(b, *reinterpret_cast<const Bindings::Entry *>(s));
        return;
    }
    for (uint32_t i = 0; i < b->size; ++i) f(b, b->entries[i]);
}

inline const Bindings::Entry * lookupEntryNoMapAttrsRealize(
    const Bindings * b, SymbolId name,
    const Bindings ** ownerOut = nullptr) noexcept
{
    if (b && b->isHamt()) {   // Change-2 #149: leaf slot's prefix aliases Entry
        if (const HamtNode::Slot * s = hamtLookupSlot(b->hamtRoot(), name)) {
            if (ownerOut) *ownerOut = b;
            return reinterpret_cast<const Bindings::Entry *>(s);
        }
        if (ownerOut) *ownerOut = nullptr;
        return nullptr;
    }
    for (const Bindings * cur = b; cur;
         cur = cur->isChain() ? cur->parent : nullptr) {
        if (const Bindings::Entry * e = cur->lookupLocalEntry(name)) {
            if (ownerOut) *ownerOut = cur;
            return e;
        }
    }
    if (ownerOut) *ownerOut = nullptr;
    return nullptr;
}

// isUnrealizedMapAttrsEntry / entryValueForImmediateDemand /
// forceEntryForImmediateDemand moved to v3/mapattrs_demand.hh (shared with
// vm.cc's valueEqual — single source of truth for "force the mapped value, not
// the stored source").  Included at the top of this file.

template <typename Keep>
inline Bindings * copyMapAttrsSubset(const Bindings * src, uint32_t n, Keep && keep)
{
    Bindings * result = Alloc::allocBindings(n);
    if (n > 0) {
        result->kind = uint8_t(Bindings::Kind::MapAttrs);
        result->parent = src->parent;
        result->aux = src->aux;
    }
    uint32_t k = 0;
    for (uint32_t i = 0; i < src->size; ++i) {
        const Bindings::Entry & e = src->entries[i];
        if (keep(e.name))
            result->entries[k++] = e;
    }
    bindingsPostConstructBarrier(result);
    return result;
}

inline bool valueEqual(VMState & vm, Value a0, Value b0)
{
    // A12b (2026-05-22): iterative valueEqual via explicit work
    // stack.  Mirror of the vm.cc::valueEqual conversion (012c0e38f).
    // Pre-fix this function C-recursed at every nested list/attrset
    // level.  Deep nested containers (e.g. lib.unique on lists of
    // packages, builtins.elem comparing nested attrset structures)
    // could hit the C-stack limit through this path.  The vm.cc copy
    // was converted first because it serves the `==` operator which
    // is the more common entry; this primops.cc copy serves
    // `builtins.filter` / `builtins.elem` / `builtins.all` /
    // `primConcatMap` and was an equal-priority A12b case.
    //
    // Differences from vm.cc::valueEqual:
    //   * No `insideContainer` parameter — primop equality always
    //     returns false on closures/primops/primopapps regardless
    //     of context.
    //   * No writeback-force on container elements — this entry
    //     point reads through pointer-by-value (la->elems[i]) and
    //     doesn't have the lvalue handle that the vm.cc copy uses
    //     for the writeback-force pattern.  If A12 writeback
    //     memoization matters here, it's the caller's job (e.g.
    //     primElem calls forceValue on the list element via the
    //     writable lvalue BEFORE invoking valueEqual).
    //
    // Same shape preserved:
    //   * Pointer-identity short-circuit on same list / attrset.
    //   * Cross-type int↔float numeric equality.
    //   * Derivation outPath short-circuit (#666 — TW's eqValues
    //     parity required by `lib.unique` on package lists).
    struct Task { Value a, b; };
    std::vector<Task> stack;
    stack.reserve(16);
    stack.push_back({a0, b0});

    static const SymbolId sType    = ir::globalInternSymbol("type");
    static const SymbolId sOutPath = ir::globalInternSymbol("outPath");

    while (!stack.empty()) {
        Task t = stack.back();
        stack.pop_back();
        Value a = t.a, b = t.b;

        // #558 Phase 2: inline WHNF tag check before forceValue
        // function call.  Same rationale as vm.cc's valueEqual.
        {
            Tag at = a.tag();
            if (__builtin_expect(at == Tag::Thunk
                                 || at == Tag::App || at == Tag::App3
                                 || at == Tag::Slot, 0))
                a = forceValue(vm, a);
            Tag bt = b.tag();
            if (__builtin_expect(bt == Tag::Thunk
                                 || bt == Tag::App || bt == Tag::App3
                                 || bt == Tag::Slot, 0))
                b = forceValue(vm, b);
        }
        if (a.tag() != b.tag()) {
            if (a.isInt() && b.isFloat()) {
                if (static_cast<double>(a.asInt()) != b.asFloat()) return false;
                continue;
            }
            if (a.isFloat() && b.isInt()) {
                if (a.asFloat() != static_cast<double>(b.asInt())) return false;
                continue;
            }
            return false;
        }
        switch (a.tag()) {
        case Tag::Int:
            if (a.asInt() != b.asInt()) return false;
            break;
        case Tag::Float:
            if (a.asFloat() != b.asFloat()) return false;
            break;
        case Tag::Bool:
            if (a.asInt() != b.asInt()) return false;
            break;
        case Tag::Null:
            break;
        case Tag::String:
            if (std::string_view(a.asString()) != std::string_view(b.asString()))
                return false;
            break;
        case Tag::Path:
            if (std::string_view(a.asPath()) != std::string_view(b.asPath()))
                return false;
            break;
        case Tag::List: {
            auto * la = a.asList(); auto * lb = b.asList();
            if (la == lb) break;
            uint32_t na = la ? la->size : 0; uint32_t nb = lb ? lb->size : 0;
            if (na != nb) return false;
            // A12b: push pairs in REVERSE so index [0] sits on top
            // of the stack — preserves left-to-right comparison
            // order + short-circuit on first mismatch.
            for (uint32_t i = na; i > 0; --i) {
                uint32_t idx = i - 1;
                stack.push_back({la->elems[idx], lb->elems[idx]});
            }
            break;
        }
        case Tag::Attrs: {
            auto * aa = a.asAttrs(); auto * bb = b.asAttrs();
            if (aa == bb) break;
            // 2026-05-19 #666: TW's eqValues (libexpr/eval.cc:3365)
            // special-cases derivations: if both sides have `type =
            // "derivation"`, compare ONLY their `outPath` (the canonical
            // derivation identity).  Skipping this caused v3's
            // `builtins.elem` / `lib.unique` to consider two references
            // to the same derivation (e.g. `pkgs.python3` vs
            // `pkgs.python3Packages.python`) as DIFFERENT when one had a
            // slightly different attr-set shape — duplicates leaked into
            // `requiredPythonModules` and propagated into the python3-env
            // buildEnv's chosenOutputs JSON, diverging the drv hash.
            auto isDerivation = [&](Bindings * b) -> bool {
                if (!b) return false;
                if (const Value * tv = b->lookup(sType)) {
                    Value f = forceValue(vm, *tv);
                    return f.isString() && f.asString()
                        && std::string_view(f.asString()) == "derivation";
                }
                return false;
            };
            if (isDerivation(aa) && isDerivation(bb)) {
                const Value * oa = aa->lookup(sOutPath);
                const Value * ob = bb->lookup(sOutPath);
                if (oa && ob) {
                    stack.push_back({*oa, *ob});
                    break;
                }
            }
            // Change-2 #149: materialise Kind::Hamt operands to Sorted so the
            // entries[] lockstep / Cursor compare below works (Hamt has no
            // entries[]).  Memoized, so repeated compares don't re-flatten.
            if (aa && aa->isHamt()) aa = const_cast<Bindings *>(aa->materialize());
            if (bb && bb->isHamt()) bb = const_cast<Bindings *>(bb->materialize());
            const bool anyChain = (aa && aa->isChain()) || (bb && bb->isChain());
            uint32_t na = aa ? (anyChain ? aa->countDistinct() : aa->size) : 0;
            uint32_t nb = bb ? (anyChain ? bb->countDistinct() : bb->size) : 0;
            if (na != nb) return false;
            if (anyChain) {
                Bindings::Cursor ca(aa);
                Bindings::Cursor cb(bb);
                std::vector<Task> pending;
                pending.reserve(na);
                while (const Bindings::Entry * ea = ca.next()) {
                    const Bindings::Entry * eb = cb.next();
                    if (!eb || ea->name != eb->name) return false;
                    pending.push_back({ea->value, eb->value});
                }
                if (cb.next()) return false;
                for (uint32_t i = static_cast<uint32_t>(pending.size()); i > 0; --i)
                    stack.push_back(pending[i - 1]);
                break;
            }
            // A12b: name-check inline (cheap), then push value-pair
            // tasks in REVERSE for left-to-right processing.
            for (uint32_t i = 0; i < na; ++i)
                if (aa->entries[i].name != bb->entries[i].name) return false;
            for (uint32_t i = na; i > 0; --i) {
                uint32_t idx = i - 1;
                stack.push_back({aa->entries[idx].value, bb->entries[idx].value});
            }
            break;
        }
        // Functions are never equal in Nix at the top level (`f == f`
        // is false).  This helper is used from primops
        // (filter/elem/etc.) which perform direct comparison —
        // closures never compare equal here.  The vm.cc valueEqual
        // has a separate code path for list/attr recursion that
        // allows pointer-identity for closures.
        case Tag::Closure:
        case Tag::PrimOp:
        case Tag::PrimOpApp:
            return false;
        case Tag::Uninitialized:
        case Tag::Thunk:
        case Tag::App:
        case Tag::App3:
        case Tag::Blackhole:
        case Tag::External:
        case Tag::Slot:
        default:
            if (a.asRaw() != b.asRaw()) return false;
            break;
        }
    }
    return true;
}

// `toStr(Value &)` was a v3-only stringifier predating the proper
// `toStringCoerce` machinery in vm.cc.  Removed when callers migrated
// to the official path; kept dormant in case future review work needs
// a v3-side stringifier without the coerce variants.  Re-add as needed.

inline Value mkStringValueOwned(std::string s)
{
    // CRIT-4: arena allocation; no per-call malloc/leak.
    char * buf = Alloc::allocChars(s.size() + 1);
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
    Value v;
    v.mkString(buf);
    return v;
}

// ---------------------------------------------------------------------------
// Primop bodies
// ---------------------------------------------------------------------------

void primLength(EvalState &, Value * args, Value & out)
{
    const Value & v = args[0];
    // #680 — TW's `builtins.length` only accepts lists
    // (libexpr/primops.cc:4109).  v3 pre-fix also accepted strings
    // as a "bonus" — silent semantic divergence (`length "abc"`
    // returned 3 in v3, errored in TW).  Use `stringLength` for
    // strings.
    if (!v.isList()) typeError("length", "list");
    out.mkInt(v.asList() ? v.asList()->size : 0);
}

void primHead(EvalState &, Value * args, Value & out)
{
    const Value & v = args[0];
    // #678 — match TW phrasing (libexpr/primops.cc:3892).
    if (!v.isList() || !v.asList() || v.asList()->size == 0)
        throw std::runtime_error("'builtins.head' called on an empty list");
    out = v.asList()->elems[0];
}

void primTail(EvalState &, Value * args, Value & out)
{
    const Value & v = args[0];
    // #678 — match TW phrasing (libexpr/primops.cc:3919).
    if (!v.isList() || !v.asList() || v.asList()->size == 0)
        throw std::runtime_error("'builtins.tail' called on an empty list");
    uint32_t n = v.asList()->size;
    ListVec * out_l = Alloc::allocList(n - 1);
    V3_STATS_INC(listsAllocated);
    for (uint32_t i = 1; i < n; ++i)
        out_l->elems[i - 1] = v.asList()->elems[i];
    listPostConstructBarrier(out_l);  // Phase D
    out.mkList(out_l);
}

void primElemAt(EvalState &, Value * args, Value & out)
{
    const Value & lst = args[0];
    const Value & idx = args[1];
    if (!lst.isList() || !idx.isInt()) typeError("elemAt", "list and int");
    uint32_t n = lst.asList() ? lst.asList()->size : 0;
    // #678 — match TW phrasing (libexpr/primops.cc:3869).
    if (idx.asInt() < 0 || static_cast<uint64_t>(idx.asInt()) >= n)
        throw std::runtime_error(
            "'builtins.elemAt' called with index "
            + std::to_string(idx.asInt())
            + " on a list of size " + std::to_string(n));
    out = lst.asList()->elems[idx.asInt()];
}

void primAttrNames(EvalState &, Value * args, Value & out)
{
    const Value & a = args[0];
    if (!a.isAttrs() || !a.asAttrs()) typeError("attrNames", "attrset");
    // Lever A (MEMORY_REPRESENTATION §6): stream the chain via Cursor
    // instead of materialise()-copying the whole base.  `totalSize()`
    // gives the distinct-name count to size the list; `forEach`
    // yields each distinct name once (overlay-wins).  The result is
    // re-sorted lexicographically below, so the cursor's SymbolId
    // order is irrelevant to output.
    const Bindings * src = a.asAttrs();
    uint32_t n = src->totalSize();
    ListVec * lv = Alloc::allocList(n);
    V3_STATS_INC(listsAllocated);
    auto & symTab = ir::globalSymbolTable();
    uint32_t i = 0;
    src->forEachName([&](SymbolId sid) {
        lv->elems[i++] = mkStringValueOwned(
            sid < symTab.size() ? symTab[sid] : std::to_string(sid));
    });
    listPostConstructBarrier(lv);  // Phase D
    // Sort lexicographically by name — matches tree-walker semantics
    // and decouples output order from the global symbol-table
    // insertion order.
    std::sort(lv->elems, lv->elems + n,
        [](const Value & x, const Value & y) {
            return std::string_view(x.asString()) < std::string_view(y.asString());
        });
    out.mkList(lv);
}

void primAttrValues(EvalState &, Value * args, Value & out)
{
    const Value & a = args[0];
    // #693 — match TW phrasing (libexpr/primops.cc forceAttrs).
    if (!a.isAttrs() || !a.asAttrs())
        throw std::runtime_error(expectedTypeButFound("a set", a));
    // Lever A: stream the chain via Cursor (see primAttrNames above).
    const Bindings * src = a.asAttrs();
    if (src && src->isHamt()) src = src->materialize();   // Change-2 #149
    uint32_t n = src->totalSize();
    auto & symTab = ir::globalSymbolTable();
    auto nameView = [&](SymbolId sid) -> std::string_view {
        return sid < symTab.size()
            ? std::string_view(symTab[sid]) : std::string_view("");
    };
    ListVec * lv = Alloc::allocList(n);
    V3_STATS_INC(listsAllocated);
    constexpr uint32_t kSmallOrder = 32;
    if (!src->isChain()) {
        // Sorted/MapAttrs entries are already in SymbolId order, not
        // tree-walker's lexical string order.  Sort compact indices instead of
        // transient (name,value) pairs so attrValues over large MapAttrs does
        // not copy every Value into a temporary C++ vector before producing the
        // ListVec.  MapAttrs entries are still realized only because the value
        // list actually demands every mapped value.
        uint32_t smallOrder[kSmallOrder];
        std::vector<uint32_t> bigOrder;
        uint32_t * order = smallOrder;
        if (n > kSmallOrder) {
            bigOrder.resize(n);
            order = bigOrder.data();
        }
        for (uint32_t i = 0; i < n; ++i) order[i] = i;
        std::sort(order, order + n,
            [&](uint32_t x, uint32_t y) {
                return nameView(src->entries[x].name) < nameView(src->entries[y].name);
            });
        auto * mut = const_cast<Bindings *>(src);
        for (uint32_t i = 0; i < n; ++i) {
            Bindings::Entry & e = mut->entries[order[i]];
            if (mut->isMapAttrs())
                mut->realizeMapAttrsEntry(&e);
            lv->elems[i] = e.value;
        }
    } else {
        // Sort entry refs, not (name,value) pairs.  Chain attrValues can be a
        // large read-only consumer; copying every Value into a transient C++
        // vector adds work before we copy the same Values into the final ListVec.
        const Bindings::Entry * smallOrder[kSmallOrder];
        std::vector<const Bindings::Entry *> bigOrder;
        const Bindings::Entry ** order = smallOrder;
        if (n > kSmallOrder) {
            bigOrder.resize(n);
            order = bigOrder.data();
        }
        Bindings::Cursor c(src);
        uint32_t k = 0;
        while (const Bindings::Entry * e = c.next())
            order[k++] = e;
        if (__builtin_expect(k != n, 0))
            throw std::runtime_error("v3 primAttrValues: chain cursor count mismatch");
        std::sort(order, order + k,
            [&](const Bindings::Entry * x, const Bindings::Entry * y) {
                return nameView(x->name) < nameView(y->name);
            });
        for (uint32_t i = 0; i < k; ++i) lv->elems[i] = order[i]->value;
    }
    listPostConstructBarrier(lv);  // Phase D coverage (primAttrValues; PhD-6)
    out.mkList(lv);
}

void primIsAttrs   (EvalState &, Value * args, Value & out) { out = args[0].isAttrs()    ? Value::vTrue : Value::vFalse; }
void primIsList    (EvalState &, Value * args, Value & out) { out = args[0].isList()     ? Value::vTrue : Value::vFalse; }
/// Peek through a v3 Bridge thunk wrapping a TW Value: returns the
/// underlying TW ValueType, or std::nullopt if `v` isn't a Bridge or
/// the source is unreadable.  Used by primIs* type predicates so a
/// bridged TW Function/Attrset/List/etc. answers correctly without
/// forcing (which would re-wrap as another Bridge per the #456
/// chase break).  No state changes; pure peek.
// (peekBridgeTwType retired — TW_VALUE_ERADICATION F4, 2026-06-02.)

/// eval/apply (#3) PAP recognition — mirror of vm.cc::isUnderappliedClosurePap.
/// An under-applied multi-arity closure is represented as an App / App3 chain
/// App(…App(closure, a0)…) whose leaf is a Closure of arity A with applied
/// depth d < A.  Such a value is WHNF: a partial application that still behaves
/// as a `lambda` for isFunction / typeOf / functionArgs.  An App3 link carries
/// two applied args (`right` + `third`), so it contributes 2 to the depth.
/// Returns the leaf Closure (and, via *appliedDepth, d) or nullptr.
/// 2026-06-10: real nixpkgs hits this constantly because the eval/apply
/// optimisation (default-ON) collapses curried `a: b: …` into one closure, so a
/// partial call like `(a: b: a + b) 1` materialises a Tag::App PAP.
static inline Closure * underappliedPapLeaf(const Value & v, size_t * appliedDepth = nullptr)
{
    Tag t = v.tag();
    if (t != Tag::App && t != Tag::App3) return nullptr;
    const Value * cur = &v;
    size_t depth = 0;
    while ((cur->tag() == Tag::App || cur->tag() == Tag::App3) && cur->asPair()) {
        depth += (cur->tag() == Tag::App3) ? 2 : 1;
        cur = &cur->asPair()->left;
    }
    if (cur->tag() == Tag::Closure && cur->asClosure()
        && cur->asClosure()->desc
        && cur->asClosure()->desc->arity > depth) {
        if (appliedDepth) *appliedDepth = depth;
        return cur->asClosure();
    }
    return nullptr;
}

void primIsFunction(EvalState &, Value * args, Value & out)
{
    // #493 step 3: peek through Bridge thunks for bridged TW lambdas
    // (e.g., the v3FormalsLambdaBridges sentinel-tLambda values that
    // round-trip back to v3 as Bridge thunks).  Without this, nixpkgs
    // loadModule's `if isFunction m then ... else import m` mis-routes
    // a bridged-lambda module to import, surfacing as
    // "v3 primop import: expected string or path".
    static const bool s_dbg =
        std::getenv("V3_DBG_IS_FUNCTION") != nullptr;
    bool isfn = (args[0].isClosure() || args[0].isPrimOp() || args[0].tag() == Tag::PrimOpApp);
    // eval/apply (#3): a closure-PAP — an under-applied arity-N closure
    // represented as a Tag::App / App3 chain whose leaf is a Closure — is a
    // function.  The arg is already WHNF here (isFunction forces it), and a
    // WHNF App-like value can only be such a PAP (ordinary lazy apps force to
    // their result).
    if (!isfn && underappliedPapLeaf(args[0]))
        isfn = true;
    if (s_dbg) std::fprintf(stderr,
        "v3 primIsFunction: tag=%d → %s\n",
        (int)args[0].tag(), isfn ? "true" : "false");
    out = isfn ? Value::vTrue : Value::vFalse;
}
void primIsString  (EvalState &, Value * args, Value & out) { out = args[0].isString()   ? Value::vTrue : Value::vFalse; }
void primIsInt     (EvalState &, Value * args, Value & out) { out = args[0].isInt()      ? Value::vTrue : Value::vFalse; }
void primIsBool    (EvalState &, Value * args, Value & out) { out = args[0].isBool()     ? Value::vTrue : Value::vFalse; }
void primIsNull    (EvalState &, Value * args, Value & out) { out = args[0].isNull()     ? Value::vTrue : Value::vFalse; }
void primIsFloat   (EvalState &, Value * args, Value & out) { out = args[0].isFloat()    ? Value::vTrue : Value::vFalse; }
void primIsPath    (EvalState &, Value * args, Value & out) { out = args[0].isPath()     ? Value::vTrue : Value::vFalse; }

// Tree-walker's `builtins.toString` uses
// `coerceToString(copyToStore=false, coerceMore=true)` — extends the
// basic primitive coerce to lists (space-joined elements), attrsets
// with __toString or outPath, paths (without store-copy), int / float
// / bool / null.  This mirrors that without the BR-3 store-copy path
// (which is only correct for derivationStrict's path attrs).
/// Internal toString coerce.  ctx is an out-param that accumulates
/// string-context entries from every nested string/attrset traversed.
/// The caller writes the merged ctx onto the result string with
/// setStringContextEntries.
///
/// REVIEW §1.6: previously dropped context for List/Attrs traversal
/// -- `toString [drvA drvB]` yielded the right text but with empty
/// context.  Now threads through.  Tree-walker uses
/// state.coerceToString with NixStringContext& accum (libexpr/eval.cc:
/// coerceToString); same shape.
static std::string toStringCoerceCtx(EvalState & state, Value v,
                                     std::vector<std::string> & ctx,
                                     bool copyPathsToStore = false)
{
    auto absorbCtx = [&](const char * s) {
        if (!s) return;
        if (auto * raw = lookupStringContextEntries(s)) {
            ctx.insert(ctx.end(), raw->begin(), raw->end());
        }
    };
    v = forceValue(*state.vm, v);
    switch (v.tag()) {
    case Tag::String: absorbCtx(v.asString());
                      return std::string(v.asString() ? v.asString() : "");
    case Tag::Path: {
        // TW splits this into TWO behaviours via the `copyToStore` flag
        // of `EvalState::coerceToString` (libexpr/eval.cc:2880-2902):
        //
        //   copyToStore=false: return the source-tree absolute path
        //     as-is.  Used by `builtins.toString` (primops.cc:4806 —
        //     `prim_toString` calls coerceToString(..., copyToStore=
        //     false)).  No store copy, no context entry.
        //
        //   copyToStore=true: copy the path to /nix/store and return
        //     the resulting store path.  Used by primDerivationStrict
        //     for `args`/`builder`/`system`/env-entry coercion
        //     (primops.cc:1727,1809) so paths-as-drv-args end up as
        //     content-addressed store entries with proper inputSrcs.
        //
        // 2026-05-19 #665: pre-fix v3 unconditionally COPIED here,
        // breaking the substitute.nix pattern `name = baseNameOf
        // (toString args.src)` where TW expects the source-tree path
        // (so baseName returns "X.sh") but v3 returned the store path
        // (so baseName returned "<hash>-X.sh") — every substituted
        // setup-hook derivation diverged, cascading into stdenv and
        // every downstream package.
        //
        // Now: the caller chooses via `copyPathsToStore`.  primToString
        // passes false (TW-compatible toString).  The bytecode-wrapper
        // path coercion (via the new __derivCoerce primop) passes true
        // so derivation args/builder/env retain the store-copy.
        if (!v.asPath()) return std::string();
        if (!copyPathsToStore || !state.nixEvalState)
            return std::string(v.asPath());
        try {
            auto & ns = *state.nixEvalState;
            nix::SourcePath sp = ns.rootPath(
                nix::CanonPath(v.asPath()));
            nix::NixStringContext twCtx;
            nix::StorePath sPath = ns.copyPathToStore(twCtx, sp);
            // Insert the copy's context (an Opaque element pointing
            // to the new store path).  copyPathToStore populates
            // twCtx with this — forward into ctx.
            for (auto & e : twCtx) ctx.push_back(e.to_string());
            return ns.store->printStorePath(sPath);
        } catch (const std::exception &) {
            // C-7(c) (CODEBASE_REVIEW_2026-06-11): we reach here ONLY with
            // copyPathsToStore=true (see the early return above), i.e. a store
            // copy was REQUESTED — this coercion feeds a derivation's
            // args/builder/env.  TW's copyPathToStore (eval.cc:2750) has NO
            // fallback: fetchToStore throws if the path is missing/uncopyable
            // and the error propagates.  The former v3 fallback to the raw
            // `/source/...` path produced a context-less string → a dropped
            // inputSrcs edge → wrong drv hash, silently.  Rethrow to match TW.
            // (The prior comment's "/no-cert-file.crt that TW also can't copy"
            // was a "mirror-TW" claim that doesn't hold — TW throws there too.)
            throw;
        }
    }
    case Tag::Int:    return std::to_string(v.asInt());
    case Tag::Float:  return std::to_string(v.asFloat());
    case Tag::Bool:   return v.asInt() == 1 ? "1" : "";
    case Tag::Null:   return "";
    case Tag::List: {
        std::string out;
        auto * lv = v.asList();
        if (!lv) return out;
        for (uint32_t i = 0; i < lv->size; ++i) {
            Value el = forceValue(*state.vm, lv->elems[i]);
            out += toStringCoerceCtx(state, el, ctx, copyPathsToStore);
            if (i + 1 < lv->size) {
                bool elIsEmptyList = el.isList()
                    && (!el.asList() || el.asList()->size == 0);
                if (!elIsEmptyList) out += ' ';
            }
        }
        return out;
    }
    case Tag::Attrs: {
        // TW behaviour: try __toString first (call it on the attrset),
        // then fall through to outPath.  See libexpr/eval.cc:2865.
        if (v.asAttrs()) {
            static const SymbolId sToString =
                ir::globalInternSymbol("__toString");
            static const SymbolId sOutPath =
                ir::globalInternSymbol("outPath");
            // __toString: call it with `self` as the single arg, then
            // recursively coerce the result.  Only fires for callable
            // shapes; non-callable falls through to outPath.
            if (auto * tsRaw = v.asAttrs()->lookup(sToString)) {
                Value tsFn = forceValue(*state.vm, *tsRaw);
                if (tsFn.isClosure() || tsFn.isPrimOp()
                    || tsFn.tag() == Tag::PrimOpApp) {
                    Value res = callClosure(*state.vm, tsFn, v);
                    Value forced = forceValue(*state.vm, res);
                    return toStringCoerceCtx(state, forced, ctx, copyPathsToStore);
                }
                // non-callable: fall through to outPath
            }
            if (auto * outV = v.asAttrs()->lookup(sOutPath)) {
                Value forced = forceValue(*state.vm, *outV);
                return toStringCoerceCtx(state, forced, ctx, copyPathsToStore);
            }
            // #760 (2026-05-22): match TW's `EvalState::coerceToString`
            // error byte-for-byte:
            //   "cannot coerce a set to a string: { extra = "meta-info"; }"
            // Pre-fix v3 emitted "v3 toString: attrset has no outPath /
            // __toString; keys=[...]" — divergent prefix + keys-only
            // diagnostic that masked the byte-equality check in
            // derivation-parity.sh.  Surfaced when #760 retired
            // SKIP_INSTALLABLE_PREEVAL (the gate that was hiding v3's
            // own error behind TW's pre-eval).  Use v3 printNixValue
            // (the simplified printer, same as TW's printAmbiguous)
            // for the value-repr — printNixValueRich would mismatch
            // because TW's coerceToString uses ValuePrinter with
            // errorPrintOptions but the difference is irrelevant for
            // small attrsets like passthru.
            std::ostringstream os;
            os << "cannot coerce a set to a string: ";
            nix::v3::printNixValue(os, v, ir::globalSymbolTable());
            throw std::runtime_error(os.str());
        }
        throw std::runtime_error(
            "cannot coerce a set to a string: { }");
    }
    case Tag::Thunk: {
        // (#483 part 4 Bridge-thunk-to-string handler retired —
        //  TW_VALUE_ERADICATION F4, 2026-06-02; no Bridge thunks exist.)
        [[fallthrough]];
    }
    case Tag::Uninitialized:
    case Tag::Closure:
    case Tag::PrimOp:
    case Tag::PrimOpApp:
    case Tag::App:
    case Tag::App3:
    case Tag::Blackhole:
    case Tag::External:
    case Tag::Slot:
    default: {
        // C-23 (CODEBASE_REVIEW_2026-06-11): a function value — a closure, a
        // primop / partially-applied primop, or an under-applied closure-PAP —
        // coerces to TW's exact "cannot coerce a function to a string" (not a
        // v3-internal "tag=%u").  Callers that pattern-match TW's wording then
        // behave identically.
        {
            Tag tg = v.tag();
            if (tg == Tag::Closure || tg == Tag::PrimOp || tg == Tag::PrimOpApp
                || ((tg == Tag::App || tg == Tag::App3)
                    && underappliedPapLeaf(v)))
                throw std::runtime_error("cannot coerce a function to a string");
        }
        // 2026-05-18 cc-wrapper bisection: enhance the diagnostic
        // when we hit a non-stringifiable value.  Specifically for
        // PrimOp / PrimOpApp / Closure, print the function NAME so
        // we can identify which Nix function leaked into a string-
        // coerce context (where TW would have evaluated it differently).
        char buf[256];
        const char * extra = "";
        std::string nameInfo;
        if (v.tag() == Tag::PrimOp && v.asPrimOp()) {
            nameInfo = std::string(" name='")
                + (v.asPrimOp()->name.empty()
                       ? "<anon>" : std::string(v.asPrimOp()->name))
                + "' arity="
                + std::to_string(v.asPrimOp()->arity);
            extra = nameInfo.c_str();
        } else if (v.tag() == Tag::Closure
                   && v.asClosure()
                   && v.asClosure()->desc) {
            nameInfo = std::string(" closure-name='")
                + (v.asClosure()->desc->name.empty()
                       ? "<anon>" : v.asClosure()->desc->name)
                + "'";
            extra = nameInfo.c_str();
        }
        std::snprintf(buf, sizeof buf,
            "v3 toString: cannot stringify type tag=%u%s",
            (unsigned)v.tag(), extra);
        throw std::runtime_error(buf);
    }
    }
}

/// No-context variant retained for callers that don't need context
/// (currently the eval-fail trace + abort/throw error formatting).
/// Forwards into toStringCoerceCtx and drops the accumulator.
static std::string toStringCoerce(EvalState & state, Value v)
{
    std::vector<std::string> dropCtx;
    return toStringCoerceCtx(state, v, dropCtx);
}

void primToString(EvalState & state, Value * args, Value & out)
{
    // foldl lever (2026-06-16): small-non-negative-int toString cache.  The
    // profile showed toString(int) is dispatch + per-call allocChars bound, NOT
    // formatting bound (an snprintf/to_chars fast-path measured SLOWER — the
    // formatting is already cheap).  The removable cost is the per-call arena
    // string allocation.  toString of a small int (list indices, versions,
    // generated attr names — the dominant case) returns a PROCESS-STATIC String
    // Value, skipping the alloc + format + the coerce machinery entirely.
    // GC-safe: the buffers are C++-static (never freed/moved) and a String
    // Value's char* is not a GC-managed pointer (isNurseryPayload is false for
    // String).  Byte-identical: same decimal text as the slow path's
    // std::to_string(asInt()); ints carry no string context.  DEFAULT-ON
    // (opt-out NIX_V3_NO_TOSTRING_INT_CACHE=1) — validated --core 21/21,
    // hello/git/firefox drvPath, 59-pkg drvPath sweep (cache-diverge=0,
    // tw-diverge=0).  RETIREMENT: drop the opt-out after a full darwin-4 sweep.
    static const bool s_toStrFast =
        std::getenv("NIX_V3_NO_TOSTRING_INT_CACHE") == nullptr;
    if (s_toStrFast) {
        Value v = args[0];
        Tag t = v.tag();
        if (t == Tag::Thunk || v.isAppLike() || t == Tag::Slot) {
            v = forceValue(*state.vm, v);
            t = v.tag();
        }
        if (t == Tag::Int) {
            constexpr int64_t kCacheN = 1024;
            int64_t n = v.asInt();
            if (n >= 0 && n < kCacheN) {
                // C++-static buffers (stable c_str() forever; the bufs array
                // never reallocates) + pre-built String Values.  Built once.
                static std::array<std::string, kCacheN> bufs;
                static const std::array<Value, kCacheN> cache = [] {
                    std::array<Value, kCacheN> c;
                    for (int64_t i = 0; i < kCacheN; ++i) {
                        bufs[i] = std::to_string(i);
                        c[i].mkString(bufs[i].c_str());
                    }
                    return c;
                }();
                out = cache[static_cast<size_t>(n)];
                return;
            }
            // Larger / negative int: format + arena-copy, no ctx.
            out = mkStringValueOwned(std::to_string(n));
            return;
        }
        // non-int → fall through to the general coercion path below.
    }
    // §1.6: thread context through nested list/attrs traversal.
    // user-facing `builtins.toString`: TW-compatible non-copying
    // behaviour for paths (eval.cc:2880-2902, copyToStore=false).
    std::vector<std::string> ctx;
    std::string s = toStringCoerceCtx(state, args[0], ctx,
                                       /*copyPathsToStore=*/false);
    out = mkStringValueOwned(std::move(s));
    if (!ctx.empty())
        setStringContextEntries(out.asString(), std::move(ctx));
}

/// Internal `__derivCoerce` primop: like `builtins.toString` but
/// COPIES path values to /nix/store and adds an Opaque context entry
/// — matches TW's coerceToString(copyToStore=true) used by
/// primDerivationStrict for `args` / `builder` / `system` / env-entry
/// coercion (libexpr/primops.cc:1727,1809).
///
/// Used by the bytecode `derivationStrict` wrapper to ensure path
/// values in drv attributes get content-addressed and recorded in
/// drv.inputSrcs — separate primop from `toString` so user-facing
/// `toString` keeps the TW non-copying semantics that nixpkgs's
/// `substitute.nix` (`name = baseNameOf (toString args.src)`) relies
/// on.  See #665 RCA in toStringCoerceCtx comment above.
void primDerivCoerce(EvalState & state, Value * args, Value & out)
{
    std::vector<std::string> ctx;
    std::string s = toStringCoerceCtx(state, args[0], ctx,
                                       /*copyPathsToStore=*/true);
    out = mkStringValueOwned(std::move(s));
    if (!ctx.empty())
        setStringContextEntries(out.asString(), std::move(ctx));
}

void primTypeOf(EvalState &, Value * args, Value & out)
{
    const Value & v = args[0];
    const char * t = "unknown";
    switch (v.tag()) {
    case Tag::Int:    t = "int";    break;
    case Tag::Float:  t = "float";  break;
    case Tag::Bool:   t = "bool";   break;
    case Tag::Null:   t = "null";   break;
    case Tag::String: t = "string"; break;
    case Tag::Path:   t = "path";   break;
    case Tag::Attrs:  t = "set";    break;
    case Tag::List:   t = "list";   break;
    case Tag::Closure:
    case Tag::PrimOp:
    case Tag::PrimOpApp: t = "lambda"; break;
    case Tag::Thunk:  t = "thunk"; break;
    // eval/apply (#3): an under-applied closure-PAP is a WHNF partial
    // application — a `lambda`, not the catch-all "unknown".  (typeOf is
    // strict, so a WHNF App/App3 reaching here can only be such a PAP;
    // ordinary deferred apps would have forced to their result.)
    case Tag::App:
    case Tag::App3:   t = underappliedPapLeaf(v) ? "lambda" : "unknown"; break;
    case Tag::Uninitialized:
    case Tag::Blackhole:
    case Tag::External:
    case Tag::Slot:
    default:          t = "unknown";
    }
    out = mkStringValueOwned(t);
}

void primStringLength(EvalState &, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("stringLength", "string");
    out.mkInt(static_cast<int64_t>(std::strlen(args[0].asString())));
}

void primAdd(EvalState &, Value * args, Value & out)
{
    const Value & a = args[0]; const Value & b = args[1];
    // #687 — TW's primAdd raises on integer overflow.  Pre-fix v3
    // produced the wrapped value (-9223372036854775808 for
    // INT64_MAX + 1) — a SILENT semantic divergence that could
    // mask integer-arithmetic bugs in nixpkgs builders.
    if (a.isInt() && b.isInt()) {
        int64_t sum;
        if (__builtin_add_overflow(a.asInt(), b.asInt(), &sum))
            throw std::runtime_error(
                "integer overflow in adding "
                + std::to_string(a.asInt()) + " + "
                + std::to_string(b.asInt()));
        out.mkInt(sum);
    }
    else if (a.isFloat() && b.isFloat()) out.mkFloat(a.asFloat() + b.asFloat());
    else if (a.isInt() && b.isFloat())   out.mkFloat(static_cast<double>(a.asInt()) + b.asFloat());
    else if (a.isFloat() && b.isInt())   out.mkFloat(a.asFloat() + static_cast<double>(b.asInt()));
    else typeError("add", "numeric");
}

void primSub(EvalState &, Value * args, Value & out)
{
    const Value & a = args[0]; const Value & b = args[1];
    // #687 — primSub overflow guard, mirror of primAdd.
    if (a.isInt() && b.isInt()) {
        int64_t diff;
        if (__builtin_sub_overflow(a.asInt(), b.asInt(), &diff))
            throw std::runtime_error(
                "integer overflow in subtracting "
                + std::to_string(a.asInt()) + " - "
                + std::to_string(b.asInt()));
        out.mkInt(diff);
    }
    else if (a.isFloat() && b.isFloat()) out.mkFloat(a.asFloat() - b.asFloat());
    else if (a.isInt() && b.isFloat())   out.mkFloat(static_cast<double>(a.asInt()) - b.asFloat());
    else if (a.isFloat() && b.isInt())   out.mkFloat(a.asFloat() - static_cast<double>(b.asInt()));
    else typeError("sub", "numeric");
}

void primMul(EvalState &, Value * args, Value & out)
{
    const Value & a = args[0]; const Value & b = args[1];
    // #687 — primMul overflow guard, mirror of primAdd.
    if (a.isInt() && b.isInt()) {
        int64_t prod;
        if (__builtin_mul_overflow(a.asInt(), b.asInt(), &prod))
            throw std::runtime_error(
                "integer overflow in multiplying "
                + std::to_string(a.asInt()) + " * "
                + std::to_string(b.asInt()));
        out.mkInt(prod);
    }
    else if (a.isFloat() && b.isFloat()) out.mkFloat(a.asFloat() * b.asFloat());
    else if (a.isInt() && b.isFloat())   out.mkFloat(static_cast<double>(a.asInt()) * b.asFloat());
    else if (a.isFloat() && b.isInt())   out.mkFloat(a.asFloat() * static_cast<double>(b.asInt()));
    else typeError("mul", "numeric");
}

void primDiv(EvalState &, Value * args, Value & out)
{
    const Value & a = args[0]; const Value & b = args[1];
    // #683 — TW's div errors on float-by-zero too (libexpr/primops.cc:
    // 4703 unconditional `if (f2 == 0) division by zero`).  Pre-fix v3
    // only guarded the Int/Int path; Float/Float and mixed produced
    // ±inf / NaN silently — a SEMANTIC divergence on numeric code that
    // could mask divide-by-zero bugs in nixpkgs builders.
    if (a.isInt() && b.isInt()) {
        if (b.asInt() == 0) throw std::runtime_error("division by zero");
        out.mkInt(a.asInt() / b.asInt());
    } else if (a.isFloat() && b.isFloat()) {
        if (b.asFloat() == 0.0) throw std::runtime_error("division by zero");
        out.mkFloat(a.asFloat() / b.asFloat());
    } else if (a.isInt() && b.isFloat()) {
        if (b.asFloat() == 0.0) throw std::runtime_error("division by zero");
        out.mkFloat(static_cast<double>(a.asInt()) / b.asFloat());
    } else if (a.isFloat() && b.isInt()) {
        if (b.asInt() == 0) throw std::runtime_error("division by zero");
        out.mkFloat(a.asFloat() / static_cast<double>(b.asInt()));
    } else typeError("div", "numeric");
}

void primThrow(EvalState &, Value * args, Value &)
{
    if (!args[0].isString()) typeError("throw", "string");
    // ThrownError derives from AssertionError so tryEval catches it
    // (matches tree-walker semantics).  #677 — emit the message verbatim
    // (no "v3 throw:" prefix) so default-mode `nix eval` byte-matches
    // TW's `«error: <msg>»` form (libexpr/primops.cc:1167 throws
    // `ThrownError(s)` with no prefix).
    throw ThrownError(std::string(args[0].asString()));
}

void primConcatLists(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isList()) typeError("concatLists", "list of lists");
    uint32_t total = 0;
    auto & outer = args[0];
    // Force each outer element (each should be a list); they're lazy
    // by default now.
    //
    // Phase 1.2 step 2 (action plan): inline WHNF skip — most concatLists
    // arguments are already-forced inner lists (the common idiom is
    // `concatLists (map f xs)` where map returns Apps that downstream
    // forces have already resolved).  Skip the forceValue function-call
    // cost for those.
    Value singleNonEmpty = Value::vEmptyList;
    bool haveNonEmpty = false;
    bool multipleNonEmpty = false;
    for (uint32_t i = 0; i < outer.asList()->size; ++i) {
        Value & e = outer.asList()->elems[i];
        Tag et = e.tag();
        if (__builtin_expect(et == Tag::Thunk
                             || et == Tag::App || et == Tag::App3
                             || et == Tag::Slot, 0))
            e = forceValue(*state.vm, e);
        if (!e.isList()) typeError("concatLists", "list of lists");
        uint32_t n = e.asList() ? e.asList()->size : 0;
        total += n;
        if (n != 0) {
            if (haveNonEmpty) multipleNonEmpty = true;
            else {
                singleNonEmpty = e;
                haveNonEmpty = true;
            }
        }
    }
    if (total == 0) {
        out = Value::vEmptyList;
        return;
    }
    if (!multipleNonEmpty) {
        out = singleNonEmpty;
        return;
    }
    ListVec * result = Alloc::allocList(total);
    V3_STATS_INC(listsAllocated);
    uint32_t k = 0;
    for (uint32_t i = 0; i < outer.asList()->size; ++i) {
        const Value & el = outer.asList()->elems[i];
        if (!el.asList()) continue;
        for (uint32_t j = 0; j < el.asList()->size; ++j)
            result->elems[k++] = el.asList()->elems[j];
    }
    listPostConstructBarrier(result);  // Phase D coverage (primConcatLists; PhD-6)
    out.mkList(result);
}

void primConcatStringsSep(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("concatStringsSep", "separator string");
    if (!args[1].isList())   typeError("concatStringsSep", "list of strings");
    std::string sep(args[0].asString());
    std::string result;
    auto * list = args[1].asList();
    // 2026-05-19 #665: accumulate string context from the separator
    // and every list element.  TW's prim_concatStringsSep
    // (libexpr/primops.cc:prim_concatStringsSep) calls coerceToString
    // on each element which appends its context to the shared
    // `context` arg; the result string carries the union.  Without
    // this, sequences like `concatStringsSep " " [ "${drv}" ... ]`
    // produce a context-free string, dropping the derivation
    // dependency — caught when utils.bash.drv was missing
    // expand-response-params from its inputDrvs in nixpkgs's
    // pkgconf-wrapper buildPhase.
    std::vector<std::string> ctx;
    auto absorb = [&](const char * s) {
        if (!s) return;
        if (auto * raw = lookupStringContextEntries(s))
            ctx.insert(ctx.end(), raw->begin(), raw->end());
    };
    absorb(args[0].asString());
    // Phase 1.2 step 2 (action plan): inline WHNF skip — most
    // concatStringsSep arguments are already-forced strings (the
    // common idiom is `concatStringsSep ":" (map toString xs)` where
    // map's per-element Apps have been resolved upstream).
    for (uint32_t i = 0; list && i < list->size; ++i) {
        if (i > 0) result += sep;
        Value el = list->elems[i];
        Tag et = el.tag();
        if (__builtin_expect(et == Tag::Thunk
                             || et == Tag::App || et == Tag::App3
                             || et == Tag::Slot, 0))
            el = forceValue(*state.vm, el);
        // 2026-05-19 #666: TW's prim_concatStringsSep
        // (libexpr/primops.cc:5282) calls coerceToString on each
        // element, which handles paths, derivation attrsets (via
        // __toString / outPath), bools, etc. — not just already-
        // strings.  Pre-fix v3 only accepted strings, so e.g.
        // buildEnv's `paths = [ drv1 drv2 ]` passed through
        // `lib.concatStringsSep " " paths` (in postBuild) was
        // rejected when each `drv` is an attrset.  Caused python3-
        // env / firefox / lutok / etc. to fall back to the v3
        // fake-store path.  Reuse toStringCoerceCtx to match TW.
        if (!el.isString()) {
            std::string coerced = toStringCoerceCtx(state, el, ctx,
                /*copyPathsToStore=*/false);
            result += coerced;
            continue;
        }
        absorb(el.asString());
        result += el.asString();
    }
    out = mkStringValueOwned(result);
    if (!ctx.empty()) {
        std::sort(ctx.begin(), ctx.end());
        ctx.erase(std::unique(ctx.begin(), ctx.end()), ctx.end());
        setStringContextEntries(out.asString(), std::move(ctx));
    }
}

void primSubstring(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isInt() || !args[1].isInt())
        typeError("substring", "(int, int, string)");
    int64_t start = args[0].asInt();
    int64_t len = args[1].asInt();
    // Match tree-walker: negative start is rejected; negative len is
    // a "to end" sentinel.
    if (start < 0)
        // #693 — match TW phrasing (libexpr/primops.cc:substring).
        throw std::runtime_error("negative start position in 'substring'");

    // C-23 (CODEBASE_REVIEW_2026-06-11): TW's prim_substring COERCES the 3rd
    // arg (coerceToString, copyToStore=false), so a path / derivation / attrset
    // with outPath/__toString is accepted (not just a bare string), with its
    // string context propagated.  The common bare-string case keeps its fast
    // path (and side-table context forwarding below); a coercible non-string is
    // coerced via toStringCoerceCtx.
    std::string srcStorage;
    std::string_view src;
    std::vector<std::string> coercedCtx;
    const char * strCtxKey = nullptr;  // side-table key, bare-string path only
    if (args[2].isString()) {
        src = std::string_view(args[2].asString());
        strCtxKey = args[2].asString();
    } else {
        srcStorage = toStringCoerceCtx(state, args[2], coercedCtx,
                                       /*copyPathsToStore=*/false);
        src = srcStorage;
    }

    if (static_cast<size_t>(start) >= src.size()) {
        out = mkStringValueOwned("");
    } else {
        size_t available = src.size() - start;
        size_t actualLen = (len < 0) ? available : std::min(static_cast<size_t>(len), available);
        out = mkStringValueOwned(std::string(src.substr(start, actualLen)));
    }
    // REVIEW §1.6: forward string-context entries from the input.
    // Tree-walker (prim_substring) propagates context unconditionally -- this
    // is what `builtins.substring 0 0 drv.outPath` relies on for ref-stripping
    // (the empty-string result carries the original drvPath context, marking
    // the derivation as a runtime dep without including the path).
    if (strCtxKey) {
        if (auto * raw = lookupStringContextEntries(strCtxKey)) {
            std::vector<std::string> copy(raw->begin(), raw->end());
            setStringContextEntries(out.asString(), std::move(copy));
        }
    } else if (!coercedCtx.empty()) {
        setStringContextEntries(out.asString(), std::move(coercedCtx));
    }
}

void primMap(EvalState & state, Value * args, Value & out)
{
    // WC-35 follow-up: tree-walker's `builtins.map` builds Tag::App
    // entries for each result element — `f x` only fires when the
    // entry is forced.  v3 was eager (callClosure per element) which
    // meant a `map f xs` over an `xs` whose element values include
    // rec siblings being constructed would force them prematurely.
    // Same root pattern as zipAttrsWith.
    //
    // Force the second arg to list shape (so we can read its size /
    // elems), then emit App entries.
    Value lst = args[1];
    if (lst.isAppLike() || lst.tag() == Tag::Thunk || lst.tag() == Tag::Slot)
        lst = forceValue(*state.vm, lst);
    if (!lst.isList()) typeError("map", "list");
    auto * src = lst.asList();
    if (!src || src->size == 0) {
        out = lst;
        return;
    }
    Value fun = args[0];
    if (fun.tag() == Tag::Closure && fun.asClosure()
        && fun.asClosure()->desc
        && fun.asClosure()->desc->identityLambda) {
        out = lst;
        return;
    }
    ListVec * result = Alloc::allocList(src->size);
    V3_STATS_INC(listsAllocated);
    for (uint32_t i = 0; i < src->size; ++i) {
        // Build App(fun, elem) — lazy.
        ValuePair * pp = Alloc::allocPair();
        pp->left  = fun;
        pp->right = src->elems[i];
        pairPostConstructBarrier(pp);  // Phase D
        Value v;
        v.mkPair(Tag::App, pp);
        result->elems[i] = v;
    }
    listPostConstructBarrier(result);  // Phase D
    out.mkList(result);
}

void primFilter(EvalState & state, Value * args, Value & out)
{
    if (!args[1].isList()) typeError("filter", "list");
    auto * src = args[1].asList();
    if (!src || src->size == 0) {
        out = args[1];
        return;
    }
    Value pred = args[0];
    std::vector<Value> kept;
    kept.reserve(src->size);
    for (uint32_t i = 0; i < src->size; ++i) {
        Value r = callClosure(*state.vm, pred, src->elems[i]);
        // Predicate result may be a thunk / app — force to WHNF.
        {
            Tag rt = r.tag();
            if (__builtin_expect(rt == Tag::Thunk
                                 || rt == Tag::App || rt == Tag::App3
                                 || rt == Tag::Slot, 0))
                r = forceValue(*state.vm, r);
        }
        if (!r.isBool()) typeError("filter", "predicate returning bool");
        if (r.asInt() == 1) kept.push_back(src->elems[i]);
    }
    if (kept.empty()) {
        out = Value::vEmptyList;
        return;
    }
    if (kept.size() == src->size) {
        out = args[1];
        return;
    }
    ListVec * result = Alloc::allocList(static_cast<uint32_t>(kept.size()));
    V3_STATS_INC(listsAllocated);
    for (size_t i = 0; i < kept.size(); ++i) result->elems[i] = kept[i];
    listPostConstructBarrier(result);  // Phase D coverage (primFilter; PhD-6)
    out.mkList(result);
}

void primFoldl(EvalState & state, Value * args, Value & out)
{
    // foldl' op nul list  —  strict left fold
    if (!args[2].isList()) typeError("foldl'", "list");
    Value op = args[0];
    Value acc = args[1];
    auto * src = args[2].asList();
    if (src) {
        for (uint32_t i = 0; i < src->size; ++i) {
            // Apply `op acc elem`.  T1: callClosure2 enters the arity-2
            // `op` body once with both args in slots, eliminating the
            // throwaway curry-PAP ValuePair (per-element) that the curried
            // callClosure(callClosure(op,acc),elem) allocated.  Falls back
            // to the curried form byte-identically for any other op shape.
            // The final `acc` IS forced each iteration since foldl' is
            // strict in the accumulator -- callers expect the seq behaviour
            // and a Tag::Thunk acc would defer subsequent op-calls' force
            // into the next iteration's first action.  Match TW's
            // `forceValue(*vAcc)` in primops.cc primFoldl'.
            acc = callClosure2(*state.vm, op, acc, src->elems[i]);
            if (__builtin_expect(acc.tag() == Tag::Thunk
                                 || acc.isAppLike()
                                 || acc.tag() == Tag::Slot, 0))
                acc = forceValue(*state.vm, acc);
        }
    }
    out = acc;
}

/// IR Phase C fused-loop FFI leaf (2026-05-18).
///
/// Semantically equivalent to `foldl' op init (map f xs)`:
///
///     __foldlMap op init f xs
///       = foldl' (acc: x: op acc (f x)) init xs
///
/// Recognised by opt_stream_fusion.cc, which rewrites the
/// `foldl'(op, init, PrimOpCall(map, [f, xs]))` IR pattern to
/// `PrimOpCall(__foldlMap, [op, init, f, xs])`.  The fused primop
/// eliminates the intermediate map result list (saving N
/// ValuePair allocations for N-element lists) and avoids one
/// traversal (map walks then foldl' walks — fused walks once).
///
/// All four args are STRICT (no lazy-arg bitmask).  The bytecode
/// wrapper in bytecode_primops.cc may override this with an
/// installBytecodePrimop call; this C body is the correctness
/// fallback.
void primFoldlMap(EvalState & state, Value * args, Value & out)
{
    if (!args[3].isList()) typeError("__foldlMap", "list as fourth arg");
    Value op   = args[0];
    Value acc  = args[1];
    Value f    = args[2];
    auto * src = args[3].asList();
    if (src) {
        for (uint32_t i = 0; i < src->size; ++i) {
            // Compute f(elem); force the result before passing to op.
            Value fx = callClosure(*state.vm, f, src->elems[i]);
            if (__builtin_expect(fx.tag() == Tag::Thunk
                                 || fx.isAppLike()
                                 || fx.tag() == Tag::Slot, 0))
                fx = forceValue(*state.vm, fx);
            // Apply op acc fx — T1 saturated 2-arg call (see primFoldl).
            acc = callClosure2(*state.vm, op, acc, fx);
            if (__builtin_expect(acc.tag() == Tag::Thunk
                                 || acc.isAppLike()
                                 || acc.tag() == Tag::Slot, 0))
                acc = forceValue(*state.vm, acc);
        }
    }
    out = acc;
}

void primGenList(EvalState & state, Value * args, Value & out)
{
    // Tree-walker's prim_genList builds App entries: each element is
    // `App(gen, idx_value)`, lazy.  v3 was eager (callClosure per i).
    // Same root pattern as zipAttrsWith / map.
    Value len = args[1];
    if (len.isAppLike() || len.tag() == Tag::Thunk || len.tag() == Tag::Slot)
        len = forceValue(*state.vm, len);
    if (!len.isInt()) typeError("genList", "int length");
    int64_t n = len.asInt();
    // #693 — match TW phrasing (libexpr/primops.cc:genList).
    if (n < 0) throw std::runtime_error("cannot create list of size " + std::to_string(n));
    Value gen = args[0];
    ListVec * result = Alloc::allocList(static_cast<uint32_t>(n));
    V3_STATS_INC(listsAllocated);
    if (gen.tag() == Tag::Closure && gen.asClosure()
        && gen.asClosure()->desc
        && gen.asClosure()->desc->identityLambda) {
        for (int64_t i = 0; i < n; ++i)
            result->elems[i].mkInt(i);
        out.mkList(result);
        return;
    }
    for (int64_t i = 0; i < n; ++i) {
        // Build App(gen, idx_int) — lazy.
        Value idx; idx.mkInt(i);
        ValuePair * pp = Alloc::allocPair();
        pp->left  = gen;
        pp->right = idx;
        pairPostConstructBarrier(pp);  // Phase D
        Value v;
        v.mkPair(Tag::App, pp);
        result->elems[i] = v;
    }
    out.mkList(result);
}

void primAll(EvalState & state, Value * args, Value & out)
{
    if (!args[1].isList()) typeError("all", "list");
    auto * src = args[1].asList();
    Value pred = args[0];
    bool all = true;
    if (src) {
        for (uint32_t i = 0; i < src->size; ++i) {
            Value r = callClosure(*state.vm, pred, src->elems[i]);
            {
                Tag rt = r.tag();
                if (__builtin_expect(rt == Tag::Thunk
                                     || rt == Tag::App || rt == Tag::App3
                                     || rt == Tag::Slot, 0))
                    r = forceValue(*state.vm, r);
            }
            if (!r.isBool()) typeError("all", "bool from predicate");
            if (r.asInt() == 0) { all = false; break; }
        }
    }
    out = all ? Value::vTrue : Value::vFalse;
}

void primAny(EvalState & state, Value * args, Value & out)
{
    if (!args[1].isList()) typeError("any", "list");
    auto * src = args[1].asList();
    Value pred = args[0];
    bool any = false;
    if (src) {
        for (uint32_t i = 0; i < src->size; ++i) {
            Value r = callClosure(*state.vm, pred, src->elems[i]);
            {
                Tag rt = r.tag();
                if (__builtin_expect(rt == Tag::Thunk
                                     || rt == Tag::App || rt == Tag::App3
                                     || rt == Tag::Slot, 0))
                    r = forceValue(*state.vm, r);
            }
            if (!r.isBool()) typeError("any", "bool from predicate");
            if (r.asInt() == 1) { any = true; break; }
        }
    }
    out = any ? Value::vTrue : Value::vFalse;
}

void primGetEnv(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("getEnv", "string");
    // #674: TW's prim_getEnv uses forceStringNoCtx; mirror it so
    // contexted strings can't be used as env-var names (would mask
    // accidental drv references in callers).
    requireNoStringContext(state, args[0], "getEnv");
    // C-19 (CODEBASE_REVIEW_2026-06-11): under pure or restricted eval TW
    // returns "" (prim_getEnv, libexpr/primops.cc:1372) — it must not leak the
    // ambient environment into a pure evaluation, where env values flow into
    // derivation `env` attrs and are store-path-affecting.  Mirror it.
    if (state.nixEvalState
        && (ffi::pureEval(*state.nixEvalState)
            || ffi::restrictEval(*state.nixEvalState))) {
        out = mkStringValueOwned("");
        return;
    }
    const char * e = std::getenv(args[0].asString());
    out = mkStringValueOwned(e ? e : "");
}

// Component-wise version comparison: matches Nix's libstore compareVersions.
// Splits each version into components (digit runs and non-digit runs) and
// compares pair-wise using Nix's specific ordering ("pre" < anything,
// shorter < longer when next is digits, etc.).  Required by the versions
// lang test which exercises pre-release ordering.
namespace {

std::string_view nextComponent(std::string_view::const_iterator & p,
                                std::string_view::const_iterator end)
{
    while (p != end && (*p == '.' || *p == '-')) ++p;
    if (p == end) return {};
    auto s = p;
    if (std::isdigit(static_cast<unsigned char>(*p)))
        while (p != end && std::isdigit(static_cast<unsigned char>(*p))) ++p;
    else
        while (p != end &&
               !std::isdigit(static_cast<unsigned char>(*p)) &&
               *p != '.' && *p != '-')
            ++p;
    return {&*s, size_t(p - s)};
}

bool componentsLT(std::string_view c1, std::string_view c2)
{
    // string -> int parse helper (returns nullopt on non-numeric).
    auto toInt = [](std::string_view sv) -> std::optional<long> {
        if (sv.empty()) return std::nullopt;
        char * end = nullptr;
        std::string s(sv);
        long v = std::strtol(s.c_str(), &end, 10);
        if (end != s.c_str() + s.size()) return std::nullopt;
        return v;
    };
    auto n1 = toInt(c1);
    auto n2 = toInt(c2);
    if (n1 && n2)                  return *n1 < *n2;
    if (c1.empty() && n2)          return true;
    if (c1 == "pre" && c2 != "pre") return true;
    if (c2 == "pre")                return false;
    if (n2)                        return true;   // assume `2.3a' < `2.3.1'
    if (n1)                        return false;
    return c1 < c2;
}

} // anonymous namespace

void primCompareVersions(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isString() || !args[1].isString())
        typeError("compareVersions", "two strings");
    // #674: TW's prim_compareVersions uses forceStringNoCtx on both
    // args (libexpr/primops.cc:1463-area).
    requireNoStringContext(state, args[0], "compareVersions");
    requireNoStringContext(state, args[1], "compareVersions");
    std::string_view v1(args[0].asString());
    std::string_view v2(args[1].asString());
    auto p1 = v1.begin();
    auto p2 = v2.begin();
    while (p1 != v1.end() || p2 != v2.end()) {
        auto c1 = nextComponent(p1, v1.end());
        auto c2 = nextComponent(p2, v2.end());
        if (componentsLT(c1, c2))     { out.mkInt(-1); return; }
        if (componentsLT(c2, c1))     { out.mkInt( 1); return; }
    }
    out.mkInt(0);
}

void primConcatMap(EvalState & state, Value * args, Value & out)
{
    Value lst = args[1];
    if (lst.isAppLike() || lst.tag() == Tag::Thunk || lst.tag() == Tag::Slot)
        lst = forceValue(*state.vm, lst);
    if (!lst.isList()) typeError("concatMap", "list");
    auto * src = lst.asList();
    Value fn = args[0];
    std::vector<Value> all;
    if (src) {
        for (uint32_t i = 0; i < src->size; ++i) {
            Value r = callClosure(*state.vm, fn, src->elems[i]);
            // Force the callback's return value — it may be a Tag::App
            // (e.g., when fn = (x: map g xs) and v3's lazy map returns
            // a list with App entries, then concatMap of that gets the
            // nested-list-as-App-entry shape).
            // #558 Phase 2: inline WHNF check.  callClosure typically
            // returns an already-forced value; skip the forceValue
            // function call for the WHNF path.
            {
                Tag rt = r.tag();
                if (__builtin_expect(rt == Tag::Thunk
                                     || rt == Tag::App || rt == Tag::App3
                                     || rt == Tag::Slot, 0))
                    r = forceValue(*state.vm, r);
            }
            if (!r.isList()) typeError("concatMap", "function returning list");
            if (r.asList())
                for (uint32_t j = 0; j < r.asList()->size; ++j)
                    all.push_back(r.asList()->elems[j]);
        }
    }
    ListVec * result = Alloc::allocList(static_cast<uint32_t>(all.size()));
    V3_STATS_INC(listsAllocated);
    for (size_t i = 0; i < all.size(); ++i) result->elems[i] = all[i];
    listPostConstructBarrier(result);  // Phase D coverage (primConcatMap; PhD-6)
    out.mkList(result);
}

void primPartition(EvalState & state, Value * args, Value & out)
{
    if (!args[1].isList()) typeError("partition", "list");
    auto * src = args[1].asList();
    Value pred = args[0];
    std::vector<Value> right_, wrong_;
    if (src) {
        for (uint32_t i = 0; i < src->size; ++i) {
            Value r = callClosure(*state.vm, pred, src->elems[i]);
            // The predicate may return a thunk / app / closure-eval-
            // pending value — force it to WHNF before the bool check.
            {
                Tag rt = r.tag();
                if (__builtin_expect(rt == Tag::Thunk
                                     || rt == Tag::App || rt == Tag::App3
                                     || rt == Tag::Slot, 0))
                    r = forceValue(*state.vm, r);
            }
            if (!r.isBool()) typeError("partition", "predicate returning bool");
            if (r.asInt() == 1) right_.push_back(src->elems[i]);
            else                  wrong_.push_back(src->elems[i]);
        }
    }
    auto mkList = [](std::vector<Value> & v) {
        ListVec * l = Alloc::allocList(static_cast<uint32_t>(v.size()));
        V3_STATS_INC(listsAllocated);
        for (size_t i = 0; i < v.size(); ++i) l->elems[i] = v[i];
        listPostConstructBarrier(l);  // Phase D coverage (primPartition; PhD-6)
        Value out;
        out.mkList(l);
        return out;
    };
    Value rightV = mkList(right_);
    Value wrongV = mkList(wrong_);

    // Intern via the global table so the resulting attrset's SymbolIds
    // match what other CUs and the JSON printer use.
    SymbolId sRight = ir::globalInternSymbol("right");
    SymbolId sWrong = ir::globalInternSymbol("wrong");

    Bindings * b = Alloc::allocBindings(2);
    V3_STATS_INC(attrsetsAllocated);
    if (sRight < sWrong) {
        bindingsSetEntry(b, 0, {sRight, 0, rightV});  // Phase D
        bindingsSetEntry(b, 1, {sWrong, 0, wrongV});
    } else {
        bindingsSetEntry(b, 0, {sWrong, 0, wrongV});  // Phase D
        bindingsSetEntry(b, 1, {sRight, 0, rightV});
    }
    out.mkAttrs(b);
}

/// Helper: intern a string into the global symbol table so the
/// resulting SymbolId is usable across CUs (matches what lower.cc
/// emits in OP_ATTRS_INIT and what attrset bindings store).
inline SymbolId vmIntern(EvalState & /*state*/, std::string_view s)
{
    return ir::globalInternSymbol(s);
}

inline std::string_view vmSymName(EvalState & /*state*/, SymbolId id)
{
    auto & st = ir::globalSymbolTable();
    return id < st.size() ? std::string_view(st[id]) : std::string_view("");
}

/// listToAttrs: takes a list of `{ name = "..."; value = ...; }` and
/// builds an attrset.
void primListToAttrs(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isList()) typeError("listToAttrs", "list");
    auto * src = args[0].asList();
    if (!src || src->size == 0) {
        Bindings * b = Alloc::allocBindings(0);
        V3_STATS_INC(attrsetsAllocated);
        out.mkAttrs(b);
        return;
    }
    SymbolId nameSym  = vmIntern(state, "name");
    SymbolId valueSym = vmIntern(state, "value");
    std::vector<std::pair<SymbolId, Value>> entries;
    entries.reserve(src->size);
    for (uint32_t i = 0; i < src->size; ++i) {
        Value el = forceValue(*state.vm, src->elems[i]);
        if (!el.isAttrs() || !el.asAttrs())
            typeError("listToAttrs", "list of attrsets");
        const Value * nvRaw = el.asAttrs()->lookup(nameSym);
        const Value * vvRaw = el.asAttrs()->lookup(valueSym);
        if (!nvRaw || !vvRaw)
            typeError("listToAttrs", "{ name = string; value = ...; }");
        Value nv = forceValue(*state.vm, *nvRaw);
        if (!nv.isString())
            typeError("listToAttrs", "{ name = string; value = ...; }");
        SymbolId k = vmIntern(state, nv.asString());
        // value stays lazy on purpose
        entries.emplace_back(k, *vvRaw);
    }
    // listToAttrs in Nix is *first-wins* on duplicate keys (matches the
    // tree-walker's behaviour and what `eval-okay-listtoattrs` exercises).
    // Stable-sort preserves insertion order within a key so the first
    // encounter survives the dedupe pass below.
    std::stable_sort(entries.begin(), entries.end(),
        [](auto & a, auto & b) { return a.first < b.first; });
    uint32_t uniqueCount = 0;
    SymbolId prev = std::numeric_limits<SymbolId>::max();
    for (auto & p : entries) {
        if (uniqueCount != 0 && p.first == prev)
            continue; // keep the first occurrence
        prev = p.first;
        ++uniqueCount;
    }

    Bindings * b = Alloc::allocBindings(uniqueCount);
    V3_STATS_INC(attrsetsAllocated);
    size_t outIdx = 0;
    prev = std::numeric_limits<SymbolId>::max();
    for (auto & p : entries) {
        if (outIdx != 0 && p.first == prev)
            continue; // keep the first occurrence
        prev = p.first;
        b->entries[outIdx].name  = p.first;
        b->entries[outIdx].pos   = 0;
        b->entries[outIdx].value = p.second;
        ++outIdx;
    }
    assert(outIdx == uniqueCount);
    bindingsPostConstructBarrier(b);  // Phase D batch barrier
    out.mkAttrs(b);
}

// #693 — TW phrasing for `expected a set/list but found <type>: <value>` errors.
// Mirrors libexpr/eval.cc's forceAttrs/forceList plus errorPrintOptions.
static std::string expectedTypeButFound(const char * expected,
                                         const Value & v)
{
    const char * art = "a"; const char * name = "value";
    Tag t = v.tag();
    if (t == Tag::Int)        { art = "an"; name = "integer"; }
    else if (t == Tag::Float) { art = "a";  name = "float"; }
    else if (t == Tag::Bool)  { art = "a";  name = "Boolean"; }
    else if (t == Tag::Null)  { art = "";   name = "null"; }
    else if (t == Tag::String){ art = "a";  name = "string"; }
    else if (t == Tag::Path)  { art = "a";  name = "path"; }
    else if (t == Tag::List)  { art = "a";  name = "list"; }
    else if (t == Tag::Attrs) { art = "a";  name = "set"; }
    else if (t == Tag::Closure || t == Tag::PrimOp || t == Tag::PrimOpApp)
                              { art = "a";  name = "function"; }
    auto valRepr = [&]() -> std::string {
        if (t == Tag::Int)    return std::to_string(v.asInt());
        if (t == Tag::Float)  { std::ostringstream os; os << v.asFloat(); return os.str(); }
        if (t == Tag::Bool)   return v.asInt() == 1 ? "true" : "false";
        if (t == Tag::Null)   return "null";
        if (t == Tag::String) return v.asString() ? std::string("\"") + v.asString() + "\"" : "\"\"";
        if (t == Tag::Path)   return v.asPath() ? std::string(v.asPath()) : "/";
        if (t == Tag::List)   return v.asList() && v.asList()->size > 0 ? "[ ... ]" : "[ ]";
        if (t == Tag::Attrs)  return v.asAttrs() && v.asAttrs()->size > 0 ? "{ ... }" : "{ }";
        // #820 (2026-05-26): TW's printFunction emits `«lambda <name>? @
        // <file>:<line>:<col>»` (eval.cc:1245 cites `ValuePrinter` which
        // delegates to `printFunction` in libexpr/print.cc).  Mirror
        // that here so error messages like "expected a set but found a
        // function: «lambda @ p6b.nix:1:43»" match TW byte-for-byte.
        // Without this match, run-inherit-from-laziness-tests' p6b
        // assertion fails on cosmetic-but-load-bearing string equality.
        if (t == Tag::Closure && v.asClosure() && v.asClosure()->desc) {
            std::string tok = "«lambda";
            const auto & d = *v.asClosure()->desc;
            if (!d.contextualName.empty()) {
                tok += ' ';
                tok += d.contextualName;
            }
            if (auto * ps = nix::v3::resolvePosSnapshot(d.posHandle)) {
                tok += " @ ";
                if (ps->file.empty() || ps->file == "<string>")
                    tok += "«string»";
                else if (ps->file == "<stdin>")
                    tok += "«stdin»";
                else if (ps->file == "<unknown>")
                    tok += "«none»";
                else
                    tok += ps->file;
                tok += ':';
                tok += std::to_string(ps->line);
                tok += ':';
                tok += std::to_string(ps->column);
            }
            tok += "»";
            return tok;
        }
        return "<value>";
    };
    std::string msg = "expected ";
    msg += expected;
    msg += " but found ";
    if (*art) { msg += art; msg += ' '; }
    msg += name;
    msg += ": ";
    msg += valRepr();
    return msg;
}

void primRemoveAttrs(EvalState & state, Value * args, Value & out)
{
    // #693 — match TW phrasing (libexpr/primops.cc:removeAttrs uses
    // forceAttrs/forceList which produce these messages).
    if (!args[0].isAttrs())
        throw std::runtime_error(expectedTypeButFound("a set", args[0]));
    if (!args[1].isList())
        throw std::runtime_error(expectedTypeButFound("a list", args[1]));
    const Bindings * src = args[0].asAttrs();
    auto * names = args[1].asList();
    if (!src || !names || names->size == 0) { out = args[0]; return; }
    std::unordered_set<SymbolId> toRemove;
    for (uint32_t i = 0; i < names->size; ++i) {
        // 2026-05-18: force each element to WHNF.  v3's lazy list
        // primops (mapAttrs/genList) install Tag::App entries that
        // resolve to strings only on force.  Without this, a
        // `removeAttrs s (map f xs)` call sees Tag::App where it
        // expects Tag::String.  Mirror primAttrNames' force pattern.
        Value el = names->elems[i];
        if (__builtin_expect(el.tag() == Tag::Thunk
                             || el.isAppLike()
                             || el.tag() == Tag::Slot, 0))
            el = forceValue(*state.vm, el);
        if (!el.isString()) typeError("removeAttrs", "list of strings");
        // #734: TW's prim_removeAttrs iterates with forceStringNoCtx per
        // element (libexpr/primops.cc).  Reject contexted names here too.
        requireNoStringContext(state, el, "removeAttrs");
        toRemove.insert(vmIntern(state, el.asString()));
    }
    if (src->isMapAttrs()) {
        uint32_t kExact = 0;
        for (uint32_t i = 0; i < src->size; ++i)
            if (toRemove.count(src->entries[i].name) == 0)
                ++kExact;
        Bindings * result = copyMapAttrsSubset(
            src, kExact,
            [&](SymbolId name) { return toRemove.count(name) == 0; });
        V3_STATS_INC(attrsetsAllocated);
        out.mkAttrs(result);
        return;
    }
    // #747 two-pass to avoid arena slack: pass 1 counts kept
    // entries, pass 2 allocates exact and fills.  Without this, the
    // original allocBindings(src->size) call pinned the full src
    // size in the arena even when many entries were removed.
    // #746 attribution on hello.drvPath measured 17.4 % slack here.
    uint32_t kExact = 0;
    forEachEntryNoMapAttrsRealize(src, [&](const Bindings::Entry & e) {
        if (toRemove.count(e.name) == 0)
            ++kExact;
    });
    Bindings * result = Alloc::allocBindings(kExact);
    V3_STATS_INC(attrsetsAllocated);
    auto copySrcEntry = [&](const Bindings * owner,
                            const Bindings::Entry & e) -> Bindings::Entry {
        if (owner && owner->isMapAttrs()
            && (e.pos & Bindings::kMapAttrsUnrealizedPosBit) != 0)
        {
            auto * mutOwner = const_cast<Bindings *>(owner);
            auto * mutEntry = const_cast<Bindings::Entry *>(&e);
            mutOwner->realizeMapAttrsEntry(mutEntry);
            return *mutEntry;
        }
        return e;
    };
    uint32_t k = 0;
    forEachEntryRefNoMapAttrsRealize(src, [&](const Bindings * owner,
                                              const Bindings::Entry & e) {
        if (toRemove.count(e.name) == 0) {
            result->entries[k++] = copySrcEntry(owner, e);
        }
    });
    // k == kExact by construction; allocBindings already set the size.
    bindingsPostConstructBarrier(result);  // Phase D batch barrier
    out.mkAttrs(result);
}

void primIntersectAttrs(EvalState &, Value * args, Value & out)
{
    if (!args[0].isAttrs() || !args[1].isAttrs())
        typeError("intersectAttrs", "two attrsets");
    const Bindings * keep = args[0].asAttrs();
    const Bindings * src  = args[1].asAttrs();
    if (!keep || !src) {
        Bindings * b = Alloc::allocBindings(0);
        V3_STATS_INC(attrsetsAllocated);
        out.mkAttrs(b);
        return;
    }
    // PLAN_BEAT_TW_V2 §0.3.1 — iterate the SMALLER side, binary-search
    // the larger.  The dominant nixpkgs pattern is the callPackage
    // shape `intersectAttrs (functionArgs f) pkgs`: |keep| ≈ formals
    // count (~10), |src| ≈ |pkgs| (~20 k).  The previous code merged
    // O(|keep|+|src|) in pass 1 (already linear) but pass 2 walked the
    // FULL `src` (~20 k) doing `keep->lookup` per entry — O(|src|·log
    // |keep|) ≈ 66 k comparisons.  Driving the loop from the smaller
    // side and binary-searching the larger is O(min·log max) ≈ 140
    // comparisons in that pattern (~100× less work), and it is never
    // worse than the merge in any size relation.
    //
    // Byte-identical by construction: `intersectAttrs e1 e2` keeps the
    // entries of e2 (`src`) whose name appears in e1 (`keep`).  The
    // result set and its values are input-determined; emitting in the
    // iterated side's sorted name-order yields the same sorted
    // Bindings regardless of which side drives the loop, and the
    // emitted entry (name, pos, value) is always copied from `src`.
    //
    const uint32_t keepN = keep->countDistinct();
    const uint32_t srcN  = src->countDistinct();
    const bool iterKeep   = keepN <= srcN;
    const Bindings * iter   = iterKeep ? keep : src;   // smaller — drives the loop
    const Bindings * search = iterKeep ? src  : keep;  // larger  — binary-searched
    // Pass 1: count the intersection (exact alloc, no arena slack —
    // #746/#747: a single-pass over-allocate of |src| was 386.3 MB of
    // pure waste from 1664 calls).
    uint32_t kExact = 0;
    forEachEntryNoMapAttrsRealize(iter, [&](const Bindings::Entry & e) {
        if (search->has(e.name)) ++kExact;
    });
    Bindings * result = Alloc::allocBindings(kExact);
    if (src->isMapAttrs() && kExact > 0) {
        result->kind = uint8_t(Bindings::Kind::MapAttrs);
        result->parent = src->parent;
        result->aux = src->aux;
    }
    V3_STATS_INC(attrsetsAllocated);
    const bool resultPreservesMapAttrs = result->isMapAttrs();
    auto copySrcEntry = [&](const Bindings * owner,
                            const Bindings::Entry & e) -> Bindings::Entry {
        if (!resultPreservesMapAttrs
            && owner && owner->isMapAttrs()
            && (e.pos & Bindings::kMapAttrsUnrealizedPosBit) != 0)
        {
            auto * mutOwner = const_cast<Bindings *>(owner);
            auto * mutEntry = const_cast<Bindings::Entry *>(&e);
            mutOwner->realizeMapAttrsEntry(mutEntry);
            return *mutEntry;
        }
        return e;
    };
    // Pass 2: fill — always emit the `src` entry (e2's value wins).
    uint32_t k = 0;
    if (iterKeep) {
        // iter == keep: look the matched entry up in src (the value side).
        forEachEntryNoMapAttrsRealize(iter, [&](const Bindings::Entry & e) {
            const Bindings * srcOwner = nullptr;
            if (const Bindings::Entry * se =
                    lookupEntryNoMapAttrsRealize(src, e.name, &srcOwner))
                result->entries[k++] = copySrcEntry(srcOwner, *se);
        });
    } else {
        // iter == src: emit the src entry directly when its name is in keep.
        forEachEntryRefNoMapAttrsRealize(iter, [&](const Bindings * owner,
                                                   const Bindings::Entry & e) {
            if (keep->has(e.name))
                result->entries[k++] = copySrcEntry(owner, e);
        });
    }
    // k == kExact by construction; allocBindings already set the size.
    bindingsPostConstructBarrier(result);  // Phase D batch barrier
    out.mkAttrs(result);
}

void primMapAttrs(EvalState &, Value * args, Value & out)
{
    Value fn = args[0];
    if (!args[1].isAttrs()) typeError("mapAttrs", "attrset");
    auto * src = args[1].asAttrs();
    if (!src) { out = args[1]; return; }
    if (fn.tag() == Tag::Closure
        && fn.asClosure()
        && fn.asClosure()->desc
        && fn.asClosure()->desc->secondArgIdentityLambda) {
        out = args[1];
        return;
    }
    uint32_t n = src->countDistinct();
    if (n == 0) { out.mkAttrs(Alloc::allocBindings(0)); return; }
    Bindings * result = Alloc::allocBindings(n);
    result->kind = uint8_t(Bindings::Kind::MapAttrs);
    result->parent = src;
    result->aux = fn;
    V3_STATS_INC(attrsetsAllocated);
    recordBindingsOrigin(result, 0, "primMapAttrs");
    uint32_t i = 0;
    auto copyEntry = [&](const Bindings::Entry & e) {
        result->entries[i].name = e.name;
        result->entries[i].pos =
            (e.pos & Bindings::kPosMask) | Bindings::kMapAttrsUnrealizedPosBit;
        // Preserve the old mapAttrs snapshot semantics: the source value is
        // captured at construction time.  The attr-name string is synthesized
        // only if this mapped entry is actually demanded.  When the source is
        // itself MapAttrs, this copies the raw lazy entry; realization resolves
        // the parent entry on demand so mapAttrs composition stays lazy.
        result->entries[i].value = e.value;
        ++i;
    };
    if (src->isMapAttrs()) {
        for (uint32_t j = 0; j < src->size; ++j)
            copyEntry(src->entries[j]);
    } else {
        src->forEach(copyEntry);
    }
    bindingsPostConstructBarrier(result);
    out.mkAttrs(result);
}

void primElem(EvalState & state, Value * args, Value & out)
{
    if (!args[1].isList()) typeError("elem", "list");
    auto * src = args[1].asList();
    bool found = false;
    // C-13 (CODEBASE_REVIEW_2026-06-11): the needle (arg0) is registered LAZY,
    // so it arrives unforced.  TW never forces the needle when the list
    // short-circuits — `builtins.elem (throw "x") []` returns false in TW but
    // threw in v3 when arg0 was eagerly pre-forced.  Force it here only when
    // there is at least one element to compare against (matching TW's
    // per-comparison force); an empty list returns false without touching it.
    if (src && src->size > 0) {
        Value x = forceValue(*state.vm, args[0]);
        for (uint32_t i = 0; i < src->size; ++i) {
            // A12 (2026-05-17) writeback-force: list elements built by
            // primMapAttrs / primGenList are Tag::App and need to be
            // resolved before valueEqual.  v3's forceValue takes Value
            // by value, so without writeback the resolved WHNF is lost
            // and every elem call re-runs the mapAttrs lambda for every
            // entry.  Tree-walker's forceValue takes Value& and its
            // list elements are Value*, so forces memoize via the
            // pointer.  Mirror that by forcing through the lvalue here.
            // Synthetic probe (test/repro-583-mapattrs-app-cache.nix)
            // confirms: TW = 3 forces, v3 without fix = 5N+ forces, v3
            // with fix = 3 forces.  Falsifies the prior memo note's
            // "not a generic memoization bug" conclusion — the bug is
            // generic but the probe used trivial lambda bodies that
            // hid the cost.  See PATH_B_INVESTIGATION_2026-05-16.md.
            Value & el = src->elems[i];
            if (el.isAppLike()
                || el.tag() == Tag::Thunk
                || el.tag() == Tag::Slot)
                el = forceValue(*state.vm, el);
            if (valueEqual(*state.vm, x, el)) { found = true; break; }
        }
    }
    out = found ? Value::vTrue : Value::vFalse;
}

void primGetAttr(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("getAttr", "string");
    if (!args[1].isAttrs())  typeError("getAttr", "attrset");
    // #734: TW's prim_getAttr calls forceStringNoCtx on the name arg
    // (libexpr/primops.cc).  v3 must reject contexted names.
    requireNoStringContext(state, args[0], "getAttr");
    SymbolId k = vmIntern(state, args[0].asString());
    auto * b = args[1].asAttrs();
    // #678 — match TW's `attribute '<name>' missing`
    // (libexpr/eval.cc:2766) instead of v3-specific phrasing.
    if (!b) throw std::runtime_error(
        "attribute '" + std::string(args[0].asString()) + "' missing");
    const Value * v = b->lookup(k);
    if (!v) throw std::runtime_error(
        "attribute '" + std::string(args[0].asString()) + "' missing");
    out = *v;
}

void primHasAttr(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("hasAttr", "string");
    if (!args[1].isAttrs())  typeError("hasAttr", "attrset");
    // #734: TW's prim_hasAttr calls forceStringNoCtx on the name arg.
    requireNoStringContext(state, args[0], "hasAttr");
    SymbolId k = vmIntern(state, args[0].asString());
    auto * b = args[1].asAttrs();
    out = (b && b->has(k)) ? Value::vTrue : Value::vFalse;
}

void primCatAttrs(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("catAttrs", "string");
    if (!args[1].isList())   typeError("catAttrs", "list");
    SymbolId k = vmIntern(state, args[0].asString());
    auto * lst = args[1].asList();
    std::vector<Value> kept;
    if (lst) {
        for (uint32_t i = 0; i < lst->size; ++i) {
            Value el = forceValue(*state.vm, lst->elems[i]);
            if (!el.isAttrs() || !el.asAttrs()) continue;
            const Value * v = el.asAttrs()->lookup(k);
            if (v) kept.push_back(*v);
        }
    }
    ListVec * result = Alloc::allocList(static_cast<uint32_t>(kept.size()));
    V3_STATS_INC(listsAllocated);
    for (size_t i = 0; i < kept.size(); ++i) result->elems[i] = kept[i];
    listPostConstructBarrier(result);  // Phase D coverage (primCatAttrs; PhD-6)
    out.mkList(result);
}

void primReplaceStrings(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isList() || !args[1].isList() || !args[2].isString())
        typeError("replaceStrings", "(list, list, string)");
    auto * froms = args[0].asList();
    auto * tos   = args[1].asList();
    if (!froms || !tos || froms->size != tos->size)
        // #689 — TW phrasing (libexpr/primops.cc:replaceStrings).
        throw std::runtime_error(
            "'from' and 'to' arguments passed to builtins.replaceStrings "
            "have different lengths");
    // 2026-05-19 #665: collect string-context from the input string
    // and any `to` element whose pattern actually matched (TW's
    // prim_replaceStrings at libexpr/primops.cc:5328-5353 maintains
    // an outer NixStringContext from forceString(input) + accumulates
    // forceString(to[i].ctx) on each matched replacement).  Without
    // this, `replaceStrings ["@x@"] ["${drv}"] s` produces a context-
    // free output, dropping the derivation dep.
    std::vector<std::string> ctxAccum;
    auto absorb = [&](const char * s) {
        if (!s) return;
        if (auto * raw = lookupStringContextEntries(s))
            ctxAccum.insert(ctxAccum.end(), raw->begin(), raw->end());
    };
    absorb(args[2].asString());
    // Force `from` elements upfront -- every iteration of the outer
    // loop reads them, and they're lazy by default.  `to` elements
    // stay lazy and are forced inside the match branch (matches
    // tree-walker; `replaceStrings ["match" "miss"] [.. (throw)] ..`
    // must NOT throw for the unmatched index -- see eval-okay-
    // replacestrings line 9 for the lang-test requirement).
    //
    // REVIEW §2.3 was checked but the review's claimed parity gap
    // doesn't hold: tree-walker IS lazy on `to`.  Eager-force here
    // would break eval-okay-replacestrings (regressed and reverted).
    for (uint32_t j = 0; j < froms->size; ++j) {
        froms->elems[j] = forceValue(*state.vm, froms->elems[j]);
        if (!froms->elems[j].isString())
            typeError("replaceStrings", "list of strings");
    }
    // Match Nix's tree-walker behaviour for replaceStrings:
    //  - At each position, scan `from` left-to-right, take first match.
    //  - An empty `from` matches the empty string at every position
    //    (including end-of-string), inserting `to` between each character
    //    (and at the start and end).  E.g. `replaceStrings [""] ["X"] "abc"`
    //    yields `"XaXbXcX"`.
    std::string s(args[2].asString());
    std::string result;
    size_t i = 0;
    auto tryReplaceAt = [&](size_t pos) -> int {
        for (uint32_t j = 0; j < froms->size; ++j) {
            std::string_view fv(froms->elems[j].asString());
            bool match = fv.empty()
                ? true
                : (pos + fv.size() <= s.size() && s.compare(pos, fv.size(), fv) == 0);
            if (!match) continue;
            Value t = forceValue(*state.vm, tos->elems[j]);
            if (!t.isString()) typeError("replaceStrings", "list of strings");
            result.append(t.asString());
            absorb(t.asString());
            return static_cast<int>(fv.size());
        }
        return -1;
    };
    while (i < s.size()) {
        int adv = tryReplaceAt(i);
        if (adv < 0)      { result.push_back(s[i]); ++i; }
        else if (adv == 0){ result.push_back(s[i]); ++i; } // empty match: copy 1 char + replacement
        else              { i += adv; }
    }
    // Final empty-match at end-of-string (handles `["" ...]` -> trailing X).
    tryReplaceAt(s.size());
    out = mkStringValueOwned(result);
    if (!ctxAccum.empty()) {
        std::sort(ctxAccum.begin(), ctxAccum.end());
        ctxAccum.erase(std::unique(ctxAccum.begin(), ctxAccum.end()), ctxAccum.end());
        setStringContextEntries(out.asString(), std::move(ctxAccum));
    }
}

void primAbort(EvalState &, Value * args, Value &)
{
    if (!args[0].isString()) typeError("abort", "string");
    // AbortError is a plain runtime_error (not derived from AssertionError),
    // so tryEval does NOT catch it -- matches tree-walker's nix::Abort.
    //
    // #677 — match TW's exact phrasing
    // (libexpr/primops.cc:1147): "evaluation aborted with the
    // following error message: '<msg>'".  Drops the v3-specific
    // prefix so default-mode `nix eval` byte-matches TW.
    throw AbortError(
        std::string("evaluation aborted with the following error message: '")
        + args[0].asString() + "'");
}

void primSeq(EvalState &, Value * args, Value & out)
{
    // seq: forces first arg, returns second.  Force already happened in
    // the caller's strict context (Force inserted by lowerExpr); we
    // just return args[1] here.  (For lazy semantics this would matter
    // more, but our v3 currently uses strict eval almost everywhere.)
    (void)args;
    out = args[1];
}

/// Recursively force every thunk reachable from `v`, propagating any
/// error.  Lists/attrsets are traversed; functions are not entered.
/// Tracks visited containers to break cycles like `let as = {y = as;}; in as`.
static Value forceDeepRec(VMState & vm, Value v, std::unordered_set<const void *> & seen)
{
    v = forceValue(vm, v);
    if (v.isList() && v.asList()) {
        if (!seen.insert(v.asList()).second) return v;
        for (uint32_t i = 0; i < v.asList()->size; ++i)
            v.asList()->elems[i] = forceDeepRec(vm, v.asList()->elems[i], seen);
        // Phase D barrier (PhD-6, 2026-06-15): unlike print.cc forceDeep (whose
        // worklist `tlDeepForceRoots` IS a scavenge root — gc.cc:992 — so its
        // list writebacks are root-covered), forceDeepRec walks via a plain C++
        // local `v` that is NOT a scavenge root.  The raw `elems[i] =` writeback
        // above can store a freshly-forced nursery value (Closure/ListVec) into a
        // tenured list; if that list is later reachable only through a non-
        // remembered tenured container, the scavenger never visits it and the
        // nursery value dangles.  Register it in the remembered set.  No scavenge
        // fires during the recursion (primDeepSeq runs at exitDepth>=1), so
        // `v.asList()` is stable and one post-loop barrier suffices.  Mirrors the
        // attrs branch's per-entry bindingsSetValue below.
        listPostConstructBarrier(v.asList());
    } else if (v.isAttrs() && v.asAttrs()) {
        // ChainBindings: deep-force every value in the WHOLE chain (overlay +
        // parent), else a `throw` in a parent value would escape deepSeq.
        // MapAttrs: force the mapped value, not the stored source value, but
        // avoid realizing each mapped entry into an App3 first.
        const Bindings * b = v.asAttrs();
        if (b->isChain() || b->isMapAttrs()) {
            if (!seen.insert(b).second) return v;
            forEachEntryRefNoMapAttrsRealize(b, [&](const Bindings * owner,
                                                     const Bindings::Entry & e) {
                forceDeepRec(vm, entryValueForImmediateDemand(vm, owner, e), seen);
            });
        } else {
            if (!seen.insert(v.asAttrs()).second) return v;
            for (uint32_t i = 0; i < v.asAttrs()->size; ++i)
                bindingsSetValue(v.asAttrs(), i,  // Phase D
                    forceDeepRec(vm, v.asAttrs()->entries[i].value, seen));
        }
    }
    return v;
}

static Value forceDeepRec(VMState & vm, Value v)
{
    std::unordered_set<const void *> seen;
    return forceDeepRec(vm, v, seen);
}

void primDeepSeq(EvalState & state, Value * args, Value & out)
{
    // Force `args[0]` deeply, throwing on any contained error, then
    // return `args[1]`.  Matches tree-walker semantics — used to
    // ensure errors in lazy structure are surfaced before returning.
    forceDeepRec(*state.vm, args[0]);
    out = args[1];
}

/// builtins.unsafeGetAttrPos NAME ATTRS — return a `{file, line, column}`
/// attrset for the AST position of NAME's definition, or null if no
/// such position is known.  Backed by the per-attr position side-table
/// (see alloc.hh) which is populated by OP_ATTRS_INIT[_DYN] /
/// OP_ATTRS_REC_INIT during compilation.  The pool value is itself
/// just a snapshot — the actual nix::PosTable lookup happened during
/// lowering and the result is held in the global posSnapshotPool.
void primUnsafeGetAttrPos(EvalState & state, Value * args, Value & out)
{
    // #693 — TW (libexpr/primops.cc) uses forceStringNoCtx / forceAttrs
    // which produce `expected a <T> but found <T>: <value>`.
    if (!args[0].isString())
        throw std::runtime_error(expectedTypeButFound("a string", args[0]));
    if (!args[1].isAttrs()) {
        out.mkNull();
        return;
    }
    if (!args[1].isAttrs() || !args[1].asAttrs()) {
        out.mkNull();
        return;
    }
    SymbolId nameId = ir::globalInternSymbol(args[0].asString());
    uint32_t handle = lookupAttrPos(args[1].asAttrs(), nameId);
    const PosSnapshot * snap = resolvePosSnapshot(handle);
    if (!snap) { out.mkNull(); return; }
    // TW behavior (libexpr/eval.cc:1007 mkPos): return null when the
    // position's origin is NOT a SourcePath — synthetic sources like
    // `<string>` (used by `nix eval --expr`), `<stdin>`, or `<unknown>`
    // are not user-visible file paths and shouldn't surface as a
    // `{ file = "<string>"; line; column }` attrset.  v3 stores the
    // synthetic markers as literal strings in PosSnapshot::file; check
    // for them here to match TW's null-on-non-SourcePath behavior.
    if (snap->file.empty()
        || snap->file == "<string>"
        || snap->file == "<stdin>"
        || snap->file == "<unknown>")
    {
        out.mkNull();
        return;
    }
    SymbolId sFile   = vmIntern(state, "file");
    SymbolId sLine   = vmIntern(state, "line");
    SymbolId sColumn = vmIntern(state, "column");
    Bindings * b = Alloc::allocBindings(3);
    V3_STATS_INC(attrsetsAllocated);
    std::vector<std::pair<SymbolId, Value>> entries(3);
    Value vFile = mkStringValueOwned(snap->file);
    Value vLine; vLine.mkInt(snap->line);
    Value vCol;  vCol.mkInt(snap->column);
    entries[0] = {sFile,   vFile};
    entries[1] = {sLine,   vLine};
    entries[2] = {sColumn, vCol};
    std::sort(entries.begin(), entries.end(),
        [](auto & a, auto & b) { return a.first < b.first; });
    for (size_t i = 0; i < entries.size(); ++i) {
        b->entries[i].name  = entries[i].first;
        bindingsSetValue(b, static_cast<uint32_t>(i), entries[i].second);  // Phase D
    }
    out.mkAttrs(b);
}

/// builtins.toPath path-or-string -> path.
/// String inputs must be absolute paths (start with '/').  Tree-walker
/// rejects relative strings; v3 matches.  Also accepts attrsets with
/// `__toString` or `outPath` (the standard Nix coercion path).
void primToPath(EvalState & state, Value * args, Value & out)
{
    auto fromString = [&](const char * s) {
        if (!s || s[0] != '/')
            // #693 — match TW phrasing (libexpr/primops.cc:toPath).
            throw std::runtime_error(
                std::string("string '") + (s ? s : "")
                + "' doesn't represent an absolute path");
        // REVIEW §2.9: normalize via CanonPath so `/nix/store/../etc/passwd`
        // and other `..` / `.` / double-slash shapes can't slip through
        // as a path Value.  CanonPath rejects any traversal that would
        // escape its initial root.
        std::string canon;
        try {
            canon = nix::CanonPath(s).abs();
        } catch (const std::exception & e) {
            throw std::runtime_error(
                std::string("v3 toPath: invalid path '") + s + "': " + e.what());
        }
        const size_t n = canon.size() + 1;
        char * buf = Alloc::allocChars(n);
        std::memcpy(buf, canon.data(), canon.size());
        buf[canon.size()] = '\0';
        out.mkPath(buf);
    };
    Value v = forceValue(*state.vm, args[0]);
    if (v.isPath())   { out = v; return; }
    if (v.isString()) { fromString(v.asString()); return; }
    if (v.isAttrs() && v.asAttrs()) {
        static const SymbolId tsId  = ir::globalInternSymbol("__toString");
        static const SymbolId outId = ir::globalInternSymbol("outPath");
        if (auto * fn = v.asAttrs()->lookup(tsId)) {
            Value forced = forceValue(*state.vm, *fn);
            Value s = callClosure(*state.vm, forced, v);
            s = forceValue(*state.vm, s);
            if (s.isString()) { fromString(s.asString()); return; }
        }
        if (auto * op = v.asAttrs()->lookup(outId)) {
            Value forced = forceValue(*state.vm, *op);
            if (forced.isString()) { fromString(forced.asString()); return; }
            if (forced.isPath())   { out = forced; return; }
        }
    }
    // #693 — TW's toPath uses coerceToString-style errors for non-
    // string/non-path/non-coercible args.  Mirror the
    // `cannot coerce <type> to a string: <value>` shape that the
    // shared error sites already produce (vm.cc:coerceToString /
    // primops.cc:pathExists).
    const char * art = "a"; const char * name = "value";
    Tag t = v.tag();
    if (t == Tag::Int)   { art = "an"; name = "integer"; }
    else if (t == Tag::Float) { art = "a";  name = "float"; }
    else if (t == Tag::Bool)  { art = "a";  name = "Boolean"; }
    else if (t == Tag::Null)  { art = "";   name = "null"; }
    else if (t == Tag::List)  { art = "a";  name = "list"; }
    else if (t == Tag::Attrs) { art = "a";  name = "set"; }
    else if (t == Tag::Closure || t == Tag::PrimOp || t == Tag::PrimOpApp)
                              { art = "a";  name = "function"; }
    auto val = [&]() -> std::string {
        if (t == Tag::Int)   return std::to_string(v.asInt());
        if (t == Tag::Float) { std::ostringstream os; os << v.asFloat(); return os.str(); }
        if (t == Tag::Bool)  return v.asInt() == 1 ? "true" : "false";
        if (t == Tag::Null)  return "null";
        if (t == Tag::List)  return v.asList() && v.asList()->size > 0 ? "[ ... ]" : "[ ]";
        if (t == Tag::Attrs) return v.asAttrs() && v.asAttrs()->size > 0 ? "{ ... }" : "{ }";
        return "<value>";
    }();
    std::string msg = "cannot coerce ";
    if (*art) { msg += art; msg += ' '; }
    msg += name;
    msg += " to a string: ";
    msg += val;
    throw std::runtime_error(msg);
}

/// builtins.splitVersion — TW's algorithm (libstore/names.cc:54
/// `nextComponent`).  Splits version string into components:
///   - Skip '.' and '-' separators.
///   - Each component is either ALL DIGITS or ALL NON-DIGIT-NON-SEP.
///   - So "1.0.0-rc1" → ["1", "0", "0", "rc", "1"] (the "rc1" tail
///     splits into "rc" then "1" because of the letter→digit boundary).
///
/// Pre-fix v3 only split on '.' and '-', producing ["1","0","0","rc1"]
/// which made `compareVersions` give wrong results for any version
/// like "1.0-rc1" vs "1.0-rc2" (it compared the strings "rc1" vs
/// "rc2" as opaque tokens rather than splitting into "rc"="rc" then
/// 1<2).  This is a SEMANTIC bug affecting nixpkgs version
/// resolution.
void primSplitVersion(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("splitVersion", "string");
    // #674: TW's prim_splitVersion uses forceStringNoCtx; v3 must
    // reject contexted strings to match.
    requireNoStringContext(state, args[0], "splitVersion");
    std::string_view s(args[0].asString());
    std::vector<std::string_view> parts;
    auto isSep = [](char c) { return c == '.' || c == '-'; };
    auto p = s.begin();
    while (p != s.end()) {
        // Skip separators.
        while (p != s.end() && isSep(*p)) ++p;
        if (p == s.end()) break;
        auto start = p;
        if (std::isdigit(static_cast<unsigned char>(*p))) {
            // All digits.
            while (p != s.end() && std::isdigit(static_cast<unsigned char>(*p))) ++p;
        } else {
            // All non-digit, non-separator.
            while (p != s.end() && !std::isdigit(static_cast<unsigned char>(*p))
                   && !isSep(*p)) ++p;
        }
        parts.emplace_back(start, p - start);
    }
    ListVec * lv = Alloc::allocList(static_cast<uint32_t>(parts.size()));
    V3_STATS_INC(listsAllocated);
    for (size_t i = 0; i < parts.size(); ++i)
        lv->elems[i] = mkStringValueOwned(std::string(parts[i]));
    out.mkList(lv);
}

/// Helper: clone a v3 string with the same contents but a fresh
/// payload buffer (so context tagging is per-string-value).
static Value cloneString(const char * s)
{
    return mkStringValueOwned(std::string(s ? s : ""));
}

void primUnsafeDiscardStringContext(EvalState & state, Value * args, Value & out)
{
    // #757c: mirror TW's prim_unsafeDiscardStringContext
    // (libexpr/primops/context.cc:9-15) which uses `coerceToString`
    // (NOT forceString) — accepts paths, derivations (via
    // __toString / outPath), and other coercible values, dropping
    // any context that the coercion would normally have attached.
    // Pre-fix v3 rejected non-strings outright, diverging from TW
    // on haskell-nix's `unsafeDiscardStringContext "${pkgs.X}"`
    // pattern (where the inside isn't WHNF-string).
    if (args[0].isString()) {
        // Fast path: string in → string out, context-stripped.
        out = cloneString(args[0].asString());
        return;
    }
    // Slow path: coerce + drop context.
    std::vector<std::string> ctx;
    std::string s = toStringCoerceCtx(state, args[0], ctx,
                                       /*copyPathsToStore=*/false);
    out = mkStringValueOwned(std::move(s));
    // Deliberately do NOT call setStringContextEntries — the whole
    // point of unsafeDiscardStringContext is to drop context.
}

void primHasContext(EvalState &, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("hasContext", "string");
    out = lookupStringContextEntries(args[0].asString())
        ? Value::vTrue : Value::vFalse;
}

void primGetContext(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("getContext", "string");
    auto * raw = lookupStringContextEntries(args[0].asString());
    if (!raw) {
        Bindings * b = Alloc::allocBindings(0);
        out.mkAttrs(b);
        return;
    }
    auto ctx = decodeStringContext(*raw);
    // Group entries by store-path string; per group, collect:
    //   path        — present (i.e. an Opaque element matched).
    //   outputs     — list of output names (Built elements).
    //   allOutputs  — true if a DrvDeep matched.
    SymbolId sPath       = vmIntern(state, "path");
    SymbolId sOutputs    = vmIntern(state, "outputs");
    SymbolId sAllOutputs = vmIntern(state, "allOutputs");
    if (!state.nixEvalState) {
        Bindings * b = Alloc::allocBindings(0);
        out.mkAttrs(b);
        return;
    }
    auto & ns = *state.nixEvalState;
    struct Group { bool isPath = false; std::vector<std::string> outputs; bool allOutputs = false; };
    std::map<std::string, Group> groups;
    for (auto & e : ctx) {
        if (auto * o = std::get_if<nix::NixStringContextElem::Opaque>(&e.raw)) {
            groups[ns.store->printStorePath(o->path)].isPath = true;
        } else if (auto * d = std::get_if<nix::NixStringContextElem::DrvDeep>(&e.raw)) {
            groups[ns.store->printStorePath(d->drvPath)].allOutputs = true;
        } else if (auto * b = std::get_if<nix::NixStringContextElem::Built>(&e.raw)) {
            // Built carries a DrvPath (single drv) plus an output name.
            std::string drvPath;
            if (auto * dp = std::get_if<nix::SingleDerivedPath::Opaque>(&(*b->drvPath).raw()))
                drvPath = ns.store->printStorePath(dp->path);
            if (!drvPath.empty())
                groups[drvPath].outputs.push_back(b->output);
        }
    }
    std::vector<std::pair<SymbolId, Value>> entries;
    entries.reserve(groups.size());
    for (auto & [path, g] : groups) {
        std::vector<std::pair<SymbolId, Value>> subEntries;
        if (g.isPath) subEntries.emplace_back(sPath, Value::vTrue);
        if (g.allOutputs) subEntries.emplace_back(sAllOutputs, Value::vTrue);
        if (!g.outputs.empty()) {
            std::sort(g.outputs.begin(), g.outputs.end());
            ListVec * lv = Alloc::allocList(static_cast<uint32_t>(g.outputs.size()));
            V3_STATS_INC(listsAllocated);
            for (size_t i = 0; i < g.outputs.size(); ++i)
                lv->elems[i] = mkStringValueOwned(g.outputs[i]);
            Value lvVal;
            lvVal.mkList(lv);
            subEntries.emplace_back(sOutputs, lvVal);
        }
        std::sort(subEntries.begin(), subEntries.end(),
            [](auto & a, auto & b) { return a.first < b.first; });
        Bindings * sb = Alloc::allocBindings(static_cast<uint32_t>(subEntries.size()));
        V3_STATS_INC(attrsetsAllocated);
        for (size_t i = 0; i < subEntries.size(); ++i) {
            sb->entries[i].name  = subEntries[i].first;
            bindingsSetValue(sb, static_cast<uint32_t>(i), subEntries[i].second);  // Phase D
        }
        Value subVal;
        subVal.mkAttrs(sb);
        entries.emplace_back(vmIntern(state, path), subVal);
    }
    std::sort(entries.begin(), entries.end(),
        [](auto & a, auto & b) { return a.first < b.first; });
    Bindings * bb = Alloc::allocBindings(static_cast<uint32_t>(entries.size()));
    V3_STATS_INC(attrsetsAllocated);
    for (size_t i = 0; i < entries.size(); ++i) {
        bb->entries[i].name  = entries[i].first;
        bindingsSetValue(bb, static_cast<uint32_t>(i), entries[i].second);  // Phase D
    }
    out.mkAttrs(bb);
}

/// builtins.appendContext s ctx — add `ctx`'s entries to `s`'s context.
/// `ctx` is an attrset of `store-path -> { path; outputs; allOutputs; }`.
void primAppendContext(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isString())
        typeError("appendContext", "(string, attrset)");
    Value ctxV = forceValue(*state.vm, args[1]);
    if (!ctxV.isAttrs() || !ctxV.asAttrs())
        typeError("appendContext", "second arg attrset");
    // Start with the existing context.
    auto & symTab = ir::globalSymbolTable();
    auto existing = lookupStringContext(args[0].asString());
    nix::NixStringContext ctx = existing;
    auto * ctxB = ctxV.asAttrs();
    if (!state.nixEvalState) {
        out = cloneString(args[0].asString());
        return;
    }
    auto & ns = *state.nixEvalState;
    SymbolId sPath       = vmIntern(state, "path");
    SymbolId sOutputs    = vmIntern(state, "outputs");
    SymbolId sAllOutputs = vmIntern(state, "allOutputs");
    for (uint32_t i = 0; i < ctxB->size; ++i) {
        SymbolId k = ctxB->entries[i].name;
        std::string pathStr(k < symTab.size() ? symTab[k] : "");
        Value sub = forceValue(*state.vm, ctxB->entries[i].value);
        if (!sub.isAttrs() || !sub.asAttrs()) continue;
        nix::StorePath storePath = ns.store->parseStorePath(pathStr);
        if (auto * pp = sub.asAttrs()->lookup(sPath)) {
            Value pv = forceValue(*state.vm, *pp);
            if (pv.isBool() && pv.asInt() == 1)
                ctx.insert(nix::NixStringContextElem{nix::NixStringContextElem::Opaque{.path = storePath}});
        }
        if (auto * ao = sub.asAttrs()->lookup(sAllOutputs)) {
            Value av = forceValue(*state.vm, *ao);
            if (av.isBool() && av.asInt() == 1)
                ctx.insert(nix::NixStringContextElem{nix::NixStringContextElem::DrvDeep{.drvPath = storePath}});
        }
        if (auto * outsRaw = sub.asAttrs()->lookup(sOutputs)) {
            Value ov = forceValue(*state.vm, *outsRaw);
            if (ov.isList() && ov.asList()) {
                for (uint32_t j = 0; j < ov.asList()->size; ++j) {
                    Value e = forceValue(*state.vm, ov.asList()->elems[j]);
                    if (!e.isString()) continue;
                    nix::SingleDerivedPath dp{nix::SingleDerivedPath::Opaque{.path = storePath}};
                    nix::ref<nix::SingleDerivedPath> drvRef =
                        nix::make_ref<nix::SingleDerivedPath>(dp);
                    ctx.insert(nix::NixStringContextElem{
                        nix::NixStringContextElem::Built{
                            .drvPath = drvRef, .output = e.asString()}});
                }
            }
        }
    }
    out = cloneString(args[0].asString());
    if (!ctx.empty())
        setStringContext(out.asString(), ctx);
}

/// builtins.addDrvOutputDependencies — turn each Opaque entry in the
/// string's context into a DrvDeep entry.  No-op for non-Opaque
/// entries.  Idempotent.  Uses the same cloneString pattern so we
/// don't mutate the original buffer's table entry.
void primAddDrvOutputDependencies(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isString())
        typeError("addDrvOutputDependencies", "string");
    auto existing = lookupStringContext(args[0].asString());
    // Tree-walker requires exactly one context entry which must be a
    // single .drv path (Opaque or DrvDeep).  v3 must mirror that.
    // #692 — match TW's exact error phrasing
    // (libexpr/primops.cc:addDrvOutputDependencies family).  Drops the
    // "v3 addDrvOutputDependencies:" debug prefix and uses TW's
    // `context of string 'X' must have exactly one element, but has N`
    // pattern.  The trailing ", but has 0/N" carries the actual count.
    if (existing.empty())
        throw std::runtime_error(
            "context of string '" + std::string(args[0].asString())
            + "' must have exactly one element, but has 0");
    if (existing.size() > 1)
        throw std::runtime_error(
            "context of string '" + std::string(args[0].asString())
            + "' must have exactly one element, but has "
            + std::to_string(existing.size()));
    const auto & e = *existing.begin();
    if (!std::holds_alternative<nix::NixStringContextElem::Opaque>(e.raw) &&
        !std::holds_alternative<nix::NixStringContextElem::DrvDeep>(e.raw))
        throw std::runtime_error(
            "context entry of string '" + std::string(args[0].asString())
            + "' is not a derivation path");
    nix::NixStringContext ctx;
    if (auto * o = std::get_if<nix::NixStringContextElem::Opaque>(&e.raw)) {
        // Opaque entries are also rejected if they don't end in .drv —
        // tree-walker requires the path be a derivation.
        if (!o->path.name().ends_with(".drv"))
            throw std::runtime_error(
                "context entry of string '" + std::string(args[0].asString())
                + "' is not a derivation path");
        ctx.insert(nix::NixStringContextElem{nix::NixStringContextElem::DrvDeep{.drvPath = o->path}});
    } else {
        ctx.insert(e);
    }
    out = cloneString(args[0].asString());
    if (!ctx.empty()) setStringContext(out.asString(), ctx);
    (void)state;
}

/// builtins.unsafeDiscardOutputDependency — turn DrvDeep entries
/// (`=<drvPath>`) into Opaque entries (`<drvPath>`).  Other entries
/// pass through.
void primUnsafeDiscardOutputDependency(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isString())
        typeError("unsafeDiscardOutputDependency", "string");
    auto existing = lookupStringContext(args[0].asString());
    nix::NixStringContext ctx;
    for (auto & e : existing) {
        if (auto * d = std::get_if<nix::NixStringContextElem::DrvDeep>(&e.raw)) {
            ctx.insert(nix::NixStringContextElem{nix::NixStringContextElem::Opaque{.path = d->drvPath}});
        } else {
            ctx.insert(e);
        }
    }
    out = cloneString(args[0].asString());
    if (!ctx.empty()) setStringContext(out.asString(), ctx);
    (void)state;
}

/// builtins.__nixPath : list of `{prefix, path}` attrsets.  Reads the
/// host EvalState's LookupPath.  Used implicitly by `<x>` syntax.
void primNixPath(EvalState & state, Value *, Value & out)
{
    if (!state.nixEvalState) {
        ListVec * empty = Alloc::allocList(0);
        out.mkList(empty);
        return;
    }
    auto lookupPath = state.nixEvalState->getLookupPath();
    auto & lp = lookupPath.elements;
    ListVec * lv = Alloc::allocList(static_cast<uint32_t>(lp.size()));
    V3_STATS_INC(listsAllocated);
    SymbolId sPath   = vmIntern(state, "path");
    SymbolId sPrefix = vmIntern(state, "prefix");
    size_t i = 0;
    for (auto & el : lp) {
        Bindings * b = Alloc::allocBindings(2);
        SymbolId nA = sPath, nB = sPrefix;
        Value vA = mkStringValueOwned(el.path.s);
        Value vB = mkStringValueOwned(el.prefix.s);
        if (nA < nB) {
            bindingsSetEntry(b, 0, {nA, 0, vA});  // Phase D
            bindingsSetEntry(b, 1, {nB, 0, vB});
        } else {
            bindingsSetEntry(b, 0, {nB, 0, vB});  // Phase D
            bindingsSetEntry(b, 1, {nA, 0, vA});
        }
        Value v;
        v.mkAttrs(b);
        lv->elems[i++] = v;
    }
    out.mkList(lv);
}

/// builtins.__findFile : list-of-{prefix, path} → name → resolved path.
/// Looks up `name` in the search path entries and returns the matching
/// SourcePath.  Throws if no entry matches.
void primFindFile(EvalState & state, Value * args, Value & out)
{
    if (!state.nixEvalState)
        throw std::runtime_error("v3 primop findFile: no nix EvalState wired");
    if (!args[0].isList()) typeError("findFile", "list of {path, prefix}");
    if (!args[1].isString()) typeError("findFile", "string");

    // Build a LookupPath from the v3 list.  Each element is an attrset
    // with `path` (string-or-path) and `prefix` (string).
    nix::LookupPath lp;
    auto * lst = args[0].asList();
    if (lst) {
        SymbolId sPath   = vmIntern(state, "path");
        SymbolId sPrefix = vmIntern(state, "prefix");
        for (uint32_t i = 0; i < lst->size; ++i) {
            Value el = forceValue(*state.vm, lst->elems[i]);
            if (!el.isAttrs() || !el.asAttrs()) continue;
            const Value * pV = el.asAttrs()->lookup(sPath);
            const Value * prV = el.asAttrs()->lookup(sPrefix);
            std::string p, prefix;
            if (pV) {
                Value f = forceValue(*state.vm, *pV);
                if (f.isString()) p = f.asString();
                else if (f.isPath()) p = f.asPath();
            }
            if (prV) {
                Value f = forceValue(*state.vm, *prV);
                if (f.isString()) prefix = f.asString();
            }
            if (p.empty()) continue;
            lp.elements.push_back({nix::LookupPath::Prefix{prefix},
                                   nix::LookupPath::Path{p}});
        }
    }
    auto sp = state.nixEvalState->findFile(lp, args[1].asString());
    // CRIT-4: arena allocation.
    const std::string & abs = sp.path.abs();
    char * buf = Alloc::allocChars(abs.size() + 1);
    std::memcpy(buf, abs.data(), abs.size());
    buf[abs.size()] = '\0';
    out.mkPath(buf);
}

/// builtins.zipAttrsWith fn list-of-attrsets:
///   merge a list of attrsets, applying `fn name [values]` to combine
///   per-name lists.  Order in the value list mirrors source order.
void primZipAttrsWith(EvalState & state, Value * args, Value & out)
{
    // WC-35 root-cause: tree-walker's lib.attrsets.zipAttrsWith uses
    // `genAttrs names (name: f name (catAttrs name sets))` — entries
    // are built lazily.  v3 had an eager-call version that called
    // `f name list` for EVERY name at zipAttrsWith time, which forced
    // each module's per-name config attribute (via pushDownProperties)
    // even for names we never queried.  In nixpkgs's lib/modules.nix,
    // building pushedDownDefinitionsByName then forced `warnings`,
    // `assertions`, etc. of pkgs/top-level/config.nix's config
    // attribute, which transitively forced the `config` rec sibling
    // currently being built — deadlock.
    //
    // Match tree-walker by emitting Tag::App entries: each entry value
    // is `App(App(fn, name_str), values_list)`.  Forcing the entry
    // chases the App chain via OP_FORCE / forceValue's normal App
    // resolution.
    Value fn = args[0];
    if (!args[1].isList()) typeError("zipAttrsWith", "list of attrsets");
    auto * lst = args[1].asList();
    // Group by symbol id, preserving value order.  Forcing each list
    // entry to attrset shape is required to enumerate names — same
    // strictness tree-walker has.
    std::unordered_map<SymbolId, std::vector<Value>> byName;
    if (lst) {
        for (uint32_t i = 0; i < lst->size; ++i) {
            Value attrs = forceValue(*state.vm, lst->elems[i]);
            if (!attrs.isAttrs() || !attrs.asAttrs()) continue;
            const Bindings * ab = attrs.asAttrs();
            ab->forEach([&](const Bindings::Entry & en) {
                byName[en.name].push_back(en.value);
            });
        }
    }
    std::vector<std::pair<SymbolId, Value>> entries;
    entries.reserve(byName.size());
    auto & symTab = ir::globalSymbolTable();
    for (auto & [sid, vs] : byName) {
        // Build the values list eagerly (cheap — just allocates the
        // ListVec; entries themselves stay lazy).
        ListVec * vl = Alloc::allocList(static_cast<uint32_t>(vs.size()));
        V3_STATS_INC(listsAllocated);
        for (size_t i = 0; i < vs.size(); ++i) vl->elems[i] = vs[i];
        // Phase D barrier (PhD-6, 2026-06-15): the lazy entries `vs[i]` are
        // (typically) nursery thunks, and `vl` is tenured when the nursery was
        // full at allocList time (nurseryOrArena).  Without this the scavenger
        // never visits `vl` (it's neither young nor in the remembered set) and
        // its nursery thunks dangle — the git.drvPath missed-root that blocked
        // the nursery flip.  Mirrors primMap/primTail/primAttrNames.
        listPostConstructBarrier(vl);  // Phase D coverage (primZipAttrsWith)
        Value lv;
        lv.mkList(vl);
        // Build name string.
        std::string nm = sid < symTab.size() ? symTab[sid] : std::to_string(sid);
        Value nameV = mkStringValueOwned(nm);
        // 2026-05-30 RESTORATION of Tag::App3 with separate memo slot
        // — see primMapAttrs above for the design rationale.  Pack
        // `fn name list` into a single ValuePair (left=fn, right=name,
        // third=list, evaluated=memo sink).  The original Day 9-11
        // regression cause (memo collision with arg2) is fixed by the
        // separate `third` slot.
        //
        // Note: this is the C fallback for zipAttrsWith.  The default
        // is the bytecode-installed version per commit 1243c158b
        // (Tier 2c); this site fires only with NIX_V3_NO_BC_ZIP_ATTRS_WITH=1.
        ValuePair * pp = Alloc::allocPair();
        pp->left   = fn;
        pp->right  = nameV;
        pp->third  = lv;
        pairPostConstructBarrier(pp);  // Phase D
        Value app3; app3.mkPair(Tag::App3, pp);
        entries.emplace_back(sid, app3);
    }
    std::sort(entries.begin(), entries.end(),
        [](const auto & a, const auto & b) { return a.first < b.first; });
    Bindings * b = Alloc::allocBindings(static_cast<uint32_t>(entries.size()));
    V3_STATS_INC(attrsetsAllocated);
    for (size_t i = 0; i < entries.size(); ++i) {
        b->entries[i].name = entries[i].first;
        bindingsSetValue(b, static_cast<uint32_t>(i), entries[i].second);  // Phase D
    }
    out.mkAttrs(b);
}

/// builtins.trace msg val: print msg to stderr, return val unchanged.
// Forward decl — defined later in this TU.
nlohmann::json valueToJson(EvalState & state, const Value & v);
nlohmann::json valueToJsonWithContext(
    EvalState & state, const Value & v, nix::NixStringContext & context);

void primTrace(EvalState & state, Value * args, Value & out)
{
    Value v = args[0];
    // Fast paths for primitives.
    if (v.isString())   std::fprintf(stderr, "trace: %s\n", v.asString());
    else if (v.isInt()) std::fprintf(stderr, "trace: %lld\n", (long long)v.asInt());
    else if (v.isFloat()) std::fprintf(stderr, "trace: %g\n", v.asFloat());
    else if (v.isBool()) std::fprintf(stderr, "trace: %s\n", v.asInt() == 1 ? "true" : "false");
    else if (v.isNull()) std::fprintf(stderr, "trace: null\n");
    else if (v.isPath()) std::fprintf(stderr, "trace: %s\n", v.asPath());
    else {
        // FFI_KILL_TODO T1.3 (2026-06-01): v3-native trace printer.
        //
        // Was: routed through `v3ToTreeWalkerPublic` + `nix::ValuePrinter`
        // for shape-preserving output (preserves «thunk» / <LAMBDA> /
        // <PRIMOP> markers + cycle detection).
        //
        // Now: v3 has `printNixValueRich` (print.cc:514+) which produces
        // byte-equal output to TW's ValuePrinter — same shape markers,
        // same cycle handling, same derivation compact form.  Eliminates
        // the v3→TW bridge for trace.
        //
        // Falls back to valueToJson if rich printer throws (unexpected;
        // rich printer is non-throwing by design but defensive).
        try {
            std::stringstream ss;
            auto & symTab = ir::globalSymbolTable();
            std::set<const void *> seen;
            // Use the no-VM overload (Closure/Thunk print as <LAMBDA>/
            // «thunk» tokens without forcing — same as TW's lazy print).
            printNixValueRich(ss, v, symTab, seen);
            std::fprintf(stderr, "trace: %s\n", ss.str().c_str());
        } catch (...) {
            // Fallback: best-effort JSON dump if the rich printer fails.
            try {
                auto j = valueToJson(state, v);
                std::fprintf(stderr, "trace: %s\n", j.dump().c_str());
            } catch (...) {
                std::fprintf(stderr, "trace: <complex value>\n");
            }
        }
    }
    out = args[1];
}

/// builtins.traceVerbose: same as trace but only when --trace-verbose;
/// for v3 we treat it as plain trace (no flag plumbing yet).
void primTraceVerbose(EvalState & state, Value * args, Value & out)
{
    primTrace(state, args, out);
}

void primBaseNameOf(EvalState &, Value * args, Value & out)
{
    // Mirrors tree-walker's legacyBaseNameOf: at most ONE trailing
    // slash is stripped, so `baseNameOf "a/"` is "a" but
    // `baseNameOf "a//"` is "" (a 10-year-old quirk that
    // `eval-okay-baseNameOf.nix` pins down).
    std::string_view s;
    bool inputIsString = args[0].isString();
    if (inputIsString) s = args[0].asString();
    else if (args[0].isPath()) s = args[0].asPath();
    else typeError("baseNameOf", "string or path");
    if (s.empty()) { out = mkStringValueOwned(""); return; }
    size_t last = s.size() - 1;
    if (s[last] == '/' && last > 0) last -= 1;
    size_t pos = s.rfind('/', last);
    if (pos == std::string_view::npos) pos = 0;
    else pos += 1;
    out = mkStringValueOwned(std::string(s.substr(pos, last - pos + 1)));
    // #672 follow-up: when input was a String with context (e.g. the
    // interpolation of a derivation), TW propagates that context to
    // the output.  Without this, callers that derive a basename from
    // a store path lose the underlying drv reference.  Paths have no
    // context to propagate.
    if (inputIsString) {
        if (auto * raw = lookupStringContextEntries(args[0].asString())) {
            std::vector<std::string> copy = *raw;
            setStringContextEntries(out.asString(), std::move(copy));
        }
    }
}

void primDirOf(EvalState &, Value * args, Value & out)
{
    std::string s;
    bool isPathV = false;
    if (args[0].isString()) s = args[0].asString();
    else if (args[0].isPath()) { s = args[0].asPath(); isPathV = true; }
    else typeError("dirOf", "string or path");
    auto pos = s.find_last_of('/');
    std::string dir = (pos == std::string::npos) ? "." :
                      (pos == 0) ? "/" : s.substr(0, pos);
    if (isPathV) {
        Value v;
        // CRIT-4: arena allocation for long-lived path payload.
        char * buf = Alloc::allocChars(dir.size() + 1);
        std::memcpy(buf, dir.data(), dir.size()); buf[dir.size()] = '\0';
        v.mkPath(buf);
        out = v;
    } else {
        out = mkStringValueOwned(dir);
        // #672 follow-up: propagate input string context (same
        // reasoning as primBaseNameOf — Path inputs have no context).
        if (auto * raw = lookupStringContextEntries(args[0].asString())) {
            std::vector<std::string> copy = *raw;
            setStringContextEntries(out.asString(), std::move(copy));
        }
    }
}

void primPathExists(EvalState & state, Value * args, Value & out)
{
    std::string s;
    if (args[0].isString()) {
        // #741 Phase 4 measurement: path with non-empty context →
        // potential IFD (path may resolve to a derivation output).
        // TW's primPathExists doesn't realisePath, so v3 likely
        // matches — but the context presence is still the right
        // discriminator for "could need a build".
        auto * ctxEntries = lookupStringContextEntries(args[0].asString());
        if (ctxEntries && !ctxEntries->empty())
            ++allocStats().ifdProbeWithCtx[kIfdPathExists];
        s = args[0].asString();
    }
    else if (args[0].isPath()) s = args[0].asPath();
    else {
        // #692 — TW (libexpr/primops.cc:prim_pathExists) uses
        // `state.coerceToString(..., coerceMore=false, copyToStore=false)`
        // which produces `cannot coerce <type> to a string: <value>`
        // for non-string/non-path args.  Mirror that exact text by
        // delegating to the shared valueRepr-style format.
        const char * art = "a"; const char * name = "value";
        Tag t = args[0].tag();
        if (t == Tag::Int)        { art = "an"; name = "integer"; }
        else if (t == Tag::Float) { art = "a";  name = "float"; }
        else if (t == Tag::Bool)  { art = "a";  name = "Boolean"; }
        else if (t == Tag::Null)  { art = "";   name = "null"; }
        else if (t == Tag::List)  { art = "a";  name = "list"; }
        else if (t == Tag::Attrs) { art = "a";  name = "set"; }
        else if (t == Tag::Closure || t == Tag::PrimOp || t == Tag::PrimOpApp)
                                  { art = "a";  name = "function"; }
        // Render the value using a small inline helper that mirrors
        // TW's `ValuePrinter(state, v, errorPrintOptions)` for the
        // common cases (small attrsets/lists rendered fully; large
        // ones truncated).  Matches vm.cc:valueRepr behavior.
        auto val = [&](const Value & v) -> std::string {
            Tag tt = v.tag();
            if (tt == Tag::Int)   return std::to_string(v.asInt());
            if (tt == Tag::Float) { std::ostringstream os; os << v.asFloat(); return os.str(); }
            if (tt == Tag::Bool)  return v.asInt() == 1 ? "true" : "false";
            if (tt == Tag::Null)  return "null";
            if (tt == Tag::List) {
                if (!v.asList() || v.asList()->size == 0) return "[ ]";
                // Truncated for brevity at one-level (no recursion to
                // avoid pulling in vm.cc's valueRepr).
                return "[ ... ]";
            }
            if (tt == Tag::Attrs) {
                if (!v.asAttrs() || v.asAttrs()->size == 0) return "{ }";
                // Render small attrsets fully matching TW.
                auto * b = v.asAttrs();
                const auto & symTab = ir::globalSymbolTable();
                std::string out = "{ ";
                uint32_t n = b->size;
                uint32_t lim = n > 10 ? 10 : n;
                for (uint32_t i = 0; i < lim; ++i) {
                    uint32_t nameIdx = b->entries[i].name;
                    std::string nm = (nameIdx < symTab.size())
                        ? symTab[nameIdx] : std::string("<sym?>");
                    out += nm;
                    out += " = ";
                    // One-level only — leaf scalars rendered; deeper
                    // containers truncated to keep error message tight.
                    const Value & e = b->entries[i].value;
                    if (e.isInt())   out += std::to_string(e.asInt());
                    else if (e.isString() && e.asString()) {
                        out += "\""; out += e.asString(); out += "\"";
                    }
                    else if (e.isBool()) out += e.asInt() == 1 ? "true" : "false";
                    else if (e.isNull()) out += "null";
                    else if (e.isList())  out += "[ ... ]";
                    else if (e.isAttrs()) out += "{ ... }";
                    else                  out += "<...>";
                    out += "; ";
                }
                if (n > lim) out += "...; ";
                out += "}";
                return out;
            }
            return "<value>";
        }(args[0]);
        std::string msg = "cannot coerce ";
        if (*art) { msg += art; msg += ' '; }
        msg += name;
        msg += " to a string: ";
        msg += val;
        throw std::runtime_error(msg);
    }

    // REVIEW §1.7: route through nix::EvalState::realisePath when a TW
    // EvalState is available so pure-eval / restricted-eval modes can
    // refuse out-of-allowed-roots probes (security-relevant: a probe
    // that returns true/false reveals filesystem layout).  Tree-walker
    // catches RestrictedPathError and returns false; do the same.
    if (state.nixEvalState) {
        auto & ns = *state.nixEvalState;
        try {
            // Bridge to a TW Value so realisePath can use its existing
            // type dispatch (string vs path vs context) without us
            // duplicating the logic.
            nix::Value tw;
            if (args[0].isString()) tw.mkString(s, ns.mem);
            else                    tw.mkPath(nix::SourcePath(ns.rootFS, nix::CanonPath(s)), ns.mem);
            // mustBeDir mirrors tree-walker (primops.cc:2128) — trailing
            // slash forces full symlink resolution + dir check.
            bool mustBeDir =
                args[0].isString() && (s.ends_with("/") || s.ends_with("/."));
            auto symRes = mustBeDir
                ? nix::SymlinkResolution::Full
                : nix::SymlinkResolution::Ancestors;
            auto path = ns.realisePath(nix::noPos, tw, symRes);
            auto st = path.maybeLstat();
            bool exists = st && (!mustBeDir || st->type == nix::SourceAccessor::tDirectory);
            out = exists ? Value::vTrue : Value::vFalse;
            return;
        } catch (const nix::RestrictedPathError &) {
            out = Value::vFalse;
            return;
        } catch (...) {
            // Anything else: fall through to the pre-§1.7 best-effort
            // direct stat (e.g. when path conversion through TW fails
            // for a malformed input).
        }
    }
    // Fallback (no TW state): match tree-walker semantics -- a broken
    // symlink still "exists" for pathExists (lstat-shaped).
    std::error_code ec;
    auto stat = std::filesystem::symlink_status(s, ec);
    out = (!ec && stat.type() != std::filesystem::file_type::not_found)
        ? Value::vTrue : Value::vFalse;
}

void primSplitString(EvalState &, Value * args, Value & out)
{
    if (!args[0].isString() || !args[1].isString())
        typeError("splitString", "(separator, string)");
    std::string_view sep(args[0].asString());
    std::string_view s(args[1].asString());
    std::vector<Value> parts;
    if (sep.empty()) {
        // Empty separator: split into per-character strings.
        for (char c : s) {
            std::string single(1, c);
            parts.push_back(mkStringValueOwned(single));
        }
    } else {
        size_t pos = 0;
        while (pos <= s.size()) {
            size_t next = s.find(sep, pos);
            if (next == std::string_view::npos) {
                parts.push_back(mkStringValueOwned(std::string(s.substr(pos))));
                break;
            }
            parts.push_back(mkStringValueOwned(std::string(s.substr(pos, next - pos))));
            pos = next + sep.size();
        }
    }
    ListVec * lv = Alloc::allocList(static_cast<uint32_t>(parts.size()));
    V3_STATS_INC(listsAllocated);
    for (size_t i = 0; i < parts.size(); ++i) lv->elems[i] = parts[i];
    out.mkList(lv);
}

// C-23: forward-declare the value-comparison helper (defined later) so
// genericClosure's dedup can compare keys by VALUE, matching TW's
// std::map<Value*, CompareValues> — int/float numeric, throws on incomparable.
static bool valueLessHelper(VMState & vm, const Value & a, const Value & b);

/// builtins.genericClosure { startSet, operator } -- BFS closure of
/// startSet under operator.  Items are deduplicated by their "key" attr.
void primGenericClosure(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isAttrs() || !args[0].asAttrs())
        typeError("genericClosure", "attrset");
    SymbolId sStart = vmIntern(state, "startSet");
    SymbolId sOp    = vmIntern(state, "operator");
    SymbolId sKey   = vmIntern(state, "key");
    const Value * startVRaw = args[0].asAttrs()->lookup(sStart);
    const Value * opVRaw    = args[0].asAttrs()->lookup(sOp);
    if (!startVRaw || !opVRaw)
        typeError("genericClosure", "{ startSet, operator }");
    // Attrset entries are lazy thunks; force before structural use.
    Value startV = forceValue(*state.vm, *startVRaw);
    Value opV    = forceValue(*state.vm, *opVRaw);
    if (!startV.isList()) typeError("genericClosure", "startSet must be a list");

    // Tree-walker uses a FIFO queue.  Order matters because the lang
    // tests dedupe on first-encountered, so BFS vs DFS produces a
    // different surviving item per key.
    std::vector<Value> result;
    std::deque<Value> work;
    if (startV.asList()) {
        for (uint32_t i = 0; i < startV.asList()->size; ++i)
            work.push_back(startV.asList()->elems[i]);
    }
    // C-23 (CODEBASE_REVIEW_2026-06-11): dedup keys by VALUE, matching TW's
    // `std::map<Value*, Value*, CompareValues>` (libexpr/primops.cc:861).
    // CompareValues compares int/float NUMERICALLY and allows mixing them
    // (key `1` and key `1.0` dedup as equal), compares floats by value (no
    // 6-digit `std::to_string` collision), and THROWS on the first comparison
    // of an incomparable type (bool / attrset / mixed string-vs-int — its
    // `default:` arm).  v3 previously stringified keys: it rejected int/float
    // mixing up front, collided distinct floats, and accepted bool keys.
    // valueLessHelper reproduces CompareValues exactly (int/float numeric,
    // string/path/list, throw otherwise), so a std::set keyed by it matches
    // TW — including TW's subtlety that a SINGLE bool key is fine (no
    // comparison fires) while two bool keys throw on the second insert.
    // GC-safe: genericClosure's only allocation safe-point is the nested
    // callClosure below, and the major-GC safepoint fires ONLY at exitDepth==0
    // (the outermost dispatch loop), so no collection runs mid-BFS to disturb
    // these arena-referencing key copies.
    struct GenKeyCmp {
        VMState * vm;
        bool operator()(const Value & a, const Value & b) const {
            return valueLessHelper(*vm, a, b);
        }
    };
    std::set<Value, GenKeyCmp> seen{GenKeyCmp{state.vm}};
    auto forceKey = [&](Value & it) -> Value {
        it = forceValue(*state.vm, it);
        if (!it.isAttrs() || !it.asAttrs())
            // #693 — match TW's `expected a set but found <type>: <value>`.
            throw std::runtime_error(expectedTypeButFound("a set", it));
        const Value * kRaw = it.asAttrs()->lookup(sKey);
        // #693 — TW's `state.getAttr(state.s.key, ...)` raises this on absence.
        if (!kRaw) throw std::runtime_error("attribute 'key' missing");
        Value k = forceValue(*state.vm, *kRaw);
        // Defensive: a NaN float key has no consistent ordering (it would break
        // the set's strict-weak-ordering); reject it.  (TW would dedup NaN keys
        // as equal, but a NaN genericClosure key is absurd.)
        if (k.tag() == Tag::Float && std::isnan(k.asFloat()))
            throw std::runtime_error("NaN key is not orderable");
        return k;
    };

    while (!work.empty()) {
        Value it = work.front(); work.pop_front();
        Value key = forceKey(it);
        // valueLessHelper throws on the first incomparable comparison (bool /
        // mixed-type / attrset key) — matching TW's CompareValues.
        if (!seen.insert(key).second) continue;
        result.push_back(it);
        Value next = callClosure(*state.vm, opV, it);
        // V3-NATIVE / iterative-force discipline: callClosure returns
        // exactly what the closure body produced.  The OP_RETURN /
        // OP_TAIL_CALL paths do NOT eagerly force the return value (a
        // tail-call's last OP_CALL chain leaves whatever tag the inner
        // primop produced — Tag::List for our bytecode filter, but
        // Tag::Thunk/App if the call result happens to wrap a deferred
        // computation).  Tree-walker's `state.callFunction` returns
        // unforced too — TW callers force at the consumer side.  Match
        // that: force here before the shape check.
        if (__builtin_expect(next.tag() == Tag::Thunk
                             || next.isAppLike()
                             || next.tag() == Tag::Slot, 0))
            next = forceValue(*state.vm, next);
        if (!next.isList())
            // #693 — match TW's `expected a list but found ...` phrasing.
            throw std::runtime_error(expectedTypeButFound("a list", next));
        if (next.asList()) {
            for (uint32_t i = 0; i < next.asList()->size; ++i)
                work.push_back(next.asList()->elems[i]);
        }
    }

    ListVec * lv = Alloc::allocList(static_cast<uint32_t>(result.size()));
    V3_STATS_INC(listsAllocated);
    for (size_t i = 0; i < result.size(); ++i) lv->elems[i] = result[i];
    listPostConstructBarrier(lv);  // Phase D coverage (primGenericClosure; PhD-6)
    out.mkList(lv);
}

/// REVIEW §2.4: thread-local regex cache for primMatch / primSplit.
///
/// Without it, every `lib.versions.major` call (and every other regex
/// over a literal pattern) re-compiles the regex from scratch -- a hot
/// path in nixpkgs.  Cache up to 64 patterns LRU-evicted; std::regex
/// itself is reasonably small (~few hundred bytes per pattern).
/// Thread-local because std::regex isn't threadsafe to copy across
/// concurrent calls; per-thread caches sidestep that concern entirely.
static const std::regex & getCachedRegex(std::string_view pattern)
{
    struct Entry { std::string pat; std::regex re; };
    static thread_local std::list<Entry> lru;
    static thread_local std::unordered_map<std::string_view,
        std::list<Entry>::iterator> idx;
    static constexpr size_t kCap = 64;
    auto it = idx.find(pattern);
    if (it != idx.end()) {
        // Move to front (MRU).
        lru.splice(lru.begin(), lru, it->second);
        return it->second->re;
    }
    // Insert.  std::regex constructor throws on bad pattern; let it
    // propagate -- callers wrap in try/catch.
    lru.emplace_front(Entry{std::string(pattern),
        std::regex(std::string(pattern), std::regex::extended)});
    auto fresh = lru.begin();
    idx.emplace(std::string_view(fresh->pat), fresh);
    if (lru.size() > kCap) {
        auto old = std::prev(lru.end());
        idx.erase(std::string_view(old->pat));
        lru.pop_back();
    }
    return fresh->re;
}

/// builtins.match regex string -> list of captures or null on no-match.
/// Supports the standard regex syntax via std::regex (POSIX-ish).
void primMatch(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isString() || !args[1].isString())
        typeError("match", "(regex, string)");
    // #734: TW's prim_match (libexpr/primops.cc) calls forceStringNoCtx
    // on the regex only.  The subject is forceString(...) with a
    // context accumulator that is then discarded — context-carrying
    // subjects are permitted (typical for path-interpolated strings).
    requireNoStringContext(state, args[0], "match");
    try {
        // Match tree-walker: POSIX extended regex (`.` matches newline,
        // POSIX bracket classes like [[:alnum:]] work).
        const std::regex & re = getCachedRegex(args[0].asString());
        std::cmatch m;
        if (!std::regex_match(args[1].asString(), m, re)) {
            out = Value::vNull;
            return;
        }
        // m[0] is the entire match; captures are m[1..m.size()-1].
        size_t nGroups = m.size() > 0 ? m.size() - 1 : 0;
        ListVec * lv = Alloc::allocList(static_cast<uint32_t>(nGroups));
        V3_STATS_INC(listsAllocated);
        for (size_t i = 0; i < nGroups; ++i) {
            if (m[i + 1].matched)
                lv->elems[i] = mkStringValueOwned(m[i + 1].str());
            else
                lv->elems[i] = Value::vNull;
        }
        out.mkList(lv);
    } catch (const std::regex_error &) {
        // #689 — TW phrasing (libexpr/primops.cc): `invalid regular
        // expression '<regex>'`.  The std::regex_error message is
        // implementation-defined; TW just emits the pattern.
        throw std::runtime_error(
            std::string("invalid regular expression '")
            + (args[0].asString() ? args[0].asString() : "") + "'");
    }
}

/// builtins.split regex string -> list alternating strings and captures.
/// E.g. split "[ ]+" "hello  world" -> ["hello" [] "world"].
void primSplit(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isString() || !args[1].isString())
        typeError("split", "(regex, string)");
    // #734: TW's prim_split mirrors prim_match: forceStringNoCtx on the
    // regex only.  The subject is allowed to carry context.
    requireNoStringContext(state, args[0], "split");
    try {
        const std::regex & re = getCachedRegex(args[0].asString());
        std::string_view s(args[1].asString());
        std::vector<Value> parts;
        // REVIEW §1.8 note: std::cregex_iterator advances past zero-
        // length matches automatically (libc++ + libstdc++ both
        // implement the standard's `match_prev_avail / no_zero` shim
        // internally), and the empty pattern `""` is rejected by the
        // regex constructor before we get here.  No manual `pos += 1`
        // needed; verified across {`""`, `"a*"`, `"^"`, `"$"`, `"(?=)"`}.
        std::cregex_iterator it(s.data(), s.data() + s.size(), re);
        std::cregex_iterator end;
        size_t pos = 0;
        for (; it != end; ++it) {
            auto match = *it;
            // Add the literal piece between previous and this match.
            parts.push_back(mkStringValueOwned(std::string(s.substr(pos, match.position(0) - pos))));
            // Add the captured groups as a list.
            size_t nGroups = match.size() > 0 ? match.size() - 1 : 0;
            ListVec * caps = Alloc::allocList(static_cast<uint32_t>(nGroups));
            V3_STATS_INC(listsAllocated);
            for (size_t i = 0; i < nGroups; ++i) {
                if (match[i + 1].matched)
                    caps->elems[i] = mkStringValueOwned(match[i + 1].str());
                else
                    caps->elems[i] = Value::vNull;
            }
            Value capsV;
            capsV.mkList(caps);
            parts.push_back(capsV);
            pos = match.position(0) + match.length(0);
        }
        // Trailing piece.
        parts.push_back(mkStringValueOwned(std::string(s.substr(pos))));

        ListVec * lv = Alloc::allocList(static_cast<uint32_t>(parts.size()));
        V3_STATS_INC(listsAllocated);
        for (size_t i = 0; i < parts.size(); ++i) lv->elems[i] = parts[i];
        listPostConstructBarrier(lv);  // Phase D coverage (primSplit; PhD-6) — caps sublists
        out.mkList(lv);
    } catch (const std::regex_error &) {
        // #689 — TW phrasing (mirror of #689 fix in primMatch).
        throw std::runtime_error(
            std::string("invalid regular expression '")
            + (args[0].asString() ? args[0].asString() : "") + "'");
    }
}

/// Map nix algo string -> HashAlgorithm enum.
inline nix::HashAlgorithm parseHashAlgo(std::string_view a)
{
    if (a == "md5")    return nix::HashAlgorithm::MD5;
    if (a == "sha1")   return nix::HashAlgorithm::SHA1;
    if (a == "sha256") return nix::HashAlgorithm::SHA256;
    if (a == "sha512") return nix::HashAlgorithm::SHA512;
    if (a == "blake3") return nix::HashAlgorithm::BLAKE3;
    // #683 — match TW phrasing (libutil/hash.cc:53):
    // "unknown hash algorithm '<a>', expect 'blake3', 'md5', 'sha1',
    //  'sha256', or 'sha512'"
    throw std::runtime_error(
        "unknown hash algorithm '" + std::string(a)
        + "', expect 'blake3', 'md5', 'sha1', 'sha256', or 'sha512'");
}

/// builtins.hashString algo s -> hex string of the digest.  Backed by
/// nix::hashString (libutil) which uses libcrypto.
void primHashString(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isString() || !args[1].isString())
        typeError("hashString", "(algo, string)");
    // #734: TW's prim_hashString calls forceStringNoCtx on the algo
    // only.  The input string is forceString with a discarded context
    // accumulator — context-carrying inputs are permitted.
    requireNoStringContext(state, args[0], "hashString");
    auto algo = parseHashAlgo(args[0].asString());
    auto h = nix::hashString(algo, args[1].asString());
    out = mkStringValueOwned(h.to_string(nix::HashFormat::Base16, false));
}

void primHashFile(EvalState & state, Value * args, Value & out)
{
    // #693 — match TW's forceStringNoCtx-style phrasings.
    if (!args[0].isString())
        throw std::runtime_error(expectedTypeButFound("a string", args[0]));
    // #734: TW's prim_hashFile calls forceStringNoCtx on the algo arg.
    // The path arg may legitimately carry context (e.g. a CA path), so
    // do not require NoCtx there.
    requireNoStringContext(state, args[0], "hashFile");
    std::string path;
    if (args[1].isString())     path = args[1].asString();
    else if (args[1].isPath())  path = args[1].asPath();
    else throw std::runtime_error(expectedTypeButFound("a string", args[1]));
    auto algo = parseHashAlgo(args[0].asString());
    // #693 — TW raises `path 'X' does not exist` for missing files.
    if (!std::filesystem::exists(path))
        throw std::runtime_error("path '" + path + "' does not exist");
    auto h = nix::hashFile(algo, path);
    out = mkStringValueOwned(h.to_string(nix::HashFormat::Base16, false));
}

void primConvertHash(EvalState & state, Value * args, Value & out)
{
    // builtins.convertHash { hash; hashAlgo?; toHashFormat; } -> string
    // hashAlgo is optional when hash is in `algo:body` or SRI form.
    if (!args[0].isAttrs() || !args[0].asAttrs())
        typeError("convertHash", "attrset");
    SymbolId sHash = vmIntern(state, "hash");
    SymbolId sAlgo = vmIntern(state, "hashAlgo");
    SymbolId sFmt  = vmIntern(state, "toHashFormat");
    const Value * vhRaw = args[0].asAttrs()->lookup(sHash);
    const Value * vaRaw = args[0].asAttrs()->lookup(sAlgo);
    const Value * vfRaw = args[0].asAttrs()->lookup(sFmt);
    if (!vhRaw || !vfRaw)
        typeError("convertHash", "{ hash; hashAlgo?; toHashFormat; }");
    Value vh = forceValue(*state.vm, *vhRaw);
    Value vf = forceValue(*state.vm, *vfRaw);
    if (!vh.isString() || !vf.isString())
        typeError("convertHash", "{ hash; hashAlgo?; toHashFormat; }");

    nix::HashFormat fmt;
    std::string_view fs(vf.asString());
    if (fs == "base16")        fmt = nix::HashFormat::Base16;
    else if (fs == "nix32")    fmt = nix::HashFormat::Nix32;
    else if (fs == "base32")   fmt = nix::HashFormat::Nix32;  // alias
    else if (fs == "base64")   fmt = nix::HashFormat::Base64;
    else if (fs == "sri")      fmt = nix::HashFormat::SRI;
    // #693 — match TW phrasing (libexpr/primops.cc:convertHash).
    else throw std::runtime_error(
        "unknown hash format '" + std::string(fs)
        + "', expect 'base16', 'base32', 'base64', or 'sri'");

    nix::Hash parsed{nix::HashAlgorithm::SHA256}; // dummy default
    if (vaRaw) {
        Value va = forceValue(*state.vm, *vaRaw);
        if (!va.isString())
            typeError("convertHash", "{ hash; hashAlgo?; toHashFormat; }");
        parsed = nix::Hash::parseAny(vh.asString(), parseHashAlgo(va.asString()));
    } else {
        // No hashAlgo — infer from `algo:body` or SRI prefix.
        parsed = nix::Hash::parseAny(vh.asString(), std::nullopt);
    }
    out = mkStringValueOwned(parsed.to_string(fmt, false));
}

/// builtins.currentSystem: honour --system / settings.thisSystem via the FFI
/// leaf when a store/eval-state is wired (C-20); else fall back to host triple.
void primCurrentSystem(EvalState & state, Value *, Value & out)
{
    // C-20 (CODEBASE_REVIEW_2026-06-11): a baked-in host triple ignores any
    // `--system X` / `settings.thisSystem` override, diverging drvPaths from TW
    // for cross-system evaluation (`nix eval --system mips64-linux ...`).
    // Mirror TW's settings.getCurrentSystem() via the FFI leaf.
    if (state.nixEvalState) {
        out = mkStringValueOwned(ffi::currentSystem(*state.nixEvalState));
        return;
    }
    // Standalone (no eval-state wired): use a sensible host default.
#if defined(__APPLE__) && defined(__aarch64__)
    out = mkStringValueOwned("aarch64-darwin");
#elif defined(__APPLE__) && defined(__x86_64__)
    out = mkStringValueOwned("x86_64-darwin");
#elif defined(__linux__) && defined(__aarch64__)
    out = mkStringValueOwned("aarch64-linux");
#elif defined(__linux__) && defined(__x86_64__)
    out = mkStringValueOwned("x86_64-linux");
#else
    out = mkStringValueOwned("unknown-unknown");
#endif
}

void primCurrentTime(EvalState &, Value *, Value & out)
{
    out.mkInt(static_cast<int64_t>(std::time(nullptr)));
}

void primNixVersion(EvalState &, Value *, Value & out)
{
    // Match tree-walker's nixVersion (PACKAGE_VERSION).  nixpkgs/lib/
    // minfeatures.nix uses `compareVersions "2.18" builtins.nixVersion`
    // to decide whether to abort, so we need to surface the same
    // version string the tree-walker would.  Returning "v3-0.1"
    // (the previous v3 marker) caused nixpkgs to claim Nix 2.35 was
    // too old.
    out = mkStringValueOwned(ffi::nixVersion());
}

/// builtins.langVersion (REVIEW_2026-05-04 §6.1).  Mirrors tree-
/// walker's value at libexpr/primops.cc:5691.  Bumped when the
/// language adds a new feature (independent of primop additions).
void primLangVersion(EvalState &, Value *, Value & out)
{
    out.mkInt(6);
}

/// builtins.storeDir (REVIEW_2026-05-04 §6.1, F5).  Returns the
/// active store's directory.  Tree-walker reads `store->storeDir`
/// (libexpr/primops.cc:5669) so we mirror that.  Falls back to
/// `/nix/store` when v3 is running standalone (no nixEvalState
/// wired) so simple test harnesses don't crash.
void primStoreDir(EvalState & state, Value *, Value & out)
{
    if (state.nixEvalState) {
        // nix::EvalState::store is a `ref<Store>` (not a pointer);
        // it's always non-null when nixEvalState is wired.
        out = mkStringValueOwned(state.nixEvalState->store->storeDir);
    } else {
        out = mkStringValueOwned("/nix/store");
    }
}

/// builtins.readFile path -> string contents.
///
/// REVIEW §1.6: routes through state.realisePath for restricted-eval
/// path checks (matches tree-walker libexpr/primops.cc:2473), forwards
/// string-context entries from the input path Value (a path with
/// store-dependency context yields a string with the same context),
/// and rejects NUL bytes in the file content.
void primReadFile(EvalState & state, Value * args, Value & out)
{
    std::string path;
    if (args[0].isString()) {
        // #741 Phase 4 measurement: ctx-bearing readFile path goes
        // through realisePath below (the TW-wired branch), which
        // CAN trigger a build for un-realised DrvDeep/Built entries.
        auto * ctxEntries = lookupStringContextEntries(args[0].asString());
        if (ctxEntries && !ctxEntries->empty())
            ++allocStats().ifdProbeWithCtx[kIfdReadFile];
        path = args[0].asString();
    }
    else if (args[0].isPath()) path = args[0].asPath();
    else typeError("readFile", "string or path");

    // REVIEW §1.7-style routing: when a TW EvalState is wired, defer
    // path realisation to it so pure-eval / restricted-eval mode rules
    // apply.  Falls through to direct ifstream when no TW context
    // (v3-eval CLI standalone case).
    std::string content;
    if (state.nixEvalState) {
        auto & ns = *state.nixEvalState;
        // C-7(e) (CODEBASE_REVIEW_2026-06-11): route through realisePath and
        // let its errors propagate verbatim (mirrors primReadDir at the
        // `catch (...) { throw; }` sites below).  The former catch(...) fell
        // back to a plain ifstream on ANY realisation failure — which MASKS
        // IFD *build* failures: it either reads stale on-disk content or
        // reports a misleading "does not exist", instead of surfacing the
        // actual build error.  realisePath already yields TW-matching
        // "path '<p>' does not exist" errors for genuinely-missing paths, and
        // rethrows RestrictedPathError like TW's prim_readFile.
        nix::Value tw;
        if (args[0].isString()) tw.mkString(path, ns.mem);
        else                    tw.mkPath(nix::SourcePath(ns.rootFS, nix::CanonPath(path)), ns.mem);
        auto sp = ns.realisePath(nix::noPos, tw);
        content = sp.readFile();
    } else {
        std::ifstream f(path);
        if (!f) throw std::runtime_error("v3 primop readFile: cannot open " + path);
        std::stringstream ss;
        ss << f.rdbuf();
        content = ss.str();
    }

    // §1.6: reject NUL bytes -- nix strings are NUL-terminated, so a
    // file containing NUL would silently truncate at the first \0.
    if (content.find('\0') != std::string::npos)
        throw std::runtime_error("v3 primop readFile: file contains NUL byte");

    out = mkStringValueOwned(content);

    // #757c: mirror TW's prim_readFile (libexpr/primops.cc:2237).  TW
    // attaches context derived from the file's STORE REFERENCES
    // (filtered to those whose hash actually appears in the content),
    // NOT from the input path-string's context.  The forwarding
    // previously here (`copy from input string`) added the input
    // drv's context to the file content, breaking downstream
    // `builtins.fromJSON` / `builtins.hashString` / etc. that reject
    // context-bearing strings.  haskell-nix's lib/spdx/licenses.nix
    // hit this via `fromJSON (... readFile "${spdx-pkg}/licenses.json")`
    // — under v3-native callFlake, the spdx-pkg context leaked into
    // the JSON-string and fromJSON rejected it where TW does not.
    //
    // The new logic: if the path is in /nix/store, query its declared
    // references AND filter via PathRefScanSink (i.e. keep only refs
    // whose hash physically appears in the content).  Add those as
    // Opaque context entries.  This matches TW byte-for-byte.
    if (state.nixEvalState) {
        // Store-ref context attribution behind the FFI leaf (PathRefScanSink
        // + queryPathInfo live in ffi.cc; this TU stays out of
        // path-references.hh).  Returns the Opaque context-elem strings.
        auto ctx = ffi::storeRefsContextFor(*state.nixEvalState, path, content);
        if (!ctx.empty())
            setStringContextEntries(out.asString(), std::move(ctx));
    }
}

// (v3ToTreeWalker forward-decl retired — TW_VALUE_ERADICATION F4.)

/// builtins.readDir path -> attrset of name -> "regular"|"directory"|"symlink"|"unknown".
void primReadDir(EvalState & state, Value * args, Value & out)
{
    std::string path;
    if (args[0].isString()) {
        // #793 (2026-05-24): mirror TW's prim_readDir (libexpr/primops.cc:2542),
        // which routes args[0] through realisePath.  haskell.nix calls
        // readDir on string-with-context (`"${pkg}/some/dir"`) — the path
        // may reference a derivation output that must be realised before
        // we can scandir it.  Empty context = literal path = fast path,
        // matching primImport's discriminator.
        auto * ctxEntries = lookupStringContextEntries(args[0].asString());
        if (ctxEntries && !ctxEntries->empty() && state.nixEvalState) {
            ++allocStats().ifdProbeWithCtx[kIfdReadDir];
            auto & ns = *state.nixEvalState;
            // FFI_KILL_TODO T1.3-class strict-leaf (2026-06-01): inline
            // TW Value alloc + mkString for the string-with-ctx case.
            // Avoids v3ToTreeWalker full-recursion when we know the
            // value is a string + decoded context.  See primImport's
            // identical pattern at primops.cc:~8200 + the v3-native
            // outPath path right below for primReadDir attrset case.
            nix::Value * tw = ffi::allocValue(ns);
            nix::NixStringContext twCtx = decodeStringContext(*ctxEntries);
            tw->mkString(args[0].asString(), twCtx, ns.mem);
            try {
                auto resolved = ns.realisePath(nix::noPos, *tw);
                path = resolved.path.abs();
            } catch (...) {
                throw;  // surface TW's error verbatim
            }
        } else {
            path = args[0].asString();
        }
    }
    else if (args[0].isPath()) path = args[0].asPath();
    else if (args[0].isAttrs()) {
        // #793 (2026-05-24): derivation/attrset arg — mirror TW's
        // prim_readDir, which routes the arg through realisePath →
        // coerceToPath → coerceToString.  Surfaced in haskell.nix's
        // haskell-nix-example via `builtins.readDir <cleanSourceWith>`
        // where the arg is `{ outPath = <derivation-attrs>; filterPath; }`
        // — i.e. `outPath` is ITSELF an attrset (a derivation), so the
        // coercion must recurse (outPath → its outPath → … → store-path
        // string) and may also go via `__toString`.
        //
        // TW_VALUE_ERADICATION (2026-06-02): the deleted #875 bridge
        // previously provided this RECURSIVE coercion via TW's
        // coerceToString.  The prior native handler only resolved a
        // single-level STRING `outPath`, so an attrset-valued outPath
        // (the haskell.nix shape) fell through to a type error — an F4
        // regression on HNE.  `toStringCoerceCtx` is the V3-NATIVE
        // equivalent of TW's coerceToString: it tries `__toString`
        // (calling the v3 closure on `self`) first, then `outPath`,
        // recursing, and threads the string context needed for the IFD
        // realise.  This is the same coercion `builtins.toString` uses.
        if (!state.nixEvalState)
            typeError("readDir", "string or path");
        ++allocStats().ifdProbeWithCtx[kIfdReadDir];
        auto & ns = *state.nixEvalState;
        std::vector<std::string> rctx;
        std::string coerced =
            toStringCoerceCtx(state, args[0], rctx, /*copyPathsToStore=*/false);
        try {
            path = ffi::realisePath(ns, coerced, rctx);
        } catch (...) {
            throw;  // surface TW's error verbatim
        }
    }
    else typeError("readDir", "string or path");
    std::vector<std::pair<SymbolId, Value>> entries;
    // #692 — match TW phrasing for missing paths (libexpr/primops.cc:
    // readDir).  std::filesystem::directory_iterator throws a verbose
    // libc++ error; mirror TW's `path 'X' does not exist`.
    if (!std::filesystem::exists(path))
        throw std::runtime_error("path '" + path + "' does not exist");
    for (auto & ent : std::filesystem::directory_iterator(path)) {
        std::string name = ent.path().filename().string();
        // is_symlink must be checked first: is_directory()/is_regular_file()
        // follow symlinks, which would mis-report a `ldir -> dir` entry as
        // "directory" instead of "symlink" (matches tree-walker's lstat).
        const char * type =
            ent.is_symlink()      ? "symlink"   :
            ent.is_directory()    ? "directory" :
            ent.is_regular_file() ? "regular"   :
                                    "unknown";
        SymbolId k = vmIntern(state, name);
        entries.emplace_back(k, mkStringValueOwned(type));
    }
    std::sort(entries.begin(), entries.end(),
        [](auto & a, auto & b) { return a.first < b.first; });
    Bindings * b = Alloc::allocBindings(static_cast<uint32_t>(entries.size()));
    V3_STATS_INC(attrsetsAllocated);
    for (size_t i = 0; i < entries.size(); ++i) {
        b->entries[i].name  = entries[i].first;
        bindingsSetValue(b, static_cast<uint32_t>(i), entries[i].second);  // Phase D
    }
    out.mkAttrs(b);
}

/// builtins.parseDrvName "name-1.2.3" -> { name = "name"; version = "1.2.3"; }
///
/// Matches Nix's `DrvName` constructor: split at the first '-' that is
/// *not* followed by a letter.  This places a literal trailing '-' in
/// the version (e.g. "name-that-ends-with-dash--1.0" parses as
/// {name="name-that-ends-with-dash"; version="-1.0"}) — required by the
/// derivation-name lang test.
void primParseDrvName(EvalState & state, Value * args, Value & out)
{
    // #693 — match TW's forceStringNoCtx phrasing.
    if (!args[0].isString())
        throw std::runtime_error(expectedTypeButFound("a string", args[0]));
    // #674: TW's prim_parseDrvName uses forceStringNoCtx; v3 must
    // match the rejection so contexted derivation-name strings can't
    // sneak through (e.g. callers that accidentally pass `"${drv}"`).
    requireNoStringContext(state, args[0], "parseDrvName");
    std::string s(args[0].asString());
    size_t cut = std::string::npos;
    for (size_t i = 0; i + 1 < s.size(); ++i) {
        unsigned char nxt = static_cast<unsigned char>(s[i + 1]);
        if (s[i] == '-' && !std::isalpha(nxt)) {
            cut = i; break;
        }
    }
    std::string name, version;
    if (cut == std::string::npos) { name = s; version = ""; }
    else { name = s.substr(0, cut); version = s.substr(cut + 1); }

    SymbolId sName    = vmIntern(state, "name");
    SymbolId sVersion = vmIntern(state, "version");
    Bindings * b = Alloc::allocBindings(2);
    V3_STATS_INC(attrsetsAllocated);
    Value vn = mkStringValueOwned(name);
    Value vv = mkStringValueOwned(version);
    if (sName < sVersion) { bindingsSetEntry(b, 0, {sName, 0, vn}); bindingsSetEntry(b, 1, {sVersion, 0, vv}); }  // Phase D
    else                  { bindingsSetEntry(b, 0, {sVersion, 0, vv}); bindingsSetEntry(b, 1, {sName, 0, vn}); }
    out.mkAttrs(b);
}

/// builtins.groupBy keyFn list -> { key = [items with that key]; }
void primGroupBy(EvalState & state, Value * args, Value & out)
{
    if (!args[1].isList()) typeError("groupBy", "list");
    auto * src = args[1].asList();
    Value keyFn = args[0];
    std::unordered_map<std::string, std::vector<Value>> groups;
    if (src) {
        for (uint32_t i = 0; i < src->size; ++i) {
            Value k = callClosure(*state.vm, keyFn, src->elems[i]);
            // Bytecode-closure key fn can return Tag::Thunk wrapping a
            // string (see genericClosure rationale at #624) — force to
            // WHNF before the shape check.
            if (__builtin_expect(k.tag() == Tag::Thunk
                                 || k.isAppLike()
                                 || k.tag() == Tag::Slot, 0))
                k = forceValue(*state.vm, k);
            if (!k.isString()) typeError("groupBy", "key fn returning string");
            groups[std::string(k.asString())].push_back(src->elems[i]);
        }
    }
    std::vector<std::pair<SymbolId, Value>> entries;
    entries.reserve(groups.size());
    for (auto & [name, items] : groups) {
        ListVec * lv = Alloc::allocList(static_cast<uint32_t>(items.size()));
        V3_STATS_INC(listsAllocated);
        for (size_t i = 0; i < items.size(); ++i) lv->elems[i] = items[i];
        listPostConstructBarrier(lv);  // Phase D coverage (primGroupBy; PhD-6) — lazy src elems
        Value lstV;
        lstV.mkList(lv);
        entries.emplace_back(vmIntern(state, name), lstV);
    }
    std::sort(entries.begin(), entries.end(),
        [](auto & a, auto & b) { return a.first < b.first; });
    Bindings * b = Alloc::allocBindings(static_cast<uint32_t>(entries.size()));
    V3_STATS_INC(attrsetsAllocated);
    for (size_t i = 0; i < entries.size(); ++i) {
        b->entries[i].name  = entries[i].first;
        bindingsSetValue(b, static_cast<uint32_t>(i), entries[i].second);  // Phase D
    }
    out.mkAttrs(b);
}

void primReadFileType(EvalState &, Value * args, Value & out)
{
    std::string path;
    if (args[0].isString()) path = args[0].asString();
    else if (args[0].isPath()) path = args[0].asPath();
    else typeError("readFileType", "string or path");
    std::error_code ec;
    auto status = std::filesystem::symlink_status(path, ec);
    // #693 — match TW phrasing for missing-path errors.
    if (ec) throw std::runtime_error("path '" + path + "' does not exist");
    const char * t;
    if      (std::filesystem::is_symlink(status))   t = "symlink";
    else if (std::filesystem::is_directory(status)) t = "directory";
    else if (std::filesystem::is_regular_file(status)) t = "regular";
    else                                            t = "unknown";
    out = mkStringValueOwned(t);
}

void primAddErrorContext(EvalState & state, Value * args, Value & out)
{
    // REVIEW_2026-05-04 F3 / B-3 / §6.2: was a no-op stub that silently
    // dropped the prefix.  nixpkgs `lib/modules.nix:270` calls this on
    // every module evaluation -- without context, NixOS error messages
    // lose their breadcrumb chain entirely (silent wrong-output).
    //
    // Mirror tree-walker's `prim_addErrorContext` (libexpr/primops.cc:1170):
    // force args[1] in a try/catch.  On exception, coerce args[0] to a
    // string and PREPEND it to the exception message before rethrowing.
    // The first arg (the message) is lazy -- only forced on the error
    // path -- so success-path code doesn't pay the coercion cost.
    //
    // Type preservation: if BlackholeError fires, rethrow as
    // BlackholeError so the F4 typed-fallback machinery still routes.
    // Other exceptions become std::runtime_error with the prepended
    // message (we can't generally clone arbitrary exception types).
    static const bool dbg = std::getenv("V3_DBG_ADD_ERR_CTX") != nullptr;
    auto coerceMsg = [&]() -> std::string {
        try { return toStringCoerce(state, args[0]); }
        catch (...) { return "<addErrorContext: error coercing message>"; }
    };
    // #684 — preserve the exception's RUNTIME TYPE when re-throwing.
    // Pre-fix v3 collapsed every caught exception into `std::runtime_error`,
    // breaking `tryEval` (which catches only `AssertionError`-derived) for
    // wrapped `throw`/`assert` errors: `tryEval (addErrorContext "ctx"
    // (throw "x"))` returned the error instead of `{ success = false;
    // value = false; }`.  Match TW's `e.addTrace(...)` semantics (which
    // mutates and rethrows the same exception) by preserving the type.
    //
    // Message order: TW prints `error: <orig>` at the BOTTOM with trace
    // frames above (`… <ctx>`).  v3 doesn't have the frame machinery
    // yet — emit `<orig>\n… <ctx>` so the standard one-line error grep
    // sees the original message (matching TW's bottom-line behavior).
    try {
        Value v = forceValue(*state.vm, args[1]);
        out = v;
    } catch (const ThrownError & ex) {
        auto msg = coerceMsg();
        if (dbg) std::fprintf(stderr, "v3 addErrorContext (ThrownError): %s\n", msg.c_str());
        throw ThrownError(std::string(ex.what()) + "\n… " + msg);
    } catch (const AssertionError & ex) {
        auto msg = coerceMsg();
        if (dbg) std::fprintf(stderr, "v3 addErrorContext (AssertionError): %s\n", msg.c_str());
        throw AssertionError(std::string(ex.what()) + "\n… " + msg);
    } catch (const AbortError & ex) {
        auto msg = coerceMsg();
        if (dbg) std::fprintf(stderr, "v3 addErrorContext (AbortError): %s\n", msg.c_str());
        throw AbortError(std::string(ex.what()) + "\n… " + msg);
    } catch (const BlackholeError & ex) {
        auto msg = coerceMsg();
        if (dbg) std::fprintf(stderr, "v3 addErrorContext (BlackholeError): %s\n", msg.c_str());
        throw BlackholeError(std::string(ex.what()) + "\n… " + msg);
    } catch (const std::exception & ex) {
        auto msg = coerceMsg();
        if (dbg) std::fprintf(stderr, "v3 addErrorContext: %s\n", msg.c_str());
        throw std::runtime_error(std::string(ex.what()) + "\n… " + msg);
    }
}

/// Construct a v3-side derivation result that mirrors what tree-walker
/// produces from corepkgs/derivation.nix.  The outer wrapper:
///   1. calls derivationStrict to synthesize the per-output paths
///   2. picks `outputs[0]` (default "out")
///   3. returns that output's attrset, populated with `commonAttrs //
///      { outPath; drvPath; type = "derivation"; outputName; }`.
/// `commonAttrs` = drvAttrs // listToAttrs(outputs) // { all; drvAttrs; }.
void primDerivation(EvalState & state, Value * args, Value & out);

/// Forward decls for the v3 closure bridging — used so a v3 closure
/// passed to the tree-walker (e.g. as a `filter` function on
/// `builtins.path`) becomes a real callable on the tree-walker side.
/// WC-19+: closure bridge mirrors attr/list bridges — also stores
/// a fallback Expr so primV3CallBridge1/2 can re-run the outer
/// Expr through tree-walker on a v3-only blackhole.
///
/// #875 Stage 0 (2026-05-29): per-entry access tracking for the
/// weak-bridge-eviction measurement spike.  `lastAccessGen` /
/// `accessCount` are bumped at every dispatch site (primV3CallBridge1,
/// primV3ForceAttr, primV3ForceListElem, tryUnwrapBridge1Closure,
/// tryDispatchBridge1Direct).  Cost: 12 B/entry × ~10K M5 bridges =
/// ~120 KB total — negligible against the bridge tables' multi-GB
/// transitive retention.  See `lode/WEAK_BRIDGE_EVICTION_DESIGN_2026-05-29.md`.
///
/// #875 Stage 1 (2026-05-29): `evicted` flag.  When set, the entry's
/// `v3Value` has been reset to Value{} (zero-default; releases
/// transitive retention to Boehm GC).  Dispatch routes through the
/// existing BlackholeError fallback path which re-runs `fallbackExpr`
/// via tree-walker.  Eviction is opt-in via `NIX_V3_WEAK_BRIDGES=1`.
// (#875 bridge tables [v3BridgeClosures/Attrs/Lists] + entry structs +
//  eviction/revive/sweep + access counters + gates retired —
//  TW_VALUE_ERADICATION F4, 2026-06-02; tables never populated post-F4.)


// #705 walkV3BridgeRoots lives below the anonymous-namespace close
// so the linker can see it.  See the function-body comment there.
// #705 walkImportCacheRoots_inAnon — moved after importCache()
// definition (was here at line 3581 before forward-declare was
// needed).

/// #493: side-table mapping sentinel `nix::Env *` (held in
/// `Value::lambda().env` of bridged TW lambdas) to the handle in
/// `v3BridgeClosures()` of the underlying v3 Closure (with its
/// captured upvalues).
///
/// When `v3ToTreeWalker` bridges a v3 Tag::Closure with hasFormals=true
/// to TW, instead of refusing (the pre-#493 behaviour) it constructs a
/// real `Tag::tLambda` whose `lambda.fun` points at the original
/// `nix::ExprLambda *` (recovered from `LambdaDescriptor::astLambda`)
/// and whose `lambda.env` is a freshly-allocated sentinel Env keyed
/// here.  TW's `autoCallFunction` then introspects formals via
/// `lambda.fun->getFormals()` and dispatches via `callFunction` -- the
/// v3 call hook detects the sentinel env, recovers the v3 Closure, and
/// runs the body in v3 with the original captured upvalues.
///
/// Pointer keys are stable: the sentinel Env is GC-allocated by
/// `EvalMemory::allocEnv` and held alive by the closure value
/// reference.  Boehm scans the key set indirectly via the `Closure*`
/// in `v3BridgeClosures` (traceable_allocator there).  This map is
/// non-traceable but values are size_t, not pointers, so no roots
/// needed for the values; the keys (Env*) are held by the bridged TW
/// lambda Value which is itself rooted by its consumer.
// (v3FormalsLambdaBridges side-table retired — TW_VALUE_ERADICATION F4, 2026-06-02.)

// (treeWalkerToV3 forward-decls retired — TW_VALUE_ERADICATION F4.)

// Phase 1.6 poll counter for native-C++ helpers that recurse outside
// the dispatch loop.  Bumped by the helper entries; periodic
// checkLimits() throws on cap exceed.  Threshold matches the
// dispatch loop's kPollInterval (10 000) so the polling overhead
// is consistent across VM and FFI code paths.
//
// Without this site, a runaway treeWalkerToV3 on a cyclic
// derivation graph hangs forever despite NIX_V3_MAX_WALL_TIME being
// set — the VM dispatch loop is stalled inside the FFI helper and
// can't reach its own poll.

// (v3ToTreeWalker forward-decl retired — TW_VALUE_ERADICATION F4.)

/// #455: anon-namespace forwarder so v3ToTreeWalker inside this
/// anonymous namespace can read the eager-bridge thread-local.  The
/// real definition + push/pop helpers live after the anon closes
/// (near the public bridge entry points), at file scope.  This
/// forward-decl is at file scope to avoid the anon-namespace name-
/// lookup quirk that would resolve to an anon-internal symbol.
///
/// #452 / Phase C: same trick for the shallow-TW-attrs-bridge flag.
/// When set, treeWalkerToV3's nAttrs case wraps each TW entry in a
/// v3 Bridge thunk (Tag::Thunk) instead of deeply converting.  This
/// matches TW's per-formal lazy semantics: a formals lambda body
/// only forces the entries it actually references, so blackholes
/// on mid-construction entries (NixOS module fix-points' `config`)
/// don't trip until the body would have hit them in TW too.
} } // close anon + nix::v3 to declare at file scope
// `shallowTWAttrsBridge` forward declaration retired 2026-05-18 —
// treeWalkerToV3 is now always shallow (see PROFILE_HELLO_NAME_2026-05-18.md).
namespace nix::v3 { namespace {

// #453 Phase D: bridge-primop call counters.  Atomics keep them off
// the hot-path lock; dumped from dumpPrimOpStats() at process exit
// when NIX_V3_PRIMOP_DUMP=1.  These fire when TW calls back into v3
// via the bridge primops registered in TW's primop table.  Hot
// counts here mean v3 is leaking across the cutover; reducing them
// is the Phase D goal.
// (g_bridgeCallBridge1Calls/g_bridgeForceAttrCalls counters retired — TW_VALUE_ERADICATION F4, 2026-06-02.)

// #458 step 2: shared depth counter and limit between
// primV3CallBridge1 (the legacy TW primop) and tryDispatchBridge1Direct
// (the #458 step 2 shortcut).  Without sharing they form a ping-pong:
// shortcut at depth N declines, TW dispatches primV3CallBridge1 with
// its own counter at 0, that re-enters shortcut, etc.  One thread-
// local counter for both paths so the depth ceiling actually fires
// regardless of which path is currently executing.
} // close anon ns

// (walkV3BridgeRoots/clearV3BridgesForDiag/v3BridgeTableSizes/v3BridgeUniquePtrCounts/forEachV3BridgeEntry/dumpBridgeAccessDistribution/bridge1DepthCounter/bridge1MaxDepth retired — TW_VALUE_ERADICATION F4, 2026-06-02.)
namespace {

    // (primV3CallBridge1 / primV3ForceAttr(Inner) / v3ToTreeWalker /
    //  treeWalkerToV3 retired — TW_VALUE_ERADICATION F4, 2026-06-02.)

// BR-3.1: pre-interned v3 SymbolIds for the attribute names that
// derivationStrict examines on every call.  Mirror's tree-walker's
// `EvalState::s` (eval.hh:247).  Lazy-initialised on first reference so
// the global symbol table has had a chance to come up; thread-safe via
// static-init (Magic Statics).  Read via `drvStrictSymbols()`.
struct DrvStrictSymbols {
    // Required / common.
    SymbolId name;
    SymbolId system;
    SymbolId builder;
    SymbolId args;
    SymbolId outputs;
    // Output hash / fixed-output triggers.
    SymbolId outputHash;
    SymbolId outputHashAlgo;
    SymbolId outputHashMode;
    // Phase-B/C/D triggers (we detect these at fall-back time).
    SymbolId structuredAttrs;     // __structuredAttrs
    SymbolId contentAddressed;    // __contentAddressed
    SymbolId impure;              // __impure
    SymbolId ignoreNulls;         // __ignoreNulls
    SymbolId json;                // __json (legacy)
    // Disallowed-with-structuredAttrs (warnings only; we still need to
    // recognise them).
    SymbolId allowedReferences;
    SymbolId allowedRequisites;
    SymbolId disallowedReferences;
    SymbolId disallowedRequisites;
    SymbolId maxSize;
    SymbolId maxClosureSize;
    // Result-attrset names (built up at the tail of the native path).
    SymbolId outPath;
    SymbolId drvPath;
    SymbolId type;
    // coerceToString helpers — these come from inside the IR's
    // `__toString` / `outPath` fall-back path.
    SymbolId toString;            // __toString
    SymbolId functor;             // __functor
};

static const DrvStrictSymbols & drvStrictSymbols()
{
    static const DrvStrictSymbols s = {
        .name                = ir::globalInternSymbol("name"),
        .system              = ir::globalInternSymbol("system"),
        .builder             = ir::globalInternSymbol("builder"),
        .args                = ir::globalInternSymbol("args"),
        .outputs             = ir::globalInternSymbol("outputs"),
        .outputHash          = ir::globalInternSymbol("outputHash"),
        .outputHashAlgo      = ir::globalInternSymbol("outputHashAlgo"),
        .outputHashMode      = ir::globalInternSymbol("outputHashMode"),
        .structuredAttrs     = ir::globalInternSymbol("__structuredAttrs"),
        .contentAddressed    = ir::globalInternSymbol("__contentAddressed"),
        .impure              = ir::globalInternSymbol("__impure"),
        .ignoreNulls         = ir::globalInternSymbol("__ignoreNulls"),
        .json                = ir::globalInternSymbol("__json"),
        .allowedReferences   = ir::globalInternSymbol("allowedReferences"),
        .allowedRequisites   = ir::globalInternSymbol("allowedRequisites"),
        .disallowedReferences  = ir::globalInternSymbol("disallowedReferences"),
        .disallowedRequisites  = ir::globalInternSymbol("disallowedRequisites"),
        .maxSize             = ir::globalInternSymbol("maxSize"),
        .maxClosureSize      = ir::globalInternSymbol("maxClosureSize"),
        .outPath             = ir::globalInternSymbol("outPath"),
        .drvPath             = ir::globalInternSymbol("drvPath"),
        .type                = ir::globalInternSymbol("type"),
        .toString            = ir::globalInternSymbol("__toString"),
        .functor             = ir::globalInternSymbol("__functor"),
    };
    return s;
}

// BR-3.2: v3 coerceToString with NixStringContext.
//
// Mirrors EvalState::coerceToString (eval.cc:2840) for the subset
// of cases the BR-3 native derivationStrict needs:
//   - copyToStore = true   (paths get fetched into the store and
//                            an Opaque context entry is added)
//   - canonicalizePath = true
//   - coerceMore = true    (bool/int/float/null/list also coerce)
//
// Caller threads in a `nix::NixStringContext &` accumulator: every
// string with side-table context contributes its entries; every
// path-coerce inserts an Opaque entry.  The accumulated context is
// what derivationStrict turns into drv.inputDrvs / drv.inputSrcs.
//
// Throws std::runtime_error on values that can't be coerced
// (closures without `__toString`, primops, etc.).  The Phase A
// scaffold catches these and falls back to the bridge.
//
// `errorCtx` is currently unused — Phase E (BR-3.13) will wire it
// into nicer Nix-style traces.  Keeping the parameter so the
// signature won't churn when error parity lands.
static std::string v3CoerceToString(
    EvalState & state,
    Value & v,
    nix::NixStringContext & context,
    std::string_view /*errorCtx*/);

// Helper: for an attrset that has `__toString` (a 1-arg function
// applied to the attrset itself producing a string), call it and
// coerce the result.  Returns std::nullopt if no __toString.
//
// Implements TW's `coerceToString` __toString branch
// (libexpr/eval.cc:2865-2870): call `__toString self` (where self is
// the attrset itself), then recursively coerce the result.  __toString
// is typically a Lambda / Closure but may also be a primop or a
// PrimOpApp partial application; callClosure handles all three.
static std::optional<std::string> v3TryAttrsToString(
    EvalState & state,
    Value & v,
    nix::NixStringContext & context,
    std::string_view errorCtx)
{
    const auto & sym = drvStrictSymbols();
    if (!v.isAttrs() || !v.asAttrs()) return std::nullopt;
    const Value * tsRaw = v.asAttrs()->lookup(sym.toString);
    if (!tsRaw) return std::nullopt;
    Value tsFn = forceValue(*state.vm, *tsRaw);
    // Only attempt the call for callable shapes — TW's coerceToString
    // doesn't error on a non-callable __toString, it just falls
    // through to the outPath branch.  Match that.
    if (!(tsFn.isClosure() || tsFn.isPrimOp()
          || tsFn.tag() == Tag::PrimOpApp))
        return std::nullopt;
    Value result = callClosure(*state.vm, tsFn, v);
    Value forced = forceValue(*state.vm, result);
    return v3CoerceToString(state, forced, context, errorCtx);
}

static std::string v3CoerceToString(
    EvalState & state,
    Value & v,
    nix::NixStringContext & context,
    std::string_view errorCtx)
{
    v = forceValue(*state.vm, v);

    if (v.isString()) {
        // Forward any side-table context from the v3 string into the
        // accumulator.  Strings without context (most string
        // literals) skip the lookup entirely.
        const char * buf = v.asString() ? v.asString() : "";
        if (auto * raw = lookupStringContextEntries(buf)) {
            for (auto & e : *raw)
                v3InsertContextToken(context, e, "v3CoerceToString");  // C-7(d)
        }
        return std::string(buf);
    }

    if (v.isPath()) {
        if (!state.nixEvalState) {
            throw std::runtime_error(
                "v3 BR-3 coerceToString: path coerce requires nixEvalState");
        }
        auto & ns = *state.nixEvalState;
        nix::SourcePath sp(ns.rootFS,
            nix::CanonPath(v.asPath() ? v.asPath() : ""));
        // copyPathToStore inserts the Opaque context entry on `context`
        // for us (eval.cc:2961).
        nix::StorePath dst = ns.copyPathToStore(context, sp);
        return ns.store->printStorePath(dst);
    }

    if (v.isAttrs()) {
        const auto & sym = drvStrictSymbols();
        // Try __toString first.  If absent, fall through to outPath.
        if (v.asAttrs() && v.asAttrs()->lookup(sym.toString)) {
            if (auto s = v3TryAttrsToString(state, v, context, errorCtx))
                return std::move(*s);
        }
        // outPath fallback — common case for derivations and any
        // attrset that string-coerces to its primary output.
        if (v.asAttrs()) {
            if (auto * outV = v.asAttrs()->lookup(sym.outPath)) {
                Value forced = forceValue(*state.vm, *outV);
                return v3CoerceToString(state, forced, context, errorCtx);
            }
        }
        // Include the attrset's keys in the error so callers (and
        // V3_DRV_DEBUG fallback diagnostics) can see WHICH attrset
        // failed coercion.  Truncate to first 12 keys to avoid log
        // bloat on huge attrsets.
        std::string msg = "v3 BR-3 coerceToString: attrset has neither "
                          "__toString nor outPath; keys=[";
        if (v.asAttrs()) {
            const auto & symTab = ir::globalSymbolTable();
            uint32_t lim = std::min<uint32_t>(v.asAttrs()->size, 12u);
            for (uint32_t i = 0; i < lim; ++i) {
                SymbolId sid = v.asAttrs()->entries[i].name;
                if (i) msg += ",";
                msg += (sid < symTab.size())
                    ? symTab[sid] : std::string("<sid?>");
            }
            if (v.asAttrs()->size > lim) msg += ",...";
        } else msg += "<no bindings>";
        msg += "]";
        throw std::runtime_error(msg);
    }

    // coerceMore = true cases (matching tree-walker's behaviour for
    // derivationStrict's per-attr coerce):
    if (v.isBool()) {
        return v.asInt() == 1 ? std::string("1") : std::string("");
    }
    if (v.isInt()) {
        return std::to_string(v.asInt());
    }
    if (v.tag() == Tag::Float) {
        // Match tree-walker's std::to_string(double).
        return std::to_string(v.asFloat());
    }
    if (v.tag() == Tag::Null) {
        return std::string("");
    }
    if (v.isList()) {
        std::string out;
        auto * lv = v.asList();
        if (!lv) return out;
        for (uint32_t i = 0; i < lv->size; ++i) {
            Value el = forceValue(*state.vm, lv->elems[i]);
            // Match tree-walker's "no separator before / after empty
            // sublist" quirk (eval.cc:2924).  Compute element text
            // first; only emit a separator if both this element and
            // the next non-final element are non-empty-list-shaped.
            out += v3CoerceToString(state, el, context, errorCtx);
            if (i + 1 < lv->size) {
                bool elIsEmptyList = el.isList()
                    && (!el.asList() || el.asList()->size == 0);
                if (!elIsEmptyList) out += ' ';
            }
        }
        return out;
    }

    // Closures / primops / thunks (already forced above) / external —
    // can't coerce.  Throws into the scaffold's fall-back.
    throw std::runtime_error(
        "v3 BR-3 coerceToString: cannot coerce value of unsupported tag");
}

// BR-3.4: lexicographic attr iteration helper.  Returns visible attrs sorted
// by name STRING, not SymbolId.
//
// Why this matters: v3 Bindings store entries sorted by SymbolId
// (interning order at parse time), but tree-walker's
// `attrs->lexicographicOrder(state.symbols)` sorts by the resolved
// name string.  Tree-walker's derivationStrict iterates in that
// order, and the resulting drv-hash depends on the order of
// `drv.env` insertions — so to match drvPath byte-for-byte we MUST
// iterate by name string.
//
// This is the single biggest correctness landmine in the whole port:
// if the order diverges by one swap, every dependent /nix/store path
// changes silently.
struct LexicographicAttrRef {
    SymbolId name;
    Value value;
    Value * slot;
    const Bindings * owner;
    const Bindings::Entry * entry;
};

static std::vector<LexicographicAttrRef> lexicographicAttrEntries(Bindings * b)
{
    std::vector<LexicographicAttrRef> order;
    if (!b) return order;
    order.reserve(b->countDistinct());
    if (b->isChain() || b->isHamt()) {   // #149: Hamt has no entries[] — use forEach
        forEachEntryRefNoMapAttrsRealize(b, [&](const Bindings * owner,
                                                const Bindings::Entry & e) {
            order.push_back({e.name, e.value, nullptr, owner, &e});
        });
    } else {
        for (uint32_t i = 0; i < b->size; ++i)
            order.push_back({b->entries[i].name, b->entries[i].value,
                             &b->entries[i].value, b, &b->entries[i]});
    }
    const auto & symTab = ir::globalSymbolTable();
    auto nameOf = [&](const LexicographicAttrRef & ref) -> std::string_view {
        return ref.name < symTab.size() ? std::string_view(symTab[ref.name])
                                        : std::string_view{};
    };
    std::sort(order.begin(), order.end(),
        [&](const LexicographicAttrRef & a, const LexicographicAttrRef & bRef) {
            return nameOf(a) < nameOf(bRef);
        });
    return order;
}

// BR-3.3 + 3.10 + 3.11 + 3.12: detect-fall-back predicate.
// Returns true when args[0]'s shape is one the native path can
// handle:
//   - Phase A: deferred-output simple case.
//   - Phase B (BR-3.10): fixed-output.
//   - Phase C (BR-3.11): content-addressed / impure.
//   - Phase D (BR-3.12): __structuredAttrs (JSON-attr derivations).
//
// All currently-known shapes are now in scope.  This function
// exists primarily to early-exit cheaply on non-attrset args.
static bool isSimpleDerivationAttrs(const Bindings * b)
{
    return b != nullptr;
}

// BR-3.9: native-vs-fallback counters.  Reported on process exit
// when V3_DRV_STATS=1 is set; useful to confirm the native path
// is actually firing on real workloads.
static uint64_t & drvNativeHits()      { static uint64_t v = 0; return v; }
static uint64_t & drvNativeFallbacks() { static uint64_t v = 0; return v; }

namespace {
struct DrvStatsAtExit {
    ~DrvStatsAtExit() {
        static const bool s_drvStats =
            std::getenv("V3_DRV_STATS") != nullptr;
        if (s_drvStats)
            std::fprintf(stderr,
                "v3 drv final stats: native=%llu fallback=%llu\n",
                (unsigned long long)drvNativeHits(),
                (unsigned long long)drvNativeFallbacks());
    }
};
DrvStatsAtExit _drvStatsAtExit;
}  // namespace

// BR-3.5 — Phase A native derivationStrict: builds nix::Derivation
// directly from a v3 attrset, skipping the v3↔tree-walker bridge.
// Throws std::runtime_error on any unsupported shape; the caller
// catches and falls back to the bridge.
//
// Phase A coverage: deferred-output simple case only (no
// outputHash, no __structuredAttrs, no __contentAddressed, no
// __impure — gated by isSimpleDerivationAttrs).  Phases B–D
// extend this incrementally.
//
// Builds drv.{name,builder,platform,args,env,outputs} from a v3
// attrset, processes the accumulated NixStringContext into
// drv.inputDrvs / drv.inputSrcs (BR-3.6), then writes the
// derivation (BR-3.7) and constructs the v3 result attrset.
static void primDerivationStrictNative(
    EvalState & state, Value * args, Value & out);

/// Construct a "fake" derivation attrset.  Real `derivation` interfaces
/// with the store; we accept the input attrset and tag it with a
/// synthetic `outPath` so code that just reads outPath works.  Useful
/// for testing nix expressions that build up drv attrsets without
/// actually realizing them.
void primDerivationStrict(EvalState & state, Value * args, Value & out)
{
    // Phase 13.5: V3_DRV_CALLER=1 dumps the v3 frame stack at every
    // primDerivationStrict call so we can see *who* is invoking it
    // and verify whether 100x more calls trace to the same caller
    // (v3 callsite issue) or to many distinct callers (legit graph).
    {
        static const bool s_dbg_caller =
            std::getenv("V3_DRV_CALLER") != nullptr;
        if (__builtin_expect(s_dbg_caller, 0)) {
            static std::atomic<uint64_t> seq{0};
            auto n = seq.fetch_add(1);
            // Only dump every Nth call to avoid log explosion.
            static const uint64_t stride = []() -> uint64_t {
                const char * e = std::getenv("V3_DRV_CALLER_STRIDE");
                return e ? std::strtoull(e, nullptr, 10) : 100;
            }();
            if (stride && (n % stride) == 0 && state.vm) {
                std::fprintf(stderr, "v3 DRV_CALLER[%llu] (frames=%zu):\n",
                    (unsigned long long)n, state.vm->frames.size());
                size_t lim = state.vm->frames.size();
                for (size_t i = lim; i > 0 && i + 8 > lim; --i) {
                    const auto & fr = state.vm->frames[i - 1];
                    const LambdaDescriptor * d = nullptr;
                    if (fr.thunk) d = reinterpret_cast<const LambdaDescriptor *>(fr.thunk->suspended.desc);
                    else if (fr.closure) d = fr.closure->desc;
                    const char * nm = (d && !d->name.empty()) ? d->name.c_str() : "<anon>";
                    std::fprintf(stderr, "  frame[%zu] %s codeOff=%u ip=%u\n",
                        i - 1, nm, d ? d->codeOffset : 0, fr.ip);
                }
            }
        }
    }

    // Phase 13.4: per-derivation call profiler.  Key = name + bindings
    // ptr — different ptrs for the same name means fresh args attrsets,
    // i.e. the scope/callPackage chain is *not* sharing the let-bound
    // drv across repeated accesses.  Reports periodically (every 1000
    // calls) so runaway probes still produce visible progress before
    // a ulimit kill.  Off by default; set V3_DRV_PER_DRV=1.
    {
        static const bool s_dbg_drv_perdrv =
            std::getenv("V3_DRV_PER_DRV") != nullptr;
        if (__builtin_expect(s_dbg_drv_perdrv, 0)) {
            static std::mutex mtx;
            static std::unordered_map<std::string, uint64_t> counts;
            const auto & syms = drvStrictSymbols();
            std::string drvName = "<no-name>";
            const void * bindingsPtr = nullptr;
            if (args[0].isAttrs() && args[0].asAttrs()) {
                auto * b = args[0].asAttrs();
                bindingsPtr = b;
                if (auto * nv = b->lookup(syms.name)) {
                    Value forced = forceValue(*state.vm, *nv);
                    if (forced.isString() && forced.asString())
                        drvName = forced.asString();
                    else
                        drvName = std::string("<name-tag-")
                                + std::to_string((int)forced.tag()) + ">";
                }
            }
            char buf[64];
            std::snprintf(buf, sizeof buf, " @%p", bindingsPtr);
            drvName += buf;
            std::lock_guard<std::mutex> g(mtx);
            ++counts[drvName];
            static uint64_t total = 0;
            ++total;
            if ((total % 1000) == 0) {
                std::vector<std::pair<std::string, uint64_t>> rows(
                    counts.begin(), counts.end());
                std::sort(rows.begin(), rows.end(),
                    [](const auto & a, const auto & b) { return a.second > b.second; });
                std::fprintf(stderr,
                    "v3 PRIM_DRV PROGRESS total=%llu unique=%zu top:",
                    (unsigned long long)total, rows.size());
                for (size_t i = 0; i < rows.size() && i < 5; ++i)
                    std::fprintf(stderr, " %s/%llu",
                        rows[i].first.c_str(), (unsigned long long)rows[i].second);
                std::fprintf(stderr, "\n");
            }
        }
    }

    // C-6 (CODEBASE_REVIEW_2026-06-11): fake-store synthesis (the
    // /v3-fake-store/ path at the bottom of this function) is ONLY valid
    // for standalone eval with no store wired.  When a real store is wired
    // (state.nixEvalState != nullptr) it must NEVER be reached: a native-
    // path exception there is a genuine failure (a user `throw` inside
    // builder/args/env, or a v3 bug) and silently degrading to a fabricated
    // drvPath is exactly the error-masking that cost weeks on the drvPath
    // divergence.  allowFakeStore() (NIX_V3_ALLOW_FAKE_STORE=1) restores the
    // old permissive fall-through for debugging only.

    // BR-3.5: Phase A native fast path.  Attempted ONLY if the
    // input shape passes isSimpleDerivationAttrs (cheap presence
    // check — no value forcing).  With a store wired, an exception
    // here propagates (C-6); with no store wired it falls through to
    // the fake-store synthesizer with no semantics change.
    static const bool nativeDisabled =
        std::getenv("V3_DRV_NO_NATIVE") != nullptr;
    if (!nativeDisabled
        && state.nixEvalState && args[0].isAttrs() && args[0].asAttrs()
        && isSimpleDerivationAttrs(args[0].asAttrs()))
    {
        try {
            primDerivationStrictNative(state, args, out);
            ++drvNativeHits();
            return;
        } catch (const std::exception & e) {
            ++drvNativeFallbacks();
            // BR-3.13: when V3_DRV_DEBUG is set, surface the
            // derivation's name (when readable) along with the
            // throw — makes "why did the native path bail on this
            // drv?" diagnosable without re-running with extra
            // instrumentation.  When the error is a real
            // user-facing one (missing builder etc.), the bridge
            // re-throws with proper Nix-style traces; we still
            // see this debug line first.  Cache the env-var lookup
            // (function-static bool) — derivationStrict is on the
            // hot path of nixpkgs eval, fires per drv build.  See
            // vm.cc:1808 for the canonical pattern.
            static const bool s_drvDebug =
                std::getenv("V3_DRV_DEBUG") != nullptr;
            if (__builtin_expect(s_drvDebug, 0)) {
                std::string drvName = "<unknown>";
                const auto & syms = drvStrictSymbols();
                if (auto * nv = args[0].asAttrs()->lookup(syms.name)) {
                    // Force the name attr (it may still be a Thunk
                    // when the native path throws — eg if name comes
                    // alphabetically after the attr that triggered
                    // the throw, primDerivationStrictNative's iter
                    // hasn't reached it yet).  Wrap in try/catch so
                    // diagnostic doesn't itself blow up.
                    try {
                        Value forced = forceValue(*state.vm, *nv);
                        if (forced.isString() && forced.asString())
                            drvName = forced.asString();
                    } catch (...) { drvName = "<force-failed>"; }
                }
                std::fprintf(stderr,
                    "v3 derivationStrict native fell back on `%s`: %s\n",
                    drvName.c_str(), e.what());
                // Also dump args attrNames for forensics.  Helps
                // determine whether the failing args truly belong to
                // the named derivation or some inner wrapper layer.
                if (args[0].asAttrs()) {
                    std::fprintf(stderr,
                                 "  args attrNames (%u): ",
                                 args[0].asAttrs()->size);
                    const auto & symTab = ir::globalSymbolTable();
                    const Bindings * b = args[0].asAttrs();
                    for (uint32_t i = 0; i < b->size && i < 40; ++i) {
                        SymbolId sid = b->entries[i].name;
                        std::fprintf(stderr, "%s ",
                                     sid < symTab.size()
                                         ? symTab[sid].c_str() : "<?>");
                    }
                    if (b->size > 40)
                        std::fprintf(stderr, "...(+%u more)", b->size - 40);
                    std::fprintf(stderr, "\n");
                }
            }
            // C-6: the native path is attempted ONLY when a store is
            // wired (see the `state.nixEvalState` guard on the enclosing
            // `if`), so reaching this catch means a real store is wired.
            // Rethrow the original native-path exception (preserving its
            // Nix-style trace) instead of falling through to fake-store
            // synthesis — the latter would mask the error with a wrong
            // /v3-fake-store/ drvPath.  Native has been 0-fallback across
            // hello/git/python3/coreutils/stdenv/M5, so this only surfaces
            // genuine failures.  NIX_V3_ALLOW_FAKE_STORE=1 keeps the old
            // fall-through for debugging.
            if (!allowFakeStore())
                throw;
            // fall through to the fake-store synthesizer below.
        }
    }

    // V3_DRV_NO_BRIDGE=1 makes the native-path throw user-visible
    // instead of bouncing through the TW bridge.  Used to diagnose
    // which native-path error is the actual blocker for a given
    // workload; the bridge cascade otherwise hides the root cause
    // behind a soup of follow-on TW-callback failures.
    static const bool s_drvNoBridge =
        std::getenv("V3_DRV_NO_BRIDGE") != nullptr;
    if (__builtin_expect(s_drvNoBridge, 0)) {
        // Native path either succeeded (returned above) or threw.
        // If it threw, the catch above swallowed it and we're here
        // — re-throw a generic error that includes the drv name.
        std::string drvName = "<unknown>";
        if (args[0].isAttrs() && args[0].asAttrs()) {
            const auto & syms = drvStrictSymbols();
            if (auto * nv = args[0].asAttrs()->lookup(syms.name)) {
                if (nv->isString() && nv->asString())
                    drvName = nv->asString();
            }
        }
        throw std::runtime_error(
            "v3 primDerivationStrict: native path failed for `" + drvName
            + "` and V3_DRV_NO_BRIDGE=1; check V3_DRV_DEBUG output for "
              "the underlying error");
    }
    // FFI_KILL_PLAN Phase C6 + TW_VALUE_ERADICATION F5 (2026-06-02): the
    // derivationStrict TW-bridge fallback (V3_DRV_KEEP_BRIDGE) is DELETED.
    // It was default-off since 2026-06-01 (Phase C0: 2414 native / 0
    // fallback across hello/firefox/python3/HNE/M5); this session's sweep
    // re-confirmed native=byte-equal / 0-fallback across hello/git/python3/
    // coreutils/stdenv/M5 (the 2nd-session 0-fallback retirement criterion).
    // It was the LAST v3ToTreeWalker entry site outside the bridge apparatus
    // itself; removing it makes the #875 subsystem dead code (deleted in the
    // same arc).  Native-path errors now propagate / reach the no-store path
    // below exactly as they already did by default (the gate never fired by
    // default, so this is a zero-default-behavior-change deletion).
    if (!args[0].isAttrs() || !args[0].asAttrs())
        typeError("derivationStrict", "attrset");

    // C-6: with a real store wired we must never synthesize a fake-store
    // drvPath.  Reaching here with state.nixEvalState set means the native
    // path was skipped (V3_DRV_NO_NATIVE, or the attrs failed
    // isSimpleDerivationAttrs) — produce a loud error rather than a
    // silently-wrong /v3-fake-store/ path.  (The more common case — the
    // native path throwing — is rethrown in the catch above.)
    if (state.nixEvalState && !allowFakeStore())
        throw std::runtime_error(
            "v3 derivationStrict: refusing to synthesize a /v3-fake-store/ "
            "drvPath while a real store is wired (the native derivation path "
            "was not taken for this derivation's shape). This would mask a "
            "wrong drvPath. Set NIX_V3_ALLOW_FAKE_STORE=1 to override.");

    const auto & sym = drvStrictSymbols();

    auto * src = args[0].asAttrs();
    const Value * nameVRaw = src->lookup(sym.name);
    if (!nameVRaw)
        typeError("derivationStrict", "attrset with `name` string");
    Value nameV = forceValue(*state.vm, *nameVRaw);
    if (!nameV.isString())
        typeError("derivationStrict", "attrset with `name` string");
    std::string name(nameV.asString());

    // Tree-walker rejects derivation names containing characters
    // that aren't allowed in a Nix store path: only [A-Za-z0-9+\-._?=]
    // are permitted, and the name must not start with `.`.
    auto isValidNameChar = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') ||
               c == '+' || c == '-' || c == '.' || c == '_' ||
               c == '?' || c == '=';
    };
    if (name.empty())
        throw std::runtime_error("v3 derivationStrict: derivation name is empty");
    if (name[0] == '.')
        throw std::runtime_error("v3 derivationStrict: derivation name '" + name +
                                  "' must not start with '.'");
    for (char c : name) {
        if (!isValidNameChar(c))
            throw std::runtime_error("v3 derivationStrict: invalid character '" +
                                      std::string(1, c) + "' in derivation name '" + name + "'");
    }

    // Read outputs (default ["out"]).  derivationStrict's result is
    // an attrset { drvPath; <output1>; <output2>; ... } with one
    // path per declared output.
    std::vector<std::string> outputs;
    if (auto * outV = src->lookup(sym.outputs)) {
        Value f = forceValue(*state.vm, *outV);
        if (f.isList() && f.asList()) {
            for (uint32_t i = 0; i < f.asList()->size; ++i) {
                Value el = forceValue(*state.vm, f.asList()->elems[i]);
                if (el.isString()) outputs.push_back(el.asString());
            }
        }
    }
    if (outputs.empty()) outputs.push_back("out");

    // Synthesize fake store paths.  Real nix interacts with the store;
    // v3 just produces stable identifiers good enough for tests that
    // string-interpolate / string-compare drvPath / outPath values.
    //
    // Hash a few stringy attrs (system, builder, outputHash, args) into
    // the fake path so two derivations differing only in builder don't
    // collide — required by `eval-okay-eq-derivations`.
    auto attrToString = [&](const Value * v) -> std::string {
        if (!v) return "";
        Value f = forceValue(*state.vm, *v);
        if (f.isString()) return f.asString();
        if (f.isPath())   return f.asPath();
        if (f.isInt())    return std::to_string(f.asInt());
        if (f.isBool())   return f.asInt() == 1 ? "1" : "";
        if (f.isList() && f.asList()) {
            std::string s;
            for (uint32_t i = 0; i < f.asList()->size; ++i) {
                Value el = forceValue(*state.vm, f.asList()->elems[i]);
                if (el.isString()) s += el.asString();
                else if (el.isPath()) s += el.asPath();
                s += ',';
            }
            return s;
        }
        return "";
    };
    std::string sysStr     = attrToString(src->lookup(sym.system));
    std::string builderStr = attrToString(src->lookup(sym.builder));
    std::string argsStr    = attrToString(src->lookup(sym.args));
    std::string ohStr      = attrToString(src->lookup(sym.outputHash));
    // Cheap FNV-1a hash — collision probability is plenty for tests and
    // we don't need cryptographic security for fake store paths.
    auto fnv = [](std::string_view s) {
        uint64_t h = 1469598103934665603ULL;
        for (char c : s) { h ^= (unsigned char)c; h *= 1099511628211ULL; }
        return h;
    };
    char hashBuf[17];
    std::snprintf(hashBuf, sizeof hashBuf, "%016llx",
                  (unsigned long long)fnv(name + "|" + sysStr + "|" + builderStr + "|" + argsStr + "|" + ohStr));
    std::string hashTag(hashBuf, 16);

    std::vector<std::pair<SymbolId, Value>> entries;
    entries.reserve(outputs.size() + 1);
    entries.emplace_back(sym.drvPath, mkStringValueOwned("/v3-fake-store/" + hashTag + "-" + name + ".drv"));
    for (auto & o : outputs) {
        SymbolId sO = vmIntern(state, o);
        std::string p = "/v3-fake-store/" + hashTag + "-" + name + (o == "out" ? "" : "-" + o);
        entries.emplace_back(sO, mkStringValueOwned(p));
    }
    std::sort(entries.begin(), entries.end(),
        [](auto & a, auto & b) { return a.first < b.first; });
    Bindings * b = Alloc::allocBindings(static_cast<uint32_t>(entries.size()));
    V3_STATS_INC(attrsetsAllocated);
    for (size_t i = 0; i < entries.size(); ++i) {
        b->entries[i].name  = entries[i].first;
        bindingsSetValue(b, static_cast<uint32_t>(i), entries[i].second);  // Phase D
    }
    out.mkAttrs(b);
}

// BR-3.5: Phase A native attr-loop.  Reads args[0] (a v3 Bindings*
// known-simple per isSimpleDerivationAttrs) and populates a
// nix::Derivation: name, builder, platform, args, env, outputs.
//
// Iteration order: lexicographic by name STRING (BR-3.4) — required
// for drv-hash parity with tree-walker.
//
// Throws on any unsupported shape; the caller's primDerivationStrict
// catches the throw and falls through to the existing bridge.
//
// 2026-05-17 — Option 4 hybrid: phases 4-7 (context processing,
// output configuration, writeDerivation, result attrset construction)
// are extracted into `buildAndWriteDrvNative` so both
// `primDerivationStrictNative` (the all-C path) AND the new
// `primDerivationFromPreprocessed` (the bytecode-wrapper FFI leaf)
// can share the implementation.  See the bytecode wrapper's
// installation site in bytecode_primops.cc for the protocol.
static void buildAndWriteDrvNative(
    EvalState & state,
    nix::Derivation & drv,
    const nix::NixStringContext & context,
    const std::vector<std::string> & declaredOutputs,
    bool contentAddressed, bool isImpure,
    const std::optional<std::string> & outputHashStr,
    const std::optional<std::string> & outputHashAlgoStr,
    const std::optional<std::string> & outputHashModeStr,
    Value & out)
{
    auto & ns = *state.nixEvalState;
    const auto & sym = drvStrictSymbols();

    if (drv.builder.empty())
        throw std::runtime_error(
            "v3 BR-3 native: required attribute `builder` missing");
    if (drv.platform.empty())
        throw std::runtime_error(
            "v3 BR-3 native: required attribute `system` missing");

    // ---- BR-3.6: process accumulated NixStringContext into
    // drv.inputSrcs / drv.inputDrvs.  Mirrors derivationStrictInternal
    // (eval.cc:1849).  Variant dispatch:
    //
    //   - DrvDeep{drvPath}    : add the entire FS closure of drvPath as
    //                           sources; for each derivation in the
    //                           closure, also pull in its full output
    //                           name set as an inputDrv.
    //   - Built{drvPath, out} : insert `out` into inputDrvs[drvPath].
    //   - Opaque{path}        : insert `path` into inputSrcs.
    //
    // The context comes from coerceToString calls that flowed
    // through derivation strings (`outPath`, `drvPath`) and path
    // values (copyPathToStore).
    for (auto & c : context) {
        std::visit(
            nix::overloaded{
                [&](const nix::NixStringContextElem::DrvDeep & d) {
                    nix::StorePathSet refs;
                    ns.store->computeFSClosure(d.drvPath, refs);
                    for (auto & j : refs) {
                        drv.inputSrcs.insert(j);
                        if (j.isDerivation()) {
                            drv.inputDrvs.map[j].value =
                                ns.store->readDerivation(j).outputNames();
                        }
                    }
                },
                [&](const nix::NixStringContextElem::Built & b) {
                    drv.inputDrvs.ensureSlot(*b.drvPath).value.insert(b.output);
                },
                [&](const nix::NixStringContextElem::Opaque & o) {
                    drv.inputSrcs.insert(o.path);
                },
            },
            c.raw);
    }

    // ---- BR-3.11 / BR-3.10 / BR-3.7: output configuration.
    if (contentAddressed || isImpure) {
        nix::HashAlgorithm ha = nix::HashAlgorithm::SHA256;
        if (outputHashAlgoStr) {
            if (auto parsed = nix::parseHashAlgoOpt(*outputHashAlgoStr))
                ha = *parsed;
        }
        nix::ContentAddressMethod method =
            nix::ContentAddressMethod::Raw::NixArchive;
        if (outputHashModeStr) {
            if (*outputHashModeStr == "recursive")
                method = nix::ContentAddressMethod::Raw::NixArchive;
            else
                method = nix::ContentAddressMethod::parse(*outputHashModeStr);
        }
        for (auto & o : declaredOutputs) {
            drv.env[o] = nix::hashPlaceholder(o);
            if (isImpure) {
                drv.outputs.insert_or_assign(o,
                    nix::DerivationOutput{nix::DerivationOutput::Impure{
                        .method   = method,
                        .hashAlgo = ha,
                    }});
            } else {
                drv.outputs.insert_or_assign(o,
                    nix::DerivationOutput{nix::DerivationOutput::CAFloating{
                        .method   = method,
                        .hashAlgo = ha,
                    }});
            }
        }
    } else if (outputHashStr) {
        if (declaredOutputs.size() != 1 || declaredOutputs[0] != "out") {
            throw std::runtime_error(
                "v3 BR-3 native: multiple outputs are not supported in "
                "fixed-output derivations");
        }
        std::optional<nix::HashAlgorithm> ha;
        if (outputHashAlgoStr)
            ha = nix::parseHashAlgoOpt(*outputHashAlgoStr);
        nix::Hash h = nix::newHashAllowEmpty(*outputHashStr, ha);

        nix::ContentAddressMethod method = nix::ContentAddressMethod::Raw::Flat;
        if (outputHashModeStr) {
            if (*outputHashModeStr == "recursive")
                method = nix::ContentAddressMethod::Raw::NixArchive;
            else
                method = nix::ContentAddressMethod::parse(*outputHashModeStr);
        }

        nix::DerivationOutput::CAFixed dof{
            .ca = nix::ContentAddress{
                .method = std::move(method),
                .hash   = std::move(h),
            },
        };
        drv.env["out"] = ns.store->printStorePath(
            dof.path(*ns.store, drv.name, "out"));
        drv.outputs.insert_or_assign("out", std::move(dof));
    } else {
        for (auto & o : declaredOutputs) {
            drv.env[o] = "";
            drv.outputs.insert_or_assign(
                o, nix::DerivationOutput{nix::DerivationOutput::Deferred{}});
        }
        drv.fillInOutputPaths(*ns.store);
    }

    // Materialise + cache + build result attrset.
    nix::StorePath drvPath = ffi::readOnlyMode()
        ? nix::computeStorePath(*ns.store, drv)
        : ns.store->writeDerivation(drv, ns.repair);
    std::string drvPathS = ns.store->printStorePath(drvPath);

    // #741 Phase 3e SHADOW / ACTIVE: drv-hash-keyed cache lookup.
    // drvPath IS the canonical content hash of `drv` (libstore
    // semantics: same drvPath ⇒ same drv ⇒ same effective inputs).
    // Two primop calls producing the same drvPath produce identical
    // result attrsets.  SHADOW: always continues body; ACTIVE: skips
    // the remaining libstore tail on hit.
    Value v3DrvCached;
    std::string v3DrvKey;
    bool v3DrvHit = false;
    if (value_serialize::drvHashCacheEnabled()
        || value_serialize::drvHashCacheActiveEnabled()
        || value_serialize::drvHashCacheDiskEnabled()) {
        // #828 B1 audit: instrument the drvHash lookup site so we can
        // see which call paths produce cache hits vs misses.  Helps
        // verify Phase 3e ACTIVE's "skip-on-hit is sound" invariant
        // (the assumption that two primDerivationStrict invocations
        // producing the same drvPath produce identical result
        // attrsets).
        CACHE_HOOK_DEFINE_SITE(siteDrvHashLookup,
            "primDerivationStrict-drvHash-lookup");
        CacheHookTimer drvTimer(siteDrvHashLookup);
        v3DrvKey = drvPathS;
        v3DrvHit = value_serialize::drvHashCacheLookup(v3DrvKey, v3DrvCached);
        if (v3DrvHit) cacheHookHit(siteDrvHashLookup);
        else          cacheHookMiss(siteDrvHashLookup);
    }

    // Phase 3e ACTIVE — skip-on-hit.  In-process safe because the
    // populating miss already ran hashDerivationModulo +
    // drvHashes.insert_or_assign(drvPath, h) (the libstore drvHashes
    // map is process-global; subsequent pathDerivationModulo callers
    // find drvPath via that earlier insert regardless of who put it
    // there).  In `nix eval --impure` (readOnlyMode), writeDerivation
    // was already a no-op so no .drv-file concern.
    //
    // NOT safe for cross-process replay (Phase 5): a freshly-loaded
    // cache wouldn't have populated drvHashes.  Phase 5 must cache
    // the modulo hash alongside the result and replay it on hit.
    if (v3DrvHit && value_serialize::drvHashCacheActiveEnabled()) {
        // #828 B1 audit: count the actual ACTIVE skip-on-hit firings
        // separately from the lookup hits.  drvHashCacheStats already
        // tracks `activeSkips`; we mirror it here for the per-site
        // dump so B1's audit can see exactly how often the
        // skip-the-libstore-tail optimisation fires per call site.
        CACHE_HOOK_DEFINE_SITE(siteDrvHashSkip,
            "primDerivationStrict-drvHash-active-skip");
        cacheHookFire(siteDrvHashSkip);
        cacheHookHit(siteDrvHashSkip);
        out = v3DrvCached;
        ++value_serialize::drvHashCacheStats().activeSkips;
        return;
    }

    {
        auto h = nix::hashDerivationModulo(*ns.store, drv, false);
        nix::drvHashes.insert_or_assign(drvPath, std::move(h));
    }


    std::vector<std::pair<SymbolId, Value>> entries;
    entries.reserve(1 + drv.outputs.size());

    {
        Value v3DrvPath = mkStringValueOwned(drvPathS);
        nix::NixStringContext drvCtx;
        drvCtx.insert(
            nix::NixStringContextElem{nix::NixStringContextElem::DrvDeep{
                .drvPath = drvPath}});
        setStringContext(v3DrvPath.asString(), drvCtx);
        entries.emplace_back(sym.drvPath, v3DrvPath);
    }

    for (auto & [outName, outDef] : drv.outputs) {
        SymbolId outSid = ir::globalInternSymbol(outName);
        std::optional<nix::StorePath> optStaticOutputPath =
            outDef.path(*ns.store, drv.name, outName);
        if (!optStaticOutputPath) {
            throw std::runtime_error(
                "v3 BR-3 native: output '" + outName +
                "' has no static path after fillInOutputPaths");
        }
        std::string outPathS = ns.store->printStorePath(*optStaticOutputPath);

        Value v3OutPath = mkStringValueOwned(outPathS);
        nix::NixStringContext outCtx;
        outCtx.insert(nix::NixStringContextElem{
            nix::NixStringContextElem::Built{
                .drvPath = nix::makeConstantStorePathRef(drvPath),
                .output  = outName,
            }});
        setStringContext(v3OutPath.asString(), outCtx);
        entries.emplace_back(outSid, v3OutPath);
    }

    std::sort(entries.begin(), entries.end(),
        [](auto & a, auto & b) { return a.first < b.first; });

    Bindings * resultB = Alloc::allocBindings(static_cast<uint32_t>(entries.size()));
    V3_STATS_INC(attrsetsAllocated);
    for (size_t i = 0; i < entries.size(); ++i) {
        resultB->entries[i].name  = entries[i].first;
        bindingsSetValue(resultB, static_cast<uint32_t>(i), entries[i].second);  // Phase D
    }
    out.mkAttrs(resultB);

    // #741 Phase 1: round-trip-test the result Value through the
    // value-serialiser when NIX_V3_TEST_DRV_RESULT_SERIALIZE=1.
    // Default-off; gate is a cached bool load.  No effect on the
    // result; pure observation.  Stats dumped under NIX_VM_STATS.
    value_serialize::runRoundTripTest(out);

    // #741 Phase 2: canonical-hash determinism dump when
    // NIX_V3_TEST_CANONICAL_HASH=1.  Per-result `V3-VAL-HASH: <hex>`
    // to stderr; sort + diff across two process invocations should
    // produce empty diff (the determinism falsifier).
    value_serialize::dumpCanonicalHashLine(out);

    // #741 Phase 3e SHADOW: verify hit or insert on miss.  Uses
    // drvPath as the cache key (canonical content hash of drv per
    // libstore).  mismatchHits > 0 falsifies either libstore
    // content-addressing or our serialiser determinism.
    if (!v3DrvKey.empty()) {
        if (v3DrvHit) {
            if (!value_serialize::valuesEqual(v3DrvCached, out))
                ++value_serialize::drvHashCacheStats().mismatchHits;
        } else {
            value_serialize::drvHashCacheInsert(v3DrvKey, out);
        }
    }
}

// 2026-05-17 — Option 4 hybrid FFI leaf.
//
// Takes a preprocessed-args attrset from the bytecode wrapper and
// runs phases 4-7 (context → inputs, output config, writeDerivation,
// result attrset).  The bytecode wrapper handles phases 1-3 (attr
// iteration, force, coerceToString) at bytecode level — iterative,
// no C-recursion.
//
// Input attrset shape (all fields force/cast-checked):
//   name             : string             (drv name)
//   builder          : string             (with context)
//   system           : string             (platform string)
//   args             : list of strings    (with context — drv args)
//   outputs          : list of strings    (output names; default ["out"])
//   env              : attrset of strings (with context; env vars)
//   __ignoreNulls    : bool               (unused at leaf; wrapper applied)
//   __contentAddressed : bool             (CA output config)
//   __impure         : bool               (impure output config)
//   __structuredAttrs : bool              (must be false; wrapper falls back
//                                          to C for structured)
//   outputHash       : null or string     (fixed-output hash; opt.)
//   outputHashAlgo   : null or string     (hash algorithm; opt.)
//   outputHashMode   : null or string     (hash mode; opt.)
//
// Throws on missing/malformed input.  The bytecode wrapper is
// responsible for shape correctness.
static void primDerivationFromPreprocessed(EvalState & state, Value * args, Value & out)
{
    // Per-call counter for V3_DBG_DRVPP=1 — reports how many times
    // the FFI leaf is invoked during a single eval.  TW vs v3 parity
    // expects ~one call per actual derivation node in the graph; if
    // v3 calls 100x that, a memoization gap exists upstream.
    static const bool s_dbg = std::getenv("V3_DBG_DRVPP") != nullptr;
    if (__builtin_expect(s_dbg, 0)) {
        static thread_local uint64_t calls = 0;
        ++calls;
        if (calls % 100 == 0) {
            std::fprintf(stderr,
                "v3 __derivationFromPreprocessed call #%llu\n",
                (unsigned long long)calls);
        }
    }
    if (!args[0].isAttrs() || !args[0].asAttrs())
        typeError("__derivationFromPreprocessed", "attrset");

    // #741 Phase 3a SHADOW / #885 PRODUCTION cache.  SHADOW: body
    // always runs; hit just verifies.  PRODUCTION (`NIX_V3_EVAL_RESULT_
    // CACHE_PRODUCTION=1`): on hit, assign cached value to `out` and
    // return immediately, skipping the entire body.  See
    // value_serialize.hh::evalResultCacheProductionEnabled for the
    // safety argument (idempotent side effects + first-call-populates).
    std::string v3CacheKey;
    Value v3CachedOut;
    bool v3CacheClaimed = false;
    if (value_serialize::evalResultCacheEnabled()) {
        forceDeep(*state.vm, args[0]);
        v3CacheClaimed =
            value_serialize::evalResultCacheLookup(args[0], v3CachedOut, v3CacheKey);
    }
    // PRODUCTION skip-on-hit.  Cache hit implies the body's output
    // would equal cached (verified in SHADOW with mismatch=0 across
    // hello.drvPath + HNE, 2026-05-29).  Skipping the body is safe
    // because side effects (writeDerivation, drvHashes.insert) are
    // idempotent and the first miss in this process populated them.
    if (v3CacheClaimed && value_serialize::evalResultCacheProductionEnabled()) {
        out = v3CachedOut;
        ++value_serialize::evalResultCacheStats().activeSkips;
        return;
    }
    auto v3CacheFinaliser = [&]() {
        if (v3CacheKey.empty()) return;
        if (v3CacheClaimed) {
            if (!value_serialize::valuesEqual(v3CachedOut, out))
                ++value_serialize::evalResultCacheStats().mismatchHits;
        } else {
            value_serialize::evalResultCacheInsert(v3CacheKey, out);
        }
    };

    auto * pp = args[0].asAttrs();

    // Symbol IDs we'll look up.  Cache by static-local for reuse.
    static const SymbolId sName            = ir::globalInternSymbol("name");
    static const SymbolId sBuilder         = ir::globalInternSymbol("builder");
    static const SymbolId sSystem          = ir::globalInternSymbol("system");
    static const SymbolId sArgs            = ir::globalInternSymbol("args");
    static const SymbolId sOutputs         = ir::globalInternSymbol("outputs");
    static const SymbolId sEnv             = ir::globalInternSymbol("env");
    static const SymbolId sIgnoreNulls     = ir::globalInternSymbol("__ignoreNulls");
    static const SymbolId sContentAddressed= ir::globalInternSymbol("__contentAddressed");
    static const SymbolId sImpure          = ir::globalInternSymbol("__impure");
    static const SymbolId sStructuredAttrs = ir::globalInternSymbol("__structuredAttrs");
    static const SymbolId sOutputHash      = ir::globalInternSymbol("outputHash");
    static const SymbolId sOutputHashAlgo  = ir::globalInternSymbol("outputHashAlgo");
    static const SymbolId sOutputHashMode  = ir::globalInternSymbol("outputHashMode");

    auto forceField = [&](const Value * v) -> Value {
        if (!v) return Value{};
        return forceValue(*state.vm, *v);
    };

    auto getString = [&](SymbolId sid, const std::string & label) -> std::string {
        const Value * v = pp->lookup(sid);
        if (!v) throw std::runtime_error(
            "v3 __derivationFromPreprocessed: missing field '" + label + "'");
        Value f = forceField(v);
        if (!f.isString())
            throw std::runtime_error(
                "v3 __derivationFromPreprocessed: field '" + label +
                "' is not a string (tag=" + std::to_string((int)f.tag()) + ")");
        return std::string(f.asString() ? f.asString() : "");
    };

    auto getOptString = [&](SymbolId sid) -> std::optional<std::string> {
        const Value * v = pp->lookup(sid);
        if (!v) return std::nullopt;
        Value f = forceField(v);
        if (f.tag() == Tag::Null) return std::nullopt;
        if (!f.isString()) return std::nullopt;
        return std::string(f.asString() ? f.asString() : "");
    };

    auto getBool = [&](SymbolId sid, bool defaultV) -> bool {
        const Value * v = pp->lookup(sid);
        if (!v) return defaultV;
        Value f = forceField(v);
        if (!f.isBool()) return defaultV;
        return f.asInt() == 1;
    };

    // Absorb string-context entries (v3 side-table) into a
    // NixStringContext.  Same parse path as nix::NixStringContextElem::parse;
    // each ctxStr is the unparsed "format token" v3 stores in
    // lookupStringContextEntries.
    // C-7(d): this absorbCtx feeds drv.inputSrcs/inputDrvs directly — the
    // DERIVATION path.  A dropped token here is a missing dependency edge and
    // a wrong drv hash.  v3InsertContextToken throws on an un-parseable token
    // (a v3 invariant violation) rather than silently skipping it.
    auto absorbCtx = [&](const char * s, nix::NixStringContext & ctx) {
        if (!s) return;
        auto * raw = lookupStringContextEntries(s);
        if (!raw) return;
        for (auto & token : *raw)
            v3InsertContextToken(ctx, token, "__derivationFromPreprocessed");
    };

    // ---- Parse preprocessed args ----

    nix::Derivation drv;
    drv.name = getString(sName, "name");
    nix::checkName(drv.name);
    drv.builder = getString(sBuilder, "builder");
    drv.platform = getString(sSystem, "system");

    nix::NixStringContext context;

    // builder + system have their own context entries
    {
        const Value * bV = pp->lookup(sBuilder);
        if (bV) {
            Value f = forceField(bV);
            if (f.isString()) absorbCtx(f.asString(), context);
        }
        const Value * sV = pp->lookup(sSystem);
        if (sV) {
            Value f = forceField(sV);
            if (f.isString()) absorbCtx(f.asString(), context);
        }
    }

    // Read flags
    bool ignoreNulls       = getBool(sIgnoreNulls, false);
    (void)ignoreNulls;  // wrapper applied; flag carried for completeness
    bool contentAddressed  = getBool(sContentAddressed, false);
    bool isImpure          = getBool(sImpure, false);
    bool useStructuredAttrs= getBool(sStructuredAttrs, false);

    if (useStructuredAttrs) {
        throw std::runtime_error(
            "v3 __derivationFromPreprocessed: __structuredAttrs is set; "
            "the bytecode wrapper must fall back to C "
            "__derivationStrictRaw for structured derivations.");
    }
    if (contentAddressed && isImpure)
        throw std::runtime_error(
            "v3 __derivationFromPreprocessed: derivation cannot be both "
            "content-addressed and impure");

    // Hash fields
    auto outputHashStr     = getOptString(sOutputHash);
    auto outputHashAlgoStr = getOptString(sOutputHashAlgo);
    auto outputHashModeStr = getOptString(sOutputHashMode);

    // Read outputs list
    std::vector<std::string> declaredOutputs;
    {
        const Value * v = pp->lookup(sOutputs);
        if (v) {
            Value f = forceField(v);
            if (f.isList() && f.asList()) {
                for (uint32_t i = 0; i < f.asList()->size; ++i) {
                    Value el = forceValue(*state.vm, f.asList()->elems[i]);
                    if (!el.isString())
                        throw std::runtime_error(
                            "v3 __derivationFromPreprocessed: outputs[*] "
                            "is not a string");
                    std::string s(el.asString() ? el.asString() : "");
                    if (s.empty() || s == "drvPath")
                        throw std::runtime_error(
                            "v3 __derivationFromPreprocessed: invalid "
                            "output name '" + s + "'");
                    declaredOutputs.push_back(s);
                }
            }
        }
    }
    if (declaredOutputs.empty()) declaredOutputs.push_back("out");

    // Read args list → drv.args + absorb context
    {
        const Value * v = pp->lookup(sArgs);
        if (v) {
            Value f = forceField(v);
            if (f.isList() && f.asList()) {
                for (uint32_t i = 0; i < f.asList()->size; ++i) {
                    Value el = forceValue(*state.vm, f.asList()->elems[i]);
                    if (!el.isString())
                        throw std::runtime_error(
                            "v3 __derivationFromPreprocessed: args[*] "
                            "is not a string");
                    drv.args.push_back(el.asString() ? el.asString() : "");
                    absorbCtx(el.asString(), context);
                }
            }
        }
    }

    // Read env attrset → drv.env + absorb context for each value
    {
        const Value * v = pp->lookup(sEnv);
        if (v) {
            Value f = forceField(v);
            if (!f.isAttrs())
                throw std::runtime_error(
                    "v3 __derivationFromPreprocessed: `env` is not an attrset");
            if (f.asAttrs()) {
                const auto & st = ir::globalSymbolTable();
                const Bindings * b = f.asAttrs();
                forEachEntryRefNoMapAttrsRealize(b, [&](const Bindings * owner,
                                                        const Bindings::Entry & e) {
                    SymbolId nm = e.name;
                    Value elv = forceEntryForImmediateDemand(*state.vm, owner, e);
                    if (!elv.isString())
                        throw std::runtime_error(
                            "v3 __derivationFromPreprocessed: env value "
                            "for an attr is not a string");
                    std::string keyStr(nm < st.size() ? st[nm] : "");
                    drv.env.emplace(keyStr,
                        elv.asString() ? elv.asString() : "");
                    absorbCtx(elv.asString(), context);
                });
            }
        }
    }

    // Delegate to the shared phases-4-7 helper.
    buildAndWriteDrvNative(state, drv, context, declaredOutputs,
                            contentAddressed, isImpure,
                            outputHashStr, outputHashAlgoStr, outputHashModeStr,
                            out);
    // #741 Phase 3a SHADOW: verify hit, or insert on miss.
    v3CacheFinaliser();
}

static void primDerivationStrictNative(
    EvalState & state, Value * args, Value & out)
{
    auto * src = args[0].asAttrs();
    const auto & sym = drvStrictSymbols();

    // #741 Phase 3a SHADOW cache: deep-force input, hash, look up.
    // ALWAYS continue body; on hit just verify at end.  Side effects
    // (.drv file write, drvHashes) remain authoritative via body run.
    std::string v3CacheKey;
    Value v3CachedOut;
    bool v3CacheClaimed = false;
    if (value_serialize::evalResultCacheEnabled()) {
        // #741 Phase 3a-RCA-A + 3c-RCA-B (2026-05-23): two attempted
        // deep-force approaches falsified.
        //   * forceDeep (with bindingsSetValue writeback) → broke
        //     hello.drvPath via Tag::Slot replacement.
        //   * forceDeepReadOnly (no writeback) → STILL broke
        //     hello.drvPath.  Root cause: forceValue itself fires
        //     Thunk::shapeCell cell-updates during nested thunk
        //     evaluation, which pollute outer thunks' shape state
        //     when deep-forced from a primop entry context (see
        //     lode/CELL_UPDATE_EVERYWHERE_2026-05-12.md:169 for the
        //     known precedent).
        //
        // Architectural conclusion: input-hashing from a primop
        // entry CANNOT deep-force its input safely.  The cache must
        // either (a) accept partial coverage with un-evaluated
        // hashErrors (current Phase 3a behaviour, ~11% hit rate),
        // (b) defer hashing to AFTER the primop body has done its
        // own forcing (no skip-on-hit possible), or (c) hash a
        // derived canonical form (e.g. the constructed `drv`
        // struct's content-hash) computed mid-body.
        //
        // For Phase 3a we keep (a): cache attempts lookup with the
        // partially-forced args[0]; canonicalHash throws on
        // un-evaluated indirections (now down to App / Suspended
        // Thunk per Phase 3b's chaseToWHNF), those calls miss the
        // cache and run the primop body normally.
        v3CacheClaimed =
            value_serialize::evalResultCacheLookup(args[0], v3CachedOut, v3CacheKey);
    }
    // #885 PRODUCTION skip-on-hit.  See primDerivationFromPreprocessed
    // for the safety argument.  On hit, assign cached → `out` and
    // return; the body's side effects (writeDerivation, drvHashes
    // populate, .drv file write) are idempotent and were already
    // performed by the populating MISS earlier in this process.
    if (v3CacheClaimed && value_serialize::evalResultCacheProductionEnabled()) {
        out = v3CachedOut;
        ++value_serialize::evalResultCacheStats().activeSkips;
        return;
    }
    // RAII-style end-of-function action: verify on hit, insert on miss.
    auto v3CacheFinaliser = [&]() {
        if (v3CacheKey.empty()) return;
        if (v3CacheClaimed) {
            if (!value_serialize::valuesEqual(v3CachedOut, out))
                ++value_serialize::evalResultCacheStats().mismatchHits;
        } else {
            value_serialize::evalResultCacheInsert(v3CacheKey, out);
        }
    };
    // Use a guard so even early returns / throws don't skip the
    // finaliser.  We only insert on successful body completion, so
    // we run the finaliser AT THE END, not in a destructor (a
    // destructor would also run on exception, but inserting a
    // partial / never-computed `out` would be wrong).
    // → defer to manual call at the end (no early returns in this body).

    // ---- name ----
    const Value * nameVRaw = src->lookup(sym.name);
    if (!nameVRaw)
        throw std::runtime_error(
            "v3 BR-3 native: derivation missing required `name` attr");
    Value nameV = forceValue(*state.vm, *nameVRaw);
    if (!nameV.isString())
        throw std::runtime_error(
            "v3 BR-3 native: `name` attr is not a string");
    std::string drvName(nameV.asString() ? nameV.asString() : "");
    // libstore validates the name shape — throws on bad chars / empty
    // / leading dot / etc.  Same validation tree-walker does.
    nix::checkName(drvName);

    // ---- prepare empty drv + accumulator context ----
    nix::Derivation drv;
    drv.name = drvName;
    nix::NixStringContext context;

    // ---- Phase C (BR-3.11) + Phase D (BR-3.12): pre-read flag
    // attrs.  These control the iteration / output-shape decisions
    // and must NOT end up in drv.env (tree-walker filters them via
    // the default-case switch at eval.cc:1696).
    bool ignoreNulls       = false;
    bool contentAddressed  = false;
    bool isImpure          = false;
    bool useStructuredAttrs = false;

    auto readFlagBool = [&](SymbolId sid) -> bool {
        const Value * v = src->lookup(sid);
        if (!v) return false;
        Value f = forceValue(*state.vm, *v);
        if (!f.isBool()) return false;
        return f.asInt() == 1;
    };
    ignoreNulls         = readFlagBool(sym.ignoreNulls);
    contentAddressed    = readFlagBool(sym.contentAddressed);
    isImpure            = readFlagBool(sym.impure);
    useStructuredAttrs  = readFlagBool(sym.structuredAttrs);

    if (contentAddressed && isImpure)
        throw std::runtime_error(
            "v3 BR-3 native: derivation cannot be both "
            "content-addressed and impure");

    // BR-3.12 — structuredAttrs JSON object.  When the flag is set
    // we accumulate every attr's JSON encoding here instead of
    // (or rather, in addition to) populating drv.env via coerce.
    nlohmann::json structuredJson = nlohmann::json::object();

    // ---- iterate attrs in lex order (BR-3.4) ----
    auto order = lexicographicAttrEntries(src);
    const auto & symTab = ir::globalSymbolTable();

    // Track which outputs were declared.  Default ["out"] if no
    // `outputs` attr is present (mirrors derivationStrictInternal:1640).
    std::vector<std::string> declaredOutputs;

    // Phase B (BR-3.10): fixed-output trigger fields.  Populated
    // during the attr loop if outputHash / outputHashAlgo /
    // outputHashMode are present.
    std::optional<std::string> outputHashStr;
    std::optional<std::string> outputHashAlgoStr;
    std::optional<std::string> outputHashModeStr;

    for (auto & attr : order) {
        SymbolId sid = attr.name;
        // 2026-05-20 #670/#671 ROOT CAUSE FIX: capture the attr key as a
        // std::string COPY, not a std::string_view into the global
        // symbol table.  The forceValue / valueToJsonWithContext calls
        // below run arbitrary v3 code which may intern new symbols.
        // Symbol interning grows the underlying
        // `std::vector<std::string>` symbol table; on realloc, every
        // existing std::string in the vector is moved to new storage,
        // which invalidates every previously-captured std::string_view
        // into the table.  Pre-fix, this surfaced as a non-deterministic
        // "phantom python3-3.13.12" derivation under ghc96.drvPath +
        // friends: when the dangling string_view was COPIED into the
        // structuredAttrs JSON key map after the realloc, it picked up
        // garbage bytes from whatever now lived at that address.
        // Symptom: nixpkgs `validatePythonMatches` fires "Python
        // version mismatch" because alabaster.pythonModule.outPath
        // hashes to a python3 whose drvPath differs from sphinx's
        // python.outPath — the difference is one or more drvAttrs keys
        // being corrupted in the structuredAttrs JSON of the affected
        // sub-derivation.  Reclassified from "callPackage fix-point"
        // (A-series family) to "dangling string_view across realloc"
        // (a UB pattern that touched several recursive derivation
        // evaluations).  See project_670_671_phantom_python_2026-05-20
        // and project_670_671_dangling_strview_FIX_2026-05-20.
        std::string keyStr = sid < symTab.size()
            ? std::string(symTab[sid])
            : std::string{};
        std::string_view key = keyStr;
        Value attrCopy;
        Value * attrSlot = attr.slot;
        if (attr.entry && isUnrealizedMapAttrsEntry(attr.owner, *attr.entry)) {
            attrCopy = entryValueForImmediateDemand(
                *state.vm, attr.owner, *attr.entry);
            attrSlot = &attrCopy;
        } else if (!attrSlot) {
            attrCopy = attr.value;
            attrSlot = &attrCopy;
        }
        Value & attrV = *attrSlot;

        // Skip flag attrs — they're meta, not env vars.  Mirrors
        // tree-walker's switch-case branches that don't fall through
        // to the default env-emit path (eval.cc:1696).
        if (sid == sym.ignoreNulls)       continue;
        if (sid == sym.contentAddressed)  continue;
        if (sid == sym.impure)            continue;
        if (sid == sym.structuredAttrs)   continue;

        // __ignoreNulls=true: skip null-valued attrs entirely
        // (eval.cc:1690).  Other types fall through to coerce.
        if (ignoreNulls) {
            Value forced = forceValue(*state.vm, attrV);
            if (forced.tag() == Tag::Null) continue;
        }

        // BR-3.12: under __structuredAttrs the env-emit path is
        // bypassed in favour of JSON-encoding into structuredJson.
        // builder/system/outputs/outputHash* still get extracted to
        // dedicated drv fields below — those use coerceToString,
        // which matches tree-walker's forceString[NoCtx] semantics
        // for the typical (string-typed) cases this path sees.
        if (useStructuredAttrs) {
            // 2026-05-19 #665: `args` is special — it goes to drv.args
            // ONLY and is NOT serialised into the structuredAttrs JSON.
            // Matches tree-walker's libexpr/primops.cc:1721-1732 where
            // `case args:` breaks out of the switch BEFORE reaching the
            // `default:` jsonObject-emit branch (line 1742).  Pre-fix
            // v3 emitted `args` into structuredJson too, causing
            // structured-attrs derivations (every bash53-NNN patch,
            // tarballs, etc.) to have a JSON `args` entry their TW
            // counterparts don't — drv hashes diverged for every
            // single nix eval --impure path lookup.  Tested via
            // bashNonInteractive cascade (#665).
            if (sid == sym.args) {
                Value listV = forceValue(*state.vm, attrV);
                if (!listV.isList())
                    throw std::runtime_error(
                        "v3 BR-3 native: `args` attr is not a list");
                if (listV.asList()) {
                    for (uint32_t i = 0; i < listV.asList()->size; ++i) {
                        Value el = forceValue(*state.vm, listV.asList()->elems[i]);
                        drv.args.push_back(v3CoerceToString(
                            state, el, context,
                            "while evaluating an element of `args`"));
                    }
                }
                continue;
            }
            // keyStr was captured at iteration start (#670/#671 fix).
            structuredJson[keyStr] = valueToJsonWithContext(
                state, attrV, context);
            // Special-case fields still need to populate drv.* so
            // libnixstore can write the .drv correctly.
            if (sid == sym.outputs) {
                Value listV = forceValue(*state.vm, attrV);
                if (!listV.isList())
                    throw std::runtime_error(
                        "v3 BR-3 native: `outputs` attr is not a list");
                if (listV.asList()) {
                    for (uint32_t i = 0; i < listV.asList()->size; ++i) {
                        Value el = forceValue(*state.vm, listV.asList()->elems[i]);
                        if (!el.isString())
                            throw std::runtime_error(
                                "v3 BR-3 native: `outputs` element is not a string");
                        std::string s(el.asString() ? el.asString() : "");
                        if (s.empty() || s == "drvPath")
                            throw std::runtime_error(
                                "v3 BR-3 native: invalid output name");
                        declaredOutputs.push_back(s);
                    }
                }
                continue;
            }
            // builder/system/outputHash* still extracted out to drv.
            // Only coerce-to-string for the SPECIAL fields below; under
            // __structuredAttrs every OTHER attr is already captured
            // into `structuredJson` above (via valueToJsonWithContext),
            // and calling v3CoerceToString on, say, an env attr that's
            // a nested attrset (`{ SSL_CERT_FILE = ...; }`) would
            // throw "attrset has neither __toString nor outPath" and
            // bounce the whole derivation through the TW bridge for no
            // reason.  Pre-2026-05-18 this unconditional coerce was the
            // root cause of 63 structured-attrs derivations on
            // hello.drvPath falling back to TW (see
            // project_bridge_primop_status_2026-05-18.md).
            if (sid == sym.builder
                || sid == sym.system
                || sid == sym.outputHash
                || sid == sym.outputHashAlgo
                || sid == sym.outputHashMode)
            {
                std::string s = v3CoerceToString(
                    state, attrV, context,
                    "while evaluating a derivation attribute");
                if (sid == sym.builder)             drv.builder = s;
                else if (sid == sym.system)         drv.platform = s;
                else if (sid == sym.outputHash)     outputHashStr = s;
                else if (sid == sym.outputHashAlgo) outputHashAlgoStr = s;
                else if (sid == sym.outputHashMode) outputHashModeStr = s;
            }
            // No drv.env emit under structuredAttrs.
            continue;
        }

        // `args` is special: forced as a list-of-strings.
        if (sid == sym.args) {
            Value listV = forceValue(*state.vm, attrV);
            if (!listV.isList())
                throw std::runtime_error(
                    "v3 BR-3 native: `args` attr is not a list");
            if (listV.asList()) {
                for (uint32_t i = 0; i < listV.asList()->size; ++i) {
                    Value el = forceValue(*state.vm, listV.asList()->elems[i]);
                    drv.args.push_back(v3CoerceToString(
                        state, el, context,
                        "while evaluating an element of `args`"));
                }
            }
            continue;
        }

        // `outputs` is special: forced as a list-of-strings, then
        // remembered for the per-output env vars + DerivationOutput
        // setup.  The env entry for `outputs` itself is the
        // space-joined list (matches tree-walker via coerceToString
        // on coerceMore=true list of strings).
        if (sid == sym.outputs) {
            Value listV = forceValue(*state.vm, attrV);
            if (!listV.isList())
                throw std::runtime_error(
                    "v3 BR-3 native: `outputs` attr is not a list");
            std::string joined;
            if (listV.asList()) {
                for (uint32_t i = 0; i < listV.asList()->size; ++i) {
                    Value el = forceValue(*state.vm, listV.asList()->elems[i]);
                    if (!el.isString())
                        throw std::runtime_error(
                            "v3 BR-3 native: `outputs` element is not a string");
                    std::string s(el.asString() ? el.asString() : "");
                    if (s.empty())
                        throw std::runtime_error(
                            "v3 BR-3 native: empty output name");
                    if (s == "drvPath")
                        throw std::runtime_error(
                            "v3 BR-3 native: invalid output name 'drvPath'");
                    declaredOutputs.push_back(s);
                    if (!joined.empty()) joined += ' ';
                    joined += s;
                }
            }
            drv.env.emplace(std::string(key), std::move(joined));
            continue;
        }

        // All other attrs: coerceToString → drv.env[key] = s.
        // Special-case builder + system to also fill the dedicated
        // drv.builder / drv.platform fields.  Fixed-output triggers
        // (outputHash*) are remembered for the post-loop branch.
        std::string s = v3CoerceToString(
            state, attrV, context,
            "while evaluating a derivation attribute");
        if (sid == sym.builder)
            drv.builder = s;
        else if (sid == sym.system)
            drv.platform = s;
        else if (sid == sym.outputHash)
            outputHashStr = s;
        else if (sid == sym.outputHashAlgo)
            outputHashAlgoStr = s;
        else if (sid == sym.outputHashMode)
            outputHashModeStr = s;
        // emplace gives us "first occurrence wins"; but since attrs
        // are unique by symbol within a Bindings, this is fine.
        drv.env.emplace(std::string(key), std::move(s));
    }

    if (declaredOutputs.empty())
        declaredOutputs.push_back("out");

    // BR-3.12: stash the JSON-encoded structuredAttrs into the drv.
    // The store ATerm format includes structuredAttrs (when present)
    // separately from drv.env, so this is what makes the resulting
    // drvPath byte-equal to tree-walker's structuredAttrs derivation.
    if (useStructuredAttrs) {
        nix::StructuredAttrs sa;
        // sa.structuredAttrs is nlohmann::json::object_t (a map).
        // structuredJson is a nlohmann::json with object type — extract.
        sa.structuredAttrs = structuredJson.get<nlohmann::json::object_t>();
        drv.structuredAttrs = std::move(sa);
    }

    // 2026-05-17 — Option 4 refactor: delegate phases 4-7 (context
    // processing, output config, writeDerivation, result attrset) to
    // the shared helper.  This same helper is called by the bytecode
    // hybrid's FFI leaf `__derivationFromPreprocessed`.  See
    // bytecode_primops.cc for the wrapper protocol.
    buildAndWriteDrvNative(state, drv, context, declaredOutputs,
        contentAddressed, isImpure,
        outputHashStr, outputHashAlgoStr, outputHashModeStr,
        out);
    // #741 Phase 3a SHADOW: verify hit, or insert on miss.  Runs on
    // normal completion only; exceptions (rethrown from any of the
    // calls above) skip the cache update.
    v3CacheFinaliser();
}

// Legacy inline phase-4-7 code from primDerivationStrictNative, kept as
// reference until the 2026-05-17 refactor stabilizes.  Compiled out
// (`#if 0`) so it doesn't affect runtime behavior.
#if 0
static void primDerivationStrictNative_phases_4_7_legacy_ref(
    EvalState & state, nix::Derivation & drv,
    const nix::NixStringContext & context,
    const std::vector<std::string> & declaredOutputs,
    bool contentAddressed, bool isImpure,
    const std::optional<std::string> & outputHashStr,
    const std::optional<std::string> & outputHashAlgoStr,
    const std::optional<std::string> & outputHashModeStr,
    Value & out)
{
    auto & ns = *state.nixEvalState;
    const auto & sym = drvStrictSymbols();
    if (drv.builder.empty())
        throw std::runtime_error(
            "v3 BR-3 native: required attribute `builder` missing");
    if (drv.platform.empty())
        throw std::runtime_error(
            "v3 BR-3 native: required attribute `system` missing");

    // ---- BR-3.6: process accumulated NixStringContext into
    // drv.inputSrcs / drv.inputDrvs.  Mirrors derivationStrictInternal
    // (eval.cc:1849).  Variant dispatch:
    //
    //   - DrvDeep{drvPath}    : add the entire FS closure of drvPath as
    //                           sources; for each derivation in the
    //                           closure, also pull in its full output
    //                           name set as an inputDrv.
    //   - Built{drvPath, out} : insert `out` into inputDrvs[drvPath].
    //   - Opaque{path}        : insert `path` into inputSrcs.
    //
    // The context comes from coerceToString calls that flowed
    // through derivation strings (`outPath`, `drvPath`) and path
    // values (copyPathToStore).
    for (auto & c : context) {
        std::visit(
            nix::overloaded{
                [&](const nix::NixStringContextElem::DrvDeep & d) {
                    nix::StorePathSet refs;
                    ns.store->computeFSClosure(d.drvPath, refs);
                    for (auto & j : refs) {
                        drv.inputSrcs.insert(j);
                        if (j.isDerivation()) {
                            drv.inputDrvs.map[j].value =
                                ns.store->readDerivation(j).outputNames();
                        }
                    }
                },
                [&](const nix::NixStringContextElem::Built & b) {
                    drv.inputDrvs.ensureSlot(*b.drvPath).value.insert(b.output);
                },
                [&](const nix::NixStringContextElem::Opaque & o) {
                    drv.inputSrcs.insert(o.path);
                },
            },
            c.raw);
    }

    // ---- BR-3.11 (Phase C): content-addressed / impure branch.
    // Mirrors derivationStrictInternal (eval.cc:1921).  Both
    // shapes share the same "for each declared output, set env to
    // hashPlaceholder + slot CAFloating-or-Impure" pattern; the
    // only difference is the DerivationOutput variant.
    //
    // outputHashAlgo defaults to SHA256, ingestion method defaults
    // to NixArchive (recursive) for these CA derivations.
    if (contentAddressed || isImpure) {
        nix::HashAlgorithm ha = nix::HashAlgorithm::SHA256;
        if (outputHashAlgoStr) {
            if (auto parsed = nix::parseHashAlgoOpt(*outputHashAlgoStr))
                ha = *parsed;
        }
        nix::ContentAddressMethod method =
            nix::ContentAddressMethod::Raw::NixArchive;
        if (outputHashModeStr) {
            if (*outputHashModeStr == "recursive")
                method = nix::ContentAddressMethod::Raw::NixArchive;
            else
                method = nix::ContentAddressMethod::parse(*outputHashModeStr);
        }
        for (auto & o : declaredOutputs) {
            drv.env[o] = nix::hashPlaceholder(o);
            if (isImpure) {
                drv.outputs.insert_or_assign(o,
                    nix::DerivationOutput{nix::DerivationOutput::Impure{
                        .method   = method,
                        .hashAlgo = ha,
                    }});
            } else {
                drv.outputs.insert_or_assign(o,
                    nix::DerivationOutput{nix::DerivationOutput::CAFloating{
                        .method   = method,
                        .hashAlgo = ha,
                    }});
            }
        }
    }
    // ---- BR-3.10 (Phase B): fixed-output branch.  If outputHash
    // is present, build a CAFixed output instead of the deferred
    // path.  Mirrors derivationStrictInternal (eval.cc:1895).
    //
    // Fixed-output derivations require exactly ["out"] as outputs
    // — multi-output fixed isn't supported by libnixstore.
    else if (outputHashStr) {
        if (declaredOutputs.size() != 1 || declaredOutputs[0] != "out") {
            throw std::runtime_error(
                "v3 BR-3 native: multiple outputs are not supported in "
                "fixed-output derivations");
        }
        std::optional<nix::HashAlgorithm> ha;
        if (outputHashAlgoStr)
            ha = nix::parseHashAlgoOpt(*outputHashAlgoStr);
        nix::Hash h = nix::newHashAllowEmpty(*outputHashStr, ha);

        // outputHashMode parsing — back-compat: "recursive" maps to
        // "nar" (NixArchive); otherwise feed through
        // ContentAddressMethod::parse.  Default is Flat.
        nix::ContentAddressMethod method = nix::ContentAddressMethod::Raw::Flat;
        if (outputHashModeStr) {
            if (*outputHashModeStr == "recursive")
                method = nix::ContentAddressMethod::Raw::NixArchive;
            else
                method = nix::ContentAddressMethod::parse(*outputHashModeStr);
        }

        nix::DerivationOutput::CAFixed dof{
            .ca = nix::ContentAddress{
                .method = std::move(method),
                .hash   = std::move(h),
            },
        };
        drv.env["out"] = ns.store->printStorePath(
            dof.path(*ns.store, drv.name, "out"));
        drv.outputs.insert_or_assign("out", std::move(dof));
    }
    // ---- BR-3.7: deferred-output setup + writeDerivation +
    // hashDerivationModulo cache + v3 result attrset (the regular
    // case for derivations without outputHash).
    else {
        // For deferred outputs, set env[output]="" pre-fill and slot
        // each output as Deferred{}.  fillInOutputPaths overwrites
        // the env entries with the computed paths once the input-
        // addressed hash is known.  Mirrors
        // derivationStrictInternal:1947–1959.
        for (auto & o : declaredOutputs) {
            drv.env[o] = "";
            drv.outputs.insert_or_assign(
                o, nix::DerivationOutput{nix::DerivationOutput::Deferred{}});
        }
        drv.fillInOutputPaths(*ns.store);
    }

    // Materialise the drv: in readOnlyMode (the v3-eval default;
    // also typical for `nix-instantiate --eval`) compute the path
    // without writing.  Otherwise actually write to the store.
    nix::StorePath drvPath = ffi::readOnlyMode()
        ? nix::computeStorePath(*ns.store, drv)
        : ns.store->writeDerivation(drv, ns.repair);
    std::string drvPathS = ns.store->printStorePath(drvPath);

    // Cache the hash modulo so downstream derivations (which see
    // this drv's outputs in their context) can resolve it without
    // re-reading from the store.  Mirrors eval.cc:1976.
    {
        auto h = nix::hashDerivationModulo(*ns.store, drv, false);
        nix::drvHashes.insert_or_assign(drvPath, std::move(h));
    }

    // Build the v3 result attrset: { drvPath; <output1>; <output2>; ... }
    // Sorted by SymbolId (Bindings invariant).  Each string carries
    // the appropriate NixStringContext via the v3 side-table:
    //   - drvPath value gets a DrvDeep entry (so downstream uses
    //     pull in the full closure)
    //   - per-output values get a Built{drvPath, outputName} entry
    //     (mirrors EvalState::mkOutputString → eval.cc:1029)
    std::vector<std::pair<SymbolId, Value>> entries;
    entries.reserve(1 + drv.outputs.size());

    // drvPath entry.
    {
        Value v3DrvPath = mkStringValueOwned(drvPathS);
        nix::NixStringContext drvCtx;
        drvCtx.insert(
            nix::NixStringContextElem{nix::NixStringContextElem::DrvDeep{
                .drvPath = drvPath}});
        setStringContext(v3DrvPath.asString(), drvCtx);
        entries.emplace_back(sym.drvPath, v3DrvPath);
    }

    // Per-output entries.  drv.outputs is a std::map keyed by
    // output name; we look up each declared output in turn so the
    // ordering follows declaredOutputs (which followed the user's
    // `outputs` list — but final v3 attrset is sorted by SymbolId
    // anyway via the std::sort below, so order here is fluid).
    for (auto & [outName, outDef] : drv.outputs) {
        SymbolId outSid = ir::globalInternSymbol(outName);
        // outDef.path(...) returns the concrete StorePath for this
        // output.  For Deferred outputs (post-fillInOutputPaths)
        // this is the input-addressed path.
        std::optional<nix::StorePath> optStaticOutputPath =
            outDef.path(*ns.store, drv.name, outName);
        if (!optStaticOutputPath) {
            // Should not happen for the simple deferred case.  Fall
            // back to bridge if it does.
            throw std::runtime_error(
                "v3 BR-3 native: output '" + outName +
                "' has no static path after fillInOutputPaths");
        }
        std::string outPathS = ns.store->printStorePath(*optStaticOutputPath);

        Value v3OutPath = mkStringValueOwned(outPathS);
        nix::NixStringContext outCtx;
        outCtx.insert(nix::NixStringContextElem{
            nix::NixStringContextElem::Built{
                .drvPath = nix::makeConstantStorePathRef(drvPath),
                .output  = outName,
            }});
        setStringContext(v3OutPath.asString(), outCtx);
        entries.emplace_back(outSid, v3OutPath);
    }

    // Sort by SymbolId for the Bindings invariant + binary search.
    std::sort(entries.begin(), entries.end(),
        [](auto & a, auto & b) { return a.first < b.first; });

    Bindings * resultB = Alloc::allocBindings(static_cast<uint32_t>(entries.size()));
    V3_STATS_INC(attrsetsAllocated);
    for (size_t i = 0; i < entries.size(); ++i) {
        resultB->entries[i].name  = entries[i].first;
        bindingsSetValue(resultB, static_cast<uint32_t>(i), entries[i].second);  // Phase D
    }
    out.mkAttrs(resultB);
}
#endif // legacy phase-4-7 inline reference

void primDerivation(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isAttrs() || !args[0].asAttrs())
        typeError("derivation", "attrset");
    auto * src = args[0].asAttrs();

    // 1. Run derivationStrict to get per-output paths + drvPath.
    Value strict;
    primDerivationStrict(state, args, strict);
    if (!strict.isAttrs() || !strict.asAttrs())
        throw std::runtime_error("v3 derivation: derivationStrict didn't return an attrset");
    auto * strictB = strict.asAttrs();

    // 2. Read `outputs` (default ["out"]).
    std::vector<std::string> outputs;
    SymbolId sOutputs = vmIntern(state, "outputs");
    if (auto * outV = src->lookup(sOutputs)) {
        Value f = forceValue(*state.vm, *outV);
        if (f.isList() && f.asList()) {
            for (uint32_t i = 0; i < f.asList()->size; ++i) {
                Value el = forceValue(*state.vm, f.asList()->elems[i]);
                if (el.isString()) outputs.push_back(el.asString());
            }
        }
    }
    if (outputs.empty()) outputs.push_back("out");

    SymbolId sOutPath  = vmIntern(state, "outPath");
    SymbolId sDrvPath  = vmIntern(state, "drvPath");
    SymbolId sType     = vmIntern(state, "type");
    SymbolId sOutName  = vmIntern(state, "outputName");
    // REVIEW §3: re-add `all` synthesis.  The earlier prohibition was
    // about a self-referential `all = [self]` shape; building `all` as
    // the list of per-output sub-derivations (which are independent
    // self-contained attrsets) is acyclic.
    SymbolId sAll      = vmIntern(state, "all");
    SymbolId sDrvAttrs = vmIntern(state, "drvAttrs");
    const Value * drvPathV = strictB->lookup(sDrvPath);

    // Build commonAttrs = drvAttrs // listToAttrs outputs-list // { all; drvAttrs; }.
    // For v3 we just produce the first output's value (which is what
    // the wrapper's `(builtins.head outputsList).value` returns); the
    // `all` field is omitted as it isn't structurally required by the
    // tests and would re-introduce the same cycle.
    const std::string & firstOut = outputs[0];
    SymbolId sFirstOut = vmIntern(state, firstOut);
    const Value * outPath = strictB->lookup(sFirstOut);

    // For multi-output derivations, also expose `drv.<output>` as a
    // mini-derivation-like attrset carrying that output's outPath.
    // This is the structure tree-walker's `derivation` builds via
    // listToAttrs over `outputsList`, and is what
    // `eval-okay-context-introspection`'s `drv.foo.outPath` reads.
    auto buildOutputAttrset = [&](const std::string & oName,
                                  const Value & oOutPath) -> Value {
        std::vector<std::pair<SymbolId, Value>> oEntries;
        if (drvPathV) oEntries.emplace_back(sDrvPath, *drvPathV);
        oEntries.emplace_back(sOutPath,  oOutPath);
        oEntries.emplace_back(sType,     mkStringValueOwned("derivation"));
        oEntries.emplace_back(sOutName,  mkStringValueOwned(oName));
        std::sort(oEntries.begin(), oEntries.end(),
            [](auto & a, auto & b) { return a.first < b.first; });
        Bindings * ob = Alloc::allocBindings(static_cast<uint32_t>(oEntries.size()));
        V3_STATS_INC(attrsetsAllocated);
        for (size_t i = 0; i < oEntries.size(); ++i) {
            ob->entries[i].name  = oEntries[i].first;
            bindingsSetValue(ob, static_cast<uint32_t>(i), oEntries[i].second);  // Phase D
        }
        Value v;
        v.mkAttrs(ob);
        return v;
    };

    // Result: drvAttrs // { outPath; drvPath; type = "derivation"; outputName; drvAttrs = drvAttrs; }
    std::vector<std::pair<SymbolId, Value>> entries;
    entries.reserve(src->countDistinct() + 5 + outputs.size());
    src->forEach([&](const Bindings::Entry & e) {
        entries.emplace_back(e.name, e.value);
    });
    if (outPath)  entries.emplace_back(sOutPath, *outPath);
    if (drvPathV) entries.emplace_back(sDrvPath, *drvPathV);
    entries.emplace_back(sType,    mkStringValueOwned("derivation"));
    entries.emplace_back(sOutName, mkStringValueOwned(firstOut));
    entries.emplace_back(sDrvAttrs, args[0]);
    // Per-output sub-derivations + collect them for `all`.
    std::vector<Value> allOutputs;
    allOutputs.reserve(outputs.size());
    for (auto & oName : outputs) {
        SymbolId sO = vmIntern(state, oName);
        if (auto * oP = strictB->lookup(sO)) {
            Value oAttr = buildOutputAttrset(oName, *oP);
            entries.emplace_back(sO, oAttr);
            allOutputs.push_back(oAttr);
        }
    }
    // REVIEW §3: synthesize `all` as a list of the per-output sub-
    // derivations.  Tree-walker's corepkgs/derivation.nix exposes the
    // same shape; nixpkgs consumers (e.g. multi-output drv-mapping
    // helpers) read `drv.all`.  Acyclic because each output attrset is
    // self-contained.
    if (!allOutputs.empty()) {
        ListVec * lv = Alloc::allocList(static_cast<uint32_t>(allOutputs.size()));
        V3_STATS_INC(listsAllocated);
        for (size_t i = 0; i < allOutputs.size(); ++i)
            lv->elems[i] = allOutputs[i];
        Value vAll;
        vAll.mkList(lv);
        entries.emplace_back(sAll, vAll);
    }
    std::sort(entries.begin(), entries.end(),
        [](auto & a, auto & b) { return a.first < b.first; });
    std::vector<std::pair<SymbolId, Value>> dedup;
    dedup.reserve(entries.size());
    for (auto & p : entries) {
        if (!dedup.empty() && dedup.back().first == p.first) dedup.back() = p;
        else dedup.push_back(p);
    }
    Bindings * b = Alloc::allocBindings(static_cast<uint32_t>(dedup.size()));
    V3_STATS_INC(attrsetsAllocated);
    for (size_t i = 0; i < dedup.size(); ++i) {
        b->entries[i].name  = dedup[i].first;
        bindingsSetValue(b, static_cast<uint32_t>(i), dedup[i].second);  // Phase D
    }
    out.mkAttrs(b);

    // REVIEW §3 NOTE: tree-walker stores `all = [<self>]` for single-
    // output drvs (self-referential).  v3 deliberately stores the
    // simpler form `all = [<per-output-attrset>]` -- TW's self-ref
    // requires cycle detection in --strict print + deep force, which
    // v3's bridge doesn't currently provide.  Programmatic shape is
    // the same: `drv.all`'s list elements expose `outPath / drvPath /
    // type / outputName`.  Diverges only on `--strict`-print recursion.
}

/// Cache of compiled-and-evaluated imported files.  Closures returned
/// by `import` reference their CompilationUnit's bytecode and constant
/// pools by raw pointer — those must outlive the closure, so we keep
/// the CUs (and the eval result) here for the lifetime of the process.
/// Keyed by absolute path so repeated imports are idempotent.
///
/// REVIEW §2.5: tracks (mtime, size) per cache entry so long-running
/// processes (Hydra, LSP, library consumers) re-evaluate files that
/// change on disk between imports.  Tree-walker uses mtime-based
/// invalidation; we mirror that.  CLI tools (one eval per process)
/// are unaffected -- the stat on cache hit is microseconds.
struct ImportCacheEntry {
    Value result;
    int64_t mtimeNs = 0;
    int64_t size    = 0;
    // Phase 4b LRU (2026-05-30, EXIT_GC_SPIRAL §4.1): per-entry last
    // access generation.  Bumped at every cache hit + on insert.
    // Sorting by lastAccessGen ascending gives LRU eviction order.
    // 0 = never accessed (entry was inserted but never re-hit; rare —
    // see HIT path below which bumps on read).
    uint64_t lastAccessGen = 0;
};
struct ImportCache
{
    std::deque<CompilationUnit> cus;       // stable addresses (deque doesn't reallocate)
    std::unordered_map<std::string, ImportCacheEntry> results;
    // Phase 4b LRU access counter.  Monotonic; not thread-safe (v3 is
    // single-threaded by design).  Bumped on every cache hit + insert.
    uint64_t accessCounter = 0;
};
inline ImportCache & importCache()
{
    static ImportCache c;
    return c;
}

// Phase 4b LRU (2026-05-30): eviction gate.  Default 0 = no eviction
// (preserves status quo).  When > 0, evict oldest entries when
// `results.size()` exceeds the threshold.  Opt-in via
// `NIX_V3_IMPORT_CACHE_MAX_ENTRIES=N`.
static size_t importCacheMaxEntries() noexcept
{
    static const size_t v = []() -> size_t {
        if (const char * s = std::getenv("NIX_V3_IMPORT_CACHE_MAX_ENTRIES")) {
            long n = std::strtol(s, nullptr, 10);
            if (n > 0) return static_cast<size_t>(n);
        }
        return 0;  // default: no eviction
    }();
    return v;
}

// Phase 4b LRU counters (always-on, cheap atomic relaxed).
static std::atomic<uint64_t> g_importCacheLruEvictions{0};
static std::atomic<uint64_t> g_importCacheLruSweeps{0};

// Mark an entry as just-accessed.  Caller is the cache hit path.
static inline void bumpImportEntry(ImportCacheEntry & e, ImportCache & c) noexcept
{
    e.lastAccessGen = ++c.accessCounter;
}

// LRU eviction: when results.size() > maxEntries, drop the oldest 25%.
// Called only after an insert (when growth can push us over the limit).
// Cost: O(N log N) sort of pointers; sorting + erasing 25% amortises to
// O(N) per insertion across N inserts.
static void maybeEvictOldImportEntries(ImportCache & c) noexcept
{
    const size_t maxEntries = importCacheMaxEntries();
    if (maxEntries == 0) return;
    if (c.results.size() <= maxEntries) return;

    g_importCacheLruSweeps.fetch_add(1, std::memory_order_relaxed);

    // Collect iterator + generation pairs.  Sort ascending by gen,
    // evict the oldest 25 % of entries.  Erasing from an unordered_map
    // is O(1) per erase; collecting + sorting is the dominant cost.
    std::vector<std::pair<uint64_t, std::string>> candidates;
    candidates.reserve(c.results.size());
    for (const auto & kv : c.results)
        candidates.emplace_back(kv.second.lastAccessGen, kv.first);

    // Number to evict: bring `results.size()` down to `maxEntries * 3/4`
    // so we evict a chunk, not just one entry at a time.  Smooths
    // the cost over multiple inserts.
    const size_t targetSize = (maxEntries * 3) / 4;
    const size_t toEvict = candidates.size() > targetSize
        ? candidates.size() - targetSize
        : 0;
    if (toEvict == 0) return;

    std::nth_element(candidates.begin(), candidates.begin() + toEvict,
                     candidates.end(),
                     [](const auto & a, const auto & b) {
                         return a.first < b.first;  // ascending: oldest first
                     });
    for (size_t i = 0; i < toEvict; ++i)
        c.results.erase(candidates[i].second);
    g_importCacheLruEvictions.fetch_add(toEvict, std::memory_order_relaxed);
}

} // anon ns

// ----------------------------------------------------------------------
// Import-cache size accessors (memory-bucket accounting, 2026-06-04).
//
// The `import` cache (anon-namespace `importCache()`) is the largest
// "CU cache" contributor: one parsed CompilationUnit + one cached
// eval-result Value per imported `.nix` file, retained for the process
// lifetime.  The cached Value *payloads* live in the v3 arena (counted
// by the precise mark under RootSource::CuCache); the CompilationUnit
// bytecode lives in libc-malloc'd vectors (invisible to the arena).
// These accessors expose both so live_trace.cc can size the bucket.
// Defined outside the anon namespace for external linkage; they still
// see `importCache()` (anon-namespace names are TU-visible).
// ----------------------------------------------------------------------

/// Sum of every cached CompilationUnit's libc-malloc'd container bytes
/// (the parsed bytecode the import cache retains).  NOT in the arena.
size_t importCacheBytecodeBytes() noexcept
{
    size_t total = 0;
    for (const CompilationUnit & cu : importCache().cus)
        total += cu.approxBytesUsed();
    return total;
}

/// Number of cached CompilationUnits (one per imported file).
size_t importCacheCuCount() noexcept
{
    return importCache().cus.size();
}

/// M-10 (CODEBASE_REVIEW_2026-06-11): total string-constant REFERENCES across
/// all cached CUs (= sum of each CU's stringConstants.size()).  Compared
/// against the interned pool's unique-entry count to decide whether interning
/// is a net memory win on this workload (high ref:unique ratio ⇒ win, since
/// each dedup'd ref drops 24 B while each unique ref costs +8 B vs the old
/// owned-std::string layout).  Reported under NIX_V3_STRINGCONST_STATS=1.
size_t importCacheStringConstRefs() noexcept
{
    size_t total = 0;
    for (const CompilationUnit & cu : importCache().cus)
        total += cu.stringConstants.size();
    return total;
}

/// Number of cached eval-result entries (the Value cache).
size_t importCacheResultCount() noexcept
{
    return importCache().results.size();
}

/// #139 CU-shrink RCA: decompose the per-CU libc footprint into fields so we can
/// tell what is runtime-irreducible (code/symbols/ICs) vs droppable diagnostic
/// side-tables (forceEmitSites, LambdaDescriptor::name).  Print, don't return —
/// it's a one-shot measurement gated by the caller.
void importCachePrintFieldBreakdown() noexcept
{
    size_t code = 0, ints = 0, floats = 0, strRefs = 0, symTbl = 0, symBody = 0;
    size_t lambdas = 0, lamNames = 0, lamFormals = 0, lamOffs = 0, prim = 0;
    size_t attrIC = 0, recIC = 0, forceSites = 0, fixed = 0;
    size_t nLambdas = 0, nForceSites = 0;
    for (const CompilationUnit & cu : importCache().cus) {
        fixed   += sizeof(CompilationUnit);
        code    += cu.code.capacity() * sizeof(Instruction);
        ints    += cu.intConstants.capacity() * sizeof(int64_t);
        floats  += cu.floatConstants.capacity() * sizeof(double);
        strRefs += cu.stringConstants.capacity() * sizeof(const std::string *);
        symTbl  += cu.symbolTable.capacity() * sizeof(std::string);
        for (const auto & s : cu.symbolTable) symBody += s.capacity();
        lambdas += cu.lambdas.capacity() * sizeof(LambdaDescriptor);
        for (const auto & ld : cu.lambdas) {
            lamNames   += ld.name.capacity();
            lamFormals += ld.formals.capacity() * sizeof(LambdaDescriptor::Formal);
            ++nLambdas;
        }
        lamOffs += cu.lambdaCodeOffsets.capacity() * sizeof(uint32_t);
        prim    += cu.primops.capacity() * sizeof(const PrimOp *);
        attrIC  += cu.attrSelectCache.capacity() * sizeof(CompilationUnit::AttrSelectIC);
        recIC   += cu.recSlotCache.capacity() * sizeof(CompilationUnit::RecSlotIC);
        forceSites += cu.forceEmitSites.capacity()
                      * sizeof(std::pair<uint32_t, const char *>);
        nForceSites += cu.forceEmitSites.size();
    }
    const size_t diag = forceSites + lamNames;   // droppable in default builds
    const size_t irreducible = code + ints + floats + strRefs + symTbl + symBody
        + lambdas + lamFormals + lamOffs + prim + attrIC + recIC + fixed;
    auto mb = [](size_t b) { return b / (1024.0 * 1024.0); };
    std::fprintf(stderr,
        "\n[#139 CU-field breakdown] %zu CUs, %zu lambdas, %zu force-sites\n"
        "  IRREDUCIBLE (runtime-needed):\n"
        "    code(bytecode)   %8.2f MB\n"
        "    symbolTable      %8.2f MB  (refs %.2f + bodies %.2f)\n"
        "    attrSelectIC     %8.2f MB\n"
        "    recSlotIC        %8.2f MB\n"
        "    lambdas+formals  %8.2f MB\n"
        "    consts(int/flt/strRef) %8.2f MB\n"
        "    primops+offsets+fixed  %8.2f MB\n"
        "  DROPPABLE (diagnostic, default-off readers):\n"
        "    forceEmitSites   %8.2f MB  <-- 16B/force-site, read only by V3_DBG_FORCE_SITE\n"
        "    lambda names     %8.2f MB  <-- read only by disassembler/opcycle\n"
        "  >> irreducible=%.2f MB  droppable=%.2f MB (%.1f%% of CU footprint)\n",
        importCache().cus.size(), nLambdas, nForceSites,
        mb(code), mb(symTbl + symBody), mb(strRefs), mb(symBody),
        mb(attrIC), mb(recIC), mb(lambdas + lamFormals),
        mb(ints + floats + strRefs), mb(prim + lamOffs + fixed),
        mb(forceSites), mb(lamNames),
        mb(irreducible), mb(diag),
        100.0 * diag / (irreducible + diag ? irreducible + diag : 1));
}

// 2026-05-29 evening (DIAG analysis spike): clear in-memory import
// cache result set.  Used by run.cc's end-of-eval hook to test
// whether the LiveTracer's "concentrated retention" finding
// (62.8 % at all-packages.nix:9112) is held by ImportCache.
// Clearing drops the roots; subsequent dumpV3LiveFraction sees the
// nixpkgs evaluation graph as freeable.
//
// Note: ONLY clears `results` (the Value cache).  `cus` (the
// CompilationUnit storage) stays so cached bytecode persists.
// Safe to call after run() returns and before subsequent dumps;
// unsafe mid-eval (would orphan in-flight imports).
//
// Gate: NIX_V3_END_OF_EVAL_CLEAR_IMPORT_CACHE=1 invokes from
// run.cc:runRootExpr after run() returns.
void clearImportCacheResultsForDiag() noexcept
{
    auto & cache = importCache();
    cache.results.clear();
}

// 2026-05-29 evening (production end-of-eval clear).  Promotes the
// DIAG spikes (clearV3BridgesForDiag + clearImportCacheResultsForDiag)
// to a single public entry point for the `nix eval` CLI to call
// AFTER rendering completes.
//
// Mechanism: drop the global-root retention sources that pin the
// transitive evaluation graph at end-of-eval.  Per
// `lode/BRIDGES_HOLD_RETENTION_2026-05-29.md`, bridges hold 99.8-
// 99.9 % of arena live bytes; clearing them makes the entire eval
// graph unreachable from globals.  Combined with import-cache
// clear (1 MB additional residual), the live set drops to near
// zero, allowing subsequent free-list reclamation OR process-exit
// page reclaim to proceed unencumbered.
//
// SAFETY: ONLY safe to call when no further TW callbacks into v3
// are expected.  Callers are responsible for sequencing:
//   * src/nix/eval.cc::run() — call AFTER rendering completes;
//     no further v3 calls expected before process exit
//   * `nix repl` and other interactive contexts — DO NOT CALL;
//     subsequent expressions need the bridges
//
// Opt-out: NIX_V3_KEEP_GLOBAL_ROOTS=1 — useful for benchmarking
// against the pre-clear baseline, or for users who chain multiple
// evals in-process.
//
// Reports # of bridge entries cleared to stderr when NIX_VM_STATS=1.
void clearPostEvalGlobalRoots() noexcept
{
    static const bool s_keep =
        std::getenv("NIX_V3_KEEP_GLOBAL_ROOTS") != nullptr;
    if (s_keep) return;

    // (#875 bridge tables retired — TW_VALUE_ERADICATION F4, 2026-06-02;
    //  nothing to clear there.  The import cache remains a global root.)
    const size_t nImports  = importCache().results.size();
    importCache().results.clear();

    static const bool s_stats = std::getenv("NIX_VM_STATS") != nullptr;
    if (s_stats) {
        std::fprintf(stderr,
            "v3-direct post-eval clear: dropped %zu import-cache results.  "
            "Eval graph now unreachable from global roots.\n",
            nImports);
    }
}

// #705 (2026-05-21): walk import-cache results as scavenger roots.
// Each entry holds a Value whose payload may carry a nursery
// pointer (Closure/Bindings/etc.); without walking, a repeat
// `builtins.import` returns a stale pointer after scavenge.
void walkImportCacheRoots(const std::function<void(Value &)> & visit)
{
    auto & cache = importCache();
    for (auto & [key, entry] : cache.results) {
        (void)key;
        visit(entry.result);
    }
}

namespace {

/// Stat a path and produce (mtime_ns, size).  Returns (0, -1) on
/// failure -- the caller treats negative size as "uncacheable" and
/// always re-evaluates.  Uses ::stat on macOS; UTC nanosecond mtime
/// works across all the platforms v3 builds on.
inline std::pair<int64_t, int64_t> importStat(const std::string & path)
{
    struct ::stat st{};
    if (::stat(path.c_str(), &st) != 0) return {0, -1};
    int64_t mtimeNs =
#if defined(__APPLE__)
        (int64_t)st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
        (int64_t)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#endif
    return {mtimeNs, (int64_t)st.st_size};
}

/// T-1 (CODEBASE_REVIEW_2026-06-11): deterministic fingerprint of the env gates
/// that change EMITTED BYTECODE.  The CU disk-cache key keys only on
/// (path, content, schema); a bisect run with e.g. NIX_V3_NO_DEFER=1 would
/// otherwise write CUs under the SAME key as a default run, so a later warm run
/// reads a wrong-codegen CU (and vice versa) — silently measuring the wrong
/// arm, which can retroactively explain "irreproducible" bisect results.  Mixing
/// this fingerprint into the key namespaces the cache per gate-config.  In
/// production (no gates set) the fingerprint is EMPTY, so existing warm caches
/// are unchanged.  Keep this list in sync with the getenv() reads in emit.cc /
/// opt_*.cc / cli/lower_v3.hh / ir.cc (test/lint-cache-coherence.sh enforces).
static const std::string & codegenGateFingerprint()
{
    static const std::string fp = []() {
        // Sorted canonical list of codegen-affecting gates (see header).
        static const char * const kGates[] = {
            "NIX_V3_DBG_OPT_STRICT", "NIX_V3_DBG_STRICTNESS",
            "NIX_V3_DBG_STRICT_CALL_UNTHUNK", "NIX_V3_NO_APP_SPINE_FOLD",
            "NIX_V3_NO_BETA_REDUCE", "NIX_V3_NO_CALL_N", "NIX_V3_NO_CONST_REMAT",
            "NIX_V3_NO_CROSS_FN_STRICTNESS", "NIX_V3_NO_DAG_DEMOTE",
            "NIX_V3_NO_DEFER", "NIX_V3_NO_DETHUNK_STRICT",
            "NIX_V3_NO_EAGER_FORCED_LET", "NIX_V3_NO_EVAL_APPLY",
            "NIX_V3_NO_FORMALS_DEMOTE", "NIX_V3_NO_FUNC_STRICTNESS",
            "NIX_V3_NO_FUSE_RECBIND", "NIX_V3_NO_FUSE_SETGET",
            "NIX_V3_NO_GENLIST_UNROLL", "NIX_V3_NO_GET_LOCAL2",
            "NIX_V3_NO_IF_FOLD", "NIX_V3_NO_LETREC_DEMOTE", "NIX_V3_NO_OPT",
            "NIX_V3_NO_OPT_STRICT", "NIX_V3_NO_PRIMOP_FOLD", "NIX_V3_NO_RBSR_SLOT",
            "NIX_V3_NO_R_BRANCH", "NIX_V3_NO_R_CALL", "NIX_V3_NO_R_IF",
            "NIX_V3_NO_R_RETURN", "NIX_V3_NO_R_STRCONCAT2", "NIX_V3_NO_REC_UNTHUNK",
            "NIX_V3_NO_REG_PRIMOP2", "NIX_V3_NO_SELECTOR_LAMBDA",
            "NIX_V3_NO_SEQ_FORCE", "NIX_V3_NO_STRICT_CALL_UNTHUNK",
            "NIX_V3_OCCUR_DCE", "NIX_V3_OPT_PHASE_LIMIT", "NIX_V3_SKIP_FORCE_LINES",
            "NIX_V3_STAGE4_ALL_MODULES", "NIX_V3_STREAM_FUSION",
        };
        std::string s;
        for (const char * g : kGates) {
            // lint:allow-getenv — cold: this builds the disk-cache key
            // fingerprint ONCE at static init (the enclosing []{...}() runs
            // exactly once), not on any per-opcode/per-force path.  The arg
            // is a loop variable so it cannot be a cached static-const bool.
            const char * v = std::getenv(g);
            if (v) { s += g; s += '='; s += v; s += ';'; }
        }
        return s;
    }();
    return fp;
}

/// builtins.import path -- read the file at `path`, parse, lower, run.
/// Returns the resulting v3 Value.  Requires state.nixEvalState to be
/// set (the host EvalState providing parser + symbol table).
void primImport(EvalState & state, Value * args, Value & out)
{
    if (!state.nixEvalState)
        throw std::runtime_error("v3 primop import: no nix EvalState wired (run via v3-eval)");

    // PARSER_PROJECT_PLAN §5.3: the imported file is parsed by the
    // v3-native parser and lowered directly to IR (no nix::Expr / no gate
    // — native is the only path now).  Home dir for the parser's `~/x`
    // resolution — same source TW's parser uses (getHome()), cached once.
    static const std::string s_homePath = ffi::homeDir();
    std::string path;
    // #741 Phase 4b RCA (2026-05-24): track whether this is an
    // actual IFD-class call (string with context, or attrset arg)
    // vs a literal-path import.  Used to scope the forceDeep +
    // disk-cache-insert below — we MUST NOT forceDeep nixpkgs-
    // internal lazy attrsets just because the user has the cache
    // gate on; that explodes both wall and cache size.
    bool isIfdImport = false;
    if (args[0].isString()) {
        // #757b: if the string carries build context (Built or DrvDeep
        // entries from `"${pkgs.X}/some/path"`), the path may not yet
        // exist on disk — we must realise the context (build the
        // referenced derivations) before reading.  Bridge to TW and
        // route through realisePath, mirroring TW's `import` semantics
        // at libexpr/primops.cc:436 (`state.realisePath(pos, v, ...)`).
        //
        // For plain strings without context (the common case — plain
        // file paths), skip the bridge to keep the fast-path cheap.
        auto * ctxEntries = lookupStringContextEntries(args[0].asString());
        if (ctxEntries && !ctxEntries->empty()) {
            // #741 Phase 4 measurement: this is the discriminator for
            // potentially-real-IFD imports.  Empty context = literal
            // path = no IFD.  Non-empty context = the path mentions
            // store-path-bearing values, which MAY trigger a build via
            // realisePath below.  This counter is a strict upper bound
            // on actual IFD events for the `import` primop kind.
            ++allocStats().ifdProbeWithCtx[kIfdImport];
            isIfdImport = true;
            auto & ns = *state.nixEvalState;
            // FFI_KILL_TODO T1.4 (2026-06-01): v3ToTwBySite[2] counter
            // retired alongside the v3ToTreeWalker call that previously
            // marshalled this string.  The inline-TW-Value-alloc path
            // below is a strict leaf — no v3ToTreeWalker recursion,
            // no bridge-table push, no cycle map.
            // #795 Phase A2: trace IFD-class imports (the calls that
            // realisePath may build).  Gated under V3_DBG_IFD=1 because
            // it fires per IFD event (expensive to log unconditionally).
            static const bool s_dbgIfd = std::getenv("V3_DBG_IFD") != nullptr;
            if (s_dbgIfd) {
                std::fprintf(stderr, "v3 IFD-IMPORT-STR-CTX path=%s ctxN=%zu\n",
                    args[0].asString(), ctxEntries->size());
                for (auto it = ctxEntries->begin();
                     it != ctxEntries->end() && std::distance(ctxEntries->begin(), it) < 4;
                     ++it)
                    std::fprintf(stderr, "  ctx[%zu]=%s\n",
                        (size_t)std::distance(ctxEntries->begin(), it),
                        it->c_str());
                std::fflush(stderr);
            }
            // FFI_KILL_TODO T1.4 (2026-06-01): inline the TW Value
            // construction for Tag::String case.  Avoids the full
            // v3ToTreeWalker function call (cycle-protection map,
            // seen-map lookup, Bridge thunk short-circuit) when we
            // KNOW the value is a string-with-context.  This is the
            // "strict leaf" pattern: v3 has already resolved the
            // string + context; we only need TW's realisePath to
            // take a TW string Value.
            //
            // Pre-existing branch already verified args[0].isString()
            // at line 8164.  Context is in ctxEntries (decoded above).
            nix::Value * tw = ffi::allocValue(ns);
            if (ctxEntries && !ctxEntries->empty()) {
                nix::NixStringContext twCtx = decodeStringContext(*ctxEntries);
                tw->mkString(args[0].asString(), twCtx, ns.mem);
            } else {
                tw->mkString(args[0].asString(), ns.mem);
            }
            try {
                auto resolved = ns.realisePath(nix::noPos, *tw);
                path = resolved.path.abs();
            } catch (...) {
                throw;  // surface TW's error verbatim
            }
        } else {
            path = args[0].asString();
        }
    }
    else if (args[0].isPath()) path = args[0].asPath();
    else if (args[0].isAttrs()) {
        // #741 Phase 4 measurement: attrset arg (typically a
        // derivation) → DEFINITELY goes through realisePath →
        // potential IFD event.  Count under withCtx[kIfdImport].
        ++allocStats().ifdProbeWithCtx[kIfdImport];
        isIfdImport = true;
        // #695 follow-on: IFD support.  When args[0] is a derivation
        // attrset (or any attrset with `__toString` / `outPath`), bridge
        // to TW so its realisePath does the build-context dance.
        // realisePath:
        //   - Forces the value.
        //   - Coerces to string with context (DrvDeep / Opaque / ...).
        //   - For DrvDeep context, calls realiseContext → buildPaths.
        //   - Returns a SourcePath to the realised store output.
        // TW's import (libexpr/primops.cc:434) uses exactly this pattern.
        // For non-IFD plain string/path, the fast-path above stays cheap;
        // we only pay the bridge tax when we'd otherwise typeError.
        auto & ns = *state.nixEvalState;
        // TW_VALUE_ERADICATION (2026-06-02): mirror TW's `import`
        // (libexpr/primops.cc:434) → realisePath → coerceToPath →
        // coerceToString.  An attrset arg (a derivation, or
        // haskell.nix's `{ outPath = <drv-attrs>; … }` where `outPath`
        // is ITSELF an attrset) must coerce RECURSIVELY via `__toString`
        // (called on `self`) then `outPath`, threading the build/store
        // context (DrvDeep / Opaque) so realisePath can perform the IFD
        // build.  The deleted #875 bridge provided this via TW's
        // coerceToString; `toStringCoerceCtx` is the V3-NATIVE
        // equivalent.  The prior native handler resolved only a
        // single-level STRING `outPath`, so an attrset-valued outPath
        // (the haskell.nix shape) fell through to a type error — the
        // same F4 regression class fixed in primReadDir.
        std::vector<std::string> ictx;
        std::string coerced =
            toStringCoerceCtx(state, args[0], ictx, /*copyPathsToStore=*/false);
        try {
            path = ffi::realisePath(ns, coerced, ictx);
        } catch (...) {
            // Surface TW's error verbatim (build failures, missing
            // outputs, restricted-eval, etc.).
            throw;
        }
    }
    else {
        // #493 diag: when args[0] is a Bridge thunk, print the TW
        // source's REAL type so we can trace force-chase issues.
        static const bool s_dbgImportPrim =
            std::getenv("V3_DBG_IMPORT") != nullptr;
        if (__builtin_expect(s_dbgImportPrim, 0)) {
            std::fprintf(stderr,
                "v3 primop import: arg tag=%d", (int)args[0].tag());
            // (Bridge-thunk import-debug peek retired — TW_VALUE_ERADICATION F4.)
            std::fprintf(stderr, "\n");
        }
        typeError("import", "string or path");
    }

    // WC-38 diagnostic: log every import path + sequence number to compare
    // import-order vs tree-walker.
    static const bool s_dbg_import =
        std::getenv("V3_DBG_IMPORT") != nullptr;

    // #769 per-phase timing (V3_TIMING-gated; zero overhead when off).
    using ImpClock = std::chrono::steady_clock;
    static const bool s_impTimingEn = importTimingEnabled();
    auto impStamp = []() { return ImpClock::now(); };
    auto impBumpNs = [&](uint64_t & accum, ImpClock::time_point start) {
        if (!s_impTimingEn) return;
        accum += static_cast<uint64_t>(std::chrono::duration_cast<
            std::chrono::nanoseconds>(ImpClock::now() - start).count());
    };

    auto & cache = importCache();
    if (auto it = cache.results.find(path); it != cache.results.end()) {
        // REVIEW §2.5: validate stat (mtime, size) hasn't changed
        // since cache insert.  Daemons (Hydra / LSP) re-evaluate
        // edited files; CLIs see a microsecond stat overhead.
        auto [mtimeNs, sz] = importStat(path);
        if (sz >= 0 && mtimeNs == it->second.mtimeNs && sz == it->second.size) {
            if (s_dbg_import) {
                static std::atomic<uint64_t> seqHit{0};
                std::fprintf(stderr, "v3 IMPORT-HIT[%llu]: %s\n",
                    (unsigned long long)seqHit.fetch_add(1), path.c_str());
            }
            if (s_impTimingEn) ++importTimingTotals().resultCacheHits;
            // Phase 4b LRU (2026-05-30): mark this entry as recently used.
            bumpImportEntry(it->second, cache);
            out = it->second.result;
            return;
        }
        // Stat differs -- file changed.  Drop entry, re-evaluate.
        // Note: the prior CompilationUnit stays in cus (deque appends
        // never invalidate prior entries) so any closures referencing
        // it remain valid.  Memory grows linearly in changes -- daemons
        // that hot-reload heavily may want a periodic cus.clear()
        // between top-level evals (clearBridgeTables-style).
        if (s_dbg_import)
            std::fprintf(stderr,
                "v3 IMPORT-INVAL: %s (mtime/size changed)\n", path.c_str());
        cache.results.erase(it);
    }

    // #741 Phase 4 (2026-05-23) — disk-backed import-result cache.
    // Layers ABOVE the in-memory cache.results map for cross-process
    // replay.  On warm cache: deserialise the cached result Value,
    // populate in-memory cache, return — skips parse + lower + run.
    //
    // #741 Phase 4b: default-ON since 2026-05-24 (commit XXXXXXXXX).
    // Validation: 1.91× wall-positive on multi-IFD-heavy synthetic,
    // 1.82× on 1M-element single-IFD, wall-neutral on no-IFD
    // workloads (hello.name, hello.drvPath both within 0.5σ).  True-
    // COLD ≈ OFF (cold-tax is invisible at realistic IFD counts).
    // Opt-OUT via NIX_V3_NO_IFD_IMPORT_CACHE_DISK=1.  Docs:
    // lode/PHASE_4B_SCALE_TEST_2026-05-24.md +
    // lode/PHASE_4B_MULTI_IFD_2026-05-24.md.
    //
    // Key derivation: SHA-256("ifd-import\0" + path).  The trailing
    // null + namespace string keeps the key disjoint from
    // disk_cache's drvHash cache namespace (also via the EvalResults
    // table per Phase 5 substrate).
    //
    // Soundness: `path` is the resolved store-path-prefixed output
    // (post-realisePath), which IS content-addressed.  Two evals
    // producing the same path mean the same derivation's output is
    // being imported → same file bytes → same parsed expression →
    // same v3 Value.  No stat-check needed (store paths are immutable
    // by libstore invariant).
    //
    // Retirement criterion: 30 days of nightly nixpkgs CI without
    // regression on cardano-node M5 + haskell.nix smoke + standard
    // hello.drvPath/firefox.name workloads, then delete the opt-out
    // entirely.
    // T-7 (CODEBASE_REVIEW_2026-06-11): the blanket NIX_V3_NO_DISK_CACHE must
    // ALSO disable the IFD EvalResult disk cache — previously only the
    // narrower NIX_V3_NO_IFD_IMPORT_CACHE_DISK did, so a "cold cache" A/B run
    // with NIX_V3_NO_DISK_CACHE=1 still hit the IFD result cache and measured a
    // partially-warm run.
    static const bool s_ifdImportDiskCache =
        std::getenv("NIX_V3_NO_IFD_IMPORT_CACHE_DISK") == nullptr
        && std::getenv("NIX_V3_NO_DISK_CACHE") == nullptr;
    // #741 Phase 4b RCA fix: only consult the disk cache for ACTUAL
    // IFD imports (string-with-ctx or attrset arg).  Non-IFD imports
    // are literal-path nixpkgs files — they're already handled
    // efficiently by the in-memory `cache.results` map + the CU disk
    // cache; disk-caching their full result Value adds nothing and
    // explodes cache size + cold wall.
    // T-5 (CODEBASE_REVIEW_2026-06-11): key the IFD EvalResult disk cache on the
    // path's narHash (content), not the path alone.  An INPUT-addressed output
    // is not content-addressed in its path, so the same path can hold different
    // content after a non-deterministic rebuild; path-only keying then serves
    // stale content (and is unsafe in AOT snapshots across machines).  If the
    // narHash is unavailable, skip the disk cache entirely (safe: re-eval).
    std::optional<std::string> ifdNarHash;
    if (s_ifdImportDiskCache && isIfdImport)
        ifdNarHash = ffi::storePathNarHash(*state.nixEvalState, path);
    if (s_ifdImportDiskCache && isIfdImport && ifdNarHash) {
        CACHE_HOOK_DEFINE_SITE(siteImportIfdLookup,
            "primImport-ifd-disk-lookup");
        CacheHookTimer timer(siteImportIfdLookup);
        std::string keyBytes;
        keyBytes.reserve(12 + path.size() + ifdNarHash->size());
        keyBytes.append("ifd-import");
        keyBytes.push_back('\0');
        keyBytes.append(path);
        keyBytes.push_back('\0');
        keyBytes.append(*ifdNarHash);  // T-5: content hash
        auto diskKey = disk_cache::computeKeyForString(keyBytes);
        auto blob = disk_cache::lookupEvalResult(diskKey);
        if (blob) {
            cacheHookHit(siteImportIfdLookup);
            try {
                out = value_serialize::deserialize(*blob);
                // Populate in-memory cache so subsequent calls hit there.
                auto [mt, sz] = importStat(path);
                {
                    auto [iter, inserted] = cache.results.emplace(path,
                        ImportCacheEntry{out, mt, sz, 0});
                    if (inserted) bumpImportEntry(iter->second, cache);  // Phase 4b LRU
                }
                maybeEvictOldImportEntries(cache);  // Phase 4b LRU
                if (s_dbg_import) {
                    std::fprintf(stderr, "v3 IMPORT-DISK-HIT: %s\n",
                        path.c_str());
                }
                return;
            } catch (...) {
                // Deserialise failed (e.g. cached value uses tags we
                // can't round-trip — closures, primops).  Fall through
                // to fresh parse + eval.  The in-memory cache will
                // still be populated; disk insert below will re-write
                // (insert-or-ignore semantics in EvalResults).
                if (s_dbg_import)
                    std::fprintf(stderr,
                        "v3 IMPORT-DISK-DESERR: %s — falling through\n",
                        path.c_str());
            }
        } else {
            // #827 A3: blob == nullopt — disk-cache miss for this IFD key.
            // (The "hit then deserialise-fail" branch above also counts
            // as a miss semantically, but we keep its existing
            // fall-through behaviour — the eval re-runs and the new
            // insert overwrites.)
            cacheHookMiss(siteImportIfdLookup);
        }
    }
    // #755 instrumentation: RSS-per-import to localize which file
    // dominates the cumulative memory cost.  Same trigger as
    // V3_DBG_IMPORT for combined output.  Cheap (one task_info /
    // getrusage per import).
    auto rssMBImp = []() -> uint64_t {
#if defined(__APPLE__)
        mach_task_basic_info_data_t info;
        mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
        if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                      (task_info_t)&info, &count) == KERN_SUCCESS)
            return info.resident_size / (1024 * 1024);
#else
        struct rusage ru;
        if (getrusage(RUSAGE_SELF, &ru) == 0)
            return (uint64_t)ru.ru_maxrss / 1024;
#endif
        return 0;
    };
    if (s_dbg_import) {
        static std::atomic<uint64_t> seqMiss{0};
        std::fprintf(stderr, "v3 IMPORT-MISS[%llu] RSS=%lluMB: %s\n",
            (unsigned long long)seqMiss.fetch_add(1),
            (unsigned long long)rssMBImp(), path.c_str());
    }

    auto & ns = *state.nixEvalState;

    // Default: rootFS (the real filesystem under restricted-mode rules
    // + the augmented store accessor).  Use `resolveExprPath` to
    // follow symlink chains and append `default.nix` when the path is
    // a directory (matches tree-walker's import semantics).  If the
    // path isn't on the real FS (e.g. the `<nix/fetchurl.nix>`
    // corepkgs entry) we fall back to corepkgsFS.
    //
    // REVIEW §1.5: keep the resolved SourcePath around so the disk
    // cache key is computed from `sp.readFile()` (post-resolveExprPath)
    // rather than the raw user input.  Without this the cache key
    // skipped invalidation on `dir/default.nix` rewriting (the raw
    // `path` for a directory import points at a non-file).
    // #770b (2026-05-22): resolve the path and compute the cache key
    // FIRST, parse only on cache miss.  Previously primImport always
    // parsed before checking the disk cache; on a cache hit the parse
    // output was discarded (the cached CU is used instead) yet the
    // parse cost (~95 ms / 269 imports = 350 µs per file on
    // hello.drvPath) was still paid.  Splitting resolve+keyCompute
    // from parse lets cache hits skip parse entirely.
    nix::SourcePath resolvedSp{ns.rootFS, nix::CanonPath::root};
    bool haveResolved = false;
    bool useCorepkgs = false;
    std::string corepkgsPath;
    try {
        nix::SourcePath sp(ns.rootFS, nix::CanonPath(path));
        sp = nix::resolveExprPath(sp);
        resolvedSp = sp;
        haveResolved = true;
    } catch (...) {
        corepkgsPath = path;
        if (!corepkgsPath.empty() && corepkgsPath.front() == '/')
            corepkgsPath = corepkgsPath.substr(1);
        nix::SourcePath cp(ns.corepkgsFS.cast<nix::SourceAccessor>(),
                           nix::CanonPath(corepkgsPath));
        if (!cp.pathExists()) throw;
        resolvedSp = cp;
        haveResolved = true;
        useCorepkgs = true;
    }

    // VM-4: try the disk cache before lower+compile.  primImport is
    // the ideal integration point — direct access to source path
    // and content; serialized CUs round-trip through the SymbolId
    // remap in serialize::deserializeCU.
    //
    // #777 promotion (2026-05-23): default-ON.  Hyperfine shows
    // -174 ms / -13 % on hello.drvPath after #777's zero-copy
    // symbolTable read; nixpkgs 64-package sweep -16 s (-10.7 %).
    // Pre-#777 was within noise; the deserialize cost is now
    // small enough that the compile savings dominate.  Retirement
    // criterion: if a future commit regresses cache wall-clock to
    // ≤ default + 1 σ on either hello.drvPath or the 64-pkg sweep,
    // flip back to opt-in (gate name re-becomes NIX_V3_DISK_CACHE
    // = enable; rename NIX_V3_NO_DISK_CACHE → drop).
    static const bool diskCacheEnabled =
        std::getenv("NIX_V3_NO_DISK_CACHE") == nullptr;
    // #495 follow-on: content-cache (in-memory) is default-on.  Compute
    // the content hash whenever EITHER cache is enabled.  Content
    // cache lets v3EvalEntry's later lookup with a fresh-parse Expr*
    // hit and skip re-lowering -- the trigger for the broader-thunkify
    // upvalue bug.
    static const bool contentCacheEnabled =
        std::getenv("NIX_V3_NO_CONTENT_CACHE") == nullptr;
    disk_cache::CacheKey diskKey{};
    if ((diskCacheEnabled || contentCacheEnabled) && haveResolved) {
        // REVIEW §1.5: hash the resolved file content (post-symlink,
        // post-default.nix rewriting), not the raw input path.  Symlink
        // retargeting + dir/default.nix selection both invalidate
        // correctly because the read path changes the hash input.
        //
        // #815 RCA fix (Light variant, 2026-05-25): ALSO mix the resolved
        // source PATH into the key.  Two store paths can hold identical
        // file content (e.g., the same nixpkgs checkout copied into two
        // /nix/store/<hash>-source entries — common when a workload
        // pins nixpkgs via flake.lock and another workload uses a
        // separate getFlake "nixpkgs" snapshot).  Path-literals in the
        // source (`./foo.patch`) are RESOLVED AT COMPILE TIME against
        // the containing file's directory and baked into the bytecode
        // as absolute store paths.  Identical-content files at different
        // directories therefore produce DIFFERENT BYTECODE — but with
        // content-only keying they collide in the cache, and a cache
        // HIT on one workload's CU is incorrect when loaded under the
        // other workload's path resolution.
        //
        // The observed symptom (haskell-nix-example after a 5-pkg
        // nixpkgs sweep) was a cache HIT on jq/package.nix producing
        // path strings pointing at the sweep's nixpkgs store path
        // (`/nix/store/77db...`) while HNE's evaluator expected the
        // path-literals to resolve under HNE's nixpkgs store path
        // (`/nix/store/lsdw...`).  The mismatch propagated into a
        // call-site argument list and surfaced as "function called
        // with unexpected argument 'git'" several files later.
        //
        // The cache itself is still content-addressed for the bulk
        // of CUs (same path → same key); we only invalidate when the
        // path differs.  This is the correct behaviour: bytecode is
        // path-dependent today, so cache key must be too.
        auto tKey = impStamp();
        try {
            std::string content = resolvedSp.resolveSymlinks().readFile();
            // Mix in the resolved source path's string representation
            // (post-symlink-resolve, so a symlink retargeting still
            // produces the same key as the symlink target).  We use a
            // length-prefixed concatenation so distinct (path, content)
            // pairs cannot alias into the same hash input.
            std::string pathStr = resolvedSp.resolveSymlinks().path.abs();
            std::string keyBytes;
            keyBytes.reserve(pathStr.size() + content.size() + sizeof(uint64_t));
            const uint64_t pathLen = pathStr.size();
            keyBytes.append(reinterpret_cast<const char *>(&pathLen),
                            sizeof(pathLen));
            keyBytes.append(pathStr);
            keyBytes.append(content);
            // T-1 (CODEBASE_REVIEW_2026-06-11): mix in the codegen env-gate
            // fingerprint so a CU compiled under e.g. NIX_V3_NO_DEFER=1 lands in
            // a SEPARATE cache namespace from a default-codegen CU (empty
            // fingerprint in production → key unchanged).  Without this a bisect
            // gate poisons every later warm run and vice versa.
            keyBytes.append(codegenGateFingerprint());
            // PARSER_PROJECT_PLAN §5.3: native lowering is now the ONLY
            // path, so the CU disk cache is consistent again (all CUs are
            // native-lowered) and re-enabled.  The key is SOURCE content +
            // path; the lowerer "version" is carried by
            // serialize::kSchemaVersion — BUMP IT on any incompatible
            // native-lowering change (the operating rule that previously
            // covered lower.cc), else a stale CU from an older lowerer
            // could be restored (the qtbase.drvPath warm-cache divergence).
            diskKey = disk_cache::computeKeyForString(keyBytes);
        } catch (...) {
            // Read failure -> empty key -> cache lookup is skipped,
            // and no insert happens later.  Same fallback as before.
        }
        impBumpNs(importTimingTotals().keyComputeNs, tKey);
    }
    // #815 RCA diagnostic (2026-05-25): V3_DBG_DISK_CACHE_LOG=path
    // logs each lookup (hit/miss) and insert with the source path.
    // Used to identify shared CUs between two workloads when
    // diagnosing the cross-workload cache regression.  Format:
    // "HIT key=hex path=...\n" or "MISS key=hex path=...\n" or
    // "INSERT key=hex path=...\n".  Append-mode so multiple
    // processes don't clobber each other.
    //
    // V3_DBG_DISK_CACHE_BLOCK=hex1,hex2,...  blocks specific cache
    // keys from being read (lookup returns nullopt, forcing fresh
    // compile).  Used to bisect which cache entry triggers a
    // cross-workload divergence.  Hex prefix matching (substring
    // is acceptable; minimum 8 hex chars).
    static const char * s_diskCacheLog = std::getenv("V3_DBG_DISK_CACHE_LOG");
    static const char * s_diskCacheBlock = std::getenv("V3_DBG_DISK_CACHE_BLOCK");
    auto logCacheEvent = [&](const char * tag, const disk_cache::CacheKey & k) {
        if (!s_diskCacheLog) return;
        FILE * f = std::fopen(s_diskCacheLog, "a");
        if (!f) return;
        std::fprintf(f, "%s key=%s path=%s\n", tag, k.hex().c_str(), path.c_str());
        std::fclose(f);
    };
    auto isKeyBlocked = [&](const disk_cache::CacheKey & k) -> bool {
        if (!s_diskCacheBlock || !*s_diskCacheBlock) return false;
        std::string hex = k.hex();
        const char * cur = s_diskCacheBlock;
        while (*cur) {
            const char * end = std::strchr(cur, ',');
            size_t len = end ? size_t(end - cur) : std::strlen(cur);
            if (len > 0 && len <= hex.size()
                && std::strncmp(hex.c_str(), cur, len) == 0)
                return true;
            if (!end) break;
            cur = end + 1;
        }
        return false;
    };
    if (diskCacheEnabled && !diskKey.empty()) {
        if (isKeyBlocked(diskKey)) {
            logCacheEvent("BLOCK", diskKey);
            // Fall through to fresh compile path.
            goto skipDiskCacheLookup;
        }
        CACHE_HOOK_DEFINE_SITE(siteCuLookup, "primImport-cu-disk-lookup");
        CacheHookTimer cuTimer(siteCuLookup);
        auto tLookup = impStamp();
        auto blob = disk_cache::lookup(diskKey);
        impBumpNs(importTimingTotals().diskLookupNs, tLookup);
        if (blob) {
            cacheHookHit(siteCuLookup);
            logCacheEvent("HIT", diskKey);
            try {
                auto tDes = impStamp();
                cache.cus.push_back(serialize::deserializeCU(*blob));
                impBumpNs(importTimingTotals().deserializeNs, tDes);

                // #815 RCA: V3_DBG_DESERIALIZE_VERIFY=path forks a side
                // path that ALSO fresh-compiles the same source in this
                // process and compares the deserialized CU's bytecode
                // to the fresh CU's bytecode byte-by-byte (post-remap).
                // Any divergence reveals a non-deterministic shape that
                // survives serialize+deserialize but produces different
                // bytecode cross-process.  Output goes to <path> as one
                // line per diverging entry: "DIFF path=X cached_size=N
                // fresh_size=M first_diff_ip=K cached=0x... fresh=0x...".
                static const char * s_verifyLog =
                    std::getenv("V3_DBG_DESERIALIZE_VERIFY");
                if (s_verifyLog) {
                    try {
                        // Fresh re-compile via the SAME native path the
                        // cached CU was produced by (so the byte-compare is
                        // apples-to-apples post-Stage-2 flip).
                        nix::v3::ast::ParserState v3st2;
                        std::string text2;
                        std::optional<nix::PosTable::Origin> origin2;
                        if (useCorepkgs) {
                            nix::SourcePath cp(ns.corepkgsFS.cast<nix::SourceAccessor>(),
                                               nix::CanonPath(corepkgsPath));
                            text2 = cp.resolveSymlinks().readFile();
                            if (auto par = cp.path.parent()) v3st2.basePath = par->abs();
                            v3st2.homePath = s_homePath;
                            nix::v3::parser::parseString(v3st2, text2);
                            origin2.emplace(ns.positions.addOrigin(nix::Pos::Origin(cp), text2.size()));
                        } else {
                            text2 = resolvedSp.resolveSymlinks().readFile();
                            if (auto par = resolvedSp.path.parent()) v3st2.basePath = par->abs();
                            v3st2.homePath = s_homePath;
                            nix::v3::parser::parseString(v3st2, text2);
                            origin2.emplace(ns.positions.addOrigin(nix::Pos::Origin(resolvedSp), text2.size()));
                        }
                        static const bool s_dbg815v =
                            std::getenv("V3_DBG_815_FUNCID") != nullptr;
                        if (s_dbg815v) std::fprintf(stderr,
                            "v3 FUNCID --- begin verify path=%s ---\n",
                            path.c_str());
                        auto module2 = nix::v3::lowerV3Ast(ns.symbols, v3st2.result,
                            &ns.positions, *origin2, &nix::v3::twBaseEnvGlobals(ns));
                        nix::v3::ir::optimise(module2);
                        nix::v3::ir::computeFreeVars(module2);
                        // R1 trigger trace mirror: see main-path dump above.
                        {
                            static const char * s_dumpPathV =
                                std::getenv("V3_DBG_DUMP_IR_PATH");
                            if (s_dumpPathV && *s_dumpPathV
                                && path.size() >= std::strlen(s_dumpPathV)
                                && path.compare(path.size() - std::strlen(s_dumpPathV),
                                                std::strlen(s_dumpPathV), s_dumpPathV) == 0) {
                                std::fprintf(stderr,
                                    "v3 IR-DUMP --- verify path=%s ---\n%s"
                                    "v3 IR-DUMP --- end verify path=%s ---\n",
                                    path.c_str(),
                                    nix::v3::ir::dumpModule(module2).c_str(),
                                    path.c_str());
                            }
                        }
                        auto cu2 = compile(module2);
                        if (s_dbg815v) std::fprintf(stderr,
                            "v3 FUNCID --- end   verify path=%s ---\n",
                            path.c_str());
                        auto & cu1 = cache.cus.back();
                        FILE * vf = std::fopen(s_verifyLog, "a");
                        if (vf) {
                            bool sameCode = (cu1.code == cu2.code);
                            bool sameLambdas = (cu1.lambdas.size() == cu2.lambdas.size());
                            // #815 RCA: also compare per-FuncId NAMES.
                            // If writer and reader allocate FuncIds to
                            // DIFFERENT named bindings, that's the bug.
                            size_t nameDiffs = 0;
                            size_t firstNameDiff = SIZE_MAX;
                            // #815 follow-on: NAME equality is necessary
                            // but NOT sufficient — many lambdas share
                            // names ("<thunk>", "<formals>", etc.).  Two
                            // cu.lambdas[K] entries with the same name
                            // can differ in nUpvalues / nLocals /
                            // hasFormals / ellipsis / formals.size() /
                            // formals[].name — those are STRUCTURAL
                            // mismatches that prove FuncIds map to
                            // DIFFERENT source entities cross-process.
                            // Track per-field diff counters.
                            size_t sigDiffs = 0, formalCountDiffs = 0,
                                   formalNameDiffs = 0, nUpvDiffs = 0,
                                   nLocDiffs = 0, hasFormalsDiffs = 0,
                                   ellipsisDiffs = 0;
                            size_t firstSigDiff = SIZE_MAX;
                            size_t minLam = std::min(cu1.lambdas.size(),
                                                     cu2.lambdas.size());
                            // #815: cu.symbolTable is left EMPTY post-#781b
                            // (both serialize and emit no longer populate
                            // it; runtime reads names from the v3 global
                            // symbol table directly).  So resolve names
                            // through the GLOBAL table — after remap on
                            // the deserialize side, every SymbolId in
                            // cu1's structures is a valid reader-process
                            // global SymbolId.  cu2 (fresh compiled in the
                            // same reader process) is likewise indexed
                            // into the global table.
                            const auto & gst = nix::v3::ir::globalSymbolTable();
                            auto symName = [&](const v3::CompilationUnit & cu,
                                               uint32_t sid) -> const char * {
                                (void)cu;
                                if (sid < gst.size())
                                    return gst[sid].c_str();
                                return "<oor>";
                            };
                            for (size_t i = 0; i < minLam; ++i) {
                                const auto & la = cu1.lambdas[i];
                                const auto & lb = cu2.lambdas[i];
                                if (la.name != lb.name) {
                                    if (firstNameDiff == SIZE_MAX) firstNameDiff = i;
                                    nameDiffs++;
                                }
                                bool thisSigDiff = false;
                                if (la.nUpvalues != lb.nUpvalues) { nUpvDiffs++; thisSigDiff = true; }
                                if (la.nLocals != lb.nLocals)     { nLocDiffs++; thisSigDiff = true; }
                                if (la.hasFormals != lb.hasFormals){ hasFormalsDiffs++; thisSigDiff = true; }
                                if (la.ellipsis != lb.ellipsis)   { ellipsisDiffs++; thisSigDiff = true; }
                                if (la.formals.size() != lb.formals.size()) {
                                    formalCountDiffs++; thisSigDiff = true;
                                } else {
                                    // Same formal count — compare each formal name via
                                    // the per-process symbolTable (NOT raw SymbolId,
                                    // which is process-local).
                                    bool posOrderDiff = false;
                                    for (size_t fi = 0; fi < la.formals.size(); ++fi) {
                                        const char * an = symName(cu1, la.formals[fi].name);
                                        const char * bn = symName(cu2, lb.formals[fi].name);
                                        if (std::strcmp(an, bn) != 0) {
                                            posOrderDiff = true;
                                            break;
                                        }
                                    }
                                    if (posOrderDiff) {
                                        // ORDER differs.  Check whether the SET still matches.
                                        std::vector<std::string> aSet, bSet;
                                        aSet.reserve(la.formals.size());
                                        bSet.reserve(lb.formals.size());
                                        for (auto & f : la.formals) aSet.emplace_back(symName(cu1, f.name));
                                        for (auto & f : lb.formals) bSet.emplace_back(symName(cu2, f.name));
                                        std::sort(aSet.begin(), aSet.end());
                                        std::sort(bSet.begin(), bSet.end());
                                        if (aSet != bSet) {
                                            formalNameDiffs++; thisSigDiff = true;
                                        }
                                        // If set matches but order differs, it's
                                        // benign (linear-search validation works).
                                        // Still record as a SOFT diff for visibility.
                                        else {
                                            // ORDER-ONLY diff — log but don't count
                                            // as a structural sig diff.
                                        }
                                    }
                                }
                                if (thisSigDiff) {
                                    sigDiffs++;
                                    if (firstSigDiff == SIZE_MAX) firstSigDiff = i;
                                }
                            }
                            std::fprintf(vf, "VERIFY path=%s code=%s "
                                "(cached=%zu fresh=%zu) lambdas=%s "
                                "(cached=%zu fresh=%zu) name_diffs=%zu/%zu "
                                "sig_diffs=%zu (nUpv=%zu nLoc=%zu hasFormals=%zu "
                                "ellipsis=%zu formalCount=%zu formalName=%zu)",
                                path.c_str(),
                                sameCode ? "SAME" : "DIFF",
                                cu1.code.size(), cu2.code.size(),
                                sameLambdas ? "SAME" : "DIFF",
                                cu1.lambdas.size(), cu2.lambdas.size(),
                                nameDiffs, minLam,
                                sigDiffs, nUpvDiffs, nLocDiffs, hasFormalsDiffs,
                                ellipsisDiffs, formalCountDiffs, formalNameDiffs);
                            if (firstNameDiff != SIZE_MAX) {
                                std::fprintf(vf,
                                    " (first name diff at fid=%zu: cached='%s' fresh='%s')",
                                    firstNameDiff,
                                    cu1.lambdas[firstNameDiff].name.c_str(),
                                    cu2.lambdas[firstNameDiff].name.c_str());
                            }
                            if (firstSigDiff != SIZE_MAX) {
                                const auto & la = cu1.lambdas[firstSigDiff];
                                const auto & lb = cu2.lambdas[firstSigDiff];
                                std::fprintf(vf,
                                    " (first sig diff at fid=%zu name=cached'%s'/fresh'%s' "
                                    "nUpv=%u/%u nLoc=%u/%u hasFormals=%u/%u "
                                    "ellipsis=%u/%u formals=%zu/%zu)",
                                    firstSigDiff,
                                    la.name.c_str(), lb.name.c_str(),
                                    (unsigned)la.nUpvalues, (unsigned)lb.nUpvalues,
                                    (unsigned)la.nLocals,   (unsigned)lb.nLocals,
                                    (unsigned)la.hasFormals,(unsigned)lb.hasFormals,
                                    (unsigned)la.ellipsis,  (unsigned)lb.ellipsis,
                                    la.formals.size(),      lb.formals.size());
                                // If formal counts match, dump per-formal name diff.
                                if (la.formals.size() == lb.formals.size()) {
                                    for (size_t fi = 0; fi < la.formals.size(); ++fi) {
                                        const char * an = symName(cu1, la.formals[fi].name);
                                        const char * bn = symName(cu2, lb.formals[fi].name);
                                        if (std::strcmp(an, bn) != 0) {
                                            std::fprintf(vf,
                                                " formal[%zu]: cached='%s' fresh='%s'",
                                                fi, an, bn);
                                            break;
                                        }
                                    }
                                }
                            }
                            // #815 deeper: compare CONTENTS (not just
                            // counts) of constant pools.  Order matters
                            // because OP_LIT_STR / OP_LIT_INT_BIG /
                            // OP_LIT_FLOAT / OP_CALL_PRIMOP use the
                            // index into the pool as the operand.  Pool
                            // order is determined by emit-encounter
                            // order; if any process-local data influences
                            // the encounter order (Map iteration, etc.),
                            // pools diverge.
                            bool sameStrs = (cu1.stringConstants == cu2.stringConstants);
                            bool sameInts = (cu1.intConstants == cu2.intConstants);
                            bool sameFloats = (cu1.floatConstants == cu2.floatConstants);
                            bool samePrims = (cu1.primops.size() == cu2.primops.size());
                            if (samePrims) {
                                for (size_t pi = 0; pi < cu1.primops.size(); ++pi) {
                                    if (cu1.primops[pi] != cu2.primops[pi]) {
                                        samePrims = false; break;
                                    }
                                }
                            }
                            std::fprintf(vf, " ints=%zu/%zu(%s) strs=%zu/%zu(%s) "
                                "floats=%zu/%zu(%s) prims=%zu/%zu(%s)\n",
                                cu1.intConstants.size(), cu2.intConstants.size(),
                                sameInts ? "SAME" : "DIFF",
                                cu1.stringConstants.size(), cu2.stringConstants.size(),
                                sameStrs ? "SAME" : "DIFF",
                                cu1.floatConstants.size(), cu2.floatConstants.size(),
                                sameFloats ? "SAME" : "DIFF",
                                cu1.primops.size(), cu2.primops.size(),
                                samePrims ? "SAME" : "DIFF");
                            if (!sameStrs) {
                                // Dump first stringConstant diff.
                                size_t minS = std::min(cu1.stringConstants.size(),
                                                       cu2.stringConstants.size());
                                for (size_t si = 0; si < minS; ++si) {
                                    if (cu1.stringConstants[si] != cu2.stringConstants[si]) {
                                        std::fprintf(vf,
                                            "  first string diff at idx=%zu: cached='%.80s' fresh='%.80s'\n",
                                            si,
                                            cu1.stringConstants[si]->c_str(),  // M-10: interned ptr
                                            cu2.stringConstants[si]->c_str());
                                        break;
                                    }
                                }
                            }
                            if (!sameCode) {
                                size_t minN = std::min(cu1.code.size(), cu2.code.size());
                                size_t firstDiff = SIZE_MAX;
                                for (size_t i = 0; i < minN; ++i) {
                                    if (cu1.code[i] != cu2.code[i]) {
                                        firstDiff = i; break;
                                    }
                                }
                                std::fprintf(vf, "  first_diff_ip=%zu",
                                    firstDiff == SIZE_MAX ? minN : firstDiff);
                                if (firstDiff != SIZE_MAX) {
                                    Op opC = decodeOp(cu1.code[firstDiff]);
                                    Op opF = decodeOp(cu2.code[firstDiff]);
                                    uint32_t arC = decodeOperand(cu1.code[firstDiff]);
                                    uint32_t arF = decodeOperand(cu2.code[firstDiff]);
                                    std::fprintf(vf,
                                        " cached={%s(0x%02x), arg=%u} fresh={%s(0x%02x), arg=%u}",
                                        opName(opC), (unsigned)opC, arC,
                                        opName(opF), (unsigned)opF, arF);
                                }
                                std::fprintf(vf, "\n");
                                // Disassemble +-3 instructions around firstDiff in BOTH.
                                if (firstDiff != SIZE_MAX) {
                                    uint32_t lo = firstDiff > 6 ? (uint32_t)(firstDiff - 6) : 0;
                                    uint32_t hiC = (uint32_t)std::min<size_t>(cu1.code.size(), firstDiff + 8);
                                    uint32_t hiF = (uint32_t)std::min<size_t>(cu2.code.size(), firstDiff + 8);
                                    std::fprintf(vf, "  cached-disasm [%u..%u]:\n", lo, hiC);
                                    disassembleWindow(vf, cu1, lo, hiC);
                                    std::fprintf(vf, "  fresh-disasm [%u..%u]:\n", lo, hiF);
                                    disassembleWindow(vf, cu2, lo, hiF);
                                }
                                // #815 deep walker: classify the diff bytes
                                // as SymbolId-only (benign — global symbol
                                // table permuted) vs SEMANTIC (different
                                // names / operands at the same offset).
                                // Walks both code streams in lock-step
                                // through the same OP_* dispatcher used by
                                // remapSymbolsInBytecode.
                                if (cu1.code.size() == cu2.code.size()) {
                                    size_t opDiffs = 0;        // opcode itself differs
                                    size_t symIdDiffs = 0;     // SymbolId VALUES differ but string matches
                                    size_t symStrDiffs = 0;    // SymbolId-resolved STRINGS differ
                                    size_t otherDiffs = 0;     // non-SymbolId operand diffs
                                    size_t firstSymStrDiff = SIZE_MAX;
                                    uint32_t firstSymStrA = 0, firstSymStrB = 0;
                                    auto resolve = [&](uint32_t sid) -> std::string {
                                        if (sid < gst.size()) return gst[sid];
                                        return std::string("<sid?") + std::to_string(sid) + ">";
                                    };
                                    auto compareSymOperand = [&](size_t ip, uint32_t a, uint32_t b) {
                                        if (a == b) return;
                                        std::string sa = resolve(a);
                                        std::string sb = resolve(b);
                                        if (sa != sb) {
                                            ++symStrDiffs;
                                            if (firstSymStrDiff == SIZE_MAX) {
                                                firstSymStrDiff = ip;
                                                firstSymStrA = a;
                                                firstSymStrB = b;
                                            }
                                        } else {
                                            ++symIdDiffs;
                                        }
                                    };
                                    size_t ipA = 0, ipB = 0;
                                    const size_t N = cu1.code.size();
                                    size_t firstOpDiffIp = SIZE_MAX;
                                    Op firstOpDiffA = OP_HALT, firstOpDiffB = OP_HALT;
                                    while (ipA < N && ipB < N) {
                                        uint32_t wA = cu1.code[ipA], wB = cu2.code[ipB];
                                        Op opA = decodeOp(wA), opB = decodeOp(wB);
                                        uint32_t arA = decodeOperand(wA), arB = decodeOperand(wB);
                                        if (opA != opB) {
                                            ++opDiffs;
                                            firstOpDiffIp = ipA;
                                            firstOpDiffA = opA; firstOpDiffB = opB;
                                            // give up on this stream — opcodes
                                            // diverged, can't continue lock-step.
                                            break;
                                        }
                                        ipA++; ipB++;
                                        // Mirror remapSymbolsInBytecode's logic
                                        // for which operands are SymbolIds.
                                        #pragma GCC diagnostic push
                                        #pragma GCC diagnostic ignored "-Wswitch-enum"
                                        switch (opA) {
                                        case OP_ATTRS_HAS:
                                        case OP_WITH_LOOKUP:
                                            compareSymOperand(ipA - 1, arA, arB);
                                            break;
                                        case OP_ATTRS_SELECT:
                                            compareSymOperand(ipA - 1, arA, arB);
                                            ipA++; ipB++;  // IC follow-up
                                            break;
                                        case OP_REC_BINDING_SLOT_REF:
                                            compareSymOperand(ipA - 1, arA, arB);
                                            ipA++; ipB++;  // IC follow-up
                                            break;
                                        case OP_GET_UPVALUE_REC_BINDING:
                                            // §2(b): operand is the SymbolId
                                            // (compare like RBSR); trailer is
                                            // [upvalIdx, icIdx] — skip both.
                                            compareSymOperand(ipA - 1, arA, arB);
                                            ipA += 2; ipB += 2;
                                            break;
                                        case OP_GET_UPVALUE_REC_BINDING_SLOT:
                                            // item 5a: operand is the SymbolId;
                                            // trailer [dst, upvalIdx, icIdx] —
                                            // skip 3.
                                            compareSymOperand(ipA - 1, arA, arB);
                                            ipA += 3; ipB += 3;
                                            break;
                                        case OP_R_PRIMOP2:
                                            // reg-VM: operand=poIdx (not a
                                            // symbol); trailer [dst, descAB] —
                                            // skip both, no symbol compare.
                                            ipA += 2; ipB += 2;
                                            break;
                                        case OP_R_BRANCH_FALSE:
                                            // reg-VM: operand=target; trailer
                                            // [cond_slot] — skip, no symbol.
                                            ipA++; ipB++;
                                            break;
                                        case OP_R_CALL:
                                            // reg-VM: operand=dst slot; trailer
                                            // [(callee<<12)|arg] — skip, no symbol.
                                            ipA++; ipB++;
                                            break;
                                        case OP_R_STR_CONCAT2:
                                            // reg-VM: operand=dst slot; trailer
                                            // [(forceStr<<24)|(a<<12)|b] — skip.
                                            ipA++; ipB++;
                                            break;
                                        case OP_ATTRS_INIT: {
                                            uint32_t n = arA;
                                            for (uint32_t i = 0; i < n; ++i) {
                                                if (ipA < N && ipB < N)
                                                    compareSymOperand(ipA, cu1.code[ipA], cu2.code[ipB]);
                                                // skip pos
                                                if (ipA + 1 < N && ipB + 1 < N
                                                    && cu1.code[ipA + 1] != cu2.code[ipB + 1])
                                                    ++otherDiffs;
                                                ipA += 2; ipB += 2;
                                            }
                                            break;
                                        }
                                        case OP_ATTRS_INIT_DYN: {
                                            uint32_t nStatic = (arA >> 12) & 0xFFFu;
                                            uint32_t nDyn    =  arA        & 0xFFFu;
                                            for (uint32_t i = 0; i < nStatic; ++i) {
                                                if (ipA < N && ipB < N)
                                                    compareSymOperand(ipA, cu1.code[ipA], cu2.code[ipB]);
                                                if (ipA + 1 < N && ipB + 1 < N
                                                    && cu1.code[ipA + 1] != cu2.code[ipB + 1])
                                                    ++otherDiffs;
                                                ipA += 2; ipB += 2;
                                            }
                                            ipA += nDyn; ipB += nDyn;
                                            break;
                                        }
                                        case OP_ATTRS_REC_INIT:
                                        case OP_ATTRS_LET_REC_INIT:
                                        case OP_ATTRS_REC_INIT_TAIL: {
                                            uint32_t n = arA;
                                            for (uint32_t i = 0; i < n; ++i) {
                                                if (ipA < N && ipB < N)
                                                    compareSymOperand(ipA, cu1.code[ipA], cu2.code[ipB]);
                                                if (ipA + 1 < N && ipB + 1 < N
                                                    && cu1.code[ipA + 1] != cu2.code[ipB + 1])
                                                    ++otherDiffs;
                                                ipA += 2; ipB += 2;
                                            }
                                            break;
                                        }
                                        case OP_POS: {
                                            // PosIdx operand — process-local;
                                            // not a SymbolId.  Track as benign.
                                            if (arA != arB) ++otherDiffs;
                                            break;
                                        }
                                        default:
                                            if (arA != arB) ++otherDiffs;
                                            break;
                                        }
                                        #pragma GCC diagnostic pop
                                    }
                                    std::fprintf(vf,
                                        "  walk-result: opDiffs=%zu symStrDiffs=%zu "
                                        "symIdDiffs=%zu otherDiffs=%zu",
                                        opDiffs, symStrDiffs, symIdDiffs, otherDiffs);
                                    if (firstSymStrDiff != SIZE_MAX) {
                                        std::fprintf(vf,
                                            " (first symStr diff at ip=%zu: cached='%s'(%u) fresh='%s'(%u))",
                                            firstSymStrDiff,
                                            resolve(firstSymStrA).c_str(), firstSymStrA,
                                            resolve(firstSymStrB).c_str(), firstSymStrB);
                                    }
                                    if (firstOpDiffIp != SIZE_MAX) {
                                        std::fprintf(vf,
                                            " (first op diff at ip=%zu: cached=%s(0x%02x) fresh=%s(0x%02x))",
                                            firstOpDiffIp,
                                            opName(firstOpDiffA), (unsigned)firstOpDiffA,
                                            opName(firstOpDiffB), (unsigned)firstOpDiffB);
                                        // Dump a window around the op diff.
                                        std::fprintf(vf, "\n  cached-disasm around op-diff [%zu..%zu]:\n",
                                            firstOpDiffIp > 6 ? firstOpDiffIp - 6 : 0,
                                            std::min<size_t>(cu1.code.size(), firstOpDiffIp + 8));
                                        disassembleWindow(vf, cu1,
                                            firstOpDiffIp > 6 ? (uint32_t)(firstOpDiffIp - 6) : 0,
                                            (uint32_t)std::min<size_t>(cu1.code.size(), firstOpDiffIp + 8));
                                        std::fprintf(vf, "  fresh-disasm around op-diff [%zu..%zu]:\n",
                                            firstOpDiffIp > 6 ? firstOpDiffIp - 6 : 0,
                                            std::min<size_t>(cu2.code.size(), firstOpDiffIp + 8));
                                        disassembleWindow(vf, cu2,
                                            firstOpDiffIp > 6 ? (uint32_t)(firstOpDiffIp - 6) : 0,
                                            (uint32_t)std::min<size_t>(cu2.code.size(), firstOpDiffIp + 8));
                                    }
                                    std::fprintf(vf, "\n");
                                }
                            }
                            std::fclose(vf);
                        }
                    } catch (const std::exception & ev) {
                        FILE * vf = std::fopen(s_verifyLog, "a");
                        if (vf) {
                            std::fprintf(vf, "VERIFY-FAIL path=%s err=%s\n",
                                path.c_str(), ev.what());
                            std::fclose(vf);
                        }
                    }
                }

                auto tRun = impStamp();
                out = run(cache.cus.back());
                impBumpNs(importTimingTotals().runNs, tRun);
                if (s_impTimingEn) ++importTimingTotals().diskCacheHits;
                auto [mt, sz] = importStat(path);
                {
                    auto [iter, inserted] = cache.results.emplace(path,
                        ImportCacheEntry{out, mt, sz, 0});
                    if (inserted) bumpImportEntry(iter->second, cache);  // Phase 4b LRU
                }
                maybeEvictOldImportEntries(cache);  // Phase 4b LRU
                return;
            } catch (const std::exception & ex) {
                cache.cus.pop_back();
                // Phase-13 review: a corrupt or stale disk-cache blob
                // (e.g., a CRIT-1-class SymbolId remap miss surfacing as
                // "name not found") would silently fall back to a fresh
                // lower+compile and mask the regression.  V3_STRICT_DISK_CACHE=1
                // re-throws, surfacing the fault in tests.
                static const bool strict =
                    std::getenv("V3_STRICT_DISK_CACHE") != nullptr;
                if (strict) {
                    throw std::runtime_error(
                        std::string("v3 disk-cache restore failed for ")
                            + path + ": " + ex.what());
                }
                // Fall through to fresh lower+compile.
            }
        } else {
            logCacheEvent("MISS", diskKey);
            cacheHookMiss(siteCuLookup);
        }
    }
skipDiskCacheLookup:

    // #755 fix: scope `module` tightly so its std::vector<Block> /
    // std::vector<Function> heap storage is freed BEFORE we recurse
    // into `run()` — which may transitively call `primImport` again
    // (cardano-node M5 has 52+ nested imports of nixpkgs.lib files;
    // each nested call would otherwise keep its own `module` on the
    // C++ stack with all its IR vectors live in libc malloc).
    // After `compile(module)` produces the CU, the bytecode is self-
    // contained and the IR is no longer needed.  The CU itself stays
    // alive in the cache.cus deque.  The parse lives in this scope too
    // (#770b: deferred past the disk-cache lookup so a hit skips it) so
    // the native AST arena (v3st) is freed alongside the IR, before run().
    {
        // -- parse + lower (always native; PARSER_PROJECT_PLAN §5.3) --
        // The imported file is parsed by the v3-native parser and lowered
        // directly to IR — no nix::Expr.  Relative (`./x`) / home (`~/x`)
        // path literals resolve against the file's dir / $HOME, exactly as
        // TW's parseExprFromFile does; the PosTable::Origin makes
        // attr/formal + __curPos positions byte-match TW.  canLowerV3 is
        // total for parser-produced ASTs (the throw is a should-never-fire
        // guard).  corepkgs reads from the virtual corepkgsFS accessor.
        auto tParse = impStamp();
        nix::v3::ast::ParserState v3st;           // owns the native AST
        std::string text;
        std::optional<nix::PosTable::Origin> nativeOrigin;
        if (useCorepkgs) {
            nix::SourcePath cp(ns.corepkgsFS.cast<nix::SourceAccessor>(),
                               nix::CanonPath(corepkgsPath));
            text = cp.resolveSymlinks().readFile();
            if (auto par = cp.path.parent()) v3st.basePath = par->abs();
            v3st.homePath = s_homePath;
            nix::v3::parser::parseString(v3st, text);
            nativeOrigin.emplace(ns.positions.addOrigin(nix::Pos::Origin(cp), text.size()));
        } else {
            text = resolvedSp.resolveSymlinks().readFile();
            if (auto par = resolvedSp.path.parent()) v3st.basePath = par->abs();
            v3st.homePath = s_homePath;
            nix::v3::parser::parseString(v3st, text);
            nativeOrigin.emplace(ns.positions.addOrigin(
                nix::Pos::Origin(resolvedSp), text.size()));
        }
        if (!nix::v3::canLowerV3(v3st.result))
            throw nix::Error("v3 import: native lowering cannot handle " + path);
        ++importTimingTotals().nativeLowered;
        impBumpNs(importTimingTotals().parseNs, tParse);

        if (s_dbg_import) std::fprintf(stderr,
            "v3 IMPORT-PHASE before-lower [native] RSS=%lluMB: %s\n",
            (unsigned long long)rssMBImp(), path.c_str());

        // -- lower --------------------------------------------------
        if (s_impTimingEn) ++importTimingTotals().calls;
        auto tLower = impStamp();
        auto module = nix::v3::lowerV3Ast(ns.symbols, v3st.result, &ns.positions,
                                          *nativeOrigin, &nix::v3::twBaseEnvGlobals(ns));
        impBumpNs(importTimingTotals().lowerNs, tLower);
        if (s_dbg_import) std::fprintf(stderr,
            "v3 IMPORT-PHASE after-lower RSS=%lluMB: %s\n",
            (unsigned long long)rssMBImp(), path.c_str());
        auto tOpt = impStamp();
        nix::v3::ir::optimise(module);
        if (s_dbg_import) std::fprintf(stderr,
            "v3 IMPORT-PHASE after-optimise RSS=%lluMB: %s\n",
            (unsigned long long)rssMBImp(), path.c_str());
        nix::v3::ir::computeFreeVars(module);
        impBumpNs(importTimingTotals().optimiseNs, tOpt);
        if (s_dbg_import) std::fprintf(stderr,
            "v3 IMPORT-PHASE after-freeVars RSS=%lluMB: %s\n",
            (unsigned long long)rssMBImp(), path.c_str());
        // R1 trigger trace: V3_DBG_DUMP_IR_PATH=<path-suffix> dumps the
        // post-lower+optimise+computeFreeVars IR for any primImport whose
        // resolved path ends with the configured suffix.  Used to compare
        // cold-lower vs warm-verify-lower IR for the 4 residual DIFF files
        // (perl, all-packages, python-packages, lua-5).
        {
            static const char * s_dumpPath =
                std::getenv("V3_DBG_DUMP_IR_PATH");
            if (s_dumpPath && *s_dumpPath
                && path.size() >= std::strlen(s_dumpPath)
                && path.compare(path.size() - std::strlen(s_dumpPath),
                                std::strlen(s_dumpPath), s_dumpPath) == 0) {
                std::fprintf(stderr,
                    "v3 IR-DUMP --- main path=%s ---\n%s"
                    "v3 IR-DUMP --- end main path=%s ---\n",
                    path.c_str(),
                    nix::v3::ir::dumpModule(module).c_str(),
                    path.c_str());
            }
        }
        auto tCompile = impStamp();
        cache.cus.push_back(compile(module));
        impBumpNs(importTimingTotals().compileNs, tCompile);
        // #772 spike: survey bytecode dedup ratio (zero-cost when
        // NIX_V3_DEDUP_SURVEY is unset).  Captures the LOWER BOUND
        // on Stage 9 cell-level cache potential.
        surveyCUBytecodeDedup(cache.cus.back());
        if (s_dbg_import) std::fprintf(stderr,
            "v3 IMPORT-PHASE after-compile RSS=%lluMB: %s\n",
            (unsigned long long)rssMBImp(), path.c_str());
        if (diskCacheEnabled && !diskKey.empty()
            && serialize::isCacheable(cache.cus.back())) {
            CACHE_HOOK_DEFINE_SITE(siteCuInsert,
                "primImport-cu-disk-insert");
            CacheHookTimer insTimer(siteCuInsert);
            try {
                auto tInsert = impStamp();
                std::string blob = serialize::serializeCU(cache.cus.back());
                disk_cache::insert(diskKey, blob);
                impBumpNs(importTimingTotals().diskInsertNs, tInsert);
                logCacheEvent("INSERT", diskKey);
                cacheHookInsert(siteCuInsert, blob.size());
            } catch (...) { /* best-effort */ }
        }
        // `module` destructed here, freeing all IR-side vectors
        // before the recursive `run()` below.
    }
    if (s_dbg_import) std::fprintf(stderr,
        "v3 IMPORT-PHASE before-run RSS=%lluMB: %s\n",
        (unsigned long long)rssMBImp(), path.c_str());
    // Each imported file is its own CompilationUnit; we re-enter the
    // VM to run it with its own top-level frame.  Keep the CU alive
    // (it's borrowed by closures returned from the eval).
    auto tRun = impStamp();
    out = run(cache.cus.back());
    impBumpNs(importTimingTotals().runNs, tRun);
    // #741 Phase 4b (2026-05-23): force-deep the imported result
    // BEFORE persisting to in-memory + disk cache.  Imported `rec {
    // ... }` attrsets produce Bindings whose entries are Tag::Thunk
    // or Tag::Slot indirections (let-rec machinery); the Phase 1
    // serialiser is WHNF-only and throws on un-forced indirections.
    //
    // Safety analysis (vs. Phase 3a-RCA-A shapeCell concern):
    //   * Phase 3a-RCA-A: forceDeep on args[0] of derivationStrict
    //     mutated let-rec slots in the SURROUNDING eval scope —
    //     bindingsSetValue replaced Slot entries that were still
    //     in-use by the outer expression.
    //   * Here at import-exit: the imported expression's let-rec
    //     slots are finalized by the time `run()` returned (OP_RETURN
    //     fired in the imported CU's dispatch).  The result attrset
    //     is the SOLE consumer of those slots — it's the primop's
    //     return value, not shared with surrounding state.  Writeback
    //     replaces Slot with WHNF safely.
    //
    // Gated by NIX_V3_IFD_IMPORT_CACHE_DISK so the cold-mode forceDeep
    // cost only fires when the cache is wanted.  Also gated on
    // isIfdImport — RCA 2026-05-24: scoping this to ACTUAL IFD
    // imports avoids forceDeep'ing every nixpkgs-internal lazy
    // attrset (which were the source of the +78 % cold tax on
    // ifd-large measurements).
    if (s_ifdImportDiskCache && isIfdImport) {
        try {
            out = forceDeep(*state.vm, out);
        } catch (...) {
            // forceDeep threw (e.g. a thunk inside the imported
            // value raised).  Don't propagate — the user's later
            // attribute access would hit the same error.  Leave
            // out un-forced; serialise below will fail on it and
            // skip the cache insert.
        }
    }
    {
        auto [mt, sz] = importStat(path);
        auto [iter, inserted] = cache.results.emplace(path,
            ImportCacheEntry{out, mt, sz, 0});
        if (inserted) bumpImportEntry(iter->second, cache);  // Phase 4b LRU
    }
    maybeEvictOldImportEntries(cache);  // Phase 4b LRU
    // #741 Phase 4 — also persist to disk-backed import cache.
    // Serialiser failures (closures, etc.) are silently skipped;
    // subsequent invocations just see a disk-cache miss + recompute.
    // Also gated on isIfdImport (RCA 2026-05-24).
    // T-5: insert under the SAME content-keyed key as the lookup (reusing the
    // narHash computed above); skip if unavailable (kept the cache off rather
    // than persist an unsound path-only entry).
    if (s_ifdImportDiskCache && isIfdImport && ifdNarHash) {
        CACHE_HOOK_DEFINE_SITE(siteImportIfdInsert,
            "primImport-ifd-disk-insert");
        CacheHookTimer timer(siteImportIfdInsert);
        try {
            std::string keyBytes;
            keyBytes.reserve(12 + path.size() + ifdNarHash->size());
            keyBytes.append("ifd-import");
            keyBytes.push_back('\0');
            keyBytes.append(path);
            keyBytes.push_back('\0');
            keyBytes.append(*ifdNarHash);  // T-5: content hash
            auto diskKey = disk_cache::computeKeyForString(keyBytes);
            std::string blob;
            value_serialize::serialize(out, blob);
            disk_cache::insertEvalResult(diskKey, blob);
            cacheHookInsert(siteImportIfdInsert, blob.size());
        } catch (...) {
            // Result Value not serialisable (closure/function/etc.).
            // Skip silently — the in-memory cache still has it for
            // this process.
        }
    }
    if (s_dbg_import) {
        std::fprintf(stderr, "v3 IMPORT-DONE RSS=%lluMB: %s\n",
            (unsigned long long)rssMBImp(), path.c_str());
    }
}

/// XML escape: `<>&"` and unprintable chars become entities.
static std::string xmlEscape(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '&': out += "&amp;"; break;
        case '"': out += "&quot;"; break;
        default:  out.push_back(c); break;
        }
    }
    return out;
}

/// Recursive XML serializer mirroring tree-walker's printValueAsXML
/// (no source-location tracking — v3 doesn't carry that yet anyway).
static void valueToXml(EvalState & state, std::string & out, Value v, int indent,
                       nix::NixStringContext & context)
{
    auto pad = [&](int n) { for (int i = 0; i < n; ++i) out += "  "; };
    v = forceValue(*state.vm, v);
    pad(indent);
    switch (v.tag()) {
    case Tag::String:
        // #674: accumulate the input string's side-table context into
        // the caller's accumulator so the resulting XML string carries
        // every referenced drv/path forward (matches TW's
        // printValueAsXML, libexpr/eval-xml.cc).
        if (auto * raw = lookupStringContextEntries(v.asString())) {
            for (auto & e : *raw)
                v3InsertContextToken(context, e, "toXML");  // C-7(d)
        }
        out += "<string value=\""; out += xmlEscape(v.asString()); out += "\" />\n";
        return;
    case Tag::Int:
        out += "<int value=\""; out += std::to_string(v.asInt()); out += "\" />\n";
        return;
    case Tag::Float:
        out += "<float value=\""; out += std::to_string(v.asFloat()); out += "\" />\n";
        return;
    case Tag::Bool:
        out += "<bool value=\""; out += v.asInt() == 1 ? "true" : "false"; out += "\" />\n";
        return;
    case Tag::Null:
        out += "<null />\n";
        return;
    case Tag::Path:
        out += "<path value=\""; out += xmlEscape(v.asPath()); out += "\" />\n";
        return;
    case Tag::List:
        out += "<list>\n";
        if (v.asList())
            for (uint32_t i = 0; i < v.asList()->size; ++i)
                valueToXml(state, out, v.asList()->elems[i], indent + 1, context);
        pad(indent);
        out += "</list>\n";
        return;
    case Tag::Attrs: {
        out += "<attrs>\n";
        const Bindings * ab = v.asAttrs();
        if (ab) {
            // Sort by name for stable output.
            // #670/#671 follow-on: store names as OWNING std::string,
            // not string_view, because the recursive valueToXml call
            // below may force values that intern new symbols and
            // realloc the global symbol table — invalidating any
            // string_views into it.  Same UB pattern as the
            // structuredAttrs branch (commit bcc8d6cf1).
            auto & symTab = ir::globalSymbolTable();
            std::vector<std::pair<std::string, Value>> entries;
            entries.reserve(ab->countDistinct());
            ab->forEach([&](const Bindings::Entry & e) {
                SymbolId sid = e.name;
                std::string nm = sid < symTab.size()
                    ? std::string(symTab[sid]) : std::string();
                entries.emplace_back(std::move(nm), e.value);
            });
            std::sort(entries.begin(), entries.end(),
                [](auto & a, auto & b) { return a.first < b.first; });
            for (auto & [nm, val] : entries) {
                pad(indent + 1);
                out += "<attr name=\""; out += xmlEscape(nm); out += "\">\n";
                valueToXml(state, out, val, indent + 2, context);
                pad(indent + 1);
                out += "</attr>\n";
            }
        }
        pad(indent);
        out += "</attrs>\n";
        return;
    }
    case Tag::Closure:
    case Tag::PrimOp:
    case Tag::PrimOpApp:
        out += "<function />\n";
        return;
    case Tag::Uninitialized:
    case Tag::Thunk:
    case Tag::App:
    case Tag::App3:
    case Tag::Blackhole:
    case Tag::External:
    case Tag::Slot:
    default:
        out += "<unevaluated />\n";
        return;
    }
}

void primToXML(EvalState & state, Value * args, Value & out)
{
    // #674: thread NixStringContext through the XML serializer so the
    // resulting string carries every drv/path reference encountered
    // (matches TW's prim_toXML at libexpr/primops.cc, which uses
    // printValueAsXML with a NixStringContext accumulator).
    std::string s = "<?xml version='1.0' encoding='utf-8'?>\n<expr>\n";
    nix::NixStringContext context;
    valueToXml(state, s, args[0], 1, context);
    s += "</expr>\n";
    out = mkStringValueOwned(s);
    if (!context.empty())
        setStringContext(out.asString(), context);
}

/// builtins.parseFlakeRef "github:NixOS/nixpkgs/23.05?dir=lib"
/// → { type = "github"; owner = "NixOS"; repo = "nixpkgs"; ref = "23.05"; dir = "lib"; }
/// Minimal hand-rolled parser covering the github / git / path /
/// url-with-query forms exercised by the lang tests.  The full
/// flake-ref grammar is much richer; this stub plus tree-walker's
/// fetchers would be the cutover point for end-to-end flakes.
static void splitOnce(std::string_view s, char sep, std::string_view & lhs, std::string_view & rhs)
{
    auto p = s.find(sep);
    if (p == std::string_view::npos) { lhs = s; rhs = {}; return; }
    lhs = s.substr(0, p);
    rhs = s.substr(p + 1);
}

void primParseFlakeRef(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("parseFlakeRef", "string");
    std::string_view s(args[0].asString());
    // Split off any `?key=value&...` query string.
    std::string_view base = s, query;
    splitOnce(s, '?', base, query);
    std::vector<std::pair<std::string, std::string>> attrs;

    // type:rest
    std::string_view typ, rest;
    splitOnce(base, ':', typ, rest);
    if (rest.empty()) {
        // Bare path like "/foo/bar".
        attrs.emplace_back("type", "path");
        attrs.emplace_back("path", std::string(base));
    } else if (typ == "github" || typ == "gitlab" || typ == "sourcehut") {
        attrs.emplace_back("type", std::string(typ));
        // owner/repo[/ref]
        std::string_view owner, after;
        splitOnce(rest, '/', owner, after);
        std::string_view repo, ref;
        splitOnce(after, '/', repo, ref);
        attrs.emplace_back("owner", std::string(owner));
        attrs.emplace_back("repo",  std::string(repo));
        if (!ref.empty())
            attrs.emplace_back("ref", std::string(ref));
    } else if (typ == "git" || typ == "hg" || typ == "tarball" || typ == "file") {
        attrs.emplace_back("type", std::string(typ));
        attrs.emplace_back("url",  std::string(rest));
    } else if (typ == "path") {
        attrs.emplace_back("type", "path");
        attrs.emplace_back("path", std::string(rest));
    } else {
        // Fallback: type with raw url body.
        attrs.emplace_back("type", std::string(typ));
        attrs.emplace_back("url",  std::string(rest));
    }
    // Apply query-string overrides (key=value, &-separated).
    while (!query.empty()) {
        std::string_view part, qrest;
        splitOnce(query, '&', part, qrest);
        std::string_view k, v;
        splitOnce(part, '=', k, v);
        attrs.emplace_back(std::string(k), std::string(v));
        query = qrest;
    }
    // Build sorted Bindings.
    std::vector<std::pair<SymbolId, Value>> entries;
    entries.reserve(attrs.size());
    for (auto & p : attrs)
        entries.emplace_back(vmIntern(state, p.first), mkStringValueOwned(p.second));
    std::sort(entries.begin(), entries.end(),
        [](auto & a, auto & b) { return a.first < b.first; });
    Bindings * b = Alloc::allocBindings(static_cast<uint32_t>(entries.size()));
    V3_STATS_INC(attrsetsAllocated);
    for (size_t i = 0; i < entries.size(); ++i)  // Phase D
        bindingsSetEntry(b, static_cast<uint32_t>(i), {entries[i].first, 0, entries[i].second});
    out.mkAttrs(b);
}

void primFlakeRefToString(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isAttrs() || !args[0].asAttrs())
        typeError("flakeRefToString", "attrset");
    auto * src = args[0].asAttrs();
    auto getStr = [&](const char * name) -> std::string {
        SymbolId id = vmIntern(state, name);
        auto * v = src->lookup(id);
        if (!v) return {};
        Value f = forceValue(*state.vm, *v);
        if (f.isString()) return std::string(f.asString());
        // Match tree-walker: negative-int attrs raise; non-string,
        // non-int attrs would too (we just reject all non-strings).
        if (f.isInt()) {
            if (f.asInt() < 0)
                throw std::runtime_error("v3 flakeRefToString: negative value given for flake ref attr " +
                                          std::string(name) + ": " + std::to_string(f.asInt()));
            return std::to_string(f.asInt());
        }
        throw std::runtime_error("v3 flakeRefToString: flake ref attr '" +
                                  std::string(name) + "' is not a string");
    };
    std::string typ = getStr("type");
    std::string out_s;
    if (typ == "github" || typ == "gitlab" || typ == "sourcehut") {
        out_s = typ + ":" + getStr("owner") + "/" + getStr("repo");
        std::string ref = getStr("ref");
        if (!ref.empty()) out_s += "/" + ref;
    } else if (typ == "path") {
        out_s = "path:" + getStr("path");
    } else {
        // Generic url-bearing type.
        out_s = typ + ":" + getStr("url");
    }
    // Query-string for known extra attrs.
    std::string q;
    for (const char * key : {"dir", "rev", "ref", "narHash"}) {
        if (std::string(key) == "ref") continue;  // already in path for github-style
        std::string val = getStr(key);
        if (val.empty()) continue;
        if (!q.empty()) q += "&";
        q += std::string(key) + "=" + val;
    }
    if (!q.empty()) out_s += "?" + q;
    out = mkStringValueOwned(out_s);
}

/// Convert a toml::value (toml11) into a v3 Value recursively.
static Value tomlToValue(EvalState & state, const toml::value & t)
{
    Value v;
    switch (t.type()) {
    case toml::value_t::table: {
        auto & tab = t.as_table();
        std::vector<std::pair<SymbolId, Value>> entries;
        entries.reserve(tab.size());
        for (auto & elem : tab) {
            // Reject NUL-bearing keys: tree-walker does, and the v3
            // bytecode-level Bindings::Entry can't represent them
            // safely (names are NUL-terminated SymbolId-keyed).
            if (elem.first.find('\0') != std::string::npos)
                throw std::runtime_error("v3 fromTOML: attribute name contains null byte");
            entries.emplace_back(vmIntern(state, elem.first), tomlToValue(state, elem.second));
        }
        std::sort(entries.begin(), entries.end(),
            [](auto & a, auto & b) { return a.first < b.first; });
        Bindings * b = Alloc::allocBindings(static_cast<uint32_t>(entries.size()));
        V3_STATS_INC(attrsetsAllocated);
        for (size_t i = 0; i < entries.size(); ++i) {
            b->entries[i].name  = entries[i].first;
            bindingsSetValue(b, static_cast<uint32_t>(i), entries[i].second);  // Phase D
        }
        v.mkAttrs(b);
        return v;
    }
    case toml::value_t::array: {
        auto & arr = t.as_array();
        ListVec * lv = Alloc::allocList(static_cast<uint32_t>(arr.size()));
        V3_STATS_INC(listsAllocated);
        for (size_t i = 0; i < arr.size(); ++i)
            lv->elems[i] = tomlToValue(state, arr[i]);
        listPostConstructBarrier(lv);  // Phase D coverage (fromTOML; PhD-6) — nested values
        v.mkList(lv);
        return v;
    }
    case toml::value_t::boolean: v = t.as_boolean() ? Value::vTrue : Value::vFalse; return v;
    case toml::value_t::integer:  v.mkInt(t.as_integer()); return v;
    case toml::value_t::floating: v.mkFloat(t.as_floating()); return v;
    case toml::value_t::string: {
        const auto & s = t.as_string();
        if (s.find('\0') != std::string::npos)
            throw std::runtime_error("v3 fromTOML: string contains null byte");
        v = mkStringValueOwned(s); return v;
    }
    case toml::value_t::local_datetime:
    case toml::value_t::offset_datetime:
    case toml::value_t::local_date:
    case toml::value_t::local_time: {
        // Match tree-walker: bare TOML datetime values are only
        // accepted when the `parse-toml-timestamps` experimental
        // feature is enabled.  Without it, raise — this matches the
        // upstream eval-fail-fromTOML-timestamps test.
        if (!nix::experimentalFeatureSettings.isEnabled(nix::Xp::ParseTomlTimestamps))
            throw std::runtime_error("v3 fromTOML: Dates and times are not supported");
        // Normalize the format before serializing so we get the same
        // canonical RFC3339 spelling tree-walker emits: upper-case `T`
        // delimiter, mandatory seconds, subsecond precision rounded up
        // to the next multiple of 3 (or 0 if no fractional component).
        // Mirrors libexpr/primops/fromTOML.cc's normalizeDatetimeFormat.
        auto normalizeSubsecond = [](const toml::local_time & lt) -> size_t {
            if (lt.millisecond != 0 || lt.microsecond != 0 || lt.nanosecond != 0) {
                if (lt.microsecond != 0 || lt.nanosecond != 0) {
                    if (lt.nanosecond != 0) return 9;
                    return 6;
                }
                return 3;
            }
            return 0;
        };
        toml::value tw = t;
        if (tw.is_local_datetime()) {
            tw.as_local_datetime_fmt() = {
                .delimiter = toml::datetime_delimiter_kind::upper_T,
                .has_seconds = true,
                .subsecond_precision = normalizeSubsecond(tw.as_local_datetime().time),
            };
        } else if (tw.is_offset_datetime()) {
            tw.as_offset_datetime_fmt() = {
                .delimiter = toml::datetime_delimiter_kind::upper_T,
                .has_seconds = true,
                .subsecond_precision = normalizeSubsecond(tw.as_offset_datetime().time),
            };
        } else if (tw.is_local_time()) {
            tw.as_local_time_fmt() = {
                .has_seconds = true,
                .subsecond_precision = normalizeSubsecond(tw.as_local_time()),
            };
        }
        // Render the datetime via toml11's stream operator and tag it.
        std::ostringstream s;
        s << tw;
        std::string str = s.str();
        SymbolId sType = vmIntern(state, "_type");
        SymbolId sVal  = vmIntern(state, "value");
        Bindings * b = Alloc::allocBindings(2);
        V3_STATS_INC(attrsetsAllocated);
        Value typeV = mkStringValueOwned("timestamp");
        Value valV  = mkStringValueOwned(str);
        if (sType < sVal) { bindingsSetEntry(b, 0, {sType, 0, typeV}); bindingsSetEntry(b, 1, {sVal, 0, valV}); }  // Phase D
        else              { bindingsSetEntry(b, 0, {sVal, 0, valV}); bindingsSetEntry(b, 1, {sType, 0, typeV}); }
        v.mkAttrs(b);
        return v;
    }
    case toml::value_t::empty: v.mkNull(); return v;
    }
    v.mkNull();
    return v;
}

/// builtins.fromTOML s -- parse a TOML document into a v3 attrset.
void primFromTOML(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("fromTOML", "string");
    std::istringstream stream{std::string(args[0].asString())};
    try {
        out = tomlToValue(state, toml::parse(stream, "fromTOML"));
    } catch (std::exception & e) {
        // #692 — match TW phrasing (libexpr/primops.cc:fromTOML).
        throw std::runtime_error(std::string("while parsing TOML: ") + e.what());
    }
}

// BR-4: native builtins.path.  Mirrors prim_path / addPath
// (libexpr/primops.cc:3083 / :2944) for the no-filter case.
//
// Skips the v3->tw bridge encode + tw->v3 decode round-trip; calls
// fetchToStore directly against state.nixEvalState->store.  Filter
// closures are tricky to drive from native (they would re-enter
// v3's VM on every directory entry); when present we fall back to
// the bridge, which already invokes them via tree-walker's regular
// callFunction path (the legacy __v3_call_bridge_2 shim was removed
// in the Phase-13 cleanup pass; its WC-19 safety net is now built
// into the regular bridge).
//
// Throws on any unsupported shape; caller's primPath catches and
// falls through to the existing bridge.
static void primPathNative(EvalState & state, Value * args, Value & out);

/// builtins.path with a `filter` closure — native (no TW bridge).  Extracts
/// path/name/recursive/sha256 like primPathNative, but drives the libstore
/// copy through ffi::addPathFiltered with a per-entry callClosure callback so
/// the v3 filter closure never crosses into TW.  See primPath's dispatch.
static void primPathFilteredNative(EvalState & state, Value * args, Value & out);

/// builtins.path { path; name?; filter?; recursive?; sha256?; }:
/// add a path to the v3 store and return its store-path string with
/// Opaque NixStringContext.  Mirrors tree-walker's prim_path.
///
/// Native fast path (BR-4) handles the no-filter case in one call
/// to fetchToStore; falls back to the bridge when a `filter`
/// closure is supplied (the closure would have to re-enter the v3
/// VM on every fs entry — left to a follow-up).
void primPath(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isAttrs() || !args[0].asAttrs())
        typeError("path", "attrset");
    // V3-NATIVE builtins.path (TW_VALUE_ERADICATION F3/F4): primPathNative /
    // primPathFilteredNative fully transcribe TW's prim_path + addPath
    // (coercion of the `path` arg + the in-store-context refs branch + the
    // per-entry `filter` closure + `sha256`) via ffi::addPathFull — byte-
    // identical to TW with NO v3->TW marshalling.  Errors propagate (TW
    // throws too).  The former TW-bridge fallback (v3ToTreeWalker →
    // callFunction(builtins.path) → treeWalkerToV3) was the LAST builtins.path
    // bridge feeder; it is DELETED here, along with the V3_PATH_NO_NATIVE A/B
    // gate it backed.  Validated byte-equal across derivation (refs branch),
    // toFile (context), attrset outPath/__toString, plain path, and the
    // filter cases — all with bridge tables = 0.
    if (state.nixEvalState) {
        SymbolId sFilter = ir::globalInternSymbol("filter");
        if (!args[0].asAttrs()->lookup(sFilter))
            primPathNative(state, args, out);
        else
            primPathFilteredNative(state, args, out);
        return;
    }
    // C-7(a) (CODEBASE_REVIEW_2026-06-11): with no host store wired we used
    // to fabricate a /v3-fake-store/<name> path unconditionally.  That keys
    // store-path fabrication on `state.nixEvalState` being unset — which can
    // happen spuriously on a future thread/fiber entry — silently producing a
    // store path that diverges from any real store.  Refuse by default and
    // surface "no store wired"; allowFakeStore() restores the placeholder for
    // pure no-store AST eval / embedding debugging.
    if (!allowFakeStore())
        throw std::runtime_error(
            "v3 builtins.path: no store is wired (state.nixEvalState is null), "
            "so a real store path cannot be computed. Set "
            "NIX_V3_ALLOW_FAKE_STORE=1 to fabricate a placeholder /v3-fake-store/ "
            "path for store-less AST evaluation.");
    // Placeholder path so pure AST evaluation still proceeds without a store.
    SymbolId sPath = vmIntern(state, "path");
    SymbolId sName = vmIntern(state, "name");
    auto * src = args[0].asAttrs();
    const Value * pathV = src->lookup(sPath);
    if (!pathV)
        typeError("path", "attrset with `path`");
    Value forcedPath = forceValue(*state.vm, *pathV);
    std::string p;
    if (forcedPath.isString())     p = forcedPath.asString();
    else if (forcedPath.isPath())  p = forcedPath.asPath();
    else typeError("path", "{ path = string-or-path; ... }");
    std::string name;
    if (auto * nameV = src->lookup(sName)) {
        Value f = forceValue(*state.vm, *nameV);
        if (f.isString()) name = f.asString();
    }
    if (name.empty()) {
        auto pos = p.find_last_of('/');
        name = pos == std::string::npos ? p : p.substr(pos + 1);
        if (name.empty()) name = "source";
    }
    // Fallback: fake store path.
    std::string outPath = "/v3-fake-store/" + name;
    // CRIT-4: arena allocation.
    char * buf = Alloc::allocChars(outPath.size() + 1);
    std::memcpy(buf, outPath.data(), outPath.size());
    buf[outPath.size()] = '\0';
    out.mkPath(buf);
}

// BR-4 native builtins.path body (gated on filter being absent —
// see primPath).
static void primPathNative(EvalState & state, Value * args, Value & out)
{
    auto & ns = *state.nixEvalState;
    auto * src = args[0].asAttrs();

    // Use SymbolIds.  The names here are not in drvStrictSymbols
    // because builtins.path uses different attr names (path, name,
    // filter, recursive, sha256).
    static const SymbolId sPath      = ir::globalInternSymbol("path");
    static const SymbolId sName      = ir::globalInternSymbol("name");
    static const SymbolId sRecursive = ir::globalInternSymbol("recursive");
    static const SymbolId sSha256    = ir::globalInternSymbol("sha256");

    // Read `path` (required).  Coerce like TW's coerceToPath (copyToStore=
    // false): string (with its context) / path / derivation / attrset with
    // outPath|__toString.  Collect the resulting string context so the
    // refs branch in ffi::addPathFull can realise it (the in-store-path-
    // with-references case that addToStore-CA-hashes the refs in).
    const Value * pathRaw = src->lookup(sPath);
    if (!pathRaw)
        throw std::runtime_error(
            "v3 BR-4 native: missing required `path` attribute");
    std::vector<std::string> pathCtx;
    std::string pathStr =
        toStringCoerceCtx(state, *pathRaw, pathCtx, /*copyPathsToStore=*/false);

    // Read `name` (optional; "" → ffi::addPathFull defaults to baseName).
    std::string name;
    if (auto * nameV = src->lookup(sName)) {
        Value f = forceValue(*state.vm, *nameV);
        if (!f.isString())
            throw std::runtime_error(
                "v3 BR-4 native: `name` is not a string");
        name = f.asString() ? f.asString() : "";
    }

    // Read `recursive` (optional, default true → NixArchive).
    bool recursive = true;
    if (auto * rV = src->lookup(sRecursive)) {
        Value f = forceValue(*state.vm, *rV);
        if (!f.isBool())
            throw std::runtime_error(
                "v3 BR-4 native: `recursive` is not a bool");
        recursive = (f.asInt() == 1);
    }

    // Read `sha256` (optional).
    std::optional<std::string> sha256;
    if (auto * shaV = src->lookup(sSha256)) {
        Value f = forceValue(*state.vm, *shaV);
        if (!f.isString())
            throw std::runtime_error(
                "v3 BR-4 native: `sha256` is not a string");
        sha256 = std::string(f.asString() ? f.asString() : "");
    }

    ffi::FetchUrlResult r = ffi::addPathFull(
        ns, pathStr, pathCtx, name, recursive, sha256, /*v3filter=*/nullptr);
    Value v3Out = mkStringValueOwned(r.printedStorePath);
    std::vector<std::string> ctx{ r.opaqueContextElem };
    setStringContextEntries(v3Out.asString(), std::move(ctx));
    out = v3Out;
}

// builtins.path with a `filter` closure, driven natively.  Mirrors
// primPathNative's path/name/recursive/sha256 extraction (so the no-filter
// and filter cases agree byte-for-byte on store-path computation), but the
// libstore copy runs through ffi::addPathFiltered with a PathFilter that
// RE-ENTERS v3's VM per directory entry via callClosure — exactly the
// primFilterSource pattern.  No nix::Value crosses to TW: this retires the
// 20034-dispatch builtins.path bridge feeder on cardano-node.
static void primPathFilteredNative(EvalState & state, Value * args, Value & out)
{
    auto & ns = *state.nixEvalState;
    auto * src = args[0].asAttrs();

    static const SymbolId sPath      = ir::globalInternSymbol("path");
    static const SymbolId sName      = ir::globalInternSymbol("name");
    static const SymbolId sFilter    = ir::globalInternSymbol("filter");
    static const SymbolId sRecursive = ir::globalInternSymbol("recursive");
    static const SymbolId sSha256    = ir::globalInternSymbol("sha256");

    // path (required) — coerced like primPathNative (copyToStore=false),
    // collecting context for the refs branch.
    const Value * pathRaw = src->lookup(sPath);
    if (!pathRaw)
        throw std::runtime_error(
            "v3 builtins.path (filtered): missing required `path` attribute");
    std::vector<std::string> pathCtx;
    std::string pathStr =
        toStringCoerceCtx(state, *pathRaw, pathCtx, /*copyPathsToStore=*/false);

    // name (optional → baseName, resolved inside ffi::addPathFull when "").
    std::string name;
    if (auto * nameV = src->lookup(sName)) {
        Value f = forceValue(*state.vm, *nameV);
        if (!f.isString())
            throw std::runtime_error(
                "v3 builtins.path (filtered): `name` is not a string");
        name = f.asString() ? f.asString() : "";
    }

    // recursive (optional, default true → NixArchive).
    bool recursive = true;
    if (auto * rV = src->lookup(sRecursive)) {
        Value f = forceValue(*state.vm, *rV);
        if (!f.isBool())
            throw std::runtime_error(
                "v3 builtins.path (filtered): `recursive` is not a bool");
        recursive = (f.asInt() == 1);
    }

    // sha256 (optional).
    std::optional<std::string> sha256;
    if (auto * shaV = src->lookup(sSha256)) {
        Value f = forceValue(*state.vm, *shaV);
        if (!f.isString())
            throw std::runtime_error(
                "v3 builtins.path (filtered): `sha256` is not a string");
        sha256 = std::string(f.asString() ? f.asString() : "");
    }

    // filter (required here — caller only routes us when present).
    const Value * filterRaw = src->lookup(sFilter);
    if (!filterRaw)
        throw std::runtime_error(
            "v3 builtins.path (filtered): missing `filter` (internal dispatch bug)");
    Value filterFn = forceValue(*state.vm, *filterRaw);

    // Per-entry predicate: callClosure(filter, absPath)(type) -> Bool.  Runs
    // inside fetchToStore (nested v3 eval on the same VMState — STG-10).
    // std::function (not auto) so its address binds to addPathFull's param.
    std::function<bool(const std::string &, const std::string &)> v3filter =
        [&](const std::string & p, const std::string & type) -> bool {
        // P-5: saturated 2-arg call (no per-entry throwaway curry-PAP).
        Value r2 = callClosure2(*state.vm, filterFn,
                                mkStringValueOwned(p), mkStringValueOwned(type));
        r2 = forceValue(*state.vm, r2);
        if (r2.tag() != Tag::Bool)
            throw std::runtime_error(
                "while evaluating the return value of the path filter function: "
                "expected a Boolean");
        return r2.asInt() == 1;
    };

    ffi::FetchUrlResult r =
        ffi::addPathFull(ns, pathStr, pathCtx, name, recursive, sha256, &v3filter);
    Value v3Out = mkStringValueOwned(r.printedStorePath);
    std::vector<std::string> ctx{ r.opaqueContextElem };
    setStringContextEntries(v3Out.asString(), std::move(ctx));
    out = v3Out;
}

/// builtins.scopedImport scope path -- like import, but extends the
/// base env with `scope`'s entries while evaluating the file.
///
/// Strategy: build a synthetic AST `λ __scope__: with __scope__; <body>`
/// where the body is parsed against a custom staticEnv that has the
/// scope's keys at the front (so they shadow the base-env primops on
/// lookup, e.g. `import` in scope wins over the builtin `import`).
/// parseExprFromString runs bindVars in one pass; the resulting v3
/// closure is then applied to the scope value.
void primScopedImport(EvalState & state, Value * args, Value & out)
{
    if (!state.nixEvalState)
        throw std::runtime_error("v3 primop scopedImport: no nix EvalState wired");
    Value scope = forceValue(*state.vm, args[0]);
    // #693 — match TW's forceAttrs / forceString-or-path phrasings.
    if (!scope.isAttrs() || !scope.asAttrs())
        throw std::runtime_error(expectedTypeButFound("a set", scope));
    std::string path;
    if (args[1].isString()) path = args[1].asString();
    else if (args[1].isPath()) path = args[1].asPath();
    else throw std::runtime_error(expectedTypeButFound("a string", args[1]));

    auto & ns = *state.nixEvalState;

    // Resolve path through symlinks + maybe append default.nix.
    nix::SourcePath sp(ns.rootFS, nix::CanonPath(path));
    sp = nix::resolveExprPath(sp);

    // Read file source and synthesize a wrapper that re-binds every
    // scope attribute as a let binding so it shadows the base-env
    // primops (otherwise `import` etc. would resolve to the builtin
    // before our `with __scope__;` got a chance).  This mirrors
    // tree-walker's scopedImport which adds the scope to a staticEnv
    // *above* staticBaseEnv.
    std::string src = sp.resolveSymlinks().readFile();
    std::string wrapped;
    wrapped += "__scope__: let ";
    auto * sb = scope.asAttrs();
    auto & symTab = ir::globalSymbolTable();
    sb->forEach([&](const Bindings::Entry & e) {
        SymbolId sid = e.name;
        std::string n = sid < symTab.size() ? symTab[sid] : "";
        if (n.empty()) return;
        // Quote names that can't be plain identifiers.  Conservative:
        // allow [a-zA-Z_][a-zA-Z0-9_'-]*.
        bool plain = !n.empty() &&
            ((n[0] >= 'a' && n[0] <= 'z') || (n[0] >= 'A' && n[0] <= 'Z') || n[0] == '_');
        for (size_t k = 1; plain && k < n.size(); ++k) {
            char c = n[k];
            plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                 || (c >= '0' && c <= '9') || c == '_' || c == '\'' || c == '-';
        }
        // Skip reserved keywords.
        if (n == "if" || n == "then" || n == "else" || n == "assert"
            || n == "with" || n == "let" || n == "in" || n == "rec"
            || n == "inherit" || n == "or") return;
        if (!plain) return;
        wrapped += n + " = __scope__." + n + "; ";
    });
    wrapped += "in (\n" + src + "\n)";

    // PARSER_PROJECT_PLAN §5.3 site 3: native-parse+lower the synthetic
    // scopedImport wrapper (the only path now — no nix::Expr).  The
    // wrapper's relative-path literals (inside `src`) resolve against the
    // imported file's dir, matching parseExprFromString(.., sp.parent()).
    // canLowerV3 is total for parsed source (throw = should-never-fire).
    static const std::string s_siHome = ffi::homeDir();
    nix::v3::ast::ParserState siSt;
    if (auto par = sp.path.parent()) siSt.basePath = par->abs();  // file's dir
    siSt.homePath = s_siHome;
    nix::v3::parser::parseString(siSt, wrapped);
    if (!nix::v3::canLowerV3(siSt.result))
        throw nix::Error("v3 scopedImport: native lowering cannot handle " + path);
    auto siSrcRef = nix::make_ref<std::string>(wrapped);
    auto siOrigin = ns.positions.addOrigin(
        nix::Pos::String{.source = siSrcRef}, siSrcRef->size());

    auto module = nix::v3::lowerV3Ast(ns.symbols, siSt.result, &ns.positions, siOrigin,
                                      &nix::v3::twBaseEnvGlobals(ns));
    nix::v3::ir::optimise(module);
    nix::v3::ir::computeFreeVars(module);
    auto & cache = importCache();
    cache.cus.push_back(compile(module));
    Value fn = run(cache.cus.back());

    // Apply the lambda to the scope value.
    out = callClosure(*state.vm, fn, scope);
}

/// builtins.functionArgs lam → { name = false; ... } where the bool
/// indicates whether the formal has a default value.  For simple
/// lambdas (no formals) returns an empty attrset.
void primFunctionArgs(EvalState & state, Value * args, Value & out)
{
    Value v = args[0];
    // #493 step 3: peek through a Bridge thunk wrapping a TW Lambda
    // (e.g., the v3FormalsLambdaBridges sentinel-tLambda values that
    // round-trip back to v3 as Bridge thunks).  Read formals directly
    // from the TW ExprLambda; matches what TW's primFunctionArgs would
    // produce, just bridged.  Without this peek, bridged-lambda
    // arguments hit the typeError path.
    // (#493 step 3 Bridge-lambda formals peek retired — TW_VALUE_ERADICATION
    //  F4, 2026-06-02; no Bridge thunks exist.)
    if (v.tag() == Tag::Closure && v.asClosure() && v.asClosure()->desc) {
        const LambdaDescriptor * desc = v.asClosure()->desc;
        if (!desc->hasFormals) {
            Bindings * b = Alloc::allocBindings(0);
            V3_STATS_INC(attrsetsAllocated);
            out.mkAttrs(b);
            return;
        }
        // Build sorted entries; carry pos handle so we can populate
        // the per-attr side-table below for `unsafeGetAttrPos`.
        std::vector<std::tuple<SymbolId, Value, uint32_t>> entries;
        entries.reserve(desc->formals.size());
        for (auto & f : desc->formals) {
            Value bv = f.hasDefault ? Value::vTrue : Value::vFalse;
            entries.emplace_back(f.name, bv, f.pos);
        }
        std::sort(entries.begin(), entries.end(),
            [](auto & a, auto & b) { return std::get<0>(a) < std::get<0>(b); });
        Bindings * b = Alloc::allocBindings(static_cast<uint32_t>(entries.size()));
        V3_STATS_INC(attrsetsAllocated);
        for (size_t i = 0; i < entries.size(); ++i) {
            b->entries[i].name = std::get<0>(entries[i]);
            b->entries[i].pos  = std::get<2>(entries[i]);  // #752 inline
            bindingsSetValue(b, static_cast<uint32_t>(i),  // Phase D
                             std::get<1>(entries[i]));
        }
        // #803 H10 diag: log when functionArgs is called on a lambda
        // whose formals look like nix-prefetch-scripts (has git-lfs +
        // pijul or similar VCS-tool signature).  Helps catch any future
        // re-occurrence of the haskell.nix-example failing call.  Gated
        // under V3_DBG_FUNCTIONARGS_TRACE=1.
        static const bool s_dbgFA =
            std::getenv("V3_DBG_FUNCTIONARGS_TRACE") != nullptr;
        if (__builtin_expect(s_dbgFA, 0)) {
            bool hasGitLfs = false, hasPijul = false, hasGit = false;
            const auto & syms = ir::globalSymbolTable();
            for (auto & f : desc->formals) {
                SymbolId sid = f.name;
                std::string_view n = sid < syms.size() ? std::string_view(syms[sid]) : "";
                if (n == "git-lfs") hasGitLfs = true;
                else if (n == "pijul") hasPijul = true;
                else if (n == "git") hasGit = true;
            }
            if (hasGitLfs && hasPijul) {
                std::fprintf(stderr,
                    "v3 FA-TRACE nix-prefetch-style desc=%p name='%s' "
                    "ctx='%s' formals.size=%zu hasGit=%d\n",
                    (const void *)desc, desc->name.c_str(),
                    desc->contextualName.c_str(),
                    desc->formals.size(), hasGit);
                std::fflush(stderr);
            }
        }
        out.mkAttrs(b);
        return;
    }
    if (v.tag() == Tag::PrimOp || v.tag() == Tag::PrimOpApp) {
        // PrimOps don't have introspectable formals; return empty.
        Bindings * b = Alloc::allocBindings(0);
        V3_STATS_INC(attrsetsAllocated);
        out.mkAttrs(b);
        return;
    }
    // eval/apply (#3): an under-applied closure-PAP behaves as the remaining
    // curried function.  Its applied depth is ≥1 (it is an App node), so the
    // next unbound parameter is always an `extraParams` arg — a positional
    // arg with no named formals (the descriptor's `formals` belong to the
    // already-applied first parameter).  TW likewise reports `{}` for the
    // partially-applied `(a: b: …) x` ⇒ `functionArgs (b: …)` = { }.
    if (underappliedPapLeaf(v)) {
        Bindings * b = Alloc::allocBindings(0);
        V3_STATS_INC(attrsetsAllocated);
        out.mkAttrs(b);
        return;
    }
    typeError("functionArgs", "lambda");
}

/// nlohmann::json -> v3 Value (recursive).
Value jsonToValue(EvalState & state, const nlohmann::json & j)
{
    Value out;
    if (j.is_null())     { out.mkNull(); return out; }
    if (j.is_boolean())  { out = j.get<bool>() ? Value::vTrue : Value::vFalse; return out; }
    if (j.is_number_integer()) {
        // Reject values that don't fit in int64_t — nlohmann distinguishes
        // signed vs unsigned numbers, so a JSON literal larger than
        // INT64_MAX comes back as is_number_unsigned() && is_number_integer().
        if (j.is_number_unsigned()) {
            uint64_t u = j.get<uint64_t>();
            if (u > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
                throw std::runtime_error("v3 fromJSON: integer value out of range");
        }
        out.mkInt(j.get<int64_t>()); return out;
    }
    if (j.is_number_float()) {
        out.mkFloat(j.get<double>()); return out;
    }
    if (j.is_string()) {
        // Reject embedded NULs — Nix strings are NUL-terminated C strings
        // at the bytecode level, and tree-walker rejects them too.
        std::string s = j.get<std::string>();
        if (s.find('\0') != std::string::npos)
            throw std::runtime_error("v3 fromJSON: string contains null byte");
        out = mkStringValueOwned(std::move(s)); return out;
    }
    if (j.is_array()) {
        ListVec * lv = Alloc::allocList(static_cast<uint32_t>(j.size()));
        V3_STATS_INC(listsAllocated);
        for (size_t i = 0; i < j.size(); ++i) lv->elems[i] = jsonToValue(state, j[i]);
        listPostConstructBarrier(lv);  // Phase D coverage (fromJSON; PhD-6) — nested values
        out.mkList(lv);
        return out;
    }
    if (j.is_object()) {
        std::vector<std::pair<SymbolId, Value>> entries;
        entries.reserve(j.size());
        for (auto it = j.begin(); it != j.end(); ++it) {
            const std::string & key = it.key();
            if (key.find('\0') != std::string::npos)
                throw std::runtime_error("v3 fromJSON: attribute name contains null byte");
            SymbolId k = vmIntern(state, key);
            entries.emplace_back(k, jsonToValue(state, it.value()));
        }
        std::sort(entries.begin(), entries.end(),
            [](auto & a, auto & b) { return a.first < b.first; });
        Bindings * b = Alloc::allocBindings(static_cast<uint32_t>(entries.size()));
        V3_STATS_INC(attrsetsAllocated);
        for (size_t i = 0; i < entries.size(); ++i) {
            b->entries[i].name = entries[i].first;
            bindingsSetValue(b, static_cast<uint32_t>(i), entries[i].second);  // Phase D
        }
        out.mkAttrs(b);
        return out;
    }
    throw std::runtime_error("v3 jsonToValue: unsupported JSON type");
}

/// v3 Value -> nlohmann::json.
nlohmann::json valueToJson(EvalState & state, const Value & vRaw)
{
    using json = nlohmann::json;
    // Force first — App / Thunk reach this primop unchanged when
    // wrapped in attrsets/lists.
    Value v = forceValue(*state.vm, vRaw);
    switch (v.tag()) {
    case Tag::Null:   return json(nullptr);
    case Tag::Bool:   return json(v.asInt() == 1);
    case Tag::Int:    return json(v.asInt());
    case Tag::Float:  return json(v.asFloat());
    case Tag::String: return json(std::string(v.asString()));
    case Tag::Path:   return json(std::string(v.asPath()));
    case Tag::List: {
        json arr = json::array();
        if (v.asList())
            for (uint32_t i = 0; i < v.asList()->size; ++i)
                arr.push_back(valueToJson(state, v.asList()->elems[i]));
        return arr;
    }
    case Tag::Attrs: {
        // `__toString self` overrides JSON serialization — call it
        // and use the resulting string.  Standard Nix coercion.
        if (v.asAttrs()) {
            static const SymbolId tsId = ir::globalInternSymbol("__toString");
            if (auto * fn = v.asAttrs()->lookup(tsId)) {
                Value forced = forceValue(*state.vm, *fn);
                Value s = callClosure(*state.vm, forced, v);
                s = forceValue(*state.vm, s);
                if (s.isString()) return json(std::string(s.asString()));
            }
            // `outPath` (a derivation-like value) — serialize as the
            // path string.
            static const SymbolId outId = ir::globalInternSymbol("outPath");
            if (auto * op = v.asAttrs()->lookup(outId)) {
                Value forced = forceValue(*state.vm, *op);
                if (forced.isString()) return json(std::string(forced.asString()));
                if (forced.isPath())   return json(std::string(forced.asPath()));
            }
        }
        json obj = json::object();
        if (v.asAttrs()) {
            const Bindings * jb = v.asAttrs();
            jb->forEach([&](const Bindings::Entry & en) {
                // #670/#671 follow-on: capture the key as std::string
                // BEFORE valueToJson runs.  valueToJson's recursive
                // forceValue may intern new symbols, which grows the
                // global symbol table's std::vector<std::string> and
                // invalidates string_views into it.  In `obj[k] = v`,
                // LHS and RHS evaluation order is unsequenced; if RHS
                // runs first and reallocs, the LHS std::string(k)
                // would copy from dangling memory.  Same UB pattern
                // as the structuredAttrs branch (commit bcc8d6cf1).
                std::string k = std::string(vmSymName(state, en.name));
                obj[std::move(k)] = valueToJson(state, en.value);
            });
        }
        return obj;
    }
    case Tag::Uninitialized:
    case Tag::Closure:
    case Tag::PrimOp:
    case Tag::PrimOpApp:
    case Tag::Thunk:
    case Tag::App:
    case Tag::App3:
    case Tag::Blackhole:
    case Tag::External:
    case Tag::Slot:
    default:
        throw std::runtime_error("v3 toJSON: unsupported value type");
    }
}

void primFromJSON(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("fromJSON", "string");
    // #734: TW's prim_fromJSON calls forceStringNoCtx on the input.
    requireNoStringContext(state, args[0], "fromJSON");
    auto j = nlohmann::json::parse(std::string(args[0].asString()), nullptr, /*allow_exceptions=*/true);
    out = jsonToValue(state, j);
}

void primToJSON(EvalState & state, Value * args, Value & out)
{
    // 2026-05-19 #672: builtins.toJSON must preserve string contexts of
    // interpolated derivations / paths.  Pre-fix v3 used valueToJson()
    // which dropped contexts, breaking luaPackages / nixpkgs callPackage
    // chains that use `lib.generators.toLua` which emits `toJSON "${drv}"`
    // — the resulting writeText derivation lost inputDrvs entries for
    // every interpolated derivation, producing divergent .drv hashes
    // (luaPackages.dkjson etc.).  Tree-walker's prim_toJSON uses
    // printValueAsJSON with a NixStringContext& accum (libexpr/primops.cc:
    // 2154); switch to v3's matching valueToJsonWithContext + register
    // entries on the output string buffer.
    nix::NixStringContext context;
    auto j = valueToJsonWithContext(state, args[0], context);
    out = mkStringValueOwned(j.dump());
    if (!context.empty())
        setStringContext(out.asString(), context);
}

// BR-3.12: context-tracking JSON serialization.  Mirrors
// valueToJson but threads a NixStringContext accumulator: every
// string with side-table context contributes its entries; every
// path coerce inserts an Opaque (via copyPathToStore).  Used by
// the native derivationStrict path under __structuredAttrs.
nlohmann::json valueToJsonWithContext(
    EvalState & state, const Value & vRaw, nix::NixStringContext & context)
{
    using json = nlohmann::json;
    Value v = forceValue(*state.vm, vRaw);
    switch (v.tag()) {
    case Tag::Null:   return json(nullptr);
    case Tag::Bool:   return json(v.asInt() == 1);
    case Tag::Int:    return json(v.asInt());
    case Tag::Float:  return json(v.asFloat());
    case Tag::String: {
        const char * buf = v.asString() ? v.asString() : "";
        if (auto * raw = lookupStringContextEntries(buf)) {
            for (auto & e : *raw)
                v3InsertContextToken(context, e, "toJSON");  // C-7(d)
        }
        return json(std::string(buf));
    }
    case Tag::Path: {
        if (!state.nixEvalState)
            throw std::runtime_error(
                "v3 BR-3 valueToJsonWithContext: path requires nixEvalState");
        auto & ns = *state.nixEvalState;
        nix::SourcePath sp(ns.rootFS,
            nix::CanonPath(v.asPath() ? v.asPath() : ""));
        nix::StorePath dst = ns.copyPathToStore(context, sp);
        return json(ns.store->printStorePath(dst));
    }
    case Tag::List: {
        json arr = json::array();
        if (v.asList())
            for (uint32_t i = 0; i < v.asList()->size; ++i)
                arr.push_back(valueToJsonWithContext(
                    state, v.asList()->elems[i], context));
        return arr;
    }
    case Tag::Attrs: {
        // __toString self overrides JSON serialization (matches Nix
        // coercion rules); check before outPath so an attrset with both
        // (e.g. lib.makeStorePathAppendable) uses __toString.  #672
        // (2026-05-19): used by builtins.toJSON via primToJSON, which
        // previously bypassed __toString — eval-okay-tojson exercises
        // this with `k = { __toString = self: self.a; a = "foo"; }`.
        if (v.asAttrs()) {
            static const SymbolId tsId = ir::globalInternSymbol("__toString");
            if (auto * fn = v.asAttrs()->lookup(tsId)) {
                Value forced = forceValue(*state.vm, *fn);
                Value s = callClosure(*state.vm, forced, v);
                s = forceValue(*state.vm, s);
                return valueToJsonWithContext(state, s, context);
            }
            const auto & sym = drvStrictSymbols();
            // outPath fallback for derivations.
            if (auto * op = v.asAttrs()->lookup(sym.outPath)) {
                Value forced = forceValue(*state.vm, *op);
                if (forced.isString() || forced.isPath()) {
                    return valueToJsonWithContext(state, forced, context);
                }
            }
        }
        json obj = json::object();
        if (v.asAttrs()) {
            const Bindings * jb = v.asAttrs();
            jb->forEach([&](const Bindings::Entry & en) {
                // #670/#671 follow-on (same as valueToJson above): copy
                // the key to an OWNING std::string before the recursive
                // valueToJsonWithContext, whose forceValue can grow
                // the global symbol table and invalidate string_views.
                std::string k = std::string(vmSymName(state, en.name));
                obj[std::move(k)] = valueToJsonWithContext(
                    state, en.value, context);
            });
        }
        return obj;
    }
    case Tag::Closure:
    case Tag::PrimOp:
    case Tag::PrimOpApp:
        // #693 — match TW phrasing for function-in-JSON.
        throw std::runtime_error("cannot convert a function to JSON");
    case Tag::Uninitialized:
    case Tag::Thunk:
    case Tag::App:
    case Tag::App3:
    case Tag::Blackhole:
    case Tag::External:
    case Tag::Slot:
    default:
        // For other unsupported tags, fall back to a generic error
        // that still matches TW's typical JSON-conversion error
        // family.
        throw std::runtime_error("cannot convert a non-JSON-encodable value to JSON");
    }
}

/// builtins.sort: sort a list using a comparator.  cmp(a, b) is true if
/// a should come before b.
void primSort(EvalState & state, Value * args, Value & out)
{
    if (!args[1].isList()) typeError("sort", "list");
    auto * src = args[1].asList();
    Value cmp = args[0];
    if (!src || src->size <= 1) { out = args[1]; return; }
    ListVec * result = Alloc::allocList(src->size);
    V3_STATS_INC(listsAllocated);
    for (uint32_t i = 0; i < src->size; ++i) result->elems[i] = src->elems[i];
    // C-21 (CODEBASE_REVIEW_2026-06-11): TW's builtins.sort is STABLE (peeksort)
    // and tolerant of comparators that aren't a strict weak ordering, where
    // std::sort (introsort) reorders equal keys and is UB on a bad comparator
    // (equal keys can reach drv args → hash divergence).  std::stable_sort is
    // the minimum to match TW's equal-key ordering; a full peeksort port (for
    // the non-strict-weak-comparator robustness) is a follow-up.
    std::stable_sort(result->elems, result->elems + src->size,
        [&](const Value & a, const Value & b) {
            // P-5 (CODEBASE_REVIEW_2026-06-11): saturated 2-arg call — enters
            // an arity-2 comparator body once with both args in slots, instead
            // of the curried callClosure(callClosure(cmp,a),b) which allocated
            // a throwaway curry-PAP ValuePair PER COMPARISON (O(n log n) of
            // them). Falls back to the curried form byte-identically for any
            // other comparator shape.
            Value r = callClosure2(*state.vm, cmp, a, b);
            // Bytecode-closure comparator can return Tag::Thunk wrapping
            // a bool — force to WHNF before the shape check.
            if (__builtin_expect(r.tag() == Tag::Thunk
                                 || r.isAppLike()
                                 || r.tag() == Tag::Slot, 0))
                r = forceValue(*state.vm, r);
            if (!r.isBool()) typeError("sort", "comparator returning bool");
            return r.asInt() == 1;
        });
    out.mkList(result);
}

/// builtins.bitAnd / bitOr / bitXor on int.
void primBitAnd(EvalState &, Value * args, Value & out)
{
    if (!args[0].isInt() || !args[1].isInt()) typeError("bitAnd", "two ints");
    out.mkInt(args[0].asInt() & args[1].asInt());
}
void primBitOr(EvalState &, Value * args, Value & out)
{
    if (!args[0].isInt() || !args[1].isInt()) typeError("bitOr", "two ints");
    out.mkInt(args[0].asInt() | args[1].asInt());
}
void primBitXor(EvalState &, Value * args, Value & out)
{
    if (!args[0].isInt() || !args[1].isInt()) typeError("bitXor", "two ints");
    out.mkInt(args[0].asInt() ^ args[1].asInt());
}

/// floor / ceil for floats.
void primFloor(EvalState &, Value * args, Value & out)
{
    if (args[0].isInt())   { out = args[0]; return; }
    if (!args[0].isFloat()) typeError("floor", "float or int");
    out.mkInt(static_cast<int64_t>(std::floor(args[0].asFloat())));
}
void primCeil(EvalState &, Value * args, Value & out)
{
    if (args[0].isInt())   { out = args[0]; return; }
    if (!args[0].isFloat()) typeError("ceil", "float or int");
    out.mkInt(static_cast<int64_t>(std::ceil(args[0].asFloat())));
}

/// stringLength has a 1-arg version; stringToInt would be nice but
/// nix has only specific primops.  Add fromString-ish helpers:
void primParseInt(EvalState &, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("parseInt", "string");
    try {
        int64_t v = std::stoll(args[0].asString());
        out.mkInt(v);
    } catch (...) {
        throw std::runtime_error("v3 parseInt: invalid integer");
    }
}

/// tryEval: forces the argument; returns
///   { success = true;  value = result;       } on success,
///   { success = false; value = false;        } on caught exception.
void primTryEval(EvalState & state, Value * args, Value & out)
{
    SymbolId sSuccess = vmIntern(state, "success");
    SymbolId sValue   = vmIntern(state, "value");
    Value successV, valueV;
    try {
        valueV = forceValue(*state.vm, args[0]);
        successV = Value::vTrue;
    }
    // Match tree-walker semantics: catch only AssertionError-class
    // exceptions (assert / throw).  Type errors, abort, infinite
    // recursion, etc. propagate.  v3::AssertionError covers v3-side
    // throws; nix::AssertionError covers anything that crossed in
    // through the bridge from tree-walker code.
    catch (const AssertionError &) {
        successV = Value::vFalse;
        valueV = Value::vFalse;
    }
    catch (const ::nix::AssertionError &) {
        successV = Value::vFalse;
        valueV = Value::vFalse;
    }
    Bindings * b = Alloc::allocBindings(2);
    V3_STATS_INC(attrsetsAllocated);
    if (sSuccess < sValue) {
        bindingsSetEntry(b, 0, {sSuccess, 0, successV});  // Phase D
        bindingsSetEntry(b, 1, {sValue, 0, valueV});
    } else {
        bindingsSetEntry(b, 0, {sValue, 0, valueV});  // Phase D
        bindingsSetEntry(b, 1, {sSuccess, 0, successV});
    }
    out.mkAttrs(b);
}

/// Forward-declared so primLessThan can recurse through list elements.
static bool valueLessHelper(VMState & vm, const Value & a, const Value & b);

void primLessThan(EvalState & state, Value * args, Value & out)
{
    bool r = valueLessHelper(*state.vm, args[0], args[1]);
    out = r ? Value::vTrue : Value::vFalse;
}

static bool valueLessHelper(VMState & vm, const Value & a, const Value & b)
{
    if      (a.isInt() && b.isInt())     return a.asInt() < b.asInt();
    else if (a.isFloat() && b.isFloat()) return a.asFloat() < b.asFloat();
    else if (a.isInt() && b.isFloat())   return static_cast<double>(a.asInt()) < b.asFloat();
    else if (a.isFloat() && b.isInt())   return a.asFloat() < static_cast<double>(b.asInt());
    else if (a.isString() && b.isString())
        return std::string_view(a.asString()) < std::string_view(b.asString());
    // C-23 (CODEBASE_REVIEW_2026-06-11): TW compares two paths lexically; v3
    // rejected them ("expected comparable types").
    else if (a.isPath() && b.isPath())
        return std::string_view(a.asPath()) < std::string_view(b.asPath());
    else if (a.isList() && b.isList()) {
        // Lexicographic compare; force lazy elements as we go.
        uint32_t na = a.asList() ? a.asList()->size : 0;
        uint32_t nb = b.asList() ? b.asList()->size : 0;
        uint32_t n = std::min(na, nb);
        for (uint32_t i = 0; i < n; ++i) {
            Value ai = forceValue(vm, a.asList()->elems[i]);
            Value bi = forceValue(vm, b.asList()->elems[i]);
            if (valueLessHelper(vm, ai, bi)) return true;
            if (valueLessHelper(vm, bi, ai)) return false;
        }
        return na < nb;
    }
    typeError("lessThan", "comparable types");
}

} // namespace

// ---------------------------------------------------------------------------
// Public registry API
// ---------------------------------------------------------------------------

const PrimOp * findPrimOp(std::string_view name)
{
    std::lock_guard<std::mutex> g(registryMutex());
    auto it = registry().find(std::string(name));
    return it == registry().end() ? nullptr : &it->second;
}

const std::unordered_map<std::string, PrimOp> & allRegisteredPrimOps()
{
    return registry();
}

void setNixEvalState(nix::EvalState * st) { tlNixEvalState = st; }
nix::EvalState * getNixEvalState() { return tlNixEvalState; }

// STG-14b (#516): shared thread-local Bridge thunk cache.  See
// primop.hh getOrAllocBridgeThunkCached for full rationale.  The cache
// is intentionally pointer-keyed without an ABA stamp -- a TW Value
// may legitimately transition through tThunk -> tAttrs under our
// Bridge thunk, and we MUST return the same Thunk* across that
// transition.  Boehm-GC recycling of live Value addresses is rare in
// practice; the small risk of stale-bridge-on-recycle is worth it for
// the much larger win of consistent Thunk* identity (which is what
// v3's blackhole detection uses).
//
// GC_AUDIT_ROUND_2 Round 1 #3 (LATENT, documented 2026-05-21):
// The values stored here are `Thunk *` headers allocated via
// `Alloc::allocBridgeThunk` (tenured arena, always Boehm-rooted) and
// their bridge state has NO `cell` write-back today (vm.cc:5806's
// `prepHookUpvaluesAndWiths` is defunct; Bridge thunks carry only the
// `bridgeSrc` pointer).  Failure to walk this cache from the
// scavenger is therefore safe RIGHT NOW.  If the Bridge cell-update
// protocol is ever reintroduced (or `bridgeSrc` ever points at a
// nursery-resident object), this cache must be walked as a scavenger
// root.  See `src/libexpr-v3/lode/GC_AUDIT_ROUND_2_2026-05-21.md` §1
// row 3 for the diagnosis.
// (bridgeThunkCache + getOrAllocBridgeThunkCached retired —
//  TW_VALUE_ERADICATION F4, 2026-06-02.)

// `clearBridgeTables()` removed in REVIEW_2026-05-06b PR4 hygiene
// pass -- zero callers; daemon lifetime-aware bridge cleanup is a
// separate, larger problem (see primop.hh comment).

VMState * activeV3VM() { return tlActiveV3VMRef(); }
ScopedActiveV3VM::ScopedActiveV3VM(VMState * cur)
    : prev(tlActiveV3VMRef()) { tlActiveV3VMRef() = cur; }
ScopedActiveV3VM::~ScopedActiveV3VM() { tlActiveV3VMRef() = prev; }

// Per-primop call counters keyed by primop name (string_view backed by
// the registered PrimOp::name).  Aggregates across the whole process —
// invaluable for confirming which primops are hot on real workloads
// (nixpkgs / cardano-node / NixOS modules) rather than on synthetic
// fib/attrs benchmarks.
namespace {
struct PrimOpCounter {
    std::unordered_map<std::string, uint64_t> counts;
    // #788 (2026-05-23) per-primop wall-clock accumulator.  Gated by
    // NIX_VM_PRIMOP_TIME=1.  Time is in nanoseconds for the primop's
    // body execution INCLUDING any nested forceValue / callClosure
    // calls (those re-enter the dispatch loop; per #790 the outer
    // primop dispatch correctly inclusive-accounts that nested time).
    // Independent gate from the count counter so the timing overhead
    // (one chrono call per primop = ~10-20 ns) is paid only when the
    // diagnostic is on.
    std::unordered_map<std::string, uint64_t> nanos;
    std::mutex                                mtx;
};
PrimOpCounter & primOpCounter()
{
    // #453 Phase D: heap-allocated and intentionally leaked so the
    // mutex outlives the static-destruction phase.  The previous
    // function-static had an atexit destruction-order race with libc++
    // (mutex destroyed before some atexit-registered dump handlers
    // ran), which is why earlier code couldn't call dumpPrimOpStats
    // from atexit.  Now it can.
    static PrimOpCounter * c = new PrimOpCounter();
    return *c;
}
} // anonymous namespace

// #452 / Phase C → 2026-05-18: the `tlsShallowTWAttrsBridge` flag
// was retired when treeWalkerToV3 became always-shallow for both
// nAttrs and nList cases.  The previous conditional (deep recursion
// when the flag was unset) accounted for ~60% of CPU on hello.drvPath
// per the sample profile, and the shallow path was always
// semantically correct (Bridge thunks force lazily; force-time
// conversion matches the original deep result).  pushShallowTWAttrsBridge
// / popShallowTWAttrsBridge / shallowTWAttrsBridge symbols were
// referenced only from vm.cc; their call sites are now no-ops.  See
// PROFILE_HELLO_NAME_2026-05-18.md option #1.

void bumpPrimOpCallCount(const PrimOp * po)
{
    if (!po) return;
    auto & c = primOpCounter();
    std::lock_guard<std::mutex> g(c.mtx);
    c.counts[std::string(po->name)]++;
}

// #788 (2026-05-23) — per-primop wall-clock accumulator.  Called
// from vm.cc OP_CALL_PRIMOP under NIX_VM_PRIMOP_TIME=1.  Adds the
// elapsed body-time (chrono-measured by the caller) to the per-
// name bucket.  Mutex-protected for cross-thread safety but on
// the single-threaded VM the overhead is just one uncontended
// lock per call (~20-30 ns).
void bumpPrimOpNanos(const PrimOp * po, uint64_t deltaNs)
{
    if (!po) return;
    auto & c = primOpCounter();
    std::lock_guard<std::mutex> g(c.mtx);
    c.nanos[std::string(po->name)] += deltaNs;
}

void dumpPrimOpStats(std::FILE * out)
{
    // (bridge-primop call counters retired with the bridge apparatus —
    //  TW_VALUE_ERADICATION F4, 2026-06-02.)
    auto & c = primOpCounter();
    std::lock_guard<std::mutex> g(c.mtx);
    if (c.counts.empty()) return;
    // Sort by descending count — the top of the list is what we
    // actually care about when reasoning about bridge cost / native
    // candidates.
    std::vector<std::pair<std::string, uint64_t>> rows(
        c.counts.begin(), c.counts.end());
    std::sort(rows.begin(), rows.end(),
        [](const auto & a, const auto & b) { return a.second > b.second; });
    std::fprintf(out, "v3 primop call counts (top 30 of %zu):\n",
                 rows.size());
    for (size_t i = 0; i < rows.size() && i < 30; ++i)
        std::fprintf(out, "  %8llu  %s\n",
                     (unsigned long long)rows[i].second,
                     rows[i].first.c_str());

    // #788 per-primop wall-clock breakdown (only present when
    // NIX_VM_PRIMOP_TIME=1 was set during eval).  Sorted by total
    // nanos descending — the top of the list is the actual lever
    // for primop-side optimisation.
    if (!c.nanos.empty()) {
        std::vector<std::pair<std::string, uint64_t>> trows(
            c.nanos.begin(), c.nanos.end());
        std::sort(trows.begin(), trows.end(),
            [](const auto & a, const auto & b) { return a.second > b.second; });
        uint64_t totalNs = 0;
        for (auto & r : trows) totalNs += r.second;
        std::fprintf(out,
            "v3 primop wall-clock (top 15 of %zu; total %llu ns ≈ %.1f ms):\n",
            trows.size(),
            (unsigned long long)totalNs,
            totalNs / 1e6);
        for (size_t i = 0; i < trows.size() && i < 15; ++i) {
            uint64_t cnt = 0;
            auto cit = c.counts.find(trows[i].first);
            if (cit != c.counts.end()) cnt = cit->second;
            std::fprintf(out,
                "  %12llu ns  count=%llu  avg=%.1f ns/call  %s\n",
                (unsigned long long)trows[i].second,
                (unsigned long long)cnt,
                cnt > 0 ? (double)trows[i].second / cnt : 0.0,
                trows[i].first.c_str());
        }
    }
}

void dumpHotDescriptors(std::FILE * out, size_t limit,
                         const CompilationUnit * entryCu)
{
    // Collect every (forceCount, desc, cu) triple from importCache()'s
    // CUs and the entry CU.  Skip descriptors with zero forces — most
    // lambdas are cold.
    struct Row {
        uint64_t                       forces;
        const LambdaDescriptor *       desc;
        const CompilationUnit *        cu;
    };
    std::vector<Row> rows;
    auto walk = [&](const CompilationUnit & cu) {
        for (const auto & d : cu.lambdas) {
            if (d.forceCount > 0)
                rows.push_back({d.forceCount, &d, &cu});
        }
    };
    auto & cache = importCache();
    for (const auto & cu : cache.cus) walk(cu);
    if (entryCu) walk(*entryCu);
    if (rows.empty()) return;
    std::sort(rows.begin(), rows.end(),
        [](const Row & a, const Row & b) { return a.forces > b.forces; });
    size_t n = std::min(rows.size(), limit);
    std::fprintf(out,
        "v3 hot LambdaDescriptors (top %zu of %zu, all CUs):\n",
        n, rows.size());
    for (size_t i = 0; i < n; ++i) {
        const auto & d = *rows[i].desc;
        const PosSnapshot * ps = resolvePosSnapshot(d.posHandle);
        std::string posStr;
        if (ps && !ps->file.empty()) {
            posStr = ps->file + ":" + std::to_string(ps->line)
                   + ":" + std::to_string(ps->column);
        }
        std::fprintf(out,
            "  forces=%-9llu nUp=%-3u name=%-30s cu=%p%s%s\n",
            (unsigned long long)rows[i].forces,
            (unsigned)d.nUpvalues,
            d.name.empty() ? "<anon>" : d.name.c_str(),
            (const void *)rows[i].cu,
            posStr.empty() ? "" : "  ",
            posStr.c_str());
    }
}

// Forward to the anonymous-namespace shim (initialised at static-init
// time).  Public — exported for v3 internal callers (vm.cc OP_CALL
// Bridge-thunk branch, etc.).
// (v3ToTreeWalkerShim + v3ToTreeWalkerPublic retired —
//  TW_VALUE_ERADICATION F4, 2026-06-02.)

// (tryUnwrapBridge1Closure / tryDispatchBridge1Direct /
//  tryDispatchFormalsLambdaBridge / treeWalkerToV3Public /
//  forceBridgeThunk / tryBridgeAttrLookup / tryFastBridgeScalarTwToV3
//  retired — TW_VALUE_ERADICATION F4, 2026-06-02.)

// (BridgeStats bridge telemetry retired — TW_VALUE_ERADICATION F4, 2026-06-02.)

// (bridgeTimingEnabled bridge telemetry retired — TW_VALUE_ERADICATION F4, 2026-06-02.)

// #769 (2026-05-22) — primImport per-phase timing.  See
// `include/v3/import_timing.hh` for the rationale.  Definitions live
// here next to the bridge-timing precedent so they share the
// V3_TIMING-gated style.
ImportTimingTotals & importTimingTotals() noexcept
{
    static ImportTimingTotals t;
    return t;
}

bool importTimingEnabled() noexcept
{
    static const bool e = std::getenv("V3_TIMING") != nullptr;
    return e;
}

// (bridgeTotalNs bridge telemetry retired — TW_VALUE_ERADICATION F4, 2026-06-02.)

// (bridgeTelemetryBump bridge telemetry retired — TW_VALUE_ERADICATION F4, 2026-06-02.)

// (BridgeTimer:: bridge telemetry retired — TW_VALUE_ERADICATION F4, 2026-06-02.)

// (dumpBridgeTelemetry bridge telemetry retired — TW_VALUE_ERADICATION F4, 2026-06-02.)

// (#458 A.4 tryBridgeAttrHas retired — TW_VALUE_ERADICATION F4, 2026-06-02.)

// ---------------------------------------------------------------------------
// WC-28a: small missing primops (placeholder, __warn, break, __outputOf,
//   __storePath, __toFile).  All previously fell back to tree-walker.
//   Native v3 implementations bring v3 closer to "owns the world" (Agent B
//   B12).  Each implementation mirrors tree-walker semantics — see
//   src/libexpr/primops.cc for the canonical references.
// ---------------------------------------------------------------------------

/// builtins.placeholder "out" → output-placeholder string.  Tree-walker:
/// src/libexpr/primops.cc:2008 (prim_placeholder).  Pure function — no
/// state interaction beyond the EvalState's mem allocator.
void primPlaceholder(EvalState & state, Value * args, Value & out)
{
    (void)state;
    if (!args[0].isString()) typeError("placeholder", "string");
    auto ph = nix::hashPlaceholder(std::string_view(args[0].asString()));
    out = mkStringValueOwned(std::move(ph));
}

/// builtins.__warn "msg" v → print msg to stderr, return v.  Tree-walker:
/// src/libexpr/primops.cc:1451 (prim_warn).  v3 simplifies: emits the
/// "warning:" prefix, returns args[1] unchanged.  Doesn't honour
/// abort-on-warn settings (parity-relevant for that subset of users).
void primWarn(EvalState &, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("warn", "string");
    std::fprintf(stderr, "warning: %s\n", args[0].asString());
    out = args[1];
}

/// builtins.break v → debug-mode breakpoint, returns v.  Tree-walker:
/// src/libexpr/primops.cc:1101 (primop_break).  v3 has no debugger
/// support (canDebug() always false), so this is a pass-through.
void primBreak(EvalState &, Value * args, Value & out)
{
    out = args[0];
}

/// builtins.__storePath path → ensure path is in the store, return as
/// string with context.  Tree-walker: src/libexpr/primops.cc:2070
/// (prim_storePath).  Requires tree-walker store (state.nixEvalState).
void primStorePath(EvalState & state, Value * args, Value & out)
{
    if (!state.nixEvalState)
        throw std::runtime_error("v3 storePath: no tree-walker state available");
    auto * ns = state.nixEvalState;
    Value v = args[0];
    if (v.isPath()) {
        // ok
    } else if (v.isString()) {
        // ok
    } else {
        typeError("storePath", "path or string");
    }
    std::string pathStr = v.isPath() ? std::string(v.asPath())
                                      : std::string(v.asString());
    nix::CanonPath path(pathStr);
    if (!ns->store->isStorePath(path.abs()))
        path = nix::CanonPath(nix::canonPath(path.abs(), true).string());
    if (!ns->store->isInStore(path.abs()))
        throw std::runtime_error("v3 storePath: path '" + path.abs() +
                                  "' is not in the Nix store");
    auto path2 = ns->store->toStorePath(path.abs()).first;
    if (!ffi::readOnlyMode())
        ns->store->ensurePath(path2);
    // Build the result string V3-NATIVE (path string + Opaque context) — no
    // treeWalkerToV3 bridge (toward F4).
    out = mkStringValueOwned(path.abs());
    std::vector<std::string> ctx{
        nix::NixStringContextElem{nix::NixStringContextElem::Opaque{.path = path2}}.to_string()};
    setStringContextEntries(out.asString(), std::move(ctx));
}

/// builtins.__toFile name s → write s to store, return path.
/// Tree-walker: src/libexpr/primops.cc:2801 (prim_toFile).
void primToFile(EvalState & state, Value * args, Value & out)
{
    if (!state.nixEvalState)
        throw std::runtime_error("v3 toFile: no tree-walker state available");
    auto * ns = state.nixEvalState;
    if (!args[0].isString()) typeError("toFile", "string name");
    if (!args[1].isString()) typeError("toFile", "string contents");
    // #682 — match TW (libexpr/primops.cc:2820-2834): "name" must have
    // NO context (forceStringNoCtx); "contents" may have context, but
    // only Opaque (store-path) entries are allowed.  Built/Drv entries
    // (i.e., derivation references) trigger TW's exact rejection
    // message because the resulting store-file would shadow a real
    // derivation's outputs.
    //
    // Pre-fix v3 ignored the contents' context entirely → silently
    // wrote a file with no refs, producing a wrong-hash store path
    // when the contents contained a derivation interpolation
    // (`builtins.toFile "x" "${drv}"`).  This produced a different
    // drvPath than TW would, breaking any downstream derivation that
    // depended on the toFile output.
    requireNoStringContext(state, args[0], "toFile name");
    std::string name(args[0].asString());
    std::string contents(args[1].asString());
    nix::StorePathSet refs;
    if (args[1].asString()) {
        nix::NixStringContext ctx = lookupStringContext(args[1].asString());
        for (auto & c : ctx) {
            if (auto p = std::get_if<nix::NixStringContextElem::Opaque>(&c.raw)) {
                refs.insert(p->path);
            } else {
                // Match TW phrasing byte-for-byte
                // (libexpr/primops.cc:2828).
                throw std::runtime_error(
                    "files created by builtins.toFile may not reference derivations, but "
                    + name + " references " + c.to_string());
            }
        }
    }
    // Store add behind the FFI leaf (StringSource / FileSerialisationMethod
    // / TextInfo live in ffi.cc); build the result string V3-NATIVE from the
    // returned path + Opaque context (no treeWalkerToV3 bridge).
    ffi::FetchUrlResult r = ffi::addTextToStore(*ns, name, contents, std::move(refs), ffi::readOnlyMode());
    out = mkStringValueOwned(r.printedStorePath);
    std::vector<std::string> ctx{ r.opaqueContextElem };
    setStringContextEntries(out.asString(), std::move(ctx));
}

/// builtins.__outputOf drvRef outputName → input placeholder for that
/// derivation's named output.  Tree-walker: src/libexpr/primops.cc:2589
/// (prim_outputOf).  Used for chained derivation outputs.
///
/// REVIEW §3: forward v3-side string-context from args[0] when
/// constructing tw0 -- without it, a drvRef carrying a context entry
/// (e.g. from a chained `builtins.outputOf prevDrv "out"`) silently
/// loses the upstream derivation reference.
void primOutputOf(EvalState & state, Value * args, Value & out)
{
    if (!state.nixEvalState)
        throw std::runtime_error("v3 outputOf: no tree-walker state available");
    // TW_VALUE_ERADICATION: the coerceToSingleDerivedPath +
    // mkSingleDerivedPathString sequence (SingleDerivedPath / derived-path
    // live in ffi.cc) runs behind ffi::outputOf, returning the placeholder
    // string + its (Built) context as plain data; the v3 result is built
    // V3-NATIVE (mkString + setStringContextEntries) — no treeWalkerToV3
    // bridge.  Equivalent to the old bridge because that just copied the
    // string value + context too (see treeWalkerToV3 nString case).
    Value drvRefV = forceValue(*state.vm, args[0]);
    Value outNameV = forceValue(*state.vm, args[1]);
    if (!outNameV.isString()) typeError("outputOf", "string output name");
    std::string drvRef = drvRefV.isString() && drvRefV.asString()
        ? std::string(drvRefV.asString()) : std::string();
    std::vector<std::string> drvRefCtx;
    if (drvRefV.isString() && drvRefV.asString()) {
        if (auto * raw = lookupStringContextEntries(drvRefV.asString()))
            drvRefCtx = *raw;
    }
    ffi::StringWithContext r = ffi::outputOf(
        *state.nixEvalState, drvRef, drvRefCtx, std::string(outNameV.asString()));
    out = mkStringValueOwned(r.value);
    if (!r.contextElems.empty())
        setStringContextEntries(out.asString(), std::move(r.contextElems));
}

// ---------------------------------------------------------------------------
// WC-28b: fetch primops.  Each delegates to the corresponding tree-walker
// primop in `builtins` (since libnixfetchers integration is heavy and not
// worth duplicating for primops that are inherently store/network bound).
// Same bridge pattern as BR-4's `primPath` fall-through (primops.cc:4457).
// ---------------------------------------------------------------------------

// (TW-VALUE ERADICATION: the generic `bridgeBuiltin<N>` round-trip — which
// did `v3ToTreeWalker(args) → callFunction(builtins.<name>) → treeWalkerToV3`
// for the 8 fetchers — is GONE.  Every fetcher is now V3-NATIVE; bridgeBuiltin
// had zero remaining callers, so it's deleted along with its bridge uses.)

// TW-VALUE ERADICATION F1/F2 (TW_VALUE_ERADICATION_GOAL §4): build the
// plain-data ffi::FetchTreeInput from the v3 arg, reproducing TW's fetchTree
// helper arg-normalization (fetchTree.cc:92-194) V3-NATIVE — no
// v3ToTreeWalker, no callFunction, no TW Value.  Then ffi::fetchTree does
// the fetch (library call) + v3EmitTreeAttrs builds the result attrset.
Value v3EmitTreeAttrs(const ffi::TreeAttrsInfo & info);   // v3_call_flake.cc

static ffi::FetchTreeInput extractFetchTreeInput(
    EvalState & state, Value & arg, const char * fetcher,
    bool isFetchGit, bool allowNameArgument)
{
    auto & symTab = ir::globalSymbolTable();
    ffi::FetchTreeInput in;
    in.fetcherName = fetcher;

    Value a = forceValue(*state.vm, arg);
    std::optional<std::string> type;
    if (isFetchGit) type = "git";

    auto has = [&](const char * n) {
        for (auto & x : in.attrs) if (x.name == n) return true;
        return false;
    };
    auto boolVal = [&](const char * n) -> bool {
        for (auto & x : in.attrs)
            if (x.name == n && std::holds_alternative<bool>(x.value)) return std::get<bool>(x.value);
        return false;
    };

    if (a.isAttrs() && a.asAttrs()) {
        const Bindings * b = a.asAttrs();
        static const SymbolId sidType = ir::globalInternSymbol("type");

        if (const Value * tv = b->lookup(sidType)) {
            if (type)
                throw std::runtime_error("unexpected argument 'type'");
            Value t = forceValue(*state.vm, *tv);
            if (t.tag() != Tag::String || !t.asString())
                throw std::runtime_error(std::string(
                    "while evaluating the `type` argument passed to '") + fetcher + "': expected a string");
            if (auto * raw = lookupStringContextEntries(t.asString()); raw && !raw->empty())
                throw std::runtime_error(std::string(
                    "the string argument passed as `type` to '") + fetcher + "' is not allowed to refer to a store path");
            type = std::string(t.asString());
        } else if (!type)
            throw std::runtime_error(std::string("argument 'type' is missing in call to '") + fetcher + "'");

        in.attrs.push_back({"type", *type});

        b->forEach([&](const Bindings::Entry & e) {
            const SymbolId nameId = e.name;
            if (nameId == sidType) return;
            std::string name(nameId < symTab.size() ? symTab[nameId] : std::to_string(nameId));
            Value v = forceValue(*state.vm, e.value);
            Tag t = v.tag();
            if (t == Tag::String || t == Tag::Path) {
                std::string s = (t == Tag::String)
                    ? std::string(v.asString() ? v.asString() : "")
                    : std::string(v.asPath() ? v.asPath() : "");
                if (isFetchGit && name == "url") s = ffi::fixGitURL(s);
                in.attrs.push_back({name, std::move(s)});
            } else if (t == Tag::Bool) {
                in.attrs.push_back({name, v.asInt() == 1});
            } else if (t == Tag::Int) {
                int64_t iv = v.asInt();
                if (iv < 0)
                    throw std::runtime_error(
                        "negative value given for '" + std::string(fetcher) + "' argument '" + name
                        + "': " + std::to_string(iv));
                in.attrs.push_back({name, iv});
            } else if (name == "publicKeys") {
                nix::experimentalFeatureSettings.require(nix::Xp::VerifiedFetches);
                in.attrs.push_back({name, toJsonValue(*state.vm, v, symTab).dump()});
            } else {
                throw std::runtime_error(
                    "argument '" + name + "' to '" + std::string(fetcher)
                    + "' is the wrong type (a string, Boolean or integer is expected)");
            }
        });

        // fetchGit exportIgnore default + fetchTree shallow default + name gating.
        if (isFetchGit && !has("exportIgnore") && (!has("submodules") || !boolVal("submodules")))
            in.attrs.push_back({"exportIgnore", true});
        if (type == "git" && !isFetchGit && !has("shallow"))
            in.attrs.push_back({"shallow", true});
        if (!allowNameArgument && has("name"))
            throw std::runtime_error(std::string("argument 'name' isn’t supported in call to '") + fetcher + "'");
    } else {
        // string / URL form.
        if (a.tag() != Tag::String && a.tag() != Tag::Path)
            throw std::runtime_error(std::string(
                "while evaluating the first argument passed to '") + fetcher + "': expected a string or attrset");
        std::string url = (a.tag() == Tag::String)
            ? std::string(a.asString() ? a.asString() : "")
            : std::string(a.asPath() ? a.asPath() : "");
        if (isFetchGit) {
            in.attrs.push_back({"type", std::string("git")});
            in.attrs.push_back({"url", ffi::fixGitURL(url)});
            in.attrs.push_back({"exportIgnore", true});
        } else {
            if (!nix::experimentalFeatureSettings.isEnabled(nix::Xp::Flakes))
                throw std::runtime_error(std::string(
                    "passing a string argument to '") + fetcher
                    + "' requires the 'flakes' experimental feature");
            in.url = url;
        }
    }
    return in;
}

// F1/F2: native fetchTree-family entry (replaces the bridgeBuiltin round-trip).
static void v3FetchTree(EvalState & s, Value * a, Value & o,
                        const char * fetcher, bool isFetchGit,
                        bool allowNameArgument, bool emptyRevFallback,
                        bool isFinal = false)
{
    if (!s.nixEvalState)
        throw std::runtime_error(std::string("v3 ") + fetcher + ": no tree-walker state available");
    auto & ns = *s.nixEvalState;
    auto in = extractFetchTreeInput(s, a[0], fetcher, isFetchGit, allowNameArgument);
    ffi::TreeAttrsInfo info = ffi::fetchTree(ns, in, emptyRevFallback, isFinal);
    o = v3EmitTreeAttrs(info);
}

// forceStringNoCtx for a v3 value: force, require String, reject context
// (matches TW state.forceStringNoCtx).  Used by the fetchurl/fetchTarball
// arg extraction.
static std::string v3ForceStringNoCtx(EvalState & state, const Value & vIn, const char * what)
{
    Value v = forceValue(*state.vm, vIn);
    if (v.tag() != Tag::String || !v.asString())
        throw std::runtime_error(std::string(what) + ": expected a string");
    if (auto * raw = lookupStringContextEntries(v.asString()); raw && !raw->empty())
        throw std::runtime_error(
            std::string(what) + ": the string is not allowed to refer to a store path");
    return std::string(v.asString());
}

// TW-VALUE ERADICATION F2: native fetchurl/fetchTarball (the `fetch()`
// family).  Extracts url/sha256/name V3-NATIVE (TW fetchTree.cc:389-414),
// calls ffi::fetchUrl (the libfetchers leaf — downloadFile/downloadTarball),
// builds the store-path string V3-NATIVE with Opaque context.  No bridge.
static void v3Fetch(EvalState & s, Value * a, Value & o,
                    const char * who, bool unpack, const char * defaultName)
{
    if (!s.nixEvalState)
        throw std::runtime_error(std::string("v3 ") + who + ": no tree-walker state available");
    auto & ns = *s.nixEvalState;

    Value arg = forceValue(*s.vm, a[0]);
    std::optional<std::string> url, sha256;
    std::string name = defaultName;

    if (arg.isAttrs() && arg.asAttrs()) {
        const Bindings * b = arg.asAttrs();
        auto & symTab = ir::globalSymbolTable();
        b->forEach([&](const Bindings::Entry & e) {
            const SymbolId nid = e.name;
            std::string n(nid < symTab.size() ? symTab[nid] : std::to_string(nid));
            if (n == "url")
                url = v3ForceStringNoCtx(s, e.value, "while evaluating the url we should fetch");
            else if (n == "sha256")
                sha256 = v3ForceStringNoCtx(s, e.value, "while evaluating the sha256 of the content we should fetch");
            else if (n == "name")
                name = v3ForceStringNoCtx(s, e.value, "while evaluating the name of the content we should fetch");
            else
                throw std::runtime_error("unsupported argument '" + n + "' to '" + std::string(who) + "'");
        });
        if (!url)
            throw std::runtime_error("'url' argument required");
    } else {
        url = v3ForceStringNoCtx(s, arg, "while evaluating the url we should fetch");
    }

    ffi::FetchUrlResult r = ffi::fetchUrl(ns, *url, sha256, name, unpack, who);
    o = mkStringValueOwned(r.printedStorePath);
    std::vector<std::string> ctx{ r.opaqueContextElem };
    setStringContextEntries(o.asString(), std::move(ctx));
}

void primFetchurl    (EvalState & s, Value * a, Value & o) { v3Fetch(s, a, o, "fetchurl",     false, ""); }
// #700/step 3: v3-side wrapper for TW's `internalPrimOps["fetchFinalTree"]`.
// Unlike `builtins.fetchTree` (in TW's builtins attrset), fetchFinalTree
// is registered in TW's `internalPrimOps` map and isn't reachable via
// `bridgeBuiltin` (which looks up `builtins.<name>`).  This wrapper looks
// up the TW primop in internalPrimOps directly + dispatches the FFI fetch
// + bridges the result.  Registered as `__fetchFinalTree` for v3.
// F2 (eradication): fetchFinalTree = fetchTree with isFinal=true
// (prim_fetchFinalTree, fetchTree.cc:363).  Native — no internalPrimOps
// callFunction, no TW Value round-trip.
void primFetchFinalTree(EvalState & s, Value * a, Value & o) {
    v3FetchTree(s, a, o, "fetchTree", /*isFetchGit=*/false, /*allowName=*/false,
                /*emptyRevFallback=*/false, /*isFinal=*/true);
}
void primFetchTarball(EvalState & s, Value * a, Value & o) { v3Fetch(s, a, o, "fetchTarball", true,  "source"); }
// F1/F2 (TW-value eradication): native plain-data path — no bridgeBuiltin.
// Params mirror TW prim_fetchTree / prim_fetchGit (fetchTree.cc:230/586).
void primFetchTree   (EvalState & s, Value * a, Value & o) {
    v3FetchTree(s, a, o, "fetchTree", /*isFetchGit=*/false, /*allowName=*/false, /*emptyRevFallback=*/false);
}
void primFetchGit    (EvalState & s, Value * a, Value & o) {
    v3FetchTree(s, a, o, "fetchGit",  /*isFetchGit=*/true,  /*allowName=*/true,  /*emptyRevFallback=*/true);
}
// F2 (eradication): native fetchMercurial — extract url/rev/name V3-NATIVE,
// ffi::fetchMercurial (libfetchers hg input + fetchToStore), build the
// result attrset { outPath; branch?; rev; shortRev; revCount?; } v3-native.
// Note: fetchMercurial's result shape ≠ emitTreeAttrs (branch + 12-char
// shortRev, no narHash/lastModified), so it has its own builder.
void primFetchMercurial(EvalState & s, Value * a, Value & o) {
    if (!s.nixEvalState)
        throw std::runtime_error("v3 fetchMercurial: no tree-walker state available");
    auto & ns = *s.nixEvalState;

    Value arg = forceValue(*s.vm, a[0]);
    auto readUrl = [&](const Value & v, const char * what) -> std::string {
        if (v.tag() == Tag::String) return std::string(v.asString() ? v.asString() : "");
        if (v.tag() == Tag::Path)   return std::string(v.asPath() ? v.asPath() : "");
        throw std::runtime_error(std::string(what) + ": expected a string or path");
    };
    std::string url;
    std::optional<std::string> revOrRef;
    std::string name = "source";

    if (arg.isAttrs() && arg.asAttrs()) {
        const Bindings * b = arg.asAttrs();
        auto & symTab = ir::globalSymbolTable();
        b->forEach([&](const Bindings::Entry & e) {
            const SymbolId nid = e.name;
            std::string n(nid < symTab.size() ? symTab[nid] : std::to_string(nid));
            if (n == "url")
                url = readUrl(forceValue(*s.vm, e.value),
                              "while evaluating the `url` attribute passed to builtins.fetchMercurial");
            else if (n == "rev")
                revOrRef = v3ForceStringNoCtx(s, e.value,
                              "while evaluating the `rev` attribute passed to builtins.fetchMercurial");
            else if (n == "name")
                name = v3ForceStringNoCtx(s, e.value,
                              "while evaluating the `name` attribute passed to builtins.fetchMercurial");
            else
                throw std::runtime_error("unsupported argument '" + n + "' to 'fetchMercurial'");
        });
        if (url.empty())
            throw std::runtime_error("'url' argument required");
    } else {
        url = readUrl(arg, "while evaluating the first argument passed to builtins.fetchMercurial");
    }

    ffi::FetchMercurialResult r = ffi::fetchMercurial(ns, url, revOrRef, name);

    std::vector<std::pair<SymbolId, Value>> entries;
    {
        Value v = mkStringValueOwned(r.outPath);
        std::vector<std::string> c{ r.opaqueContextElem };
        setStringContextEntries(v.asString(), std::move(c));
        entries.emplace_back(ir::globalInternSymbol("outPath"), v);
    }
    if (r.branch)
        entries.emplace_back(ir::globalInternSymbol("branch"), mkStringValueOwned(*r.branch));
    entries.emplace_back(ir::globalInternSymbol("rev"), mkStringValueOwned(r.rev));
    entries.emplace_back(ir::globalInternSymbol("shortRev"), mkStringValueOwned(r.rev.substr(0, 12)));
    if (r.revCount) {
        Value vc; vc.mkInt(*r.revCount);
        entries.emplace_back(ir::globalInternSymbol("revCount"), vc);
    }
    std::sort(entries.begin(), entries.end(),
              [](const auto & x, const auto & y) { return x.first < y.first; });
    Bindings * bb = Alloc::allocBindings(static_cast<uint32_t>(entries.size()));
    V3_STATS_INC(attrsetsAllocated);
    for (size_t i = 0; i < entries.size(); ++i)
        bindingsSetEntry(bb, static_cast<uint32_t>(i), {entries[i].first, 0, entries[i].second});
    o.mkAttrs(bb);
}
// F2 (eradication): native fetchClosure — extract the 4 args V3-NATIVE,
// ffi::fetchClosure (openStore + copyClosure/makeContentAddressed dispatch),
// build the result store-path string v3-native with Opaque context.
void primFetchClosure(EvalState & s, Value * a, Value & o) {
    if (!s.nixEvalState)
        throw std::runtime_error("v3 fetchClosure: no tree-walker state available");
    auto & ns = *s.nixEvalState;

    Value arg = forceValue(*s.vm, a[0]);
    if (!arg.isAttrs() || !arg.asAttrs())
        throw std::runtime_error(
            "while evaluating the argument passed to builtins.fetchClosure: expected an attribute set");
    const Bindings * b = arg.asAttrs();
    auto & symTab = ir::globalSymbolTable();

    auto strOf = [&](const Value & vIn) -> std::string {
        Value fv = forceValue(*s.vm, vIn);
        if (fv.tag() == Tag::String) return std::string(fv.asString() ? fv.asString() : "");
        if (fv.tag() == Tag::Path)   return std::string(fv.asPath() ? fv.asPath() : "");
        throw std::runtime_error("fetchClosure: expected a string or path attribute");
    };

    std::optional<std::string> fromStore, fromPath, toPath;
    std::optional<bool> inputAddressed;
    b->forEach([&](const Bindings::Entry & e) {
        const SymbolId nid = e.name;
        std::string n(nid < symTab.size() ? symTab[nid] : std::to_string(nid));
        if (n == "fromStore")
            fromStore = v3ForceStringNoCtx(s, e.value,
                          "while evaluating the 'fromStore' attribute passed to builtins.fetchClosure");
        else if (n == "fromPath")
            fromPath = strOf(e.value);       // coerceToStorePath (ffi parses)
        else if (n == "toPath")
            toPath = strOf(e.value);         // "" ⇒ gap
        else if (n == "inputAddressed") {
            Value fv = forceValue(*s.vm, e.value);
            if (fv.tag() != Tag::Bool)
                throw std::runtime_error("fetchClosure: 'inputAddressed' must be a Boolean");
            inputAddressed = (fv.asInt() == 1);
        } else
            throw std::runtime_error("attribute '" + n + "' isn't supported in call to 'fetchClosure'");
    });
    if (!fromPath)
        throw std::runtime_error("attribute 'fromPath' is missing in call to 'fetchClosure'");
    if (!fromStore)
        throw std::runtime_error("attribute 'fromStore' is missing in call to 'fetchClosure'");

    ffi::FetchUrlResult r = ffi::fetchClosure(ns, *fromStore, *fromPath, toPath,
                                              inputAddressed.value_or(false));
    o = mkStringValueOwned(r.printedStorePath);
    std::vector<std::string> ctx{ r.opaqueContextElem };
    setStringContextEntries(o.asString(), std::move(ctx));
}
// F3 (eradication): native filterSource — copy the path to the store with a
// PathFilter that RE-ENTERS v3's VM (callClosure) per directory entry.  No
// bridge: the filter stays a v3 closure; ffi::addPathFiltered drives the
// libstore copy + calls back via the v3filter lambda below.
void primFilterSource(EvalState & s, Value * a, Value & o) {
    if (!s.nixEvalState)
        throw std::runtime_error("v3 filterSource: no tree-walker state available");
    auto & ns = *s.nixEvalState;

    // args[0] = filter function, args[1] = path (coerceToPath).
    Value pathV = forceValue(*s.vm, a[1]);
    std::string pathStr;
    if (pathV.tag() == Tag::Path)
        pathStr = pathV.asPath() ? pathV.asPath() : "";
    else if (pathV.tag() == Tag::String)
        pathStr = pathV.asString() ? pathV.asString() : "";
    else
        throw std::runtime_error(
            "while evaluating the second argument (the path to filter) passed to "
            "'builtins.filterSource': expected a path");
    Value filterFn = forceValue(*s.vm, a[0]);  // forceFunction (callable checked at callClosure)

    // Per-entry filter: callClosure(filterFn, absPath)(type) -> Bool.  Runs
    // inside fetchToStore (nested v3 eval on the same VMState — STG-10).
    auto v3filter = [&](const std::string & p, const std::string & type) -> bool {
        // P-5: saturated 2-arg call (no per-entry throwaway curry-PAP).
        Value r2 = callClosure2(*s.vm, filterFn,
                                mkStringValueOwned(p), mkStringValueOwned(type));
        r2 = forceValue(*s.vm, r2);
        if (r2.tag() != Tag::Bool)
            throw std::runtime_error(
                "while evaluating the return value of the path filter function: "
                "expected a Boolean");
        return r2.asInt() == 1;
    };

    // filterSource: name defaults to baseName, recursive (NixArchive), no sha256.
    ffi::FetchUrlResult r =
        ffi::addPathFiltered(ns, pathStr, /*name=*/"", /*recursive=*/true,
                             /*sha256=*/std::nullopt, v3filter);
    o = mkStringValueOwned(r.printedStorePath);
    std::vector<std::string> ctx{ r.opaqueContextElem };
    setStringContextEntries(o.asString(), std::move(ctx));
}
// Path B M3: getFlake is registered into TW via evalSettings.extraPrimOps
// (libflake/settings.cc:14).  bridgeBuiltin resolves it by name from
// TW's builtins attrset at call time — so the lazy registration order
// (libflake settings → TW state init → v3 startup → first call) works
// out as long as the experimental-features gate is on (otherwise TW
// throws on access, which the bridge propagates verbatim).
//
// Note: primParseFlakeRef and primFlakeRefToString are already
// implemented natively in v3 above (lines 7319 / 7380); only getFlake
// needs the TW bridge here.
// #758: v3-native getFlake — sole implementation.
// Forward-declare callFlakeV3 (defined in v3_call_flake.cc).  Audit Phase 4:
// callFlakeV3 now takes plain data (ffi::LockedFlakeInfo) so v3_call_flake.cc
// names no libflake type; the LockedFlake → plain-data read happens here via
// ffi::readLockedFlake (this TU legitimately holds the libflake types).
Value callFlakeV3(EvalState & state, const ffi::LockedFlakeInfo & flakeInfo);

void primGetFlake(EvalState & s, Value * a, Value & o) {
    // History:
    //   - 88199c4a0 / 511074ff6: first default-on attempt — REVERTED
    //     by 6cb4ecdb7 (over-forcing on haskell.nix flakes).
    //   - c25e9ccdb (#757)   : chase-limit raise — unblocked the
    //     legitimate 4096-deep Slot chain through composeExtensions.
    //   - 37f18b2f6 (#757b)  : primImport realisePath for strings
    //     with context.
    //   - 0c7c4f191 (#757c)  : primReadFile file-ref context source
    //     + primUnsafeDiscardStringContext coerce semantics.
    //   - (this commit, #758): v3-native callFlake is now the SOLE
    //     getFlake implementation.  Both retired env-var gates
    //     (NIX_V3_NATIVE_CALL_FLAKE, NIX_V3_NO_NATIVE_CALL_FLAKE) are
    //     no-ops; the legacy TW-bridge code path is deleted.
    //     `test/run-758-callflake-sweep.sh` proves byte-identical
    //     parity between v3-native and the (pre-retirement) bridge
    //     across 15 nixpkgs + cardano-node queries.
    //
    // callFlakeV3 requires the host TW EvalState (the lockFlake +
    // emitTreeAttrs FFI leaves reach back into it).  If it's missing we
    // throw an informative error rather than silently misbehaving.
    if (!s.nixEvalState)
        throw std::runtime_error(
            "v3 builtins.getFlake: no host TW EvalState available — "
            "callFlakeV3 needs it for the lockFlake/emitTreeAttrs FFI leaves");
    auto & ns = *s.nixEvalState;

    if (!a[0].isString()) typeError("getFlake", "string");
    std::string flakeRefS = a[0].asString();

    // (1) FFI leaf: parseFlakeRef + the unlocked-in-pure-eval guard +
    //     lockFlake + read the locked flake into plain data — all behind
    //     ffi::lockFlakeAndRead (audit Phase 4: keeps FlakeRef / LockFlags /
    //     lockFlake / flake::Settings out of this TU).
    auto flakeInfo = ffi::lockFlakeAndRead(ns, flakeRefS, ffi::pureEval(ns));

    // (2) v3-native call-flake.nix evaluation: callFlakeV3 builds the args
    //     V3-NATIVE from the plain data + applies the cached closure × 3.
    o = callFlakeV3(s, flakeInfo);
}

void registerPrimOp(const PrimOp & op)
{
    std::lock_guard<std::mutex> g(registryMutex());
    registry()[std::string(op.name)] = op;
}

void registerBuiltinPrimOps()
{
    static std::once_flag flag;
    std::call_once(flag, []() {
        registerPrimOp({"length",       1, primLength});
        registerPrimOp({"head",         1, primHead});
        registerPrimOp({"tail",         1, primTail});
        registerPrimOp({"elemAt",       2, primElemAt});
        registerPrimOp({"attrNames",    1, primAttrNames});
        registerPrimOp({"attrValues",   1, primAttrValues});
        registerPrimOp({"isAttrs",      1, primIsAttrs});
        registerPrimOp({"isList",       1, primIsList});
        registerPrimOp({"isFunction",   1, primIsFunction});
        registerPrimOp({"isString",     1, primIsString});
        registerPrimOp({"isInt",        1, primIsInt});
        registerPrimOp({"isBool",       1, primIsBool});
        registerPrimOp({"isNull",       1, primIsNull});
        registerPrimOp({"isFloat",      1, primIsFloat});
        registerPrimOp({"isPath",       1, primIsPath});
        registerPrimOp({"toString",     1, primToString});
        // Internal primop used by the bytecode derivationStrict wrapper
        // for path-copying string coercion (#665, see primDerivCoerce).
        registerPrimOp({"__derivCoerce", 1, primDerivCoerce});
        registerPrimOp({"typeOf",       1, primTypeOf});
        registerPrimOp({"stringLength", 1, primStringLength});
        registerPrimOp({"add",          2, primAdd});
        registerPrimOp({"sub",          2, primSub});
        registerPrimOp({"mul",          2, primMul});
        registerPrimOp({"div",          2, primDiv});
        registerPrimOp({"throw",        1, primThrow});
        registerPrimOp({"lessThan",     2, primLessThan});

        // Internal aliases used by the parser: `a * b` lowers to a Call of
        // `__mul`; same for __sub / __add / __div / __lessThan.
        registerPrimOp({"__add",        2, primAdd});
        registerPrimOp({"__sub",        2, primSub});
        registerPrimOp({"__mul",        2, primMul});
        registerPrimOp({"__div",        2, primDiv});
        registerPrimOp({"__lessThan",   2, primLessThan});
        // A8 phase 2 (2026-05-13): deepForceList=0b1 — OP_CALL_PRIMOP
        // pre-forces all elements of arg 0 (the outer list) iteratively
        // through the VM frame stack BEFORE calling primConcatLists.
        // primConcatLists's own `forceValue(...elems[i]...)` calls then
        // hit Evaluated thunks and return without C-recursion.  This
        // closes the dominant forceValue→dispatchLoop chain that built
        // up to 4500 C-stack frames on nixpkgs derivation construction.
        registerPrimOp({"concatLists",        1, primConcatLists,
                        /*lazyArgs=*/0, /*deepForceList=*/0b1});
        registerPrimOp({"concatStringsSep",   2, primConcatStringsSep});
        registerPrimOp({"substring",          3, primSubstring});
        // Higher-order callback primops (re-enter the VM via callClosure).
        registerPrimOp({"map",                2, primMap});
        // A8 phase 2: list-walking primops marked deepForceList for the
        // appropriate arg.  Only those whose bodies WOULD force every
        // element (no short-circuit, no laziness-preserving passes) are
        // safe to pre-force iteratively — pre-forcing must not introduce
        // a throw that lazy evaluation would have skipped.
        //   foldl' (arg 2):      op is called on every element → safe
        //   partition (arg 1):   same shape as filter
        //   listToAttrs (arg 0): body explicitly forces each entry
        //   catAttrs (arg 1):    body explicitly forces each attrset
        //   groupBy (arg 1):     keyFn is called on every element
        // Skipped (short-circuit / lazy):
        //   map: builds Tag::App entries, never forces inputs
        //   all / any: short-circuit; pre-force would surface throws
        //              that lazy eval would have skipped
        //   elem: short-circuit on first match
        //   concatMap: fn may discard its arg
        //   sort: comparator may not visit every pair
        //   filter (arg 1):      pred MAY IGNORE its arg — see below
        // T4 (LIST_ITERATION_FIX_PLAN_2026-06-08): filter is NOT safe to
        // deepForceList.  The old "pred is called on every element → safe"
        // reasoning was wrong: `filter (x: true) [1 (throw) 2]` calls the
        // pred 3× but forces NO element, so TW returns 3 while pre-forcing
        // throws.  primFilter passes each element UNFORCED into the pred
        // (callClosure doesn't force closure args) and forces only the
        // pred's RESULT, so it is lazy-correct on its own — AND single-pass
        // (one result alloc from `kept`, no per-element singleton lists),
        // unlike the bytecode `concatLists∘map` form (2M singleton ListVecs
        // on a 2M filter).  deepForceList=0 makes the C primFilter the
        // lazy + lean default; the bytecode form is now opt-in
        // (NIX_V3_BC_FILTER=1).  forceValue is iterative, so per-element
        // forcing inside the pred does not grow the C stack.
        registerPrimOp({"filter",             2, primFilter,
                        /*lazyArgs=*/0, /*deepForceList=*/0});
        // C-14 (CODEBASE_REVIEW_2026-06-11): deepForceList=0 — TW's
        // prim_foldlStrict (libexpr/primops.cc:4133) forces the function + the
        // list SPINE but never the list ELEMENTS; the op forces an element only
        // if it uses it.  Pre-forcing all elements made
        // `foldl' (a: x: a) 0 [1 (throw "boom")]` throw where TW returns 0
        // (same class as the shipped T4 filter fix).  forceValue is iterative,
        // so per-element forcing inside the fold doesn't grow the C stack.
        registerPrimOp({"foldl'",             3, primFoldl,
                        /*lazyArgs=*/0b010, /*deepForceList=*/0});
        // 2026-05-18 IR Phase C fused-loop FFI leaf: __foldlMap.
        // Args: (op, init, f, xs).  Equivalent to
        // `foldl' (acc: x: op acc (f x)) init xs`.  Recognised by
        // opt_stream_fusion.cc, which rewrites
        // `foldl'(op, init, map(f, xs))` to a __foldlMap call.
        // lazyArgs mirrors foldl' (init is lazy).  Internal — leading
        // `__` keeps it out of user-visible `builtins`.
        // C-14: deepForceList=0 (mirrors foldl' — __foldlMap is the fused
        // `foldl' (acc: x: op acc (f x)) init xs`; it must not pre-force xs's
        // elements either).
        registerPrimOp({"__foldlMap",         4, primFoldlMap,
                        /*lazyArgs=*/0b0010, /*deepForceList=*/0});
        registerPrimOp({"genList",            2, primGenList});
        registerPrimOp({"all",                2, primAll});
        registerPrimOp({"any",                2, primAny});
        registerPrimOp({"concatMap",          2, primConcatMap});
        registerPrimOp({"partition",          2, primPartition,
                        /*lazyArgs=*/0, /*deepForceList=*/0b10});
        registerPrimOp({"getEnv",             1, primGetEnv});
        registerPrimOp({"compareVersions",    2, primCompareVersions});
        registerPrimOp({"listToAttrs",        1, primListToAttrs,
                        /*lazyArgs=*/0, /*deepForceList=*/0b1});
        registerPrimOp({"removeAttrs",        2, primRemoveAttrs});
        registerPrimOp({"intersectAttrs",     2, primIntersectAttrs});
        registerPrimOp({"mapAttrs",           2, primMapAttrs});
        registerPrimOp({"elem",               2, primElem, /*lazyArgs=*/0b01});  // C-13: needle lazy
        registerPrimOp({"getAttr",            2, primGetAttr});
        registerPrimOp({"hasAttr",            2, primHasAttr});
        registerPrimOp({"catAttrs",           2, primCatAttrs,
                        /*lazyArgs=*/0, /*deepForceList=*/0b10});
        registerPrimOp({"replaceStrings",     3, primReplaceStrings});
        registerPrimOp({"abort",              1, primAbort});
        registerPrimOp({"seq",                2, primSeq,     /*lazyArgs=*/0b10});
        registerPrimOp({"deepSeq",            2, primDeepSeq, /*lazyArgs=*/0b10});
        registerPrimOp({"trace",              2, primTrace});
        registerPrimOp({"traceVerbose",       2, primTraceVerbose});
        registerPrimOp({"zipAttrsWith",       2, primZipAttrsWith});
        registerPrimOp({"unsafeGetAttrPos",   2, primUnsafeGetAttrPos});
        registerPrimOp({"toPath",             1, primToPath});
        registerPrimOp({"splitVersion",       1, primSplitVersion});
        registerPrimOp({"unsafeDiscardStringContext",      1, primUnsafeDiscardStringContext});
        registerPrimOp({"hasContext",         1, primHasContext});
        registerPrimOp({"getContext",         1, primGetContext});
        registerPrimOp({"unsafeDiscardOutputDependency",   1, primUnsafeDiscardOutputDependency});
        registerPrimOp({"appendContext",      2, primAppendContext});
        registerPrimOp({"addDrvOutputDependencies",  1, primAddDrvOutputDependencies});
        registerPrimOp({"tryEval",            1, primTryEval, /*lazyArgs=*/0b1});
        registerPrimOp({"baseNameOf",         1, primBaseNameOf});
        registerPrimOp({"dirOf",              1, primDirOf});
        registerPrimOp({"pathExists",         1, primPathExists});
        registerPrimOp({"splitString",        2, primSplitString});
        registerPrimOp({"sort",               2, primSort});
        registerPrimOp({"bitAnd",             2, primBitAnd});
        registerPrimOp({"bitOr",              2, primBitOr});
        registerPrimOp({"bitXor",             2, primBitXor});
        registerPrimOp({"floor",              1, primFloor});
        registerPrimOp({"ceil",               1, primCeil});
        registerPrimOp({"parseInt",           1, primParseInt});
        registerPrimOp({"fromJSON",           1, primFromJSON});
        registerPrimOp({"toJSON",             1, primToJSON});
        registerPrimOp({"functionArgs",       1, primFunctionArgs});
        registerPrimOp({"import",             1, primImport});
        registerPrimOp({"readFile",           1, primReadFile});
        registerPrimOp({"readDir",            1, primReadDir});
        registerPrimOp({"parseDrvName",       1, primParseDrvName});
        // C-14: deepForceList=0 — groupBy forces each element's KEY (f x, used
        // as an attr name) but TW leaves the ELEMENT itself unforced in its
        // group; pre-forcing elements threw on `groupBy (x: "k") [1 (throw)]`
        // where TW returns { k = [ 1 <thrown> ]; }.
        registerPrimOp({"groupBy",            2, primGroupBy,
                        /*lazyArgs=*/0, /*deepForceList=*/0});
        registerPrimOp({"match",              2, primMatch});
        registerPrimOp({"split",              2, primSplit});
        registerPrimOp({"hashString",         2, primHashString});
        registerPrimOp({"currentSystem",      0, primCurrentSystem});
        registerPrimOp({"currentTime",        0, primCurrentTime});
        registerPrimOp({"nixVersion",         0, primNixVersion});
        registerPrimOp({"genericClosure",     1, primGenericClosure});
        registerPrimOp({"hashFile",           2, primHashFile});
        registerPrimOp({"convertHash",        1, primConvertHash});
        registerPrimOp({"readFileType",       1, primReadFileType});
        // addErrorContext's second arg ("the wrapped value") is left
        // unforced so callers like nixpkgs `lib/modules.nix:270`
        // (`config = addErrorContext "..." config`) don't deadlock
        // against the rec-binding being constructed.  The primop body
        // returns args[1] as-is — force happens at the consumer.
        registerPrimOp({"addErrorContext", 2, primAddErrorContext, /*lazyArgs=*/0b10});
        registerPrimOp({"derivationStrict",   1, primDerivationStrict});
        // 2026-05-17: __derivationStrictRaw — the unwrapped C primop.
        // The bytecode hybrid wrapper for `derivationStrict` (installed
        // in bytecode_primops.cc) pre-forces args at bytecode level
        // (iterative via OP_FORCE), then calls __derivationStrictRaw
        // to do the actual drv work.  Exposed under a `__`-prefixed
        // name so it's invisible from `builtins.X` (per the convention
        // in getBuiltinsValue at vm.cc:8341).
        registerPrimOp({"__derivationStrictRaw", 1, primDerivationStrict});
        // 2026-05-17 Option 4 hybrid FFI leaf.  Receives a pre-
        // processed attrset built by the bytecode wrapper and runs
        // phases 4-7 (context → inputs, output config, writeDerivation,
        // result attrset) via the shared `buildAndWriteDrvNative`
        // helper.  See bytecode_primops.cc for the wrapper protocol.
        registerPrimOp({"__derivationFromPreprocessed", 1, primDerivationFromPreprocessed});
        // C++ port of corepkgs/derivation.nix — derivationStrict
        // synthesizes paths, primDerivation wraps them up with
        // commonAttrs and outputName for tree-walker parity.
        registerPrimOp({"derivation",         1, primDerivation});
        // 2026-05-17: __derivationRaw — companion to the wrapper above.
        registerPrimOp({"__derivationRaw",    1, primDerivation});
        registerPrimOp({"findFile",           2, primFindFile});
        registerPrimOp({"__findFile",         2, primFindFile});
        registerPrimOp({"nixPath",            0, primNixPath});
        registerPrimOp({"__nixPath",          0, primNixPath});
        registerPrimOp({"scopedImport",       2, primScopedImport});
        registerPrimOp({"path",               1, primPath});
        registerPrimOp({"fromTOML",           1, primFromTOML});
        registerPrimOp({"parseFlakeRef",      1, primParseFlakeRef});
        registerPrimOp({"flakeRefToString",   1, primFlakeRefToString});
        registerPrimOp({"toXML",              1, primToXML});
        // WC-28a: small previously-missing primops.
        registerPrimOp({"placeholder",        1, primPlaceholder});
        registerPrimOp({"__warn",             2, primWarn});
        registerPrimOp({"break",              1, primBreak});
        registerPrimOp({"__storePath",        1, primStorePath});
        // #698 Phase 2 diagnostic — verifies v3-side compilation of
        // call-flake.nix works.  Remove in Phase 3 once primGetFlake
        // uses the v3-native path and regression tests give end-to-end
        // coverage.
        extern void primV3CompileCallFlake(EvalState&, Value*, Value&);
        registerPrimOp({"__v3CompileCallFlake", 1, primV3CompileCallFlake});
        registerPrimOp({"__toFile",           2, primToFile});
        registerPrimOp({"__outputOf",         2, primOutputOf});
        // WC-28b: fetch primops (delegate to tree-walker builtins.X).
        registerPrimOp({"__fetchurl",         1, primFetchurl});
        registerPrimOp({"fetchurl",           1, primFetchurl});
        registerPrimOp({"fetchTarball",       1, primFetchTarball});
        registerPrimOp({"fetchTree",          1, primFetchTree});
        registerPrimOp({"fetchGit",           1, primFetchGit});
        registerPrimOp({"fetchMercurial",     1, primFetchMercurial});
        registerPrimOp({"fetchClosure",       1, primFetchClosure});
        // WC-28c: filterSource (delegate too).
        registerPrimOp({"filterSource",       2, primFilterSource});
        registerPrimOp({"__filterSource",     2, primFilterSource});
        // #700/step 3: v3-native wrapper for TW's
        // `internalPrimOps["fetchFinalTree"]`.  Used by call-flake.nix
        // when an input lacks an override + is non-relative.  In the
        // common `builtins.getFlake "X"` path, all nodes have
        // overrides supplied by callFlakeV3 → this is a fallback path
        // rarely hit; but it must exist as a v3 PrimOp Value so
        // call-flake.nix's `fetchTreeFinal` parameter has the right
        // type even when never called.
        registerPrimOp({"__fetchFinalTree",   1, primFetchFinalTree});
        // Path B M3 (2026-05-20): getFlake primop.  Registered into TW
        // via libflake's evalSettings.extraPrimOps; bridge through to
        // TW so v3-direct can resolve `builtins.getFlake` for cardano-
        // node-class flake-driven workloads.  Experimental-feature
        // gate (Xp::Flakes) enforced TW-side.  parseFlakeRef /
        // flakeRefToString are already native v3 (see lines 7319/7380).
        registerPrimOp({"getFlake",           1, primGetFlake});
        registerPrimOp({"__getFlake",         1, primGetFlake});

        // EVAL-COMP §4.5 / #414: register `__`-prefixed aliases for
        // every primop whose un-prefixed form is already in v3's
        // registry.  Tree-walker registers both forms; nixpkgs uses
        // the `__`-prefixed style heavily (legacy convention).
        // Pre-fix, calls like `__substring` / `__replaceStrings`
        // hit the lower.cc:549 `unbound variable` throw, propagated
        // up to the cutover hook, and triggered a fall-back of the
        // entire surrounding expression to tree-walker -- a heavy
        // bridge tax for a one-line registration miss.
        //
        // The aliases below mirror the un-prefixed registrations
        // above; arity / impl / lazyArgs are kept in sync by hand.
        // Stable until the underlying primops change, in which case
        // both registrations need updating.
        registerPrimOp({"__length",           1, primLength});
        registerPrimOp({"__head",             1, primHead});
        registerPrimOp({"__tail",             1, primTail});
        registerPrimOp({"__elemAt",           2, primElemAt});
        registerPrimOp({"__attrNames",        1, primAttrNames});
        registerPrimOp({"__attrValues",       1, primAttrValues});
        registerPrimOp({"__isAttrs",          1, primIsAttrs});
        registerPrimOp({"__isList",           1, primIsList});
        registerPrimOp({"__isFunction",       1, primIsFunction});
        registerPrimOp({"__isString",         1, primIsString});
        registerPrimOp({"__isInt",            1, primIsInt});
        registerPrimOp({"__isBool",           1, primIsBool});
        registerPrimOp({"__isFloat",          1, primIsFloat});
        registerPrimOp({"__isPath",           1, primIsPath});
        registerPrimOp({"__typeOf",           1, primTypeOf});
        registerPrimOp({"__stringLength",     1, primStringLength});
        registerPrimOp({"__concatLists",      1, primConcatLists});
        registerPrimOp({"__concatStringsSep", 2, primConcatStringsSep});
        registerPrimOp({"__substring",        3, primSubstring});
        registerPrimOp({"__map",              2, primMap});
        registerPrimOp({"__filter",           2, primFilter});
        registerPrimOp({"__foldl'",           3, primFoldl, /*lazyArgs=*/0b010});
        registerPrimOp({"__genList",          2, primGenList});
        registerPrimOp({"__all",              2, primAll});
        registerPrimOp({"__any",              2, primAny});
        registerPrimOp({"__concatMap",        2, primConcatMap});
        registerPrimOp({"__partition",        2, primPartition});
        registerPrimOp({"__getEnv",           1, primGetEnv});
        registerPrimOp({"__compareVersions",  2, primCompareVersions});
        registerPrimOp({"__listToAttrs",      1, primListToAttrs});
        registerPrimOp({"__intersectAttrs",   2, primIntersectAttrs});
        registerPrimOp({"__mapAttrs",         2, primMapAttrs});
        registerPrimOp({"__elem",             2, primElem, /*lazyArgs=*/0b01});  // C-13: needle lazy
        registerPrimOp({"__getAttr",          2, primGetAttr});
        registerPrimOp({"__hasAttr",          2, primHasAttr});
        registerPrimOp({"__catAttrs",         2, primCatAttrs});
        registerPrimOp({"__replaceStrings",   3, primReplaceStrings});
        registerPrimOp({"__seq",              2, primSeq,     /*lazyArgs=*/0b10});
        registerPrimOp({"__deepSeq",          2, primDeepSeq, /*lazyArgs=*/0b10});
        registerPrimOp({"__trace",            2, primTrace});
        registerPrimOp({"__traceVerbose",     2, primTraceVerbose});
        registerPrimOp({"__zipAttrsWith",     2, primZipAttrsWith});
        registerPrimOp({"__unsafeGetAttrPos", 2, primUnsafeGetAttrPos});
        registerPrimOp({"__toPath",           1, primToPath});
        registerPrimOp({"__splitVersion",     1, primSplitVersion});
        registerPrimOp({"__addErrorContext",  2, primAddErrorContext, /*lazyArgs=*/0b10});
        registerPrimOp({"__tryEval",          1, primTryEval, /*lazyArgs=*/0b1});
        registerPrimOp({"__pathExists",       1, primPathExists});
        registerPrimOp({"__sort",             2, primSort});
        registerPrimOp({"__bitAnd",           2, primBitAnd});
        registerPrimOp({"__bitOr",            2, primBitOr});
        registerPrimOp({"__bitXor",           2, primBitXor});
        registerPrimOp({"__floor",            1, primFloor});
        registerPrimOp({"__ceil",             1, primCeil});
        registerPrimOp({"__fromJSON",         1, primFromJSON});
        registerPrimOp({"__toJSON",           1, primToJSON});
        registerPrimOp({"__functionArgs",     1, primFunctionArgs});
        registerPrimOp({"__readFile",         1, primReadFile});
        registerPrimOp({"__readDir",          1, primReadDir});
        registerPrimOp({"__parseDrvName",     1, primParseDrvName});
        registerPrimOp({"__groupBy",          2, primGroupBy});
        registerPrimOp({"__match",            2, primMatch});
        registerPrimOp({"__split",            2, primSplit});
        registerPrimOp({"__hashString",       2, primHashString});
        registerPrimOp({"__genericClosure",   1, primGenericClosure});
        registerPrimOp({"__hashFile",         2, primHashFile});
        registerPrimOp({"__convertHash",      1, primConvertHash});
        registerPrimOp({"__readFileType",     1, primReadFileType});
        registerPrimOp({"__path",             1, primPath});
        registerPrimOp({"__toXML",            1, primToXML});

        // REVIEW_2026-05-04 F5 / §6.1: register the IO/store/derivation
        // primops' `__`-prefix aliases that were missing.  Each missing
        // alias was causing v3 lower to throw "unbound variable" at
        // lower.cc:566 and bridge the entire surrounding expression to
        // tree-walker.  In tree-walker, `__currentSystem` etc are the
        // CANONICAL form (libexpr/primops.cc:5650+ uses addConstant on
        // the `__` name), so v3 had inverted the convention.
        registerPrimOp({"__derivationStrict", 1, primDerivationStrict});
        registerPrimOp({"__derivation",       1, primDerivation});
        registerPrimOp({"__import",           1, primImport});
        registerPrimOp({"__scopedImport",     2, primScopedImport});
        registerPrimOp({"__placeholder",      1, primPlaceholder});
        registerPrimOp({"__currentSystem",    0, primCurrentSystem});
        registerPrimOp({"__currentTime",      0, primCurrentTime});
        registerPrimOp({"__nixVersion",       0, primNixVersion});

        // builtins.storeDir + __storeDir + builtins.langVersion +
        // __langVersion: not previously registered by v3 at all.
        // nixpkgs lib/minfeatures.nix and various store-path
        // synthesisers reference these.  Both forms (bare and __)
        // are registered for parity with tree-walker.
        registerPrimOp({"storeDir",           0, primStoreDir});
        registerPrimOp({"__storeDir",         0, primStoreDir});
        registerPrimOp({"langVersion",        0, primLangVersion});
        registerPrimOp({"__langVersion",      0, primLangVersion});
    });
}

} // namespace nix::v3
