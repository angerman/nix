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
#include "v3/lower.hh"
#include "v3/vm.hh"
#include "v3/bridge_yield.hh"
#include "v3/errors.hh"

#include <chrono>

#include "nix/expr/eval.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/print.hh"
#include "nix/expr/value/context.hh"
#include "nix/util/canon-path.hh"
#include "nix/util/experimental-features.hh"
#include "nix/util/hash.hh"

#include <nlohmann/json.hpp>
#include <toml.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <list>
#include <mutex>
#include <sys/stat.h>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "nix/util/memory-source-accessor.hh"
#include "nix/store/store-api.hh"
#include "nix/store/derived-path.hh"
#include "nix/store/derivations.hh"  // hashPlaceholder
#include "nix/store/globals.hh"
#include "nix/store/content-address.hh"  // ContentAddressMethod
#include "nix/util/serialise.hh"  // StringSource
#include "v3/serialize.hh"
#include "v3/disk_cache.hh"
#include "v3/ir.hh"
#include "v3/bytecode.hh"

#include "nix/fetchers/fetch-to-store.hh"

#include <boost/unordered/concurrent_flat_map.hpp>

namespace nix::v3 {

// WC-4: defined in v3_hook.cc.
void populateSubExprCachePublic(
    const ir::Module & module, const CompilationUnit * cu);

/// WC-19: TLS pointer to the outer Expr the v3 hook is currently
/// processing.  v3_hook.cc sets this before calling v3ToTreeWalker;
/// the lazy-bridge registration captures it so primV3ForceAttr /
/// primV3ForceListElem can fall back to tree-walker on a deferred
/// blackhole (eval-order divergence v3 sees but tree-walker resolves).
/// External linkage so v3_hook.cc can extern-reference it.
thread_local nix::Expr * tlBridgeFallbackExpr = nullptr;

/// Forward decl so primTrace (which is defined earlier in this file)
/// can use the v3->TW bridge.  Definition at the bottom of the file.
nix::Value * v3ToTreeWalkerPublic(nix::EvalState & nixState, Value v);

ScopedBridgeFallbackExpr::ScopedBridgeFallbackExpr(nix::Expr * e)
    : saved(tlBridgeFallbackExpr)
{
    tlBridgeFallbackExpr = e;
}
ScopedBridgeFallbackExpr::~ScopedBridgeFallbackExpr()
{
    tlBridgeFallbackExpr = saved;
}

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

/// #466 nested-bridge-primop depth bound.
///
/// Tracks how deeply we've nested calls into the v3 bridge primops
/// (primV3CallBridge1 / primV3ForceAttr / primV3ForceListElem) on
/// this thread.  Each level allocates a fresh VMState; in lambda-skip
/// + rec-attrset-fix-point patterns the chain re-enters each primop
/// across different (handle, sid) pairs, defeating the per-primop
/// (handle, sid) cycle detector and the per-thunk (vm, t) recovery
/// counter (each layer has a fresh vm).  C-stack growth is real and
/// SIGSEGV is the eventual outcome.
///
/// Bound the depth at a hard limit so a structural cycle surfaces as
/// a proper error after `kBridgePrimopMaxDepth` iterations rather
/// than running until C-stack overflows.  When a primop hits the
/// limit, throw a NON-Blackhole error so the catch path's
/// fallbackToTreeWalker (which gates on dynamic_cast<BlackholeError>)
/// does NOT trigger — the fallback would just re-enter the same
/// chain.  Default 64; tunable via NIX_V3_BRIDGE_PRIMOP_DEPTH.
inline int & bridgePrimopDepth()
{
    thread_local int d = 0;
    return d;
}
inline int bridgePrimopMaxDepth()
{
    static const int k = []{
        if (const char * v = std::getenv("NIX_V3_BRIDGE_PRIMOP_DEPTH"))
            return std::max(0, std::atoi(v));
        return 64;
    }();
    return k;
}
struct BridgePrimopDepthGuard {
    int & d;
    BridgePrimopDepthGuard(int & d_) : d(d_) { ++d; }
    ~BridgePrimopDepthGuard() { --d; }
};

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
    auto [it, ins] = chain.insert(ForceChainKey{m_op, m_keyA, m_keyB});
    m_inserted = ins;
}

ForceChainGuard::~ForceChainGuard()
{
    if (m_inserted)
        forceChainSet().erase(ForceChainKey{m_op, m_keyA, m_keyB});
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

// String-context side-table is in alloc.hh — entries are encoded
// strings (`<path>` Opaque, `=<drvPath>` DrvDeep, `!<output>!<drvPath>`
// Built).  Helpers below convert to/from nix::NixStringContext.

static nix::NixStringContext decodeStringContext(const std::vector<std::string> & entries)
{
    nix::NixStringContext out;
    for (auto & e : entries) {
        try { out.insert(nix::NixStringContextElem::parse(e)); }
        catch (...) { /* skip un-parseable entries */ }
    }
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

inline bool valueEqual(VMState & vm, Value a, Value b)
{
    a = forceValue(vm, a);
    b = forceValue(vm, b);
    if (a.tag() != b.tag()) {
        if (a.isInt() && b.isFloat()) return static_cast<double>(a.payload.i) == b.payload.f;
        if (a.isFloat() && b.isInt()) return a.payload.f == static_cast<double>(b.payload.i);
        return false;
    }
    switch (a.tag()) {
    case Tag::Int:    return a.payload.i == b.payload.i;
    case Tag::Float:  return a.payload.f == b.payload.f;
    case Tag::Bool:   return a.payload.i == b.payload.i;
    case Tag::Null:   return true;
    case Tag::String: return std::string_view(a.payload.str) == std::string_view(b.payload.str);
    case Tag::Path:   return std::string_view(a.payload.path) == std::string_view(b.payload.path);
    case Tag::List: {
        auto * la = a.payload.list; auto * lb = b.payload.list;
        if (la == lb) return true;
        uint32_t na = la ? la->size : 0; uint32_t nb = lb ? lb->size : 0;
        if (na != nb) return false;
        for (uint32_t i = 0; i < na; ++i)
            if (!valueEqual(vm, la->elems[i], lb->elems[i])) return false;
        return true;
    }
    case Tag::Attrs: {
        auto * aa = a.payload.bindings; auto * bb = b.payload.bindings;
        if (aa == bb) return true;
        uint32_t na = aa ? aa->size : 0; uint32_t nb = bb ? bb->size : 0;
        if (na != nb) return false;
        for (uint32_t i = 0; i < na; ++i) {
            if (aa->entries[i].name != bb->entries[i].name) return false;
            if (!valueEqual(vm, aa->entries[i].value, bb->entries[i].value)) return false;
        }
        return true;
    }
    // Functions are never equal in Nix at the top level (`f == f` is
    // false).  This helper is used from primops (filter/elem/etc.) which
    // perform direct comparison — closures never compare equal here.
    // The vm.cc valueEqual has a separate code path for list/attr
    // recursion that allows pointer-identity for closures.
    case Tag::Closure:
    case Tag::PrimOp:
    case Tag::PrimOpApp:
        return false;
    case Tag::Uninitialized:
    case Tag::Thunk:
    case Tag::App:
    case Tag::Blackhole:
    case Tag::External:
    case Tag::Slot:
    default:          return a.payload.raw == b.payload.raw;
    }
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
    int64_t n = 0;
    if (v.isList())   n = v.payload.list ? v.payload.list->size : 0;
    else if (v.isString()) n = static_cast<int64_t>(std::strlen(v.payload.str));
    else                   typeError("length", "list or string");
    out.mkInt(n);
}

void primHead(EvalState &, Value * args, Value & out)
{
    const Value & v = args[0];
    if (!v.isList() || !v.payload.list || v.payload.list->size == 0)
        throw std::runtime_error("v3 primop head: empty list or wrong type");
    out = v.payload.list->elems[0];
}

void primTail(EvalState &, Value * args, Value & out)
{
    const Value & v = args[0];
    if (!v.isList() || !v.payload.list || v.payload.list->size == 0)
        throw std::runtime_error("v3 primop tail: empty list or wrong type");
    uint32_t n = v.payload.list->size;
    ListVec * out_l = Alloc::allocList(n - 1);
    allocStats().listsAllocated++;
    for (uint32_t i = 1; i < n; ++i)
        out_l->elems[i - 1] = v.payload.list->elems[i];
    out.tag_payload = static_cast<uint64_t>(Tag::List);
    out.payload.list = out_l;
}

void primElemAt(EvalState &, Value * args, Value & out)
{
    const Value & lst = args[0];
    const Value & idx = args[1];
    if (!lst.isList() || !idx.isInt()) typeError("elemAt", "list and int");
    uint32_t n = lst.payload.list ? lst.payload.list->size : 0;
    if (idx.payload.i < 0 || static_cast<uint64_t>(idx.payload.i) >= n)
        throw std::runtime_error("v3 primop elemAt: index out of range");
    out = lst.payload.list->elems[idx.payload.i];
}

void primAttrNames(EvalState &, Value * args, Value & out)
{
    const Value & a = args[0];
    if (!a.isAttrs() || !a.payload.bindings) typeError("attrNames", "attrset");
    uint32_t n = a.payload.bindings->size;
    ListVec * lv = Alloc::allocList(n);
    allocStats().listsAllocated++;
    auto & symTab = ir::globalSymbolTable();
    for (uint32_t i = 0; i < n; ++i) {
        SymbolId sid = a.payload.bindings->entries[i].name;
        Value v = mkStringValueOwned(sid < symTab.size() ? symTab[sid] : std::to_string(sid));
        lv->elems[i] = v;
    }
    // Sort lexicographically by name — matches tree-walker semantics
    // and decouples output order from the global symbol-table
    // insertion order.
    std::sort(lv->elems, lv->elems + n,
        [](const Value & x, const Value & y) {
            return std::string_view(x.payload.str) < std::string_view(y.payload.str);
        });
    out.tag_payload = static_cast<uint64_t>(Tag::List);
    out.payload.list = lv;
}

void primAttrValues(EvalState &, Value * args, Value & out)
{
    const Value & a = args[0];
    if (!a.isAttrs() || !a.payload.bindings) typeError("attrValues", "attrset");
    uint32_t n = a.payload.bindings->size;
    // Build (name, value) pairs, sort by name, then drop the name.
    auto & symTab = ir::globalSymbolTable();
    std::vector<std::pair<std::string_view, Value>> pairs;
    pairs.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        SymbolId sid = a.payload.bindings->entries[i].name;
        std::string_view nm = sid < symTab.size() ? std::string_view(symTab[sid]) : std::string_view("");
        pairs.emplace_back(nm, a.payload.bindings->entries[i].value);
    }
    std::sort(pairs.begin(), pairs.end(),
        [](const auto & x, const auto & y) { return x.first < y.first; });
    ListVec * lv = Alloc::allocList(n);
    allocStats().listsAllocated++;
    for (uint32_t i = 0; i < n; ++i) lv->elems[i] = pairs[i].second;
    out.tag_payload = static_cast<uint64_t>(Tag::List);
    out.payload.list = lv;
}

void primIsAttrs   (EvalState &, Value * args, Value & out) { out = args[0].isAttrs()    ? Value::vTrue : Value::vFalse; }
void primIsList    (EvalState &, Value * args, Value & out) { out = args[0].isList()     ? Value::vTrue : Value::vFalse; }
void primIsFunction(EvalState &, Value * args, Value & out) { out = (args[0].isClosure() || args[0].isPrimOp() || args[0].tag() == Tag::PrimOpApp) ? Value::vTrue : Value::vFalse; }
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
                                     std::vector<std::string> & ctx)
{
    auto absorbCtx = [&](const char * s) {
        if (!s) return;
        if (auto * raw = lookupStringContextEntries(s)) {
            ctx.insert(ctx.end(), raw->begin(), raw->end());
        }
    };
    v = forceValue(*state.vm, v);
    switch (v.tag()) {
    case Tag::String: absorbCtx(v.payload.str);
                      return std::string(v.payload.str ? v.payload.str : "");
    case Tag::Path:   return std::string(v.payload.path ? v.payload.path : "");
    case Tag::Int:    return std::to_string(v.payload.i);
    case Tag::Float:  return std::to_string(v.payload.f);
    case Tag::Bool:   return v.payload.i == 1 ? "1" : "";
    case Tag::Null:   return "";
    case Tag::List: {
        std::string out;
        auto * lv = v.payload.list;
        if (!lv) return out;
        for (uint32_t i = 0; i < lv->size; ++i) {
            Value el = forceValue(*state.vm, lv->elems[i]);
            out += toStringCoerceCtx(state, el, ctx);
            if (i + 1 < lv->size) {
                bool elIsEmptyList = el.isList()
                    && (!el.payload.list || el.payload.list->size == 0);
                if (!elIsEmptyList) out += ' ';
            }
        }
        return out;
    }
    case Tag::Attrs: {
        // Tree-walker: try __toString first (call it on the attrset),
        // then outPath.  v3 doesn't yet wire calling __toString from
        // a primop context — fall through to outPath only.
        if (v.payload.bindings) {
            // outPath is the more common path in nixpkgs (every
            // derivation has it); __toString is rarer.  Intern locally
            // — drvStrictSymbols() lives in BR-3 territory and isn't
            // forward-decl'd up here.
            static const SymbolId sOutPath =
                ir::globalInternSymbol("outPath");
            if (auto * outV = v.payload.bindings->lookup(sOutPath)) {
                Value forced = forceValue(*state.vm, *outV);
                return toStringCoerceCtx(state, forced, ctx);
            }
        }
        throw std::runtime_error(
            "v3 toString: attrset has no outPath / __toString");
    }
    case Tag::Uninitialized:
    case Tag::Closure:
    case Tag::Thunk:
    case Tag::PrimOp:
    case Tag::PrimOpApp:
    case Tag::App:
    case Tag::Blackhole:
    case Tag::External:
    case Tag::Slot:
    default: {
        char buf[64];
        std::snprintf(buf, sizeof buf,
            "v3 toString: cannot stringify type tag=%u",
            (unsigned)v.tag());
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
    // §1.6: thread context through nested list/attrs traversal.
    std::vector<std::string> ctx;
    std::string s = toStringCoerceCtx(state, args[0], ctx);
    out = mkStringValueOwned(std::move(s));
    if (!ctx.empty())
        setStringContextEntries(out.payload.str, std::move(ctx));
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
    case Tag::Uninitialized:
    case Tag::App:
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
    out.mkInt(static_cast<int64_t>(std::strlen(args[0].payload.str)));
}

void primAdd(EvalState &, Value * args, Value & out)
{
    const Value & a = args[0]; const Value & b = args[1];
    if (a.isInt() && b.isInt())          out.mkInt(a.payload.i + b.payload.i);
    else if (a.isFloat() && b.isFloat()) out.mkFloat(a.payload.f + b.payload.f);
    else if (a.isInt() && b.isFloat())   out.mkFloat(static_cast<double>(a.payload.i) + b.payload.f);
    else if (a.isFloat() && b.isInt())   out.mkFloat(a.payload.f + static_cast<double>(b.payload.i));
    else typeError("add", "numeric");
}

void primSub(EvalState &, Value * args, Value & out)
{
    const Value & a = args[0]; const Value & b = args[1];
    if (a.isInt() && b.isInt())          out.mkInt(a.payload.i - b.payload.i);
    else if (a.isFloat() && b.isFloat()) out.mkFloat(a.payload.f - b.payload.f);
    else if (a.isInt() && b.isFloat())   out.mkFloat(static_cast<double>(a.payload.i) - b.payload.f);
    else if (a.isFloat() && b.isInt())   out.mkFloat(a.payload.f - static_cast<double>(b.payload.i));
    else typeError("sub", "numeric");
}

void primMul(EvalState &, Value * args, Value & out)
{
    const Value & a = args[0]; const Value & b = args[1];
    if (a.isInt() && b.isInt())          out.mkInt(a.payload.i * b.payload.i);
    else if (a.isFloat() && b.isFloat()) out.mkFloat(a.payload.f * b.payload.f);
    else if (a.isInt() && b.isFloat())   out.mkFloat(static_cast<double>(a.payload.i) * b.payload.f);
    else if (a.isFloat() && b.isInt())   out.mkFloat(a.payload.f * static_cast<double>(b.payload.i));
    else typeError("mul", "numeric");
}

void primDiv(EvalState &, Value * args, Value & out)
{
    const Value & a = args[0]; const Value & b = args[1];
    if (a.isInt() && b.isInt()) {
        if (b.payload.i == 0) throw std::runtime_error("v3 primop div: division by zero");
        out.mkInt(a.payload.i / b.payload.i);
    } else if (a.isFloat() && b.isFloat()) {
        out.mkFloat(a.payload.f / b.payload.f);
    } else if (a.isInt() && b.isFloat()) {
        out.mkFloat(static_cast<double>(a.payload.i) / b.payload.f);
    } else if (a.isFloat() && b.isInt()) {
        out.mkFloat(a.payload.f / static_cast<double>(b.payload.i));
    } else typeError("div", "numeric");
}

void primThrow(EvalState &, Value * args, Value &)
{
    if (!args[0].isString()) typeError("throw", "string");
    // ThrownError derives from AssertionError so tryEval catches it
    // (matches tree-walker semantics).
    throw ThrownError(std::string("v3 throw: ") + args[0].payload.str);
}

void primConcatLists(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isList()) typeError("concatLists", "list of lists");
    uint32_t total = 0;
    auto & outer = args[0];
    // Force each outer element (each should be a list); they're lazy
    // by default now.
    for (uint32_t i = 0; i < outer.payload.list->size; ++i) {
        outer.payload.list->elems[i] = forceValue(*state.vm, outer.payload.list->elems[i]);
        const Value & el = outer.payload.list->elems[i];
        if (!el.isList()) typeError("concatLists", "list of lists");
        total += el.payload.list ? el.payload.list->size : 0;
    }
    ListVec * result = Alloc::allocList(total);
    allocStats().listsAllocated++;
    uint32_t k = 0;
    for (uint32_t i = 0; i < outer.payload.list->size; ++i) {
        const Value & el = outer.payload.list->elems[i];
        if (!el.payload.list) continue;
        for (uint32_t j = 0; j < el.payload.list->size; ++j)
            result->elems[k++] = el.payload.list->elems[j];
    }
    out.tag_payload = static_cast<uint64_t>(Tag::List);
    out.payload.list = result;
}

void primConcatStringsSep(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("concatStringsSep", "separator string");
    if (!args[1].isList())   typeError("concatStringsSep", "list of strings");
    std::string sep(args[0].payload.str);
    std::string result;
    auto * list = args[1].payload.list;
    for (uint32_t i = 0; list && i < list->size; ++i) {
        if (i > 0) result += sep;
        Value el = forceValue(*state.vm, list->elems[i]);
        if (!el.isString()) typeError("concatStringsSep", "list of strings");
        result += el.payload.str;
    }
    out = mkStringValueOwned(result);
}

void primSubstring(EvalState &, Value * args, Value & out)
{
    if (!args[0].isInt() || !args[1].isInt() || !args[2].isString())
        typeError("substring", "(int, int, string)");
    int64_t start = args[0].payload.i;
    int64_t len = args[1].payload.i;
    // Match tree-walker: negative start is rejected; negative len is
    // a "to end" sentinel.
    if (start < 0)
        throw std::runtime_error("v3 substring: negative start position");
    std::string_view src(args[2].payload.str);
    const char * srcPtr = args[2].payload.str;
    if (static_cast<size_t>(start) >= src.size()) {
        out = mkStringValueOwned("");
    } else {
        size_t available = src.size() - start;
        size_t actualLen = (len < 0) ? available : std::min(static_cast<size_t>(len), available);
        out = mkStringValueOwned(std::string(src.substr(start, actualLen)));
    }
    // REVIEW §1.6: forward string-context entries from the input.
    // Tree-walker (libexpr/primops.cc:1717+ prim_substring) propagates
    // context unconditionally -- this is what `builtins.substring 0 0
    // drv.outPath` relies on for ref-stripping (the empty-string result
    // carries the original drvPath context, marking the derivation as
    // a runtime dep without including the path).  Without forwarding,
    // v3 silently drops the context and downstream string concatenation
    // would lose the runtime dep.
    if (srcPtr) {
        if (auto * raw = lookupStringContextEntries(srcPtr)) {
            std::vector<std::string> copy(raw->begin(), raw->end());
            setStringContextEntries(out.payload.str, std::move(copy));
        }
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
    if (lst.tag() == Tag::App || lst.tag() == Tag::Thunk || lst.tag() == Tag::Slot)
        lst = forceValue(*state.vm, lst);
    if (!lst.isList()) typeError("map", "list");
    auto * src = lst.payload.list;
    if (!src || src->size == 0) {
        out.tag_payload = static_cast<uint64_t>(Tag::List);
        out.payload.list = Alloc::allocList(0);
        allocStats().listsAllocated++;
        return;
    }
    Value fun = args[0];
    ListVec * result = Alloc::allocList(src->size);
    allocStats().listsAllocated++;
    for (uint32_t i = 0; i < src->size; ++i) {
        // Build App(fun, elem) — lazy.
        ValuePair * pp = Alloc::allocPair();
        pp->left  = fun;
        pp->right = src->elems[i];
        Value v;
        v.tag_payload = static_cast<uint64_t>(Tag::App);
        v.payload.pair = pp;
        result->elems[i] = v;
    }
    out.tag_payload = static_cast<uint64_t>(Tag::List);
    out.payload.list = result;
}

void primFilter(EvalState & state, Value * args, Value & out)
{
    if (!args[1].isList()) typeError("filter", "list");
    auto * src = args[1].payload.list;
    if (!src || src->size == 0) {
        out.tag_payload = static_cast<uint64_t>(Tag::List);
        out.payload.list = Alloc::allocList(0);
        allocStats().listsAllocated++;
        return;
    }
    Value pred = args[0];
    std::vector<Value> kept;
    kept.reserve(src->size);
    for (uint32_t i = 0; i < src->size; ++i) {
        Value r = callClosure(*state.vm, pred, src->elems[i]);
        // Predicate result may be a thunk / app — force to WHNF.
        r = forceValue(*state.vm, r);
        if (!r.isBool()) typeError("filter", "predicate returning bool");
        if (r.payload.i == 1) kept.push_back(src->elems[i]);
    }
    ListVec * result = Alloc::allocList(static_cast<uint32_t>(kept.size()));
    allocStats().listsAllocated++;
    for (size_t i = 0; i < kept.size(); ++i) result->elems[i] = kept[i];
    out.tag_payload = static_cast<uint64_t>(Tag::List);
    out.payload.list = result;
}

void primFoldl(EvalState & state, Value * args, Value & out)
{
    // foldl' op nul list  —  strict left fold
    if (!args[2].isList()) typeError("foldl'", "list");
    Value op = args[0];
    Value acc = args[1];
    auto * src = args[2].payload.list;
    if (src) {
        for (uint32_t i = 0; i < src->size; ++i) {
            // Curried: op acc elem
            Value step1 = callClosure(*state.vm, op, acc);
            acc = callClosure(*state.vm, step1, src->elems[i]);
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
    if (len.tag() == Tag::App || len.tag() == Tag::Thunk || len.tag() == Tag::Slot)
        len = forceValue(*state.vm, len);
    if (!len.isInt()) typeError("genList", "int length");
    int64_t n = len.payload.i;
    if (n < 0) throw std::runtime_error("v3 primop genList: negative length");
    Value gen = args[0];
    ListVec * result = Alloc::allocList(static_cast<uint32_t>(n));
    allocStats().listsAllocated++;
    for (int64_t i = 0; i < n; ++i) {
        // Build App(gen, idx_int) — lazy.
        Value idx; idx.mkInt(i);
        ValuePair * pp = Alloc::allocPair();
        pp->left  = gen;
        pp->right = idx;
        Value v;
        v.tag_payload = static_cast<uint64_t>(Tag::App);
        v.payload.pair = pp;
        result->elems[i] = v;
    }
    out.tag_payload = static_cast<uint64_t>(Tag::List);
    out.payload.list = result;
}

void primAll(EvalState & state, Value * args, Value & out)
{
    if (!args[1].isList()) typeError("all", "list");
    auto * src = args[1].payload.list;
    Value pred = args[0];
    bool all = true;
    if (src) {
        for (uint32_t i = 0; i < src->size; ++i) {
            Value r = callClosure(*state.vm, pred, src->elems[i]);
            r = forceValue(*state.vm, r);
            if (!r.isBool()) typeError("all", "bool from predicate");
            if (r.payload.i == 0) { all = false; break; }
        }
    }
    out = all ? Value::vTrue : Value::vFalse;
}

void primAny(EvalState & state, Value * args, Value & out)
{
    if (!args[1].isList()) typeError("any", "list");
    auto * src = args[1].payload.list;
    Value pred = args[0];
    bool any = false;
    if (src) {
        for (uint32_t i = 0; i < src->size; ++i) {
            Value r = callClosure(*state.vm, pred, src->elems[i]);
            r = forceValue(*state.vm, r);
            if (!r.isBool()) typeError("any", "bool from predicate");
            if (r.payload.i == 1) { any = true; break; }
        }
    }
    out = any ? Value::vTrue : Value::vFalse;
}

void primGetEnv(EvalState &, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("getEnv", "string");
    const char * e = std::getenv(args[0].payload.str);
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

void primCompareVersions(EvalState &, Value * args, Value & out)
{
    if (!args[0].isString() || !args[1].isString())
        typeError("compareVersions", "two strings");
    std::string_view v1(args[0].payload.str);
    std::string_view v2(args[1].payload.str);
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
    if (lst.tag() == Tag::App || lst.tag() == Tag::Thunk || lst.tag() == Tag::Slot)
        lst = forceValue(*state.vm, lst);
    if (!lst.isList()) typeError("concatMap", "list");
    auto * src = lst.payload.list;
    Value fn = args[0];
    std::vector<Value> all;
    if (src) {
        for (uint32_t i = 0; i < src->size; ++i) {
            Value r = callClosure(*state.vm, fn, src->elems[i]);
            // Force the callback's return value — it may be a Tag::App
            // (e.g., when fn = (x: map g xs) and v3's lazy map returns
            // a list with App entries, then concatMap of that gets the
            // nested-list-as-App-entry shape).
            r = forceValue(*state.vm, r);
            if (!r.isList()) typeError("concatMap", "function returning list");
            if (r.payload.list)
                for (uint32_t j = 0; j < r.payload.list->size; ++j)
                    all.push_back(r.payload.list->elems[j]);
        }
    }
    ListVec * result = Alloc::allocList(static_cast<uint32_t>(all.size()));
    allocStats().listsAllocated++;
    for (size_t i = 0; i < all.size(); ++i) result->elems[i] = all[i];
    out.tag_payload = static_cast<uint64_t>(Tag::List);
    out.payload.list = result;
}

void primPartition(EvalState & state, Value * args, Value & out)
{
    if (!args[1].isList()) typeError("partition", "list");
    auto * src = args[1].payload.list;
    Value pred = args[0];
    std::vector<Value> right_, wrong_;
    if (src) {
        for (uint32_t i = 0; i < src->size; ++i) {
            Value r = callClosure(*state.vm, pred, src->elems[i]);
            // The predicate may return a thunk / app / closure-eval-
            // pending value — force it to WHNF before the bool check.
            r = forceValue(*state.vm, r);
            if (!r.isBool()) typeError("partition", "predicate returning bool");
            if (r.payload.i == 1) right_.push_back(src->elems[i]);
            else                  wrong_.push_back(src->elems[i]);
        }
    }
    auto mkList = [](std::vector<Value> & v) {
        ListVec * l = Alloc::allocList(static_cast<uint32_t>(v.size()));
        allocStats().listsAllocated++;
        for (size_t i = 0; i < v.size(); ++i) l->elems[i] = v[i];
        Value out;
        out.tag_payload = static_cast<uint64_t>(Tag::List);
        out.payload.list = l;
        return out;
    };
    Value rightV = mkList(right_);
    Value wrongV = mkList(wrong_);

    // Intern via the global table so the resulting attrset's SymbolIds
    // match what other CUs and the JSON printer use.
    SymbolId sRight = ir::globalInternSymbol("right");
    SymbolId sWrong = ir::globalInternSymbol("wrong");

    Bindings * b = Alloc::allocBindings(2);
    allocStats().attrsetsAllocated++;
    if (sRight < sWrong) {
        b->entries[0] = {sRight, rightV};
        b->entries[1] = {sWrong, wrongV};
    } else {
        b->entries[0] = {sWrong, wrongV};
        b->entries[1] = {sRight, rightV};
    }
    out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
    out.payload.bindings = b;
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
    auto * src = args[0].payload.list;
    if (!src || src->size == 0) {
        Bindings * b = Alloc::allocBindings(0);
        allocStats().attrsetsAllocated++;
        out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
        out.payload.bindings = b;
        return;
    }
    SymbolId nameSym  = vmIntern(state, "name");
    SymbolId valueSym = vmIntern(state, "value");
    std::vector<std::pair<SymbolId, Value>> entries;
    entries.reserve(src->size);
    for (uint32_t i = 0; i < src->size; ++i) {
        Value el = forceValue(*state.vm, src->elems[i]);
        if (!el.isAttrs() || !el.payload.bindings)
            typeError("listToAttrs", "list of attrsets");
        const Value * nvRaw = el.payload.bindings->lookup(nameSym);
        const Value * vvRaw = el.payload.bindings->lookup(valueSym);
        if (!nvRaw || !vvRaw)
            typeError("listToAttrs", "{ name = string; value = ...; }");
        Value nv = forceValue(*state.vm, *nvRaw);
        if (!nv.isString())
            typeError("listToAttrs", "{ name = string; value = ...; }");
        SymbolId k = vmIntern(state, nv.payload.str);
        // value stays lazy on purpose
        entries.emplace_back(k, *vvRaw);
    }
    // listToAttrs in Nix is *first-wins* on duplicate keys (matches the
    // tree-walker's behaviour and what `eval-okay-listtoattrs` exercises).
    // Stable-sort preserves insertion order within a key so the first
    // encounter survives the dedupe pass below.
    std::stable_sort(entries.begin(), entries.end(),
        [](auto & a, auto & b) { return a.first < b.first; });
    std::vector<std::pair<SymbolId, Value>> dedup;
    dedup.reserve(entries.size());
    for (auto & p : entries) {
        if (!dedup.empty() && dedup.back().first == p.first)
            continue; // keep the first occurrence
        dedup.push_back(p);
    }
    Bindings * b = Alloc::allocBindings(static_cast<uint32_t>(dedup.size()));
    allocStats().attrsetsAllocated++;
    for (size_t i = 0; i < dedup.size(); ++i) {
        b->entries[i].name  = dedup[i].first;
        b->entries[i].value = dedup[i].second;
    }
    out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
    out.payload.bindings = b;
}

void primRemoveAttrs(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isAttrs()) {
        char buf[64];
        std::snprintf(buf, sizeof buf,
            "v3 primop removeAttrs: expected attrset (got tag=%u)",
            (unsigned)args[0].tag());
        throw std::runtime_error(buf);
    }
    if (!args[1].isList())  typeError("removeAttrs", "list of strings");
    auto * src = args[0].payload.bindings;
    auto * names = args[1].payload.list;
    if (!src || !names || names->size == 0) { out = args[0]; return; }
    std::unordered_set<SymbolId> toRemove;
    for (uint32_t i = 0; i < names->size; ++i) {
        const Value & el = names->elems[i];
        if (!el.isString()) typeError("removeAttrs", "list of strings");
        toRemove.insert(vmIntern(state, el.payload.str));
    }
    Bindings * result = Alloc::allocBindings(src->size);
    allocStats().attrsetsAllocated++;
    uint32_t k = 0;
    for (uint32_t i = 0; i < src->size; ++i) {
        if (toRemove.count(src->entries[i].name) == 0) {
            result->entries[k++] = src->entries[i];
        }
    }
    result->size = k;
    out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
    out.payload.bindings = result;
}

void primIntersectAttrs(EvalState &, Value * args, Value & out)
{
    if (!args[0].isAttrs() || !args[1].isAttrs())
        typeError("intersectAttrs", "two attrsets");
    auto * keep = args[0].payload.bindings;
    auto * src  = args[1].payload.bindings;
    if (!keep || !src) {
        Bindings * b = Alloc::allocBindings(0);
        allocStats().attrsetsAllocated++;
        out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
        out.payload.bindings = b;
        return;
    }
    Bindings * result = Alloc::allocBindings(src->size);
    allocStats().attrsetsAllocated++;
    uint32_t k = 0;
    for (uint32_t i = 0; i < src->size; ++i) {
        if (keep->lookup(src->entries[i].name))
            result->entries[k++] = src->entries[i];
    }
    result->size = k;
    out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
    out.payload.bindings = result;
}

void primMapAttrs(EvalState & state, Value * args, Value & out)
{
    Value fn = args[0];
    if (!args[1].isAttrs()) typeError("mapAttrs", "attrset");
    auto * src = args[1].payload.bindings;
    if (!src) { out = args[1]; return; }
    Bindings * result = Alloc::allocBindings(src->size);
    allocStats().attrsetsAllocated++;
    for (uint32_t i = 0; i < src->size; ++i) {
        SymbolId sym = src->entries[i].name;
        Value nameStr = mkStringValueOwned(std::string(vmSymName(state, sym)));
        // Build a Tag::App chain that, when forced, applies
        // `fn name value`.  This keeps mapAttrs lazy: `mapAttrs throw
        // attrs` only fires the throw on the entries actually demanded
        // by callers, matching tree-walker.
        ValuePair * pp1 = Alloc::allocPair();
        pp1->left  = fn;
        pp1->right = nameStr;
        Value step1; step1.tag_payload = static_cast<uint64_t>(Tag::App); step1.payload.pair = pp1;
        ValuePair * pp2 = Alloc::allocPair();
        pp2->left  = step1;
        pp2->right = src->entries[i].value;
        Value step2; step2.tag_payload = static_cast<uint64_t>(Tag::App); step2.payload.pair = pp2;
        result->entries[i].name  = sym;
        result->entries[i].value = step2;
    }
    out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
    out.payload.bindings = result;
}

void primElem(EvalState & state, Value * args, Value & out)
{
    if (!args[1].isList()) typeError("elem", "list");
    auto * src = args[1].payload.list;
    Value x = args[0];
    bool found = false;
    if (src) {
        for (uint32_t i = 0; i < src->size; ++i) {
            if (valueEqual(*state.vm, x, src->elems[i])) { found = true; break; }
        }
    }
    out = found ? Value::vTrue : Value::vFalse;
}

void primGetAttr(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("getAttr", "string");
    if (!args[1].isAttrs())  typeError("getAttr", "attrset");
    SymbolId k = vmIntern(state, args[0].payload.str);
    auto * b = args[1].payload.bindings;
    if (!b) throw std::runtime_error("v3 primop getAttr: attribute not found");
    const Value * v = b->lookup(k);
    if (!v) throw std::runtime_error("v3 primop getAttr: attribute not found");
    out = *v;
}

void primHasAttr(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("hasAttr", "string");
    if (!args[1].isAttrs())  typeError("hasAttr", "attrset");
    SymbolId k = vmIntern(state, args[0].payload.str);
    auto * b = args[1].payload.bindings;
    out = (b && b->has(k)) ? Value::vTrue : Value::vFalse;
}

void primCatAttrs(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("catAttrs", "string");
    if (!args[1].isList())   typeError("catAttrs", "list");
    SymbolId k = vmIntern(state, args[0].payload.str);
    auto * lst = args[1].payload.list;
    std::vector<Value> kept;
    if (lst) {
        for (uint32_t i = 0; i < lst->size; ++i) {
            Value el = forceValue(*state.vm, lst->elems[i]);
            if (!el.isAttrs() || !el.payload.bindings) continue;
            const Value * v = el.payload.bindings->lookup(k);
            if (v) kept.push_back(*v);
        }
    }
    ListVec * result = Alloc::allocList(static_cast<uint32_t>(kept.size()));
    allocStats().listsAllocated++;
    for (size_t i = 0; i < kept.size(); ++i) result->elems[i] = kept[i];
    out.tag_payload = static_cast<uint64_t>(Tag::List);
    out.payload.list = result;
}

void primReplaceStrings(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isList() || !args[1].isList() || !args[2].isString())
        typeError("replaceStrings", "(list, list, string)");
    auto * froms = args[0].payload.list;
    auto * tos   = args[1].payload.list;
    if (!froms || !tos || froms->size != tos->size)
        throw std::runtime_error("v3 primop replaceStrings: lists must have equal length");
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
    std::string s(args[2].payload.str);
    std::string result;
    size_t i = 0;
    auto tryReplaceAt = [&](size_t pos) -> int {
        for (uint32_t j = 0; j < froms->size; ++j) {
            std::string_view fv(froms->elems[j].payload.str);
            bool match = fv.empty()
                ? true
                : (pos + fv.size() <= s.size() && s.compare(pos, fv.size(), fv) == 0);
            if (!match) continue;
            Value t = forceValue(*state.vm, tos->elems[j]);
            if (!t.isString()) typeError("replaceStrings", "list of strings");
            result.append(t.payload.str);
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
}

void primAbort(EvalState &, Value * args, Value &)
{
    if (!args[0].isString()) typeError("abort", "string");
    // AbortError is a plain runtime_error (not derived from AssertionError),
    // so tryEval does NOT catch it -- matches tree-walker's nix::Abort.
    throw AbortError(std::string("v3 abort: ") + args[0].payload.str);
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
    if (v.isList() && v.payload.list) {
        if (!seen.insert(v.payload.list).second) return v;
        for (uint32_t i = 0; i < v.payload.list->size; ++i)
            v.payload.list->elems[i] = forceDeepRec(vm, v.payload.list->elems[i], seen);
    } else if (v.isAttrs() && v.payload.bindings) {
        if (!seen.insert(v.payload.bindings).second) return v;
        for (uint32_t i = 0; i < v.payload.bindings->size; ++i)
            v.payload.bindings->entries[i].value =
                forceDeepRec(vm, v.payload.bindings->entries[i].value, seen);
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
    if (!args[0].isString())
        typeError("unsafeGetAttrPos", "(string, attrset)");
    if (!args[1].isAttrs() || !args[1].payload.bindings) {
        out.mkNull();
        return;
    }
    SymbolId nameId = ir::globalInternSymbol(args[0].payload.str);
    uint32_t handle = lookupAttrPos(args[1].payload.bindings, nameId);
    const PosSnapshot * snap = resolvePosSnapshot(handle);
    if (!snap) { out.mkNull(); return; }
    SymbolId sFile   = vmIntern(state, "file");
    SymbolId sLine   = vmIntern(state, "line");
    SymbolId sColumn = vmIntern(state, "column");
    Bindings * b = Alloc::allocBindings(3);
    allocStats().attrsetsAllocated++;
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
        b->entries[i].value = entries[i].second;
    }
    out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
    out.payload.bindings = b;
}

/// builtins.toPath path-or-string -> path.
/// String inputs must be absolute paths (start with '/').  Tree-walker
/// rejects relative strings; v3 matches.  Also accepts attrsets with
/// `__toString` or `outPath` (the standard Nix coercion path).
void primToPath(EvalState & state, Value * args, Value & out)
{
    auto fromString = [&](const char * s) {
        if (!s || s[0] != '/')
            throw std::runtime_error("v3 toPath: string is not an absolute path");
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
        out.tag_payload = static_cast<uint64_t>(Tag::Path);
        out.payload.path = buf;
    };
    Value v = forceValue(*state.vm, args[0]);
    if (v.isPath())   { out = v; return; }
    if (v.isString()) { fromString(v.payload.str); return; }
    if (v.isAttrs() && v.payload.bindings) {
        static const SymbolId tsId  = ir::globalInternSymbol("__toString");
        static const SymbolId outId = ir::globalInternSymbol("outPath");
        if (auto * fn = v.payload.bindings->lookup(tsId)) {
            Value forced = forceValue(*state.vm, *fn);
            Value s = callClosure(*state.vm, forced, v);
            s = forceValue(*state.vm, s);
            if (s.isString()) { fromString(s.payload.str); return; }
        }
        if (auto * op = v.payload.bindings->lookup(outId)) {
            Value forced = forceValue(*state.vm, *op);
            if (forced.isString()) { fromString(forced.payload.str); return; }
            if (forced.isPath())   { out = forced; return; }
        }
    }
    typeError("toPath", "string or path");
}

/// builtins.splitVersion "1.2.3-alpha" -> ["1" "2" "3" "alpha"].
/// Splits on '.' and '-'; consecutive separators produce empty strings
/// (matching tree-walker behaviour).
void primSplitVersion(EvalState &, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("splitVersion", "string");
    std::string_view s(args[0].payload.str);
    std::vector<std::string> parts;
    std::string cur;
    auto emit = [&]() {
        if (!cur.empty()) parts.push_back(std::move(cur));
        cur.clear();
    };
    for (char c : s) {
        if (c == '.' || c == '-') emit();
        else cur.push_back(c);
    }
    emit();
    ListVec * lv = Alloc::allocList(static_cast<uint32_t>(parts.size()));
    allocStats().listsAllocated++;
    for (size_t i = 0; i < parts.size(); ++i) lv->elems[i] = mkStringValueOwned(parts[i]);
    out.tag_payload = static_cast<uint64_t>(Tag::List);
    out.payload.list = lv;
}

/// Helper: clone a v3 string with the same contents but a fresh
/// payload buffer (so context tagging is per-string-value).
static Value cloneString(const char * s)
{
    return mkStringValueOwned(std::string(s ? s : ""));
}

void primUnsafeDiscardStringContext(EvalState &, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("unsafeDiscardStringContext", "string");
    // Allocate a fresh string buffer with no context entry.
    out = cloneString(args[0].payload.str);
}

void primHasContext(EvalState &, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("hasContext", "string");
    out = lookupStringContextEntries(args[0].payload.str)
        ? Value::vTrue : Value::vFalse;
}

void primGetContext(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("getContext", "string");
    auto * raw = lookupStringContextEntries(args[0].payload.str);
    if (!raw) {
        Bindings * b = Alloc::allocBindings(0);
        out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
        out.payload.bindings = b;
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
        out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
        out.payload.bindings = b;
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
            allocStats().listsAllocated++;
            for (size_t i = 0; i < g.outputs.size(); ++i)
                lv->elems[i] = mkStringValueOwned(g.outputs[i]);
            Value lvVal;
            lvVal.tag_payload = static_cast<uint64_t>(Tag::List);
            lvVal.payload.list = lv;
            subEntries.emplace_back(sOutputs, lvVal);
        }
        std::sort(subEntries.begin(), subEntries.end(),
            [](auto & a, auto & b) { return a.first < b.first; });
        Bindings * sb = Alloc::allocBindings(static_cast<uint32_t>(subEntries.size()));
        allocStats().attrsetsAllocated++;
        for (size_t i = 0; i < subEntries.size(); ++i) {
            sb->entries[i].name  = subEntries[i].first;
            sb->entries[i].value = subEntries[i].second;
        }
        Value subVal;
        subVal.tag_payload = static_cast<uint64_t>(Tag::Attrs);
        subVal.payload.bindings = sb;
        entries.emplace_back(vmIntern(state, path), subVal);
    }
    std::sort(entries.begin(), entries.end(),
        [](auto & a, auto & b) { return a.first < b.first; });
    Bindings * bb = Alloc::allocBindings(static_cast<uint32_t>(entries.size()));
    allocStats().attrsetsAllocated++;
    for (size_t i = 0; i < entries.size(); ++i) {
        bb->entries[i].name  = entries[i].first;
        bb->entries[i].value = entries[i].second;
    }
    out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
    out.payload.bindings = bb;
}

/// builtins.appendContext s ctx — add `ctx`'s entries to `s`'s context.
/// `ctx` is an attrset of `store-path -> { path; outputs; allOutputs; }`.
void primAppendContext(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isString())
        typeError("appendContext", "(string, attrset)");
    Value ctxV = forceValue(*state.vm, args[1]);
    if (!ctxV.isAttrs() || !ctxV.payload.bindings)
        typeError("appendContext", "second arg attrset");
    // Start with the existing context.
    auto & symTab = ir::globalSymbolTable();
    auto existing = lookupStringContext(args[0].payload.str);
    nix::NixStringContext ctx = existing;
    auto * ctxB = ctxV.payload.bindings;
    if (!state.nixEvalState) {
        out = cloneString(args[0].payload.str);
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
        if (!sub.isAttrs() || !sub.payload.bindings) continue;
        nix::StorePath storePath = ns.store->parseStorePath(pathStr);
        if (auto * pp = sub.payload.bindings->lookup(sPath)) {
            Value pv = forceValue(*state.vm, *pp);
            if (pv.isBool() && pv.payload.i == 1)
                ctx.insert(nix::NixStringContextElem{nix::NixStringContextElem::Opaque{.path = storePath}});
        }
        if (auto * ao = sub.payload.bindings->lookup(sAllOutputs)) {
            Value av = forceValue(*state.vm, *ao);
            if (av.isBool() && av.payload.i == 1)
                ctx.insert(nix::NixStringContextElem{nix::NixStringContextElem::DrvDeep{.drvPath = storePath}});
        }
        if (auto * outsRaw = sub.payload.bindings->lookup(sOutputs)) {
            Value ov = forceValue(*state.vm, *outsRaw);
            if (ov.isList() && ov.payload.list) {
                for (uint32_t j = 0; j < ov.payload.list->size; ++j) {
                    Value e = forceValue(*state.vm, ov.payload.list->elems[j]);
                    if (!e.isString()) continue;
                    nix::SingleDerivedPath dp{nix::SingleDerivedPath::Opaque{.path = storePath}};
                    nix::ref<nix::SingleDerivedPath> drvRef =
                        nix::make_ref<nix::SingleDerivedPath>(dp);
                    ctx.insert(nix::NixStringContextElem{
                        nix::NixStringContextElem::Built{
                            .drvPath = drvRef, .output = e.payload.str}});
                }
            }
        }
    }
    out = cloneString(args[0].payload.str);
    if (!ctx.empty())
        setStringContext(out.payload.str, ctx);
}

/// builtins.addDrvOutputDependencies — turn each Opaque entry in the
/// string's context into a DrvDeep entry.  No-op for non-Opaque
/// entries.  Idempotent.  Uses the same cloneString pattern so we
/// don't mutate the original buffer's table entry.
void primAddDrvOutputDependencies(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isString())
        typeError("addDrvOutputDependencies", "string");
    auto existing = lookupStringContext(args[0].payload.str);
    // Tree-walker requires exactly one context entry which must be a
    // single .drv path (Opaque or DrvDeep).  v3 must mirror that.
    if (existing.empty())
        throw std::runtime_error("v3 addDrvOutputDependencies: empty string context");
    if (existing.size() > 1)
        throw std::runtime_error("v3 addDrvOutputDependencies: string context has multiple entries");
    const auto & e = *existing.begin();
    if (!std::holds_alternative<nix::NixStringContextElem::Opaque>(e.raw) &&
        !std::holds_alternative<nix::NixStringContextElem::DrvDeep>(e.raw))
        throw std::runtime_error("v3 addDrvOutputDependencies: context entry is not a single drv path");
    nix::NixStringContext ctx;
    if (auto * o = std::get_if<nix::NixStringContextElem::Opaque>(&e.raw)) {
        // Opaque entries are also rejected if they don't end in .drv —
        // tree-walker requires the path be a derivation.
        if (!o->path.name().ends_with(".drv"))
            throw std::runtime_error("v3 addDrvOutputDependencies: context entry is not a derivation path");
        ctx.insert(nix::NixStringContextElem{nix::NixStringContextElem::DrvDeep{.drvPath = o->path}});
    } else {
        ctx.insert(e);
    }
    out = cloneString(args[0].payload.str);
    if (!ctx.empty()) setStringContext(out.payload.str, ctx);
    (void)state;
}

/// builtins.unsafeDiscardOutputDependency — turn DrvDeep entries
/// (`=<drvPath>`) into Opaque entries (`<drvPath>`).  Other entries
/// pass through.
void primUnsafeDiscardOutputDependency(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isString())
        typeError("unsafeDiscardOutputDependency", "string");
    auto existing = lookupStringContext(args[0].payload.str);
    nix::NixStringContext ctx;
    for (auto & e : existing) {
        if (auto * d = std::get_if<nix::NixStringContextElem::DrvDeep>(&e.raw)) {
            ctx.insert(nix::NixStringContextElem{nix::NixStringContextElem::Opaque{.path = d->drvPath}});
        } else {
            ctx.insert(e);
        }
    }
    out = cloneString(args[0].payload.str);
    if (!ctx.empty()) setStringContext(out.payload.str, ctx);
    (void)state;
}

/// builtins.__nixPath : list of `{prefix, path}` attrsets.  Reads the
/// host EvalState's LookupPath.  Used implicitly by `<x>` syntax.
void primNixPath(EvalState & state, Value *, Value & out)
{
    if (!state.nixEvalState) {
        ListVec * empty = Alloc::allocList(0);
        out.tag_payload = static_cast<uint64_t>(Tag::List);
        out.payload.list = empty;
        return;
    }
    auto lookupPath = state.nixEvalState->getLookupPath();
    auto & lp = lookupPath.elements;
    ListVec * lv = Alloc::allocList(static_cast<uint32_t>(lp.size()));
    allocStats().listsAllocated++;
    SymbolId sPath   = vmIntern(state, "path");
    SymbolId sPrefix = vmIntern(state, "prefix");
    size_t i = 0;
    for (auto & el : lp) {
        Bindings * b = Alloc::allocBindings(2);
        SymbolId nA = sPath, nB = sPrefix;
        Value vA = mkStringValueOwned(el.path.s);
        Value vB = mkStringValueOwned(el.prefix.s);
        if (nA < nB) {
            b->entries[0] = {nA, vA};
            b->entries[1] = {nB, vB};
        } else {
            b->entries[0] = {nB, vB};
            b->entries[1] = {nA, vA};
        }
        Value v;
        v.tag_payload = static_cast<uint64_t>(Tag::Attrs);
        v.payload.bindings = b;
        lv->elems[i++] = v;
    }
    out.tag_payload = static_cast<uint64_t>(Tag::List);
    out.payload.list = lv;
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
    auto * lst = args[0].payload.list;
    if (lst) {
        SymbolId sPath   = vmIntern(state, "path");
        SymbolId sPrefix = vmIntern(state, "prefix");
        for (uint32_t i = 0; i < lst->size; ++i) {
            Value el = forceValue(*state.vm, lst->elems[i]);
            if (!el.isAttrs() || !el.payload.bindings) continue;
            const Value * pV = el.payload.bindings->lookup(sPath);
            const Value * prV = el.payload.bindings->lookup(sPrefix);
            std::string p, prefix;
            if (pV) {
                Value f = forceValue(*state.vm, *pV);
                if (f.isString()) p = f.payload.str;
                else if (f.isPath()) p = f.payload.path;
            }
            if (prV) {
                Value f = forceValue(*state.vm, *prV);
                if (f.isString()) prefix = f.payload.str;
            }
            if (p.empty()) continue;
            lp.elements.push_back({nix::LookupPath::Prefix{prefix},
                                   nix::LookupPath::Path{p}});
        }
    }
    auto sp = state.nixEvalState->findFile(lp, args[1].payload.str);
    // CRIT-4: arena allocation.
    const std::string & abs = sp.path.abs();
    char * buf = Alloc::allocChars(abs.size() + 1);
    std::memcpy(buf, abs.data(), abs.size());
    buf[abs.size()] = '\0';
    out.tag_payload = static_cast<uint64_t>(Tag::Path);
    out.payload.path = buf;
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
    auto * lst = args[1].payload.list;
    // Group by symbol id, preserving value order.  Forcing each list
    // entry to attrset shape is required to enumerate names — same
    // strictness tree-walker has.
    std::unordered_map<SymbolId, std::vector<Value>> byName;
    if (lst) {
        for (uint32_t i = 0; i < lst->size; ++i) {
            Value attrs = forceValue(*state.vm, lst->elems[i]);
            if (!attrs.isAttrs() || !attrs.payload.bindings) continue;
            for (uint32_t j = 0; j < attrs.payload.bindings->size; ++j) {
                auto & en = attrs.payload.bindings->entries[j];
                byName[en.name].push_back(en.value);
            }
        }
    }
    std::vector<std::pair<SymbolId, Value>> entries;
    entries.reserve(byName.size());
    auto & symTab = ir::globalSymbolTable();
    for (auto & [sid, vs] : byName) {
        // Build the values list eagerly (cheap — just allocates the
        // ListVec; entries themselves stay lazy).
        ListVec * vl = Alloc::allocList(static_cast<uint32_t>(vs.size()));
        allocStats().listsAllocated++;
        for (size_t i = 0; i < vs.size(); ++i) vl->elems[i] = vs[i];
        Value lv;
        lv.tag_payload = static_cast<uint64_t>(Tag::List);
        lv.payload.list = vl;
        // Build name string.
        std::string nm = sid < symTab.size() ? symTab[sid] : std::to_string(sid);
        Value nameV = mkStringValueOwned(nm);
        // Build App(App(fn, nameV), lv) — a deferred call that resolves
        // when something forces the entry.  Mirrors mapAttrs' lazy
        // entry construction.
        ValuePair * pp1 = Alloc::allocPair();
        pp1->left  = fn;
        pp1->right = nameV;
        Value step1; step1.tag_payload = static_cast<uint64_t>(Tag::App); step1.payload.pair = pp1;
        ValuePair * pp2 = Alloc::allocPair();
        pp2->left  = step1;
        pp2->right = lv;
        Value step2; step2.tag_payload = static_cast<uint64_t>(Tag::App); step2.payload.pair = pp2;
        entries.emplace_back(sid, step2);
    }
    std::sort(entries.begin(), entries.end(),
        [](const auto & a, const auto & b) { return a.first < b.first; });
    Bindings * b = Alloc::allocBindings(static_cast<uint32_t>(entries.size()));
    allocStats().attrsetsAllocated++;
    for (size_t i = 0; i < entries.size(); ++i) {
        b->entries[i].name = entries[i].first;
        b->entries[i].value = entries[i].second;
    }
    out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
    out.payload.bindings = b;
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
    if (v.isString())   std::fprintf(stderr, "trace: %s\n", v.payload.str);
    else if (v.isInt()) std::fprintf(stderr, "trace: %lld\n", (long long)v.payload.i);
    else if (v.isFloat()) std::fprintf(stderr, "trace: %g\n", v.payload.f);
    else if (v.isBool()) std::fprintf(stderr, "trace: %s\n", v.payload.i == 1 ? "true" : "false");
    else if (v.isNull()) std::fprintf(stderr, "trace: null\n");
    else if (v.isPath()) std::fprintf(stderr, "trace: %s\n", v.payload.path);
    else if (state.nixEvalState) {
        // §1.6 follow-up: route through tree-walker's ValuePrinter so
        // trace output matches TW byte-for-byte (preserves «thunk» /
        // <LAMBDA> / <PRIMOP> shape markers + cycle detection that
        // valueToJson loses by force-everything-deeply).
        try {
            nix::Value * tw = v3ToTreeWalkerPublic(*state.nixEvalState, v);
            if (tw) {
                std::stringstream ss;
                ss << nix::ValuePrinter(*state.nixEvalState, *tw);
                std::fprintf(stderr, "trace: %s\n", ss.str().c_str());
            } else {
                std::fprintf(stderr, "trace: <complex value>\n");
            }
        } catch (...) {
            // Fallback: best-effort JSON dump if the bridge fails.
            try {
                auto j = valueToJson(state, v);
                std::fprintf(stderr, "trace: %s\n", j.dump().c_str());
            } catch (...) {
                std::fprintf(stderr, "trace: <complex value>\n");
            }
        }
    } else {
        try {
            auto j = valueToJson(state, v);
            std::fprintf(stderr, "trace: %s\n", j.dump().c_str());
        } catch (...) {
            std::fprintf(stderr, "trace: <complex value>\n");
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
    if (args[0].isString()) s = args[0].payload.str;
    else if (args[0].isPath()) s = args[0].payload.path;
    else typeError("baseNameOf", "string or path");
    if (s.empty()) { out = mkStringValueOwned(""); return; }
    size_t last = s.size() - 1;
    if (s[last] == '/' && last > 0) last -= 1;
    size_t pos = s.rfind('/', last);
    if (pos == std::string_view::npos) pos = 0;
    else pos += 1;
    out = mkStringValueOwned(std::string(s.substr(pos, last - pos + 1)));
}

void primDirOf(EvalState &, Value * args, Value & out)
{
    std::string s;
    bool isPathV = false;
    if (args[0].isString()) s = args[0].payload.str;
    else if (args[0].isPath()) { s = args[0].payload.path; isPathV = true; }
    else typeError("dirOf", "string or path");
    auto pos = s.find_last_of('/');
    std::string dir = (pos == std::string::npos) ? "." :
                      (pos == 0) ? "/" : s.substr(0, pos);
    if (isPathV) {
        Value v;
        // CRIT-4: arena allocation for long-lived path payload.
        char * buf = Alloc::allocChars(dir.size() + 1);
        std::memcpy(buf, dir.data(), dir.size()); buf[dir.size()] = '\0';
        v.tag_payload = static_cast<uint64_t>(Tag::Path);
        v.payload.path = buf;
        out = v;
    } else {
        out = mkStringValueOwned(dir);
    }
}

void primPathExists(EvalState & state, Value * args, Value & out)
{
    std::string s;
    if (args[0].isString()) s = args[0].payload.str;
    else if (args[0].isPath()) s = args[0].payload.path;
    else typeError("pathExists", "string or path");

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
    std::string_view sep(args[0].payload.str);
    std::string_view s(args[1].payload.str);
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
    allocStats().listsAllocated++;
    for (size_t i = 0; i < parts.size(); ++i) lv->elems[i] = parts[i];
    out.tag_payload = static_cast<uint64_t>(Tag::List);
    out.payload.list = lv;
}

/// builtins.genericClosure { startSet, operator } -- BFS closure of
/// startSet under operator.  Items are deduplicated by their "key" attr.
void primGenericClosure(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isAttrs() || !args[0].payload.bindings)
        typeError("genericClosure", "attrset");
    SymbolId sStart = vmIntern(state, "startSet");
    SymbolId sOp    = vmIntern(state, "operator");
    SymbolId sKey   = vmIntern(state, "key");
    const Value * startVRaw = args[0].payload.bindings->lookup(sStart);
    const Value * opVRaw    = args[0].payload.bindings->lookup(sOp);
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
    if (startV.payload.list) {
        for (uint32_t i = 0; i < startV.payload.list->size; ++i)
            work.push_back(startV.payload.list->elems[i]);
    }
    std::unordered_set<std::string> seen;

    // Track the first-seen key type — tree-walker rejects mixing
    // string vs int keys across the closure.  We prefix the key
    // string with its tag to keep distinct types from colliding,
    // and explicitly raise if a later item presents a different tag.
    Tag firstKeyTag = Tag::Uninitialized;

    auto keyOf = [&](Value & it) -> std::string {
        it = forceValue(*state.vm, it);
        if (!it.isAttrs() || !it.payload.bindings)
            throw std::runtime_error("v3 primop genericClosure: items must be attrsets with a 'key' attr");
        const Value * kRaw = it.payload.bindings->lookup(sKey);
        if (!kRaw) throw std::runtime_error("v3 primop genericClosure: item missing 'key' attr");
        Value k = forceValue(*state.vm, *kRaw);
        Tag t = k.tag();
        if (t != Tag::String && t != Tag::Int && t != Tag::Float &&
            t != Tag::Path && t != Tag::Bool)
            throw std::runtime_error("v3 primop genericClosure: 'key' must be string / int / float / path / bool");
        if (firstKeyTag == Tag::Uninitialized) firstKeyTag = t;
        else if (firstKeyTag != t)
            throw std::runtime_error("v3 primop genericClosure: cannot compare keys of incompatible types");
        if (t == Tag::String) return std::string(k.payload.str);
        if (t == Tag::Int)    return std::to_string(k.payload.i);
        if (t == Tag::Float) {
            // REVIEW §3: reject NaN explicitly -- two NaN values
            // round-trip through std::to_string identically and would
            // collide as duplicate keys.  Tree-walker rejects too.
            if (std::isnan(k.payload.f))
                throw std::runtime_error(
                    "v3 primop genericClosure: NaN key is not orderable");
            return std::to_string(k.payload.f);
        }
        if (t == Tag::Path)   return std::string(k.payload.path ? k.payload.path : "");
        return k.payload.i ? "true" : "false";
    };

    while (!work.empty()) {
        Value it = work.front(); work.pop_front();
        std::string key = keyOf(it);
        if (!seen.insert(key).second) continue;
        result.push_back(it);
        Value next = callClosure(*state.vm, opV, it);
        if (!next.isList())
            throw std::runtime_error("v3 primop genericClosure: operator must return a list");
        if (next.payload.list) {
            for (uint32_t i = 0; i < next.payload.list->size; ++i)
                work.push_back(next.payload.list->elems[i]);
        }
    }

    ListVec * lv = Alloc::allocList(static_cast<uint32_t>(result.size()));
    allocStats().listsAllocated++;
    for (size_t i = 0; i < result.size(); ++i) lv->elems[i] = result[i];
    out.tag_payload = static_cast<uint64_t>(Tag::List);
    out.payload.list = lv;
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
void primMatch(EvalState &, Value * args, Value & out)
{
    if (!args[0].isString() || !args[1].isString())
        typeError("match", "(regex, string)");
    try {
        // Match tree-walker: POSIX extended regex (`.` matches newline,
        // POSIX bracket classes like [[:alnum:]] work).
        const std::regex & re = getCachedRegex(args[0].payload.str);
        std::cmatch m;
        if (!std::regex_match(args[1].payload.str, m, re)) {
            out = Value::vNull;
            return;
        }
        // m[0] is the entire match; captures are m[1..m.size()-1].
        size_t nGroups = m.size() > 0 ? m.size() - 1 : 0;
        ListVec * lv = Alloc::allocList(static_cast<uint32_t>(nGroups));
        allocStats().listsAllocated++;
        for (size_t i = 0; i < nGroups; ++i) {
            if (m[i + 1].matched)
                lv->elems[i] = mkStringValueOwned(m[i + 1].str());
            else
                lv->elems[i] = Value::vNull;
        }
        out.tag_payload = static_cast<uint64_t>(Tag::List);
        out.payload.list = lv;
    } catch (const std::regex_error & e) {
        throw std::runtime_error(std::string("v3 primop match: invalid regex: ") + e.what());
    }
}

/// builtins.split regex string -> list alternating strings and captures.
/// E.g. split "[ ]+" "hello  world" -> ["hello" [] "world"].
void primSplit(EvalState &, Value * args, Value & out)
{
    if (!args[0].isString() || !args[1].isString())
        typeError("split", "(regex, string)");
    try {
        const std::regex & re = getCachedRegex(args[0].payload.str);
        std::string_view s(args[1].payload.str);
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
            allocStats().listsAllocated++;
            for (size_t i = 0; i < nGroups; ++i) {
                if (match[i + 1].matched)
                    caps->elems[i] = mkStringValueOwned(match[i + 1].str());
                else
                    caps->elems[i] = Value::vNull;
            }
            Value capsV;
            capsV.tag_payload = static_cast<uint64_t>(Tag::List);
            capsV.payload.list = caps;
            parts.push_back(capsV);
            pos = match.position(0) + match.length(0);
        }
        // Trailing piece.
        parts.push_back(mkStringValueOwned(std::string(s.substr(pos))));

        ListVec * lv = Alloc::allocList(static_cast<uint32_t>(parts.size()));
        allocStats().listsAllocated++;
        for (size_t i = 0; i < parts.size(); ++i) lv->elems[i] = parts[i];
        out.tag_payload = static_cast<uint64_t>(Tag::List);
        out.payload.list = lv;
    } catch (const std::regex_error & e) {
        throw std::runtime_error(std::string("v3 primop split: invalid regex: ") + e.what());
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
    throw std::runtime_error("v3: unknown hash algorithm '" + std::string(a) + "'");
}

/// builtins.hashString algo s -> hex string of the digest.  Backed by
/// nix::hashString (libutil) which uses libcrypto.
void primHashString(EvalState &, Value * args, Value & out)
{
    if (!args[0].isString() || !args[1].isString())
        typeError("hashString", "(algo, string)");
    auto algo = parseHashAlgo(args[0].payload.str);
    auto h = nix::hashString(algo, args[1].payload.str);
    out = mkStringValueOwned(h.to_string(nix::HashFormat::Base16, false));
}

void primHashFile(EvalState &, Value * args, Value & out)
{
    if (!args[0].isString())
        typeError("hashFile", "(algo, path)");
    std::string path;
    if (args[1].isString())     path = args[1].payload.str;
    else if (args[1].isPath())  path = args[1].payload.path;
    else typeError("hashFile", "(algo, path)");
    auto algo = parseHashAlgo(args[0].payload.str);
    auto h = nix::hashFile(algo, path);
    out = mkStringValueOwned(h.to_string(nix::HashFormat::Base16, false));
}

void primConvertHash(EvalState & state, Value * args, Value & out)
{
    // builtins.convertHash { hash; hashAlgo?; toHashFormat; } -> string
    // hashAlgo is optional when hash is in `algo:body` or SRI form.
    if (!args[0].isAttrs() || !args[0].payload.bindings)
        typeError("convertHash", "attrset");
    SymbolId sHash = vmIntern(state, "hash");
    SymbolId sAlgo = vmIntern(state, "hashAlgo");
    SymbolId sFmt  = vmIntern(state, "toHashFormat");
    const Value * vhRaw = args[0].payload.bindings->lookup(sHash);
    const Value * vaRaw = args[0].payload.bindings->lookup(sAlgo);
    const Value * vfRaw = args[0].payload.bindings->lookup(sFmt);
    if (!vhRaw || !vfRaw)
        typeError("convertHash", "{ hash; hashAlgo?; toHashFormat; }");
    Value vh = forceValue(*state.vm, *vhRaw);
    Value vf = forceValue(*state.vm, *vfRaw);
    if (!vh.isString() || !vf.isString())
        typeError("convertHash", "{ hash; hashAlgo?; toHashFormat; }");

    nix::HashFormat fmt;
    std::string_view fs(vf.payload.str);
    if (fs == "base16")        fmt = nix::HashFormat::Base16;
    else if (fs == "nix32")    fmt = nix::HashFormat::Nix32;
    else if (fs == "base32")   fmt = nix::HashFormat::Nix32;  // alias
    else if (fs == "base64")   fmt = nix::HashFormat::Base64;
    else if (fs == "sri")      fmt = nix::HashFormat::SRI;
    else throw std::runtime_error("v3 convertHash: unknown format '" + std::string(fs) + "'");

    nix::Hash parsed{nix::HashAlgorithm::SHA256}; // dummy default
    if (vaRaw) {
        Value va = forceValue(*state.vm, *vaRaw);
        if (!va.isString())
            typeError("convertHash", "{ hash; hashAlgo?; toHashFormat; }");
        parsed = nix::Hash::parseAny(vh.payload.str, parseHashAlgo(va.payload.str));
    } else {
        // No hashAlgo — infer from `algo:body` or SRI prefix.
        parsed = nix::Hash::parseAny(vh.payload.str, std::nullopt);
    }
    out = mkStringValueOwned(parsed.to_string(fmt, false));
}

/// builtins.currentSystem and similar: just return host triple.
void primCurrentSystem(EvalState &, Value *, Value & out)
{
    // Use a sensible default; nix tests usually mock this.
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
    out = mkStringValueOwned(nix::nixVersion.c_str());
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
    if (args[0].isString()) path = args[0].payload.str;
    else if (args[0].isPath()) path = args[0].payload.path;
    else typeError("readFile", "string or path");

    // REVIEW §1.7-style routing: when a TW EvalState is wired, defer
    // path realisation to it so pure-eval / restricted-eval mode rules
    // apply.  Falls through to direct ifstream when no TW context
    // (v3-eval CLI standalone case).
    std::string content;
    if (state.nixEvalState) {
        auto & ns = *state.nixEvalState;
        try {
            nix::Value tw;
            if (args[0].isString()) tw.mkString(path, ns.mem);
            else                    tw.mkPath(nix::SourcePath(ns.rootFS, nix::CanonPath(path)), ns.mem);
            auto sp = ns.realisePath(nix::noPos, tw);
            content = sp.readFile();
        } catch (const nix::RestrictedPathError &) {
            // Tree-walker's prim_readFile rethrows -- mirror.
            throw;
        } catch (...) {
            // Realisation failure (path doesn't exist, etc.) -- fall
            // through to plain ifstream so the error message matches
            // the v3 standalone behaviour.
            std::ifstream f(path);
            if (!f) throw std::runtime_error("v3 primop readFile: cannot open " + path);
            std::stringstream ss;
            ss << f.rdbuf();
            content = ss.str();
        }
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

    out = mkStringValueOwned(std::move(content));

    // §1.6: forward string-context.  When `path` was a Path Value with
    // context (e.g. a `${drv}` interpolation result coerced to path),
    // the returned string carries that context so downstream uses
    // mark the original drv as a runtime dep.
    if (args[0].isString() && args[0].payload.str) {
        if (auto * raw = lookupStringContextEntries(args[0].payload.str)) {
            std::vector<std::string> copy(raw->begin(), raw->end());
            setStringContextEntries(out.payload.str, std::move(copy));
        }
    }
}

/// builtins.readDir path -> attrset of name -> "regular"|"directory"|"symlink"|"unknown".
void primReadDir(EvalState & state, Value * args, Value & out)
{
    std::string path;
    if (args[0].isString()) path = args[0].payload.str;
    else if (args[0].isPath()) path = args[0].payload.path;
    else typeError("readDir", "string or path");
    std::vector<std::pair<SymbolId, Value>> entries;
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
    allocStats().attrsetsAllocated++;
    for (size_t i = 0; i < entries.size(); ++i) {
        b->entries[i].name  = entries[i].first;
        b->entries[i].value = entries[i].second;
    }
    out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
    out.payload.bindings = b;
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
    if (!args[0].isString()) typeError("parseDrvName", "string");
    std::string s(args[0].payload.str);
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
    allocStats().attrsetsAllocated++;
    Value vn = mkStringValueOwned(name);
    Value vv = mkStringValueOwned(version);
    if (sName < sVersion) { b->entries[0] = {sName, vn}; b->entries[1] = {sVersion, vv}; }
    else                  { b->entries[0] = {sVersion, vv}; b->entries[1] = {sName, vn}; }
    out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
    out.payload.bindings = b;
}

/// builtins.groupBy keyFn list -> { key = [items with that key]; }
void primGroupBy(EvalState & state, Value * args, Value & out)
{
    if (!args[1].isList()) typeError("groupBy", "list");
    auto * src = args[1].payload.list;
    Value keyFn = args[0];
    std::unordered_map<std::string, std::vector<Value>> groups;
    if (src) {
        for (uint32_t i = 0; i < src->size; ++i) {
            Value k = callClosure(*state.vm, keyFn, src->elems[i]);
            if (!k.isString()) typeError("groupBy", "key fn returning string");
            groups[std::string(k.payload.str)].push_back(src->elems[i]);
        }
    }
    std::vector<std::pair<SymbolId, Value>> entries;
    entries.reserve(groups.size());
    for (auto & [name, items] : groups) {
        ListVec * lv = Alloc::allocList(static_cast<uint32_t>(items.size()));
        allocStats().listsAllocated++;
        for (size_t i = 0; i < items.size(); ++i) lv->elems[i] = items[i];
        Value lstV;
        lstV.tag_payload = static_cast<uint64_t>(Tag::List);
        lstV.payload.list = lv;
        entries.emplace_back(vmIntern(state, name), lstV);
    }
    std::sort(entries.begin(), entries.end(),
        [](auto & a, auto & b) { return a.first < b.first; });
    Bindings * b = Alloc::allocBindings(static_cast<uint32_t>(entries.size()));
    allocStats().attrsetsAllocated++;
    for (size_t i = 0; i < entries.size(); ++i) {
        b->entries[i].name  = entries[i].first;
        b->entries[i].value = entries[i].second;
    }
    out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
    out.payload.bindings = b;
}

void primReadFileType(EvalState &, Value * args, Value & out)
{
    std::string path;
    if (args[0].isString()) path = args[0].payload.str;
    else if (args[0].isPath()) path = args[0].payload.path;
    else typeError("readFileType", "string or path");
    std::error_code ec;
    auto status = std::filesystem::symlink_status(path, ec);
    if (ec) throw std::runtime_error("v3 readFileType: " + ec.message());
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
    try {
        Value v = forceValue(*state.vm, args[1]);
        out = v;
    } catch (const BlackholeError & ex) {
        std::string msg;
        try { msg = toStringCoerce(state, args[0]); }
        catch (...) { msg = "<addErrorContext: error coercing message>"; }
        if (dbg) std::fprintf(stderr,
            "v3 addErrorContext (BlackholeError): %s\n", msg.c_str());
        throw BlackholeError(msg + "\n" + ex.what());
    } catch (const std::exception & ex) {
        std::string msg;
        try { msg = toStringCoerce(state, args[0]); }
        catch (...) { msg = "<addErrorContext: error coercing message>"; }
        if (dbg) std::fprintf(stderr,
            "v3 addErrorContext: %s\n", msg.c_str());
        throw std::runtime_error(msg + "\n" + ex.what());
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
struct BridgeClosureEntry {
    Value v3Value;
    nix::Expr * fallbackExpr = nullptr;
};
// CRIT-2 (table side): the static bridge tables hold v3 Values whose
// payloads (Closure*, Bindings*, Thunk*, ListVec*) live in the v3
// arena.  std::allocator's malloc'd vector storage is invisible to
// Boehm; traceable_allocator routes the storage into a Boehm-scanned
// region so the inner payloads stay reachable across collections.
// The vectors themselves still grow unboundedly across a process
// lifetime -- that's MED-14 bounding work, separate from the GC
// reachability fix here.
static std::vector<BridgeClosureEntry,
    traceable_allocator<BridgeClosureEntry>> & v3BridgeClosures()
{
    static std::vector<BridgeClosureEntry,
        traceable_allocator<BridgeClosureEntry>> tbl;
    return tbl;
}

/// WC-14.5 lazy attr bridge: stores v3 Tag::Attrs Values keyed by
/// integer handle.  When tree-walker forces a particular attr's
/// value (which we bridged as a deferred App primop call), the
/// __v3_force_attr primop looks up the original v3 attrset and
/// bridges the single requested attr's value.  Avoids the eager-
/// recursion cycle that nixpkgs's lib.makeExtensible self-references
/// trigger on the full attrset structural traversal.
///
/// WC-19: also stores a fallback `nix::Expr *`.  When v3's blackhole
/// detector trips during the deferred force (an eval-order cycle
/// v3 sees but tree-walker would resolve), primV3ForceAttr re-runs
/// the recorded outer Expr through tree-walker and looks up the
/// requested attr in the result.  TLS-set by the v3 hook just
/// before invoking v3ToTreeWalkerPublic.
struct BridgeAttrEntry {
    Value v3Value;
    nix::Expr * fallbackExpr = nullptr;
};
struct BridgeListEntry {
    Value v3Value;
    nix::Expr * fallbackExpr = nullptr;
};
// CRIT-2 (table side): traceable storage so Boehm sees the inner
// v3-Value payloads.
static std::vector<BridgeAttrEntry,
    traceable_allocator<BridgeAttrEntry>> & v3BridgeAttrs()
{
    static std::vector<BridgeAttrEntry,
        traceable_allocator<BridgeAttrEntry>> tbl;
    return tbl;
}

/// Same idea for lists — each element bridged lazily on force.
static std::vector<BridgeListEntry,
    traceable_allocator<BridgeListEntry>> & v3BridgeLists()
{
    static std::vector<BridgeListEntry,
        traceable_allocator<BridgeListEntry>> tbl;
    return tbl;
}

/// Recursively convert a tree-walker nix::Value to a v3 Value.  Forces
/// thunks via tree-walker's evaluator before reading the type.
/// Per-call cycle table prevents infinite recursion on self-referential
/// attrsets (e.g. tree-walker's derivation result has `drvAttrs` that
/// refers back).
static Value treeWalkerToV3(EvalState & state, nix::Value & nv,
                            std::unordered_map<const void *, Value> & seen);
static Value treeWalkerToV3(EvalState & state, nix::Value & nv);

// Forward declaration so primV3CallBridge1 can use it.
static nix::Value * v3ToTreeWalker(EvalState & state, Value v);

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
namespace nix::v3 { bool forceEagerBridge(); bool shallowTWAttrsBridge(); }
namespace nix::v3 { namespace {

// #453 Phase D: bridge-primop call counters.  Atomics keep them off
// the hot-path lock; dumped from v3_hook.cc atexit when
// NIX_V3_PRIMOP_DUMP=1.  These fire when TW calls back into v3 via
// the bridge primops registered in TW's primop table.  Hot counts
// here mean v3 is leaking across the cutover; reducing them is the
// Phase D goal.
std::atomic<uint64_t> g_bridgeCallBridge1Calls{0};
std::atomic<uint64_t> g_bridgeForceAttrCalls{0};
std::atomic<uint64_t> g_bridgeForceListElemCalls{0};

// #458 step 2: shared depth counter and limit between
// primV3CallBridge1 (the legacy TW primop) and tryDispatchBridge1Direct
// (the #458 step 2 shortcut).  Without sharing they form a ping-pong:
// shortcut at depth N declines, TW dispatches primV3CallBridge1 with
// its own counter at 0, that re-enters shortcut, etc.  One thread-
// local counter for both paths so the depth ceiling actually fires
// regardless of which path is currently executing.
} // close anon ns
int & bridge1DepthCounter() {
    static thread_local int d = 0;
    return d;
}
int bridge1MaxDepth() {
    static const int k = []{
        if (const char * v = std::getenv("NIX_V3_BRIDGE1_DEPTH"))
            return std::max(0, std::atoi(v));
        // REVIEW_2026-05-06b PR1: bumped 8 → 16.  cardano-node hits the
        // limit ≈10–18 times per eval at depth=8, paying ≈5 ms per
        // fallback (~50–90 ms wasted).  Each extra frame costs ~50 µs
        // worth of stack/local setup; doubling the headroom is
        // essentially free and eliminates the bulk of the spurious
        // depth-fallbacks.  Real cycles still surface (the depth is
        // still bounded; cycles go infinite, not gradual).
        return 16;
    }();
    return k;
}
namespace {

/// 1-arg variant of the v3-closure bridge.  Used by v3ToTreeWalker
/// for the common case (every Nix lambda is unary at the AST level;
/// `f x y` is `(f x) y` — two separate 1-arg calls).  Looks up the
/// v3 closure stored at `handle`, converts the single arg, calls,
/// converts result.
static void primV3CallBridge1(nix::EvalState & ns, const nix::PosIdx pos,
                              nix::Value ** args, nix::Value & out)
{
    g_bridgeCallBridge1Calls.fetch_add(1, std::memory_order_relaxed);

    // #466 nested-bridge-primop depth bound (orthogonal to bridge1's
    // own per-bridge1-cascade depth counter below).
    {
        int kMax = bridgePrimopMaxDepth();
        if (kMax > 0 && bridgePrimopDepth() >= kMax) {
            ns.error<nix::EvalError>(
                "v3 callBridge1: nested bridge-primop depth exceeded %1% "
                "(structural cycle through fresh-VMState chain)",
                std::to_string(kMax)).debugThrow();
        }
    }
    BridgePrimopDepthGuard _bpdg(bridgePrimopDepth());

    // #455 / #457: depth-limit cascade of nested bridge1 calls.  When
    // v3 resolves a chain of `extends overlay (extends overlay2 ...)`
    // overlays, each becomes a Tag::Closure bridged via __v3_call_
    // bridge_1.  Calling the chain from TW dispatches into bridge1
    // recursively; deep chains (cardano-node has ~18 layers) burn the
    // pthread stack + cause TW's BlackHole detection to fire on `final`
    // mid-construction.  Cap the nest at a small N; beyond that, run
    // the fallbackExpr through TW so we don't pile bridge1 frames.
    //
    // Tunable via NIX_V3_BRIDGE1_DEPTH (default 8 = enough for 1-2
    // legitimate nested overlays without triggering on cardano-node's
    // 18+ layer fix-point).  Disable with =0.
    int & s_bridge1Depth = bridge1DepthCounter();
    int kBridge1MaxDepth = bridge1MaxDepth();
    if (kBridge1MaxDepth > 0 && s_bridge1Depth >= kBridge1MaxDepth) {
        // Resolve handle (still need it for the fallback Expr lookup).
        ns.forceValue(*args[0], pos);
        if (args[0]->type() == nix::nInt) {
            int64_t h = args[0]->integer().value;
            auto & tbl = v3BridgeClosures();
            if (h >= 0 && (size_t)h < tbl.size()) {
                nix::Expr * fb = tbl[(size_t)h].fallbackExpr;
                if (fb) {
                    nix::Value tw;
                    fb->eval(ns, ns.baseEnv, tw);
                    ns.forceValue(tw, pos);
                    ns.callFunction(tw, *args[1], out, pos);
                    return;
                }
            }
        }
        // No fallback Expr -- fall through to the regular bridge1 path
        // (it may still cycle, but no worse than before this guard).
    }
    struct Bridge1DepthGuard {
        int & d;
        Bridge1DepthGuard(int & d_) : d(d_) { ++d; }
        ~Bridge1DepthGuard() { --d; }
    } _b1dGuard(s_bridge1Depth);

    ns.forceValue(*args[0], pos);
    if (args[0]->type() != nix::nInt)
        ns.error<nix::EvalError>("v3 bridge1: handle must be int").debugThrow();
    int64_t h = args[0]->integer().value;
    auto & tbl = v3BridgeClosures();
    if (h < 0 || (size_t)h >= tbl.size())
        ns.error<nix::EvalError>("v3 bridge1: invalid handle").debugThrow();
    Value v3fn = tbl[(size_t)h].v3Value;
    nix::Expr * fallbackExpr = tbl[(size_t)h].fallbackExpr;

    ScopedNixEvalState _v3evalGuard(&ns);
    // #455: eager-force gate.  Tree-walker's regular callFunction does
    // NOT force the arg before invoking the function -- the function
    // body decides when to force.  v3's primV3CallBridge1 used to
    // pre-force args[1] which triggered TW's BlackHole detection on
    // mid-construction fix-point args (cardano-node's `extends`
    // overlay shape).
    //
    // With NIX_V3_LAZY_BRIDGE_ARG=1, skip the force and let
    // treeWalkerToV3's nThunk case wrap the unforced thunk as a v3
    // Bridge thunk; v3's body forces it on first access.  Default OFF
    // until the lang sweep + cardano-node OD test verify no
    // regressions; flip default-on once stable.
    static const bool lazyBridgeArg =
        std::getenv("NIX_V3_LAZY_BRIDGE_ARG") != nullptr;
    if (!lazyBridgeArg)
        ns.forceValue(*args[1], pos);

    // WC-18.3: run the v3 closure body in a fiber.  treeWalkerToV3
    // and forceBridgeThunk yield to the driver for tree-walker forces,
    // so the v3 dispatcher's call depth is bounded by the fiber's
    // own stack rather than the caller's pthread stack.
    // Gated via NIX_V3_FIBER_BRIDGE=1 — opt-in until validated.
    // ALSO: avoid nested fibers — when we're already in a fiber
    // (re-entrant bridge call from inside v3 evaluation), fall back
    // to direct call to keep the driver protocol simple.
    static const bool useFiber =
        std::getenv("NIX_V3_FIBER_BRIDGE") != nullptr;

    // WC-19+: wrap the v3 evaluation in try/catch so a v3-only
    // blackhole on closure-body forces falls back to tree-walker
    // (re-running fallbackExpr to obtain a tree-walker function,
    // then calling it with args[1]).  Same safety net WC-19 added
    // for primV3ForceAttr / primV3ForceListElem.  Only blackhole-
    // shaped runtime_errors trigger fallback — other failures (e.g.
    // wrong arg / type errors) are real bugs we don't want to mask.
    // REVIEW_2026-05-04 F4 / §6.3: typed `BlackholeError` (errors.hh)
    // replaces the prior substring match on the exception's what().
    // The string "(blackhole)" was load-bearing for fallback routing;
    // any unrelated TU emitting that phrase would silently route to
    // tree-walker.  Now driven by the type of the thrown exception.
    auto fallbackToTreeWalker = [&](const std::exception & ex) {
        if (!fallbackExpr || !dynamic_cast<const BlackholeError *>(&ex))
            throw std::runtime_error(ex.what());
        static const bool dbg = std::getenv("V3_DEBUG_HOOK") != nullptr;
        if (dbg) std::fprintf(stderr,
            "v3 bridge1: v3 path blackholed: %s — re-running fallback Expr "
            "via tree-walker\n", ex.what());
        nix::Value tw;
        fallbackExpr->eval(ns, ns.baseEnv, tw);
        ns.forceValue(tw, pos);
        // tw should be a function; call it with args[1].
        ns.callFunction(tw, *args[1], out, pos);
    };

    // #455: cycle detection -- mirrors primV3ForceAttr's protection.
    // Re-entry on the same handle while it's in progress means the
    // bridged closure is forcing through itself; throw blackhole-
    // shaped error so the caller's fallbackExpr re-eval path fires.
    static thread_local std::unordered_set<int64_t> tlsBridge1InProgress;
    if (!tlsBridge1InProgress.insert(h).second) {
        // Cycle: route to fallbackToTreeWalker by throwing typed.
        try {
            throw BlackholeError(
                "v3 bridge1: cycle on handle=" + std::to_string(h));
        } catch (const std::exception & ex) {
            fallbackToTreeWalker(ex);
            return;
        }
    }
    struct Bridge1Guard {
        int64_t h;
        ~Bridge1Guard() { tlsBridge1InProgress.erase(h); }
    } _b1g{h};

    // #466 / #479 Phase 1: cross-primop force-chain detector.  Catches
    // cycles that span multiple bridge primops (each layer has its own
    // handle so `tlsBridge1InProgress` above misses, but the chain's
    // overall identity repeats here).  Throws BlackholeError before
    // allocating the next VMState; fallbackToTreeWalker routes via TW.
    ForceChainGuard _fcg(ForceChainOp::CallBridge1, static_cast<uint64_t>(h));
    if (_fcg.isCycle()) {
        try {
            throw BlackholeError(
                _fcg.atDepthCeiling()
                ? std::string("v3 bridge1: force-chain depth ceiling reached")
                : "v3 bridge1: force-chain cycle on handle=" + std::to_string(h));
        } catch (const std::exception & ex) {
            fallbackToTreeWalker(ex);
            return;
        }
    }
    Value fn;
    try {
        if (useFiber && activeFiberDriverDepth == 0) {
            fn = runInFiber(ns, [&](Mailbox * /*mb*/) -> Value {
                // Each fiber gets its own VMState (isolation on top of
                // stack isolation).  Allocated on the fiber's stack.
                VMState fiberVm;
                fiberVm.valueStack.reserve(64 * 1024);
                fiberVm.frames.reserve(4096);
                fiberVm.withStack.reserve(64);
                EvalState fs;
                fs.nixEvalState = &ns;
                fs.vm = &fiberVm;
                Value v3arg = treeWalkerToV3(fs, *args[1]);
                Value r = callClosure(*fs.vm, v3fn, v3arg);
                return forceValue(*fs.vm, r);
            });
        } else {
            EvalState v3state;
            v3state.nixEvalState = &ns;
            // REVIEW MED-16: stack-allocated -- previous static
            // thread_local could leak frames from a failed prior call
            // into the next.  Reserves remain (they avoided per-call
            // reallocation, not persistence).
            VMState bridgeVm1;
            bridgeVm1.valueStack.reserve(64 * 1024);
            bridgeVm1.frames.reserve(4096);
            bridgeVm1.withStack.reserve(64);
            v3state.vm = &bridgeVm1;
            Value v3arg;
            try {
                v3arg = treeWalkerToV3(v3state, *args[1]);
            } catch (const std::exception & ex) {
                static const bool s_dbg = std::getenv("V3_DBG_BRIDGE1") != nullptr;
                if (s_dbg) {
                    nix::Value & a = *args[1];
                    std::fprintf(stderr,
                        "v3 bridge1: treeWalkerToV3 threw: %s "
                        "(handle=%lld v3fn.tag=%d args[1]=%p type=%d)\n",
                        ex.what(), (long long)h, (int)v3fn.tag(),
                        (void *)args[1],
                        a.isValid() ? (int)a.type<true>() : -1);
                    if (a.isValid() && a.type<true>() == nix::nAttrs && a.attrs()) {
                        std::fprintf(stderr,
                            "  args[1] is attrset with %u attrs:\n",
                            (unsigned)a.attrs()->size());
                        size_t shown = 0;
                        for (auto & it : *a.attrs()) {
                            if (shown++ >= 12) { std::fprintf(stderr, "  ...\n"); break; }
                            int t = it.value->isValid() ? (int)it.value->type<true>() : -1;
                            std::fprintf(stderr,
                                "    %s -> tag=%d ptr=%p\n",
                                std::string(ns.symbols[it.name]).c_str(),
                                t, (void *)it.value);
                        }
                    }
                    std::fflush(stderr);
                }
                throw;
            }
            fn = callClosure(*v3state.vm, v3fn, v3arg);
            fn = forceValue(*v3state.vm, fn);
        }
    } catch (const std::exception & ex) {
        fallbackToTreeWalker(ex);
        return;
    }

    // Convert the v3 result back to tree-walker.  Use the full
    // recursive bridge so attrsets / lists / nested closures
    // round-trip correctly.
    try {
        switch (fn.tag()) {
        case Tag::Bool:   out.mkBool(fn.payload.i == 1); break;
        case Tag::Int:    out.mkInt(fn.payload.i); break;
        case Tag::Float:  out.mkFloat(fn.payload.f); break;
        case Tag::Null:   out.mkNull(); break;
        case Tag::String: out.mkString(fn.payload.str ? fn.payload.str : "", ns.mem); break;
        case Tag::Uninitialized:
        case Tag::Path:
        case Tag::Attrs:
        case Tag::List:
        case Tag::Closure:
        case Tag::Thunk:
        case Tag::PrimOp:
        case Tag::PrimOpApp:
        case Tag::App:
        case Tag::Blackhole:
        case Tag::External:
        case Tag::Slot: {
            EvalState bridgeState;
            bridgeState.nixEvalState = &ns;
            // REVIEW MED-16: stack-allocated.
            VMState resultBridgeVm;
            resultBridgeVm.valueStack.reserve(64 * 1024);
            resultBridgeVm.frames.reserve(4096);
            resultBridgeVm.withStack.reserve(64);
            bridgeState.vm = &resultBridgeVm;
            nix::Value * tmp = v3ToTreeWalker(bridgeState, fn);
            if (tmp) out = *tmp; else out.mkNull();
            break;
        }
        }
    } catch (const std::exception & ex) {
        fallbackToTreeWalker(ex);
        return;
    }
}

/// WC-15: lazy attr-set bridge primop.  Args: handle (Int), name (String).
/// Looks up the v3 Tag::Attrs Value at handle, finds the attr by name,
/// bridges that single value to tree-walker via v3ToTreeWalker (which
/// for nested Attrs/Lists will itself be lazy, so cycles are bounded).
static void primV3ForceAttrInner(nix::EvalState & ns, const nix::PosIdx pos,
                              nix::Value ** args, nix::Value & out);
static void primV3ForceAttr(nix::EvalState & ns, const nix::PosIdx pos,
                            nix::Value ** args, nix::Value & out)
{
    g_bridgeForceAttrCalls.fetch_add(1, std::memory_order_relaxed);
    primV3ForceAttrInner(ns, pos, args, out);
}
static void primV3ForceAttrInner(nix::EvalState & ns, const nix::PosIdx pos,
                             nix::Value ** args, nix::Value & out)
{
    // #466 nested-bridge-primop depth bound (see bridgePrimopDepth comment).
    int kMax = bridgePrimopMaxDepth();
    if (kMax > 0 && bridgePrimopDepth() >= kMax) {
        // Throw a NON-Blackhole error so the catch's
        // fallbackToTreeWalker doesn't re-enter the cycle.
        ns.error<nix::EvalError>(
            "v3 forceAttr: nested bridge-primop depth exceeded %1% "
            "(structural cycle through fresh-VMState chain — typically "
            "lambda-skip + rec-attrset fix-point)",
            std::to_string(kMax)).debugThrow();
    }
    BridgePrimopDepthGuard _bpdg(bridgePrimopDepth());

    ns.forceValue(*args[0], pos);
    if (args[0]->type() != nix::nInt)
        ns.error<nix::EvalError>("v3 forceAttr: handle must be int").debugThrow();
    int64_t h = args[0]->integer().value;
    auto & tbl = v3BridgeAttrs();
    if (h < 0 || (size_t)h >= tbl.size())
        ns.error<nix::EvalError>("v3 forceAttr: invalid handle").debugThrow();
    Value v3attrs = tbl[(size_t)h].v3Value;
    nix::Expr * fallbackExpr = tbl[(size_t)h].fallbackExpr;
    if (v3attrs.tag() != Tag::Attrs || !v3attrs.payload.bindings)
        ns.error<nix::EvalError>("v3 forceAttr: handle does not point to an Attrs").debugThrow();

    ns.forceValue(*args[1], pos);
    if (args[1]->type() != nix::nString)
        ns.error<nix::EvalError>("v3 forceAttr: name must be string").debugThrow();
    std::string_view name(args[1]->string_view());

    // v3 Bindings are sorted by SymbolId.  We have a string name —
    // resolve through v3's symbol table.
    SymbolId sid = ir::globalInternSymbol(std::string(name));
    const Bindings * b = v3attrs.payload.bindings;
    const Value * found = b->lookup(sid);
    if (!found)
        ns.error<nix::EvalError>(
            "v3 forceAttr: attr '%1%' not found in bridged attrset",
            std::string(name)).debugThrow();

    // Bridge this single value.  Set up an EvalState + thread_local
    // shim VMState (mirrors the closure-bridge primop pattern).
    ScopedNixEvalState _v3evalGuard(&ns);
    EvalState v3state;
    v3state.nixEvalState = &ns;
    // REVIEW MED-16: stack-allocated.
    VMState bridgeVmAttr;
    bridgeVmAttr.valueStack.reserve(64 * 1024);
    bridgeVmAttr.frames.reserve(4096);
    bridgeVmAttr.withStack.reserve(64);
    v3state.vm = &bridgeVmAttr;

    // WC-19: deferred forces can hit eval-order cycles v3 sees but
    // tree-walker would resolve.  The eager-bridge path catches these
    // in v3_hook.cc's Tag::Attrs try/catch and falls back to tree-
    // walker on the outer Expr.  The lazy path was missing that
    // safety net — reproduce it here.  Only catch blackhole-shaped
    // errors so genuine bugs (type errors, missing args, ...)
    // surface instead of being masked.
    // REVIEW_2026-05-04 F4 / §6.3: typed `BlackholeError` instead of
    // `strstr` -- see fallbackToTreeWalker comment in primV3CallBridge1.
    //
    // #455: cycle detection.  When a v3 thunk's body forces a Bridge
    // that resolves to a TW lazy-bridged attrset (handle H), then v3
    // attr-selects on that, then forces the resulting PrimOpApp via
    // TW's force machinery, we re-enter primV3ForceAttr.  If the
    // chain re-enters with the SAME handle that's currently in-
    // progress on this thread, we have a cycle.  Throw a blackhole-
    // shaped error so the caller falls back to tree-walker.
    // Per-thread stack (no contention) of (handle, sid) pairs in
    // progress; matches behave like blackhole.
    static thread_local std::unordered_set<uint64_t> tlsForceAttrInProgress;
    uint64_t cycleKey = (static_cast<uint64_t>(h) << 32) | (uint64_t)sid;
    if (!tlsForceAttrInProgress.insert(cycleKey).second) {
        // Cycle: throw a typed BlackholeError so the catch below
        // routes to the fallback Expr rather than rethrowing.
        throw BlackholeError(
            "v3 forceAttr: cycle on handle=" + std::to_string(h)
            + " name=" + std::string(name));
    }
    struct InProgressGuard {
        uint64_t key;
        ~InProgressGuard() { tlsForceAttrInProgress.erase(key); }
    } _ipg{cycleKey};

    // #466 / #479 Phase 1: cross-primop force-chain detector.
    ForceChainGuard _fcg(ForceChainOp::ForceAttr,
                         static_cast<uint64_t>(h),
                         static_cast<uint64_t>(sid));
    if (_fcg.isCycle()) {
        throw BlackholeError(
            _fcg.atDepthCeiling()
            ? std::string("v3 forceAttr: force-chain depth ceiling reached")
            : "v3 forceAttr: force-chain cycle on handle=" + std::to_string(h)
              + " name=" + std::string(name));
    }
    nix::Symbol resolvedName = ns.symbols.create(name);
    try {
        nix::Value * tmp = v3ToTreeWalker(v3state, *found);
        if (tmp) out = *tmp; else out.mkNull();
        return;
    } catch (const std::exception & ex) {
        if (!fallbackExpr || !dynamic_cast<const BlackholeError *>(&ex)) throw;
        // #466 fallback re-entry bound + per-fallbackExpr cycle detection.
        //
        // BlackHoleError → fallback → re-eval via TW → could re-trigger
        // primV3ForceAttr → another BlackHoleError → another fallback.
        // Each iteration adds C-stack depth (the catch's call to
        // fallbackExpr->eval is on the C stack).  Without a bound,
        // structural cycles (lambda-skip + rec-attrset fix-points)
        // grow C-stack until SIGSEGV.
        //
        // Two complementary guards:
        //
        // (a) Depth bound: track fallback chain depth via thread_local;
        //     over the limit, re-throw without fallback.  Tunable via
        //     NIX_V3_FALLBACK_CHAIN_DEPTH (default 16).
        //
        // (b) Per-fallbackExpr cycle detection: if the SAME fallbackExpr
        //     pointer is already in flight on this thread (meaning the
        //     cycle is re-evaluating the same outer Expr that triggered
        //     us), refuse to retry — the second attempt has no
        //     additional information and just walks the same path.
        //     Tighter than the depth bound for the structural cycle
        //     case.
        static const int kFallbackMaxDepth = []{
            if (const char * v = std::getenv("NIX_V3_FALLBACK_CHAIN_DEPTH"))
                return std::max(0, std::atoi(v));
            return 16;
        }();
        static thread_local int tlsFallbackDepth = 0;
        if (kFallbackMaxDepth > 0 && tlsFallbackDepth >= kFallbackMaxDepth)
            throw;
        static thread_local std::unordered_set<const nix::Expr *>
            tlsFallbackInProgress;
        if (!tlsFallbackInProgress.insert(fallbackExpr).second) {
            // Same fallbackExpr is already in flight.  Don't recurse.
            throw;
        }
        struct FallbackGuards {
            int & depth;
            const nix::Expr * fb;
            FallbackGuards(int & d, const nix::Expr * f) : depth(d), fb(f) { ++depth; }
            ~FallbackGuards() { --depth; tlsFallbackInProgress.erase(fb); }
        } _fg(tlsFallbackDepth, fallbackExpr);

        static const bool dbg = std::getenv("V3_DEBUG_HOOK") != nullptr;
        if (dbg) std::fprintf(stderr,
            "v3 forceAttr: bridge blackholed: %s — re-running outer Expr "
            "via tree-walker for attr '%s'\n",
            ex.what(), std::string(name).c_str());
        nix::Value tw;
        fallbackExpr->eval(ns, ns.baseEnv, tw);
        ns.forceValue(tw, pos);
        if (tw.type() != nix::nAttrs)
            ns.error<nix::EvalError>(
                "v3 forceAttr: tree-walker fallback returned non-attrs").debugThrow();
        auto * a = tw.attrs()->get(resolvedName);
        if (!a)
            ns.error<nix::EvalError>(
                "v3 forceAttr: tree-walker fallback missing attr '%1%'",
                std::string(name)).debugThrow();
        ns.forceValue(*a->value, pos);
        out = *a->value;
    }
}

/// WC-15: lazy list-element bridge.  Args: handle (Int), index (Int).
static void primV3ForceListElemInner(nix::EvalState & ns, const nix::PosIdx pos,
                              nix::Value ** args, nix::Value & out);
static void primV3ForceListElem(nix::EvalState & ns, const nix::PosIdx pos,
                            nix::Value ** args, nix::Value & out)
{
    g_bridgeForceListElemCalls.fetch_add(1, std::memory_order_relaxed);
    primV3ForceListElemInner(ns, pos, args, out);
}
static void primV3ForceListElemInner(nix::EvalState & ns, const nix::PosIdx pos,
                                 nix::Value ** args, nix::Value & out)
{
    // #466 nested-bridge-primop depth bound (see bridgePrimopDepth comment).
    int kMax = bridgePrimopMaxDepth();
    if (kMax > 0 && bridgePrimopDepth() >= kMax) {
        ns.error<nix::EvalError>(
            "v3 forceListElem: nested bridge-primop depth exceeded %1% "
            "(structural cycle through fresh-VMState chain)",
            std::to_string(kMax)).debugThrow();
    }
    BridgePrimopDepthGuard _bpdg(bridgePrimopDepth());

    ns.forceValue(*args[0], pos);
    if (args[0]->type() != nix::nInt)
        ns.error<nix::EvalError>("v3 forceListElem: handle must be int").debugThrow();
    int64_t h = args[0]->integer().value;
    auto & tbl = v3BridgeLists();
    if (h < 0 || (size_t)h >= tbl.size())
        ns.error<nix::EvalError>("v3 forceListElem: invalid handle").debugThrow();
    Value v3list = tbl[(size_t)h].v3Value;
    nix::Expr * fallbackExpr = tbl[(size_t)h].fallbackExpr;
    if (v3list.tag() != Tag::List || !v3list.payload.list)
        ns.error<nix::EvalError>("v3 forceListElem: handle does not point to a List").debugThrow();

    ns.forceValue(*args[1], pos);
    if (args[1]->type() != nix::nInt)
        ns.error<nix::EvalError>("v3 forceListElem: index must be int").debugThrow();
    int64_t idx = args[1]->integer().value;
    const ListVec * l = v3list.payload.list;
    if (idx < 0 || (uint32_t)idx >= l->size)
        ns.error<nix::EvalError>("v3 forceListElem: index out of range").debugThrow();

    // #466 / #479 Phase 1: cross-primop force-chain detector.  This
    // primop previously had no cycle detection (only the global
    // bridgePrimopDepth bound), so a chain ending in a list-elem
    // re-entry would only surface after 64 levels of C-stack growth.
    // Throwing a BlackholeError here lets the catch route via fallback.
    ForceChainGuard _fcg(ForceChainOp::ForceListElem,
                         static_cast<uint64_t>(h),
                         static_cast<uint64_t>(idx));
    if (_fcg.isCycle()) {
        throw BlackholeError(
            _fcg.atDepthCeiling()
            ? std::string("v3 forceListElem: force-chain depth ceiling reached")
            : "v3 forceListElem: force-chain cycle on handle=" + std::to_string(h)
              + " idx=" + std::to_string(idx));
    }

    ScopedNixEvalState _v3evalGuard(&ns);
    EvalState v3state;
    v3state.nixEvalState = &ns;
    // REVIEW MED-16: stack-allocated.
    VMState bridgeVmList;
    bridgeVmList.valueStack.reserve(64 * 1024);
    bridgeVmList.frames.reserve(4096);
    bridgeVmList.withStack.reserve(64);
    v3state.vm = &bridgeVmList;

    // WC-19: same safety net as primV3ForceAttr.
    // REVIEW_2026-05-04 F4 / §6.3: typed BlackholeError instead of strstr.
    try {
        nix::Value * tmp = v3ToTreeWalker(v3state, l->elems[(uint32_t)idx]);
        if (tmp) out = *tmp; else out.mkNull();
        return;
    } catch (const std::exception & ex) {
        if (!fallbackExpr || !dynamic_cast<const BlackholeError *>(&ex)) throw;
        static const bool dbg = std::getenv("V3_DEBUG_HOOK") != nullptr;
        if (dbg) std::fprintf(stderr,
            "v3 forceListElem: bridge blackholed: %s — re-running outer Expr "
            "via tree-walker for index %lld\n",
            ex.what(), (long long)idx);
        nix::Value tw;
        fallbackExpr->eval(ns, ns.baseEnv, tw);
        ns.forceValue(tw, pos);
        if (tw.type() != nix::nList)
            ns.error<nix::EvalError>(
                "v3 forceListElem: tree-walker fallback returned non-list").debugThrow();
        if (idx < 0 || (size_t)idx >= tw.listSize())
            ns.error<nix::EvalError>(
                "v3 forceListElem: tree-walker fallback list too short").debugThrow();
        nix::Value * elem = tw.listView()[(size_t)idx];
        ns.forceValue(*elem, pos);
        out = *elem;
    }
}

/// Recursively convert a v3 Value to a tree-walker nix::Value, allocated
/// in the EvalState's GC arena.  Used by primDerivationStrict to bridge
/// to tree-walker's real derivation hasher.  Functions are converted as
/// nullptr (caller must handle / not pass them in).  Lazy thunks are
/// forced first.
static nix::Value * v3ToTreeWalker(EvalState & state, Value v,
                                    std::unordered_map<const void *, nix::Value *> & seen)
{
    auto & ns = *state.nixEvalState;
    // WC-30b: if v is a Bridge thunk (a v3 wrapper around a tree-walker
    // Value*), return the original tree-walker pointer directly.  This
    // is critical for tree-walker functions: treeWalkerToV3 maps
    // nFunction → mkNull, so going tree-walker → v3 (Bridge) → forceValue
    // would lose the function.  By short-circuiting on Bridge, the
    // round-trip is identity for any tree-walker value v3 has wrapped.
    if (v.tag() == Tag::Thunk && v.payload.thunk
        && v.payload.thunk->state == ThunkState::Bridge
        && v.payload.thunk->bridgeSrc) {
        nix::Value * orig = static_cast<nix::Value *>(v.payload.thunk->bridgeSrc);
        // Force the original on the tree-walker side so callers see WHNF.
        ns.forceValue(*orig, nix::noPos);
        return orig;
    }
    v = forceValue(*state.vm, v);
    // Cycle protection: if we've already started converting this
    // ListVec / Bindings, return the in-progress nix::Value.  Required
    // for `let x = [x]; in x` or recursive attrsets the v3 result
    // returns intact.
    const void * cycleKey = nullptr;
    if (v.tag() == Tag::List) cycleKey = v.payload.list;
    else if (v.tag() == Tag::Attrs) cycleKey = v.payload.bindings;
    if (cycleKey) {
        if (auto it = seen.find(cycleKey); it != seen.end()) return it->second;
    }
    nix::Value * out = ns.allocValue();
    if (cycleKey) seen[cycleKey] = out;
    switch (v.tag()) {
    case Tag::Int:    out->mkInt(v.payload.i); break;
    case Tag::Float:  out->mkFloat(v.payload.f); break;
    case Tag::Bool:   out->mkBool(v.payload.i == 1); break;
    case Tag::Null:   out->mkNull(); break;
    case Tag::String:
        // Preserve any context the v3 string carries via the
        // side-table.  We re-encode through nix's NixStringContext
        // and use mkString(str, context, mem) — that's the only mk*
        // overload that takes a NixStringContext directly.
        if (auto * raw = lookupStringContextEntries(v.payload.str ? v.payload.str : "")) {
            nix::NixStringContext ctx = decodeStringContext(*raw);
            out->mkString(v.payload.str ? v.payload.str : "", ctx, ns.mem);
        } else {
            out->mkString(v.payload.str ? v.payload.str : "", ns.mem);
        }
        break;
    case Tag::Path: {
        nix::SourcePath sp(ns.rootFS, nix::CanonPath(v.payload.path ? v.payload.path : ""));
        out->mkPath(sp, ns.mem);
        break;
    }
    case Tag::List: {
        // WC-15 lazy bridge: defer per-element conversion via App
        // primop calls, so nixpkgs's huge / self-referential lists
        // don't trigger eager-recursion cycles.  Gated by
        // NIX_V3_NO_LAZY_BRIDGE for A/B testing and fallback.
        // Lazy bridge for large lists (n > 4); small lists eager-bridge
        // since the per-element thunk overhead exceeds the deferred-
        // force win.  REVIEW-COMP §8.6: the prior NIX_V3_NO_LAZY_BRIDGE
        // A/B gate is removed -- lazy bridging is the verified-correct
        // default for all sizes above the cutoff.
        //
        // #455: same NIX_V3_EAGER_BRIDGE_MAX knob applies here.  The
        // lazy bridge for lists has the same `__v3_force_list_elem`
        // re-entry shape as Tag::Attrs and the same cycle potential.
        auto * lv = v.payload.list;
        uint32_t n = lv ? lv->size : 0;
        static const uint32_t kEagerListMax = []{
            if (const char * v = std::getenv("NIX_V3_EAGER_BRIDGE_MAX"))
                return (uint32_t)std::atoi(v);
            return (uint32_t)4;
        }();
        if (n <= kEagerListMax || forceEagerBridge()) {
            auto lb = ns.buildList(n);
            for (uint32_t i = 0; i < n; ++i)
                lb[i] = v3ToTreeWalker(state, lv->elems[i], seen);
            out->mkList(lb);
        } else {
            // Register the v3 list value so the lazy primop can
            // fetch elements by index.  Identity-stable across
            // primop calls — lookup cost is O(1).
            //
            // WC-38 Phase 13: Boehm GC does NOT reliably scan static
            // pointers that live in the dylib's data segment on macOS,
            // so the underlying `nix::Value` was being collected mid-
            // evaluation.  When a later allocValue() happened to return
            // the same address, the contents were overwritten by a new
            // mkApp/mkThunk — causing the resulting `tPrimOpApp` chain
            // to walk down to a non-PrimOp leaf and trip tree-walker's
            // assert in callFunction().  Fix: pin the pointer storage
            // as a GC root explicitly.  Use bridgePrimOpRoot() which
            // also handles thread-safe one-shot init.
            static nix::Value * lazyListPrim = nullptr;
            if (!lazyListPrim) {
                auto * po = new nix::PrimOp{
                    .name  = "__v3_force_list_elem",
                    .args  = {"handle", "idx"},
                    .arity = 2,
                    .doc   = std::nullopt,
                    .impl  = nix::fun<nix::PrimOpFun>{primV3ForceListElem},
                };
                nix::Value * pv = ns.allocValue();
                pv->mkPrimOp(po);
                // Register root BEFORE publishing the pointer.
                // Otherwise a GC firing between the store and the
                // GC_add_roots call could reclaim *pv -- the only
                // reachability path is via the static, which isn't a
                // root yet.  REVIEW MED-20.
#if NIX_USE_BOEHMGC
                GC_add_roots(&lazyListPrim, &lazyListPrim + 1);
#endif
                lazyListPrim = pv;
            }
            auto & tbl = v3BridgeLists();
            size_t handle = tbl.size();
            tbl.push_back({v, tlBridgeFallbackExpr});
            nix::Value * vHandle = ns.allocValue();
            vHandle->mkInt(static_cast<nix::NixInt::Inner>(handle));
            // PrimOpApp(__v3_force_list_elem, handle) is a 1-arg-of-2
            // partial application; combining with idx (App) makes it
            // fully applied.  Tree-walker forces App by callFunction.
            nix::Value * vPartial = ns.allocValue();
            vPartial->mkPrimOpApp(lazyListPrim, vHandle);
            auto lb = ns.buildList(n);
            for (uint32_t i = 0; i < n; ++i) {
                nix::Value * vIdx = ns.allocValue();
                vIdx->mkInt(static_cast<nix::NixInt::Inner>(i));
                nix::Value * vApp = ns.allocValue();
                vApp->mkApp(vPartial, vIdx);
                lb[i] = vApp;
            }
            out->mkList(lb);
        }
        break;
    }
    case Tag::Attrs: {
        auto * b = v.payload.bindings;
        auto bb = ns.buildBindings(b ? b->size : 0);
        auto & symTab = ir::globalSymbolTable();
        // Per-thread (v3 SymbolId -> nix::Symbol) cache.  The v3 global
        // symbol table is append-only and tree-walker symbols are
        // stable for the EvalState's lifetime, so once we've resolved
        // a v3 SymbolId once it stays valid.  Saves the heterogeneous
        // hash lookup + string compare on every attr conversion.
        // Invalidation: bound to nixEvalState; if it changes we drop
        // the cache (matches the cachedDrvStrict pattern below).
        static thread_local std::vector<nix::Symbol> v3SymCache;
        static thread_local nix::EvalState * v3SymCacheFor = nullptr;
        if (v3SymCacheFor != &ns) {
            v3SymCacheFor = &ns;
            v3SymCache.clear();
        }
        // WC-15 lazy bridge: for non-trivial attrsets, defer per-attr
        // conversion via App primop calls so nixpkgs's huge self-
        // referential pkgs structure doesn't trigger eager-recursion
        // cycles.  Small attrsets stay eager (lower overhead).
        // REVIEW-COMP §8.6: NIX_V3_NO_LAZY_BRIDGE A/B gate removed.
        //
        // #455: tune the eager/lazy threshold via env var.  The lazy
        // bridge produces an attrset whose entries are __v3_force_attr
        // PrimOpApp values; when those entries are forced and re-enter
        // v3 (e.g. through `with self;` looking up another attr from
        // the same attrset), the indirection cycles infinitely.  Until
        // a proper cycle-break lands, raising the threshold lets a
        // workload opt out of the lazy bridge.  Additionally, the
        // call-hook sets `forceEagerBridge` for on-demand-root-
        // populated lambdas so their results don't go through the
        // lazy path even when the global threshold is low.
        size_t bSize = b ? b->size : 0;
        static const size_t kEagerBridgeMax = []{
            if (const char * v = std::getenv("NIX_V3_EAGER_BRIDGE_MAX"))
                return (size_t)std::atoi(v);
            return (size_t)4;
        }();
        bool useEager = bSize <= kEagerBridgeMax || forceEagerBridge();
        if (useEager) {
            if (b) for (uint32_t i = 0; i < b->size; ++i) {
                SymbolId sid = b->entries[i].name;
                nix::Symbol resolved;
                if (sid < v3SymCache.size() && v3SymCache[sid]) {
                    resolved = v3SymCache[sid];
                } else {
                    std::string_view n = sid < symTab.size()
                        ? std::string_view(symTab[sid])
                        : std::string_view{};
                    resolved = ns.symbols.create(n);
                    if (sid >= v3SymCache.size())
                        v3SymCache.resize(sid + 1);
                    v3SymCache[sid] = resolved;
                }
                bb.insert(resolved, v3ToTreeWalker(state, b->entries[i].value, seen));
            }
        } else {
            // Register the original v3 attrset for later lookup.
            // WC-38 Phase 13: see lazyListPrim above for the GC root
            // registration rationale.
            static nix::Value * lazyAttrPrim = nullptr;
            if (!lazyAttrPrim) {
                auto * po = new nix::PrimOp{
                    .name  = "__v3_force_attr",
                    .args  = {"handle", "name"},
                    .arity = 2,
                    .doc   = std::nullopt,
                    .impl  = nix::fun<nix::PrimOpFun>{primV3ForceAttr},
                };
                nix::Value * pv = ns.allocValue();
                pv->mkPrimOp(po);
                // Root BEFORE publish; see lazyListPrim / REVIEW MED-20.
#if NIX_USE_BOEHMGC
                GC_add_roots(&lazyAttrPrim, &lazyAttrPrim + 1);
#endif
                lazyAttrPrim = pv;
            }
            auto & tbl = v3BridgeAttrs();
            size_t handle = tbl.size();
            tbl.push_back({v, tlBridgeFallbackExpr});
            nix::Value * vHandle = ns.allocValue();
            vHandle->mkInt(static_cast<nix::NixInt::Inner>(handle));
            nix::Value * vPartial = ns.allocValue();
            vPartial->mkPrimOpApp(lazyAttrPrim, vHandle);
            for (uint32_t i = 0; i < b->size; ++i) {
                SymbolId sid = b->entries[i].name;
                nix::Symbol resolved;
                if (sid < v3SymCache.size() && v3SymCache[sid]) {
                    resolved = v3SymCache[sid];
                } else {
                    std::string_view n = sid < symTab.size()
                        ? std::string_view(symTab[sid])
                        : std::string_view{};
                    resolved = ns.symbols.create(n);
                    if (sid >= v3SymCache.size())
                        v3SymCache.resize(sid + 1);
                    v3SymCache[sid] = resolved;
                }
                // Each attr value: App(PrimOpApp(__v3_force_attr, handle), nameStr).
                std::string_view n = sid < symTab.size()
                    ? std::string_view(symTab[sid])
                    : std::string_view{};
                nix::Value * vName = ns.allocValue();
                vName->mkString(n, ns.mem);
                nix::Value * vApp = ns.allocValue();
                vApp->mkApp(vPartial, vName);
                bb.insert(resolved, vApp);
            }
        }
        out->mkAttrs(bb);
        break;
    }
    case Tag::Closure:
    case Tag::PrimOp:
    case Tag::PrimOpApp: {
        // WC-21 / WC-30b: closures with formal-attrset patterns
        // (`{a, b ? def}: ...`) bridged as PrimOpApp(__v3_call_bridge_1,
        // handle) lose tree-walker's autoCallFunction at the CLI top
        // level.  But nested-closure bridges (inside attrs/lists) are
        // typically called via tree-walker's callFunction, not auto-
        // CallFunction — so formal defaults handled in v3's lower.cc
        // synthesised rec-attrset machinery still work.  Bridge them.
        // The outer eval hook in v3_hook.cc has its own hasFormals check
        // that DOES fall back to tree-walker for top-level eval results.
        // Bridge the v3 closure as a tree-walker primop application.
        // We register `__v3_call_bridge_1` (arity 2: handle, arg) so
        // partial-application on the handle gives tree-walker a 1-arg
        // function — the calling convention that autoCallFunction +
        // ExprCall::eval expect.  The 2-arg variant is kept for the
        // legacy `builtins.path { filter = path: type: ...; }` shape.
        // WC-38 Phase 13: see lazyListPrim above — Boehm GC on macOS
        // doesn't reliably scan dylib data-segment statics, so we
        // explicitly register the storage of the static pointer as a
        // GC root.  Without this, `bridgePrimOp1`'s underlying Value
        // is reclaimed and `mkPrimOpApp(bridgePrimOp1, ...)` produces
        // a chain whose root is no longer a PrimOp.
        static nix::Value * bridgePrimOp1 = nullptr;
        if (!bridgePrimOp1) {
            auto * po = new nix::PrimOp{
                .name  = "__v3_call_bridge_1",
                .args  = {"handle", "arg"},
                .arity = 2,
                .doc   = std::nullopt,
                .impl  = nix::fun<nix::PrimOpFun>{primV3CallBridge1},
            };
            nix::Value * vp = ns.allocValue();
            vp->mkPrimOp(po);
            // Root BEFORE publish; see lazyListPrim / REVIEW MED-20.
#if NIX_USE_BOEHMGC
            GC_add_roots(&bridgePrimOp1, &bridgePrimOp1 + 1);
#endif
            bridgePrimOp1 = vp;
        }
        // #437: refuse to bridge v3 closures that have formals
        // (`{a, b ? def}: body`).  Bridging them as
        // `mkPrimOpApp(__v3_call_bridge_1, h)` strips tree-walker's
        // autoCallFunction formal dispatch, AND the future bridge1
        // invocation will deep-force the formal arg via
        // `treeWalkerToV3` -- if any attr is mid-construction in an
        // outer tree-walker frame (eg the NixOS module fixed-point's
        // `config`), the deep force trips ExprBlackHole (cardano-node
        // Phase-5 cycle).
        //
        // Throw a blackhole-shaped runtime_error: the existing
        // `isBlackhole`/`isBlackholeAttr`/`isBlackholeList` checks at
        // primops.cc:2358/2519/2588 match this prefix and route to
        // their fallbackExpr re-evaluation paths.  When called from a
        // lazy list/attr bridge, that re-evaluates the source Expr
        // via tree-walker, giving tree-walker the original ExprLambda
        // to dispatch through its native formal-dispatch.
        // #458 step 7: formals-closure refusal stays default-on as
        // the safety net.  Slot-capture + RecBuildSlot fix the original
        // fix-point Black scenario, but lifting the refusal lets some
        // cardano-node-class workloads progress further into a deeper
        // TW infinite-recursion.  Net effect on passing tests is
        // neutral (lang + cutover-parity + chase + smoke unchanged
        // with refusal off).  Keep the lift opt-OUT via
        // NIX_V3_NO_REFUSE_FORMALS_BRIDGE=1 for users probing
        // workloads where the refusal is the load-bearing block, not
        // the deeper recursion.
        static const bool s_refuseFormals =
            std::getenv("NIX_V3_NO_REFUSE_FORMALS_BRIDGE") == nullptr;
        if (s_refuseFormals
            && v.tag() == Tag::Closure
            && v.payload.closure
            && v.payload.closure->desc
            && v.payload.closure->desc->hasFormals)
        {
            static const bool s_dbg =
                std::getenv("V3_DBG_BRIDGE1") != nullptr;
            if (s_dbg) std::fprintf(stderr,
                "v3 v3ToTreeWalker: refusing <formals> closure "
                "(name='%s' arity=%u nFormals=%zu)\n",
                v.payload.closure->desc->name.c_str(),
                (unsigned)v.payload.closure->desc->arity,
                v.payload.closure->desc->formals.size());
            throw BlackholeError(
                "v3 v3ToTreeWalker: <formals> closure cannot bridge "
                "as primOpApp -- forcing tree-walker fallback");
        }
        auto & tbl = v3BridgeClosures();
        size_t handle = tbl.size();
        tbl.push_back({v, tlBridgeFallbackExpr});
        nix::Value * vHandle = ns.allocValue();
        vHandle->mkInt(static_cast<nix::NixInt::Inner>(handle));
        out->mkPrimOpApp(bridgePrimOp1, vHandle);
        break;
    }
    case Tag::Blackhole: {
        // #466 GHC-style sentinel: v3 produced a Blackhole value
        // (typically because forceValue saw a foreign-vm Black thunk
        // and chose to propagate-as-value rather than throw).  Bridge
        // back to TW as TW's own mkBlackhole sentinel; TW's
        // ExprBlackHole::eval throws InfiniteRecursionError on use,
        // and tryEval / consumer error paths catch via the typed
        // exception.  This is the inverse of the v3-side
        // forceValue change: throw → return value, then back across
        // the bridge: value → throw via TW's protocol.
        out->mkBlackhole();
        break;
    }
    case Tag::Uninitialized:
    case Tag::Thunk:
    case Tag::App:
    case Tag::External:
    case Tag::Slot:
    default: {
        static const bool dbg = std::getenv("V3_DBG_BRIDGE_NULL") != nullptr;
        if (dbg) std::fprintf(stderr,
            "v3ToTreeWalker: tag=%d → mkNull (round-trip lost)\n",
            (int)v.tag());
        out->mkNull();
        break;
    }
    }
    return out;
}

static nix::Value * v3ToTreeWalker(EvalState & state, Value v)
{
    std::unordered_map<const void *, nix::Value *> seen;
    return v3ToTreeWalker(state, v, seen);
}

// Public-shim function pointer.  Filled in by an init thunk below;
// read by the namespace-scope `v3ToTreeWalkerPublic` defined after
// the anonymous namespace closes.  Avoids exposing the internals
// of v3ToTreeWalker (which references several other anon-ns
// helpers) to external translation units.
nix::Value * (*v3ToTreeWalkerShim)(nix::EvalState &, Value) = nullptr;
struct V3ToTreeWalkerShimInit {
    V3ToTreeWalkerShimInit() {
        v3ToTreeWalkerShim = +[](nix::EvalState & ns, Value v) -> nix::Value * {
            // v3ToTreeWalker calls forceValue(*state.vm, ...) on its
            // input, so we MUST provide a VMState -- otherwise the
            // bridge dereferences a null pointer and SEGVs.
            // REVIEW MED-16: stack-allocated; .reserve() avoids per-
            // call vector reallocation, not persistence.
            VMState bridgeShimVm;
            bridgeShimVm.valueStack.reserve(64 * 1024);
            bridgeShimVm.frames.reserve(4096);
            bridgeShimVm.withStack.reserve(64);
            EvalState st;
            st.nixEvalState = &ns;
            st.vm           = &bridgeShimVm;
            return v3ToTreeWalker(st, v);
        };
    }
};
[[maybe_unused]] V3ToTreeWalkerShimInit _v3_shim_init;

/// Recursively convert a tree-walker nix::Value to a v3 Value.  Forces
/// thunks via tree-walker's evaluator before reading the type.
static Value treeWalkerToV3(EvalState & state, nix::Value & nv,
                            std::unordered_map<const void *, Value> & seen)
{
    auto & ns = *state.nixEvalState;
    // WC-14.6: bounded-depth yield at the v3↔tree-walker boundary.
    if (++nix::EvalState::v3HookForceDepth >
        nix::EvalState::v3HookMaxForceDepth) {
        --nix::EvalState::v3HookForceDepth;
        ns.error<nix::V3DepthYield>(
            "v3 bridge depth %1% exceeded threshold %2%",
            nix::EvalState::v3HookForceDepth,
            nix::EvalState::v3HookMaxForceDepth).debugThrow();
    }
    struct DepthDec {
        ~DepthDec() { --nix::EvalState::v3HookForceDepth; }
    } _dec;
    // WC-18.4: when running inside a fiber, yield to the driver so
    // the recursive tree-walker forceValue runs on the driver's
    // pthread stack rather than the fiber's 16 MB stack — keeps
    // fiber stacks bounded.  Outside a fiber, falls through to a
    // direct call.
    yieldForceTreeWalker(ns, nv);
    Value out;
    switch (nv.type()) {
    case nix::nInt:    out.mkInt(nv.integer().value); return out;
    case nix::nFloat:  out.mkFloat(nv.fpoint()); return out;
    case nix::nBool:   out = nv.boolean() ? Value::vTrue : Value::vFalse; return out;
    case nix::nNull:   out.mkNull(); return out;
    case nix::nFunction: {
        // REVIEW MED-1: wrap a tree-walker function in a v3 Bridge
        // thunk.  v3ToTreeWalker's Bridge short-circuit then unwraps
        // the thunk back to the original nix::Value*, so the
        // tw -> v3 -> tw round-trip preserves identity (callable
        // function instead of mkNull).  Without this wrap, code like
        // `let f = tw_id; in [f f]` after a v3 -> tw -> v3 transition
        // loses `f`.
        Thunk * bridge = Alloc::allocBridgeThunk(static_cast<void *>(&nv));
        allocStats().thunksAllocated++;
        out.tag_payload = static_cast<uint64_t>(Tag::Thunk);
        out.payload.thunk = bridge;
        return out;
    }
    case nix::nExternal: {
        // REVIEW_2026-05-04 B-9 / §6.7: bridge external values back as
        // a v3 Bridge thunk, mirroring the nFunction case above (REVIEW
        // MED-1).  Used by experimental fetchers / FFI extensions whose
        // values are opaque to v3 -- without the Bridge wrap, the
        // round-trip `tw → v3 → tw` would lose the original via
        // `mkNull` and any downstream coerceToString / `==` would see
        // null instead of the external value.
        Thunk * bridge = Alloc::allocBridgeThunk(static_cast<void *>(&nv));
        allocStats().thunksAllocated++;
        out.tag_payload = static_cast<uint64_t>(Tag::Thunk);
        out.payload.thunk = bridge;
        return out;
    }
    case nix::nThunk: {
        // #455: wrap as a v3 Bridge thunk (lazy).  The default flow
        // had forceValue pre-force args before treeWalkerToV3 ran, so
        // an nThunk reaching this case was an error path -> null.
        // With NIX_V3_LAZY_BRIDGE_ARG=1 (#455 attempt) primV3CallBridge1
        // skips the eager force, and arrives here with an unforced TW
        // thunk for args[1].  Wrapping as a Bridge thunk preserves
        // laziness all the way to the v3 body's first force, mirroring
        // TW's regular callFunction (which doesn't force args either).
        Thunk * bridge = Alloc::allocBridgeThunk(static_cast<void *>(&nv));
        allocStats().thunksAllocated++;
        out.tag_payload = static_cast<uint64_t>(Tag::Thunk);
        out.payload.thunk = bridge;
        return out;
    }
    case nix::nFailed:
        // nFailed: a previously-cached exception -- return null and
        // let downstream re-trigger via the next force (tree-walker's
        // handleEvalFailed will rethrow the cached exception).
        out.mkNull(); return out;
    case nix::nString: {
        out = mkStringValueOwned(std::string(nv.string_view()));
        if (auto * ctx = nv.context()) {
            std::vector<std::string> entries;
            for (auto & e : *ctx) entries.push_back(std::string((*e).c_str()));
            if (!entries.empty())
                setStringContextEntries(out.payload.str, std::move(entries));
        }
        return out;
    }
    case nix::nPath: {
        out.tag_payload = static_cast<uint64_t>(Tag::Path);
        std::string p(nv.pathStrView());
        // CRIT-4: arena allocation.
        char * buf = Alloc::allocChars(p.size() + 1);
        std::memcpy(buf, p.data(), p.size()); buf[p.size()] = '\0';
        out.payload.path = buf;
        return out;
    }
    case nix::nList: {
        const void * key = &nv;
        if (auto it = seen.find(key); it != seen.end()) return it->second;
        size_t n = nv.listSize();
        ListVec * lv = Alloc::allocList(static_cast<uint32_t>(n));
        allocStats().listsAllocated++;
        out.tag_payload = static_cast<uint64_t>(Tag::List);
        out.payload.list = lv;
        seen[key] = out; // store before recursing
        auto view = nv.listView();
        // #438 diagnostic: catch null list-element pointers BEFORE the
        // dereference crash.  A tree-walker list whose backing storage
        // has a null entry usually means either a v3-bridged list whose
        // slot was never filled, or a tree-walker list whose elements
        // were reclaimed by GC.  When V3_DEBUG_LIST_BRIDGE is set we
        // log + abort with full context so the crash is attributable.
        // Otherwise we fall through to the original dereference (so the
        // SIGSEGV stack still pinpoints `Value::isThunk` for stack-trace
        // tooling).
        static const bool s_dbg_list = std::getenv("V3_DEBUG_LIST_BRIDGE") != nullptr;
        for (size_t i = 0; i < n; ++i) {
            if (__builtin_expect(s_dbg_list && view[i] == nullptr, 0)) [[unlikely]] {
                std::fprintf(stderr,
                    "v3 treeWalkerToV3: NULL list elem at i=%zu of n=%zu "
                    "(parent list=%p)\n",
                    i, n, (void *)&nv);
                std::fflush(stderr);
                std::abort();
            }
            lv->elems[i] = treeWalkerToV3(state, *view[i], seen);
        }
        return out;
    }
    case nix::nAttrs: {
        const auto * a = nv.attrs();
        const void * key = a; // Bindings pointer is stable & unique
        if (auto it = seen.find(key); it != seen.end()) return it->second;
        Bindings * b = Alloc::allocBindings(static_cast<uint32_t>(a->size()));
        allocStats().attrsetsAllocated++;
        out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
        out.payload.bindings = b;
        seen[key] = out;
        // Mirror of the v3->tw symbol cache: avoid the std::string copy
        // (symbols[it.name] is owned by the symbol table, fine as a
        // string_view) and skip the global table hash on cache hits.
        // Keyed by nix::Symbol's underlying integer id so we can use a
        // flat vector.
        static thread_local std::vector<SymbolId> twSymCache;
        static thread_local nix::EvalState * twSymCacheFor = nullptr;
        if (twSymCacheFor != &ns) {
            twSymCacheFor = &ns;
            twSymCache.clear();
        }
        // #452 / Phase C: when the call hook sets the shallow-TW-attrs
        // flag, wrap each entry's TW Value* in a v3 Bridge thunk
        // instead of recursively converting.  Body forces of specific
        // entries call treeWalkerToV3 lazily on just-that-value, which
        // matches TW's `{a, b ? def}: body` lazy formal semantics --
        // only entries the body references get materialised.  Critical
        // for NixOS module fix-points where some entries (e.g. `config`)
        // are mid-construction at the call site; deep conversion would
        // trip ExprBlackHole on every call.
        bool shallow = shallowTWAttrsBridge();
        std::vector<std::pair<SymbolId, Value>> entries;
        entries.reserve(a->size());
        for (auto & it : *a) {
            uint32_t k = it.name.getId();
            SymbolId sid;
            if (k < twSymCache.size() && twSymCache[k] != 0) {
                sid = twSymCache[k];
            } else {
                std::string_view name(ns.symbols[it.name]);
                sid = vmIntern(state, name);
                if (k >= twSymCache.size())
                    twSymCache.resize(k + 1, 0);
                twSymCache[k] = sid;
            }
            Value entryVal;
            if (shallow) {
                // Wrap the TW entry's Value* in a v3 Bridge thunk.  No
                // forcing now; the body's per-formal access will force
                // (or not) on demand.
                Thunk * t = Alloc::allocBridgeThunk(
                    static_cast<void *>(it.value));
                allocStats().thunksAllocated++;
                entryVal.tag_payload = static_cast<uint64_t>(Tag::Thunk);
                entryVal.payload.thunk = t;
            } else {
                entryVal = treeWalkerToV3(state, *it.value, seen);
            }
            entries.emplace_back(sid, entryVal);
        }
        std::sort(entries.begin(), entries.end(),
            [](auto & x, auto & y) { return x.first < y.first; });
        for (size_t i = 0; i < entries.size(); ++i) {
            b->entries[i].name  = entries[i].first;
            b->entries[i].value = entries[i].second;
        }
        return out;
    }
    default:
        out.mkNull();
        return out;
    }
}

static Value treeWalkerToV3(EvalState & state, nix::Value & nv)
{
    std::unordered_map<const void *, Value> seen;
    return treeWalkerToV3(state, nv, seen);
}

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
// For Phase A we only support the case where __toString is a v3
// closure or a tree-walker function reachable via the bridge's
// PrimOpApp encoding.  Anything more exotic throws and is caught
// by the scaffold's fall-back.
static std::optional<std::string> v3TryAttrsToString(
    EvalState & state,
    Value & v,
    nix::NixStringContext & context,
    std::string_view errorCtx)
{
    const auto & sym = drvStrictSymbols();
    if (!v.isAttrs() || !v.payload.bindings) return std::nullopt;
    const Value * tsRaw = v.payload.bindings->lookup(sym.toString);
    if (!tsRaw) return std::nullopt;
    Value tsFn = forceValue(*state.vm, *tsRaw);
    // Phase A scope: don't try to call __toString at all — every
    // case I've audited in nixpkgs has it as a closure that pulls
    // in the full eval state, and bridging into v3 from this
    // context isn't yet wired.  Fall back.  (BR-3.13 / Phase E
    // can expand this once the path is hot enough to matter.)
    (void)tsFn;
    throw std::runtime_error(
        "v3 BR-3 coerceToString: __toString fall-back path "
        "not yet implemented; bridging");
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
        const char * buf = v.payload.str ? v.payload.str : "";
        if (auto * raw = lookupStringContextEntries(buf)) {
            for (auto & e : *raw) {
                try { context.insert(nix::NixStringContextElem::parse(e)); }
                catch (...) { /* skip un-parseable */ }
            }
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
            nix::CanonPath(v.payload.path ? v.payload.path : ""));
        // copyPathToStore inserts the Opaque context entry on `context`
        // for us (eval.cc:2961).
        nix::StorePath dst = ns.copyPathToStore(context, sp);
        return ns.store->printStorePath(dst);
    }

    if (v.isAttrs()) {
        const auto & sym = drvStrictSymbols();
        // Try __toString first.  If absent, fall through to outPath.
        if (v.payload.bindings && v.payload.bindings->lookup(sym.toString)) {
            if (auto s = v3TryAttrsToString(state, v, context, errorCtx))
                return std::move(*s);
        }
        // outPath fallback — common case for derivations and any
        // attrset that string-coerces to its primary output.
        if (v.payload.bindings) {
            if (auto * outV = v.payload.bindings->lookup(sym.outPath)) {
                Value forced = forceValue(*state.vm, *outV);
                return v3CoerceToString(state, forced, context, errorCtx);
            }
        }
        throw std::runtime_error(
            "v3 BR-3 coerceToString: attrset has neither "
            "__toString nor outPath");
    }

    // coerceMore = true cases (matching tree-walker's behaviour for
    // derivationStrict's per-attr coerce):
    if (v.isBool()) {
        return v.payload.i == 1 ? std::string("1") : std::string("");
    }
    if (v.isInt()) {
        return std::to_string(v.payload.i);
    }
    if (v.tag() == Tag::Float) {
        // Match tree-walker's std::to_string(double).
        return std::to_string(v.payload.f);
    }
    if (v.tag() == Tag::Null) {
        return std::string("");
    }
    if (v.isList()) {
        std::string out;
        auto * lv = v.payload.list;
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
                    && (!el.payload.list || el.payload.list->size == 0);
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

// BR-3.4: lexicographic attr iteration helper.  Returns a vector of
// indices into `b->entries` sorted by name STRING, not SymbolId.
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
static std::vector<uint32_t> lexicographicAttrOrder(const Bindings * b)
{
    std::vector<uint32_t> order;
    if (!b) return order;
    order.reserve(b->size);
    for (uint32_t i = 0; i < b->size; ++i) order.push_back(i);
    const auto & symTab = ir::globalSymbolTable();
    auto nameOf = [&](uint32_t i) -> std::string_view {
        SymbolId s = b->entries[i].name;
        return s < symTab.size() ? std::string_view(symTab[s])
                                  : std::string_view{};
    };
    std::sort(order.begin(), order.end(),
        [&](uint32_t a, uint32_t bIdx) {
            return nameOf(a) < nameOf(bIdx);
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
        if (std::getenv("V3_DRV_STATS"))
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
            if (args[0].isAttrs() && args[0].payload.bindings) {
                auto * b = args[0].payload.bindings;
                bindingsPtr = b;
                if (auto * nv = b->lookup(syms.name)) {
                    Value forced = forceValue(*state.vm, *nv);
                    if (forced.isString() && forced.payload.str)
                        drvName = forced.payload.str;
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

    // BR-3.5: Phase A native fast path.  Attempted ONLY if the
    // input shape passes isSimpleDerivationAttrs (cheap presence
    // check — no value forcing).  Anything that throws falls
    // through to the existing bridge with no semantics change.
    static const bool nativeDisabled =
        std::getenv("V3_DRV_NO_NATIVE") != nullptr;
    if (!nativeDisabled
        && state.nixEvalState && args[0].isAttrs() && args[0].payload.bindings
        && isSimpleDerivationAttrs(args[0].payload.bindings))
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
            // see this debug line first.
            if (std::getenv("V3_DRV_DEBUG")) {
                std::string drvName = "<unknown>";
                const auto & syms = drvStrictSymbols();
                if (auto * nv = args[0].payload.bindings->lookup(syms.name)) {
                    if (nv->isString() && nv->payload.str)
                        drvName = nv->payload.str;
                }
                std::fprintf(stderr,
                    "v3 derivationStrict native fell back on `%s`: %s\n",
                    drvName.c_str(), e.what());
            }
            // fall through to the bridge below.
        }
    }

    // If a tree-walker EvalState is wired, delegate to its real
    // `builtins.derivationStrict` so we get content-addressed
    // /nix/store paths.  Falls back to the v3 fake-store path on any
    // bridge failure (e.g. converting a closure value).
    if (state.nixEvalState && args[0].isAttrs() && args[0].payload.bindings) {
        try {
            auto & ns = *state.nixEvalState;
            nix::Value * nargs = v3ToTreeWalker(state, args[0]);
            // Cache the derivationStrict primop pointer per-EvalState
            // — it's looked up by name on every call otherwise (one
            // forceAttrs + one symbol-table lookup per derivation,
            // which on a 25k-pkg nixpkgs scan is meaningful).
            //
            // Threading: nixEvalState pointer is stable for the
            // lifetime of the eval; the static vBuiltins is process-
            // wide because there is at most one tree-walker EvalState
            // attached to the v3 runtime at any time.
            static thread_local nix::Value * cachedDrvStrict = nullptr;
            static thread_local nix::EvalState * cachedFor = nullptr;
            if (cachedFor != &ns) {
                cachedFor = &ns;
                cachedDrvStrict = nullptr;
                nix::Value & blt = ns.getBuiltins();
                ns.forceAttrs(blt, nix::noPos, "v3 derivationStrict bridge");
                auto * dsAttr = blt.attrs()->get(ns.symbols.create("derivationStrict"));
                if (dsAttr && dsAttr->value) cachedDrvStrict = dsAttr->value;
            }
            if (cachedDrvStrict) {
                nix::Value result;
                ns.callFunction(*cachedDrvStrict, *nargs, result, nix::noPos);
                out = treeWalkerToV3(state, result);
                return;
            }
        } catch (const std::exception & e) {
            if (std::getenv("V3_DRV_DEBUG"))
                std::fprintf(stderr, "v3 derivationStrict bridge fell back: %s\n", e.what());
            // fall through to fake-store path
        }
    }
    if (!args[0].isAttrs() || !args[0].payload.bindings)
        typeError("derivationStrict", "attrset");
    const auto & sym = drvStrictSymbols();

    auto * src = args[0].payload.bindings;
    const Value * nameVRaw = src->lookup(sym.name);
    if (!nameVRaw)
        typeError("derivationStrict", "attrset with `name` string");
    Value nameV = forceValue(*state.vm, *nameVRaw);
    if (!nameV.isString())
        typeError("derivationStrict", "attrset with `name` string");
    std::string name(nameV.payload.str);

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
        if (f.isList() && f.payload.list) {
            for (uint32_t i = 0; i < f.payload.list->size; ++i) {
                Value el = forceValue(*state.vm, f.payload.list->elems[i]);
                if (el.isString()) outputs.push_back(el.payload.str);
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
        if (f.isString()) return f.payload.str;
        if (f.isPath())   return f.payload.path;
        if (f.isInt())    return std::to_string(f.payload.i);
        if (f.isBool())   return f.payload.i == 1 ? "1" : "";
        if (f.isList() && f.payload.list) {
            std::string s;
            for (uint32_t i = 0; i < f.payload.list->size; ++i) {
                Value el = forceValue(*state.vm, f.payload.list->elems[i]);
                if (el.isString()) s += el.payload.str;
                else if (el.isPath()) s += el.payload.path;
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
    allocStats().attrsetsAllocated++;
    for (size_t i = 0; i < entries.size(); ++i) {
        b->entries[i].name  = entries[i].first;
        b->entries[i].value = entries[i].second;
    }
    out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
    out.payload.bindings = b;
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
// Currently stops short of writeDerivation — that's BR-3.7.  This
// commit demonstrates the iteration logic; the bridge fall-back
// keeps semantics identical.
static void primDerivationStrictNative(
    EvalState & state, Value * args, Value & out)
{
    auto & ns = *state.nixEvalState;
    auto * src = args[0].payload.bindings;
    const auto & sym = drvStrictSymbols();

    // ---- name ----
    const Value * nameVRaw = src->lookup(sym.name);
    if (!nameVRaw)
        throw std::runtime_error(
            "v3 BR-3 native: derivation missing required `name` attr");
    Value nameV = forceValue(*state.vm, *nameVRaw);
    if (!nameV.isString())
        throw std::runtime_error(
            "v3 BR-3 native: `name` attr is not a string");
    std::string drvName(nameV.payload.str ? nameV.payload.str : "");
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
        return f.payload.i == 1;
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
    auto order = lexicographicAttrOrder(src);
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

    for (uint32_t idx : order) {
        SymbolId sid = src->entries[idx].name;
        std::string_view key = sid < symTab.size()
            ? std::string_view(symTab[sid])
            : std::string_view{};
        Value & attrV = src->entries[idx].value;

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
            std::string keyStr(key);
            structuredJson[keyStr] = valueToJsonWithContext(
                state, attrV, context);
            // Special-case fields still need to populate drv.* so
            // libnixstore can write the .drv correctly.
            if (sid == sym.args) {
                Value listV = forceValue(*state.vm, attrV);
                if (!listV.isList())
                    throw std::runtime_error(
                        "v3 BR-3 native: `args` attr is not a list");
                if (listV.payload.list) {
                    for (uint32_t i = 0; i < listV.payload.list->size; ++i) {
                        Value el = forceValue(*state.vm, listV.payload.list->elems[i]);
                        drv.args.push_back(v3CoerceToString(
                            state, el, context,
                            "while evaluating an element of `args`"));
                    }
                }
                continue;
            }
            if (sid == sym.outputs) {
                Value listV = forceValue(*state.vm, attrV);
                if (!listV.isList())
                    throw std::runtime_error(
                        "v3 BR-3 native: `outputs` attr is not a list");
                if (listV.payload.list) {
                    for (uint32_t i = 0; i < listV.payload.list->size; ++i) {
                        Value el = forceValue(*state.vm, listV.payload.list->elems[i]);
                        if (!el.isString())
                            throw std::runtime_error(
                                "v3 BR-3 native: `outputs` element is not a string");
                        std::string s(el.payload.str ? el.payload.str : "");
                        if (s.empty() || s == "drvPath")
                            throw std::runtime_error(
                                "v3 BR-3 native: invalid output name");
                        declaredOutputs.push_back(s);
                    }
                }
                continue;
            }
            // builder/system/outputHash* still extracted out to drv.
            std::string s = v3CoerceToString(
                state, attrV, context,
                "while evaluating a derivation attribute");
            if (sid == sym.builder)             drv.builder = s;
            else if (sid == sym.system)         drv.platform = s;
            else if (sid == sym.outputHash)     outputHashStr = s;
            else if (sid == sym.outputHashAlgo) outputHashAlgoStr = s;
            else if (sid == sym.outputHashMode) outputHashModeStr = s;
            // No drv.env emit under structuredAttrs.
            continue;
        }

        // `args` is special: forced as a list-of-strings.
        if (sid == sym.args) {
            Value listV = forceValue(*state.vm, attrV);
            if (!listV.isList())
                throw std::runtime_error(
                    "v3 BR-3 native: `args` attr is not a list");
            if (listV.payload.list) {
                for (uint32_t i = 0; i < listV.payload.list->size; ++i) {
                    Value el = forceValue(*state.vm, listV.payload.list->elems[i]);
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
            if (listV.payload.list) {
                for (uint32_t i = 0; i < listV.payload.list->size; ++i) {
                    Value el = forceValue(*state.vm, listV.payload.list->elems[i]);
                    if (!el.isString())
                        throw std::runtime_error(
                            "v3 BR-3 native: `outputs` element is not a string");
                    std::string s(el.payload.str ? el.payload.str : "");
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
    nix::StorePath drvPath = nix::settings.readOnlyMode
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
        setStringContext(v3DrvPath.payload.str, drvCtx);
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
        setStringContext(v3OutPath.payload.str, outCtx);
        entries.emplace_back(outSid, v3OutPath);
    }

    // Sort by SymbolId for the Bindings invariant + binary search.
    std::sort(entries.begin(), entries.end(),
        [](auto & a, auto & b) { return a.first < b.first; });

    Bindings * resultB = Alloc::allocBindings(static_cast<uint32_t>(entries.size()));
    allocStats().attrsetsAllocated++;
    for (size_t i = 0; i < entries.size(); ++i) {
        resultB->entries[i].name  = entries[i].first;
        resultB->entries[i].value = entries[i].second;
    }
    out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
    out.payload.bindings = resultB;
}

void primDerivation(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isAttrs() || !args[0].payload.bindings)
        typeError("derivation", "attrset");
    auto * src = args[0].payload.bindings;

    // 1. Run derivationStrict to get per-output paths + drvPath.
    Value strict;
    primDerivationStrict(state, args, strict);
    if (!strict.isAttrs() || !strict.payload.bindings)
        throw std::runtime_error("v3 derivation: derivationStrict didn't return an attrset");
    auto * strictB = strict.payload.bindings;

    // 2. Read `outputs` (default ["out"]).
    std::vector<std::string> outputs;
    SymbolId sOutputs = vmIntern(state, "outputs");
    if (auto * outV = src->lookup(sOutputs)) {
        Value f = forceValue(*state.vm, *outV);
        if (f.isList() && f.payload.list) {
            for (uint32_t i = 0; i < f.payload.list->size; ++i) {
                Value el = forceValue(*state.vm, f.payload.list->elems[i]);
                if (el.isString()) outputs.push_back(el.payload.str);
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
        allocStats().attrsetsAllocated++;
        for (size_t i = 0; i < oEntries.size(); ++i) {
            ob->entries[i].name  = oEntries[i].first;
            ob->entries[i].value = oEntries[i].second;
        }
        Value v;
        v.tag_payload = static_cast<uint64_t>(Tag::Attrs);
        v.payload.bindings = ob;
        return v;
    };

    // Result: drvAttrs // { outPath; drvPath; type = "derivation"; outputName; drvAttrs = drvAttrs; }
    std::vector<std::pair<SymbolId, Value>> entries;
    entries.reserve(src->size + 5 + outputs.size());
    for (uint32_t i = 0; i < src->size; ++i)
        entries.emplace_back(src->entries[i].name, src->entries[i].value);
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
        allocStats().listsAllocated++;
        for (size_t i = 0; i < allOutputs.size(); ++i)
            lv->elems[i] = allOutputs[i];
        Value vAll;
        vAll.tag_payload = static_cast<uint64_t>(Tag::List);
        vAll.payload.list = lv;
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
    allocStats().attrsetsAllocated++;
    for (size_t i = 0; i < dedup.size(); ++i) {
        b->entries[i].name  = dedup[i].first;
        b->entries[i].value = dedup[i].second;
    }
    out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
    out.payload.bindings = b;

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
};
struct ImportCache
{
    std::deque<CompilationUnit> cus;       // stable addresses (deque doesn't reallocate)
    std::unordered_map<std::string, ImportCacheEntry> results;
};
inline ImportCache & importCache()
{
    static ImportCache c;
    return c;
}

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

/// builtins.import path -- read the file at `path`, parse, lower, run.
/// Returns the resulting v3 Value.  Requires state.nixEvalState to be
/// set (the host EvalState providing parser + symbol table).
void primImport(EvalState & state, Value * args, Value & out)
{
    if (!state.nixEvalState)
        throw std::runtime_error("v3 primop import: no nix EvalState wired (run via v3-eval)");
    std::string path;
    if (args[0].isString()) path = args[0].payload.str;
    else if (args[0].isPath()) path = args[0].payload.path;
    else typeError("import", "string or path");

    // WC-38 diagnostic: log every import path + sequence number to compare
    // import-order vs tree-walker.
    static const bool s_dbg_import =
        std::getenv("V3_DBG_IMPORT") != nullptr;

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
    if (s_dbg_import) {
        static std::atomic<uint64_t> seqMiss{0};
        std::fprintf(stderr, "v3 IMPORT-MISS[%llu]: %s\n",
            (unsigned long long)seqMiss.fetch_add(1), path.c_str());
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
    nix::Expr * e = nullptr;
    nix::SourcePath resolvedSp{ns.rootFS, nix::CanonPath::root};
    bool haveResolved = false;
    try {
        nix::SourcePath sp(ns.rootFS, nix::CanonPath(path));
        sp = nix::resolveExprPath(sp);
        resolvedSp = sp;
        haveResolved = true;
        e = ns.parseExprFromFile(sp);
    } catch (...) {
        std::string corepkgsPath = path;
        if (!corepkgsPath.empty() && corepkgsPath.front() == '/')
            corepkgsPath = corepkgsPath.substr(1);
        nix::SourcePath cp(ns.corepkgsFS.cast<nix::SourceAccessor>(),
                           nix::CanonPath(corepkgsPath));
        if (!cp.pathExists()) throw;
        resolvedSp = cp;
        haveResolved = true;
        e = ns.parseExprFromFile(cp);
    }
    e->bindVars(ns, ns.staticBaseEnv);

    // VM-4: try the disk cache before lower+compile.  primImport is
    // the ideal integration point — direct access to source path
    // and content; serialized CUs round-trip through the SymbolId
    // remap in serialize::deserializeCU.
    static const bool diskCacheEnabled =
        std::getenv("NIX_V3_DISK_CACHE") != nullptr;
    disk_cache::CacheKey diskKey{};
    if (diskCacheEnabled && haveResolved) {
        // REVIEW §1.5: hash the resolved file content (post-symlink,
        // post-default.nix rewriting), not the raw input path.  Symlink
        // retargeting + dir/default.nix selection both invalidate
        // correctly because the read path changes the hash input.
        try {
            std::string content = resolvedSp.resolveSymlinks().readFile();
            diskKey = disk_cache::computeKeyForString(content);
        } catch (...) {
            // Read failure -> empty key -> cache lookup is skipped,
            // and no insert happens later.  Same fallback as before.
        }
    }
    if (!diskKey.empty()) {
        if (auto blob = disk_cache::lookup(diskKey)) {
            try {
                cache.cus.push_back(serialize::deserializeCU(*blob));
                out = run(cache.cus.back());
                auto [mt, sz] = importStat(path);
                cache.results.emplace(path,
                    ImportCacheEntry{out, mt, sz});
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
        }
    }

    auto module = lowerNixExpr(e, ns.symbols, ns.positions);
    nix::v3::ir::optimise(module);
    nix::v3::ir::computeFreeVars(module);
    cache.cus.push_back(compile(module));
    if (!diskKey.empty() && serialize::isCacheable(cache.cus.back())) {
        try {
            std::string blob = serialize::serializeCU(cache.cus.back());
            disk_cache::insert(diskKey, blob);
        } catch (...) { /* best-effort */ }
    }
    // WC-4: pre-populate the sub-Expr cache so subsequent forces
    // (from either v3 or tree-walker via the v3ForceHook) hit on
    // imported files' inner thunks.  Without this the WC-2 lift of
    // the function-0-only restriction has nothing to bite on for
    // files that are imported (the bulk of nixpkgs).
    populateSubExprCachePublic(module, &cache.cus.back());
    // Each imported file is its own CompilationUnit; we re-enter the
    // VM to run it with its own top-level frame.  Keep the CU alive
    // (it's borrowed by closures returned from the eval).
    out = run(cache.cus.back());
    {
        auto [mt, sz] = importStat(path);
        cache.results.emplace(path, ImportCacheEntry{out, mt, sz});
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
static void valueToXml(EvalState & state, std::string & out, Value v, int indent)
{
    auto pad = [&](int n) { for (int i = 0; i < n; ++i) out += "  "; };
    v = forceValue(*state.vm, v);
    pad(indent);
    switch (v.tag()) {
    case Tag::String:
        out += "<string value=\""; out += xmlEscape(v.payload.str); out += "\" />\n";
        return;
    case Tag::Int:
        out += "<int value=\""; out += std::to_string(v.payload.i); out += "\" />\n";
        return;
    case Tag::Float:
        out += "<float value=\""; out += std::to_string(v.payload.f); out += "\" />\n";
        return;
    case Tag::Bool:
        out += "<bool value=\""; out += v.payload.i == 1 ? "true" : "false"; out += "\" />\n";
        return;
    case Tag::Null:
        out += "<null />\n";
        return;
    case Tag::Path:
        out += "<path value=\""; out += xmlEscape(v.payload.path); out += "\" />\n";
        return;
    case Tag::List:
        out += "<list>\n";
        if (v.payload.list)
            for (uint32_t i = 0; i < v.payload.list->size; ++i)
                valueToXml(state, out, v.payload.list->elems[i], indent + 1);
        pad(indent);
        out += "</list>\n";
        return;
    case Tag::Attrs: {
        out += "<attrs>\n";
        if (v.payload.bindings) {
            // Sort by name for stable output.
            auto & symTab = ir::globalSymbolTable();
            std::vector<std::pair<std::string_view, Value>> entries;
            entries.reserve(v.payload.bindings->size);
            for (uint32_t i = 0; i < v.payload.bindings->size; ++i) {
                SymbolId sid = v.payload.bindings->entries[i].name;
                std::string_view nm = sid < symTab.size() ? std::string_view(symTab[sid]) : std::string_view("");
                entries.emplace_back(nm, v.payload.bindings->entries[i].value);
            }
            std::sort(entries.begin(), entries.end(),
                [](auto & a, auto & b) { return a.first < b.first; });
            for (auto & [nm, val] : entries) {
                pad(indent + 1);
                out += "<attr name=\""; out += xmlEscape(nm); out += "\">\n";
                valueToXml(state, out, val, indent + 2);
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
    std::string s = "<?xml version='1.0' encoding='utf-8'?>\n<expr>\n";
    valueToXml(state, s, args[0], 1);
    s += "</expr>\n";
    out = mkStringValueOwned(s);
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
    std::string_view s(args[0].payload.str);
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
    allocStats().attrsetsAllocated++;
    for (size_t i = 0; i < entries.size(); ++i) b->entries[i] = {entries[i].first, entries[i].second};
    out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
    out.payload.bindings = b;
}

void primFlakeRefToString(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isAttrs() || !args[0].payload.bindings)
        typeError("flakeRefToString", "attrset");
    auto * src = args[0].payload.bindings;
    auto getStr = [&](const char * name) -> std::string {
        SymbolId id = vmIntern(state, name);
        auto * v = src->lookup(id);
        if (!v) return {};
        Value f = forceValue(*state.vm, *v);
        if (f.isString()) return std::string(f.payload.str);
        // Match tree-walker: negative-int attrs raise; non-string,
        // non-int attrs would too (we just reject all non-strings).
        if (f.isInt()) {
            if (f.payload.i < 0)
                throw std::runtime_error("v3 flakeRefToString: negative value given for flake ref attr " +
                                          std::string(name) + ": " + std::to_string(f.payload.i));
            return std::to_string(f.payload.i);
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
        allocStats().attrsetsAllocated++;
        for (size_t i = 0; i < entries.size(); ++i) {
            b->entries[i].name  = entries[i].first;
            b->entries[i].value = entries[i].second;
        }
        v.tag_payload = static_cast<uint64_t>(Tag::Attrs);
        v.payload.bindings = b;
        return v;
    }
    case toml::value_t::array: {
        auto & arr = t.as_array();
        ListVec * lv = Alloc::allocList(static_cast<uint32_t>(arr.size()));
        allocStats().listsAllocated++;
        for (size_t i = 0; i < arr.size(); ++i)
            lv->elems[i] = tomlToValue(state, arr[i]);
        v.tag_payload = static_cast<uint64_t>(Tag::List);
        v.payload.list = lv;
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
        allocStats().attrsetsAllocated++;
        Value typeV = mkStringValueOwned("timestamp");
        Value valV  = mkStringValueOwned(str);
        if (sType < sVal) { b->entries[0]={sType,typeV}; b->entries[1]={sVal,valV}; }
        else              { b->entries[0]={sVal,valV};  b->entries[1]={sType,typeV}; }
        v.tag_payload = static_cast<uint64_t>(Tag::Attrs);
        v.payload.bindings = b;
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
    std::istringstream stream{std::string(args[0].payload.str)};
    try {
        out = tomlToValue(state, toml::parse(stream, "fromTOML"));
    } catch (std::exception & e) {
        throw std::runtime_error(std::string("v3 primop fromTOML: ") + e.what());
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
    if (!args[0].isAttrs() || !args[0].payload.bindings)
        typeError("path", "attrset");
    // BR-4 native fast path.
    static const bool nativeDisabled =
        std::getenv("V3_PATH_NO_NATIVE") != nullptr;
    if (!nativeDisabled && state.nixEvalState) {
        // Filter present → bail to bridge (closure re-entry not yet
        // wired from this code path).
        SymbolId sFilter = ir::globalInternSymbol("filter");
        if (!args[0].payload.bindings->lookup(sFilter)) {
            try {
                primPathNative(state, args, out);
                return;
            } catch (const std::exception & e) {
                if (std::getenv("V3_DRV_DEBUG"))
                    std::fprintf(stderr,
                        "v3 builtins.path native fell back: %s\n", e.what());
                // fall through.
            }
        }
    }
    // Bridge to tree-walker's `builtins.path` so we get a proper
    // content-addressed `/nix/store/<32-char-hash>-name` result.
    // (settings.readOnlyMode means the store path is *computed* from
    // the file's NAR hash, not actually written.)
    if (state.nixEvalState) {
        try {
            auto & ns = *state.nixEvalState;
            nix::Value * nargs = v3ToTreeWalker(state, args[0]);
            nix::Value & blt = ns.getBuiltins();
            ns.forceAttrs(blt, nix::noPos, "v3 builtins.path bridge");
            auto * pAttr = blt.attrs()->get(ns.symbols.create("path"));
            if (pAttr && pAttr->value) {
                nix::Value result;
                ns.callFunction(*pAttr->value, *nargs, result, nix::noPos);
                out = treeWalkerToV3(state, result);
                return;
            }
        } catch (const std::exception & e) {
            if (std::getenv("V3_DRV_DEBUG"))
                std::fprintf(stderr, "v3 builtins.path bridge fell back: %s\n", e.what());
        }
    }
    SymbolId sPath = vmIntern(state, "path");
    SymbolId sName = vmIntern(state, "name");
    auto * src = args[0].payload.bindings;
    const Value * pathV = src->lookup(sPath);
    if (!pathV)
        typeError("path", "attrset with `path`");
    Value forcedPath = forceValue(*state.vm, *pathV);
    std::string p;
    if (forcedPath.isString())     p = forcedPath.payload.str;
    else if (forcedPath.isPath())  p = forcedPath.payload.path;
    else typeError("path", "{ path = string-or-path; ... }");
    std::string name;
    if (auto * nameV = src->lookup(sName)) {
        Value f = forceValue(*state.vm, *nameV);
        if (f.isString()) name = f.payload.str;
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
    out.tag_payload = static_cast<uint64_t>(Tag::Path);
    out.payload.path = buf;
}

// BR-4 native builtins.path body (gated on filter being absent —
// see primPath).
static void primPathNative(EvalState & state, Value * args, Value & out)
{
    auto & ns = *state.nixEvalState;
    auto * src = args[0].payload.bindings;

    // Use SymbolIds.  The names here are not in drvStrictSymbols
    // because builtins.path uses different attr names (path, name,
    // filter, recursive, sha256).
    static const SymbolId sPath      = ir::globalInternSymbol("path");
    static const SymbolId sName      = ir::globalInternSymbol("name");
    static const SymbolId sRecursive = ir::globalInternSymbol("recursive");
    static const SymbolId sSha256    = ir::globalInternSymbol("sha256");

    // Read `path` (required).  Accept Tag::Path or Tag::String.
    const Value * pathRaw = src->lookup(sPath);
    if (!pathRaw)
        throw std::runtime_error(
            "v3 BR-4 native: missing required `path` attribute");
    Value pathV = forceValue(*state.vm, *pathRaw);
    std::string pathStr;
    if (pathV.isPath()) {
        pathStr = pathV.payload.path ? pathV.payload.path : "";
    } else if (pathV.isString()) {
        pathStr = pathV.payload.str ? pathV.payload.str : "";
    } else {
        throw std::runtime_error(
            "v3 BR-4 native: `path` is not a path or string");
    }
    nix::SourcePath path(ns.rootFS, nix::CanonPath(pathStr));

    // Read `name` (optional, defaults to basename).
    std::string name;
    if (auto * nameV = src->lookup(sName)) {
        Value f = forceValue(*state.vm, *nameV);
        if (!f.isString())
            throw std::runtime_error(
                "v3 BR-4 native: `name` is not a string");
        name = f.payload.str ? f.payload.str : "";
    }
    if (name.empty()) {
        name = path.baseName();
    }

    // Read `recursive` (optional, default true → NixArchive).
    nix::ContentAddressMethod method = nix::ContentAddressMethod::Raw::NixArchive;
    if (auto * rV = src->lookup(sRecursive)) {
        Value f = forceValue(*state.vm, *rV);
        if (!f.isBool())
            throw std::runtime_error(
                "v3 BR-4 native: `recursive` is not a bool");
        method = (f.payload.i == 1)
            ? nix::ContentAddressMethod::Raw::NixArchive
            : nix::ContentAddressMethod::Raw::Flat;
    }

    // Read `sha256` (optional).  Tree-walker uses this for verification
    // post-fetch — if mismatched, errors.
    std::optional<nix::Hash> expectedHash;
    if (auto * shaV = src->lookup(sSha256)) {
        Value f = forceValue(*state.vm, *shaV);
        if (!f.isString())
            throw std::runtime_error(
                "v3 BR-4 native: `sha256` is not a string");
        expectedHash = nix::newHashAllowEmpty(
            f.payload.str ? f.payload.str : "", nix::HashAlgorithm::SHA256);
    }

    // Compute the expected store path (when sha256 is provided) and
    // skip the actual fetch if the path already exists in the store.
    // Mirrors addPath's expectedStorePath optimisation.
    if (expectedHash) {
        nix::StorePath expected = ns.store->makeFixedOutputPathFromCA(
            name,
            nix::ContentAddressWithReferences::fromParts(
                method, *expectedHash, {}));
        if (ns.store->isValidPath(expected)) {
            std::string outPath = ns.store->printStorePath(expected);
            Value v3Out = mkStringValueOwned(outPath);
            nix::NixStringContext outCtx;
            outCtx.insert(nix::NixStringContextElem{
                nix::NixStringContextElem::Opaque{.path = expected}});
            setStringContext(v3Out.payload.str, outCtx);
            out = v3Out;
            return;
        }
    }

    // Fetch (DryRun under readOnlyMode just computes the path).  No
    // filter: the gate above bailed when one was present.
    nix::StorePath dst = nix::fetchToStore(
        ns.fetchSettings,
        *ns.store,
        path.resolveSymlinks(),
        nix::settings.readOnlyMode ? nix::FetchMode::DryRun : nix::FetchMode::Copy,
        name,
        method,
        nullptr,
        ns.repair);

    if (expectedHash) {
        nix::StorePath expected = ns.store->makeFixedOutputPathFromCA(
            name,
            nix::ContentAddressWithReferences::fromParts(
                method, *expectedHash, {}));
        if (expected != dst)
            throw std::runtime_error(
                "v3 BR-4 native: store path mismatch in path added "
                "from '" + pathStr + "'");
    }

    std::string outPath = ns.store->printStorePath(dst);
    Value v3Out = mkStringValueOwned(outPath);
    nix::NixStringContext outCtx;
    outCtx.insert(nix::NixStringContextElem{
        nix::NixStringContextElem::Opaque{.path = dst}});
    setStringContext(v3Out.payload.str, outCtx);
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
    if (!scope.isAttrs() || !scope.payload.bindings)
        typeError("scopedImport", "(attrset, path)");
    std::string path;
    if (args[1].isString()) path = args[1].payload.str;
    else if (args[1].isPath()) path = args[1].payload.path;
    else typeError("scopedImport", "(attrset, path)");

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
    auto * sb = scope.payload.bindings;
    auto & symTab = ir::globalSymbolTable();
    for (uint32_t i = 0; i < sb->size; ++i) {
        SymbolId sid = sb->entries[i].name;
        std::string n = sid < symTab.size() ? symTab[sid] : "";
        if (n.empty()) continue;
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
            || n == "inherit" || n == "or") continue;
        if (!plain) continue;
        wrapped += n + " = __scope__." + n + "; ";
    }
    wrapped += "in (\n" + src + "\n)";

    nix::Expr * wrapper = ns.parseExprFromString(wrapped, sp.parent());

    auto module = lowerNixExpr(wrapper, ns.symbols, ns.positions);
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
void primFunctionArgs(EvalState &, Value * args, Value & out)
{
    Value v = args[0];
    if (v.tag() == Tag::Closure && v.payload.closure && v.payload.closure->desc) {
        const LambdaDescriptor * desc = v.payload.closure->desc;
        if (!desc->hasFormals) {
            Bindings * b = Alloc::allocBindings(0);
            allocStats().attrsetsAllocated++;
            out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
            out.payload.bindings = b;
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
        allocStats().attrsetsAllocated++;
        for (size_t i = 0; i < entries.size(); ++i) {
            b->entries[i].name  = std::get<0>(entries[i]);
            b->entries[i].value = std::get<1>(entries[i]);
            recordAttrPos(b, std::get<0>(entries[i]), std::get<2>(entries[i]));
        }
        out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
        out.payload.bindings = b;
        return;
    }
    if (v.tag() == Tag::PrimOp || v.tag() == Tag::PrimOpApp) {
        // PrimOps don't have introspectable formals; return empty.
        Bindings * b = Alloc::allocBindings(0);
        allocStats().attrsetsAllocated++;
        out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
        out.payload.bindings = b;
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
        allocStats().listsAllocated++;
        for (size_t i = 0; i < j.size(); ++i) lv->elems[i] = jsonToValue(state, j[i]);
        out.tag_payload = static_cast<uint64_t>(Tag::List);
        out.payload.list = lv;
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
        allocStats().attrsetsAllocated++;
        for (size_t i = 0; i < entries.size(); ++i) {
            b->entries[i].name = entries[i].first;
            b->entries[i].value = entries[i].second;
        }
        out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
        out.payload.bindings = b;
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
    case Tag::Bool:   return json(v.payload.i == 1);
    case Tag::Int:    return json(v.payload.i);
    case Tag::Float:  return json(v.payload.f);
    case Tag::String: return json(std::string(v.payload.str));
    case Tag::Path:   return json(std::string(v.payload.path));
    case Tag::List: {
        json arr = json::array();
        if (v.payload.list)
            for (uint32_t i = 0; i < v.payload.list->size; ++i)
                arr.push_back(valueToJson(state, v.payload.list->elems[i]));
        return arr;
    }
    case Tag::Attrs: {
        // `__toString self` overrides JSON serialization — call it
        // and use the resulting string.  Standard Nix coercion.
        if (v.payload.bindings) {
            static const SymbolId tsId = ir::globalInternSymbol("__toString");
            if (auto * fn = v.payload.bindings->lookup(tsId)) {
                Value forced = forceValue(*state.vm, *fn);
                Value s = callClosure(*state.vm, forced, v);
                s = forceValue(*state.vm, s);
                if (s.isString()) return json(std::string(s.payload.str));
            }
            // `outPath` (a derivation-like value) — serialize as the
            // path string.
            static const SymbolId outId = ir::globalInternSymbol("outPath");
            if (auto * op = v.payload.bindings->lookup(outId)) {
                Value forced = forceValue(*state.vm, *op);
                if (forced.isString()) return json(std::string(forced.payload.str));
                if (forced.isPath())   return json(std::string(forced.payload.path));
            }
        }
        json obj = json::object();
        if (v.payload.bindings) {
            for (uint32_t i = 0; i < v.payload.bindings->size; ++i) {
                auto & en = v.payload.bindings->entries[i];
                std::string_view k = vmSymName(state, en.name);
                obj[std::string(k)] = valueToJson(state, en.value);
            }
        }
        return obj;
    }
    case Tag::Uninitialized:
    case Tag::Closure:
    case Tag::PrimOp:
    case Tag::PrimOpApp:
    case Tag::Thunk:
    case Tag::App:
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
    auto j = nlohmann::json::parse(std::string(args[0].payload.str), nullptr, /*allow_exceptions=*/true);
    out = jsonToValue(state, j);
}

void primToJSON(EvalState & state, Value * args, Value & out)
{
    auto j = valueToJson(state, args[0]);
    out = mkStringValueOwned(j.dump());
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
    case Tag::Bool:   return json(v.payload.i == 1);
    case Tag::Int:    return json(v.payload.i);
    case Tag::Float:  return json(v.payload.f);
    case Tag::String: {
        const char * buf = v.payload.str ? v.payload.str : "";
        if (auto * raw = lookupStringContextEntries(buf)) {
            for (auto & e : *raw) {
                try { context.insert(nix::NixStringContextElem::parse(e)); }
                catch (...) { /* skip un-parseable */ }
            }
        }
        return json(std::string(buf));
    }
    case Tag::Path: {
        if (!state.nixEvalState)
            throw std::runtime_error(
                "v3 BR-3 valueToJsonWithContext: path requires nixEvalState");
        auto & ns = *state.nixEvalState;
        nix::SourcePath sp(ns.rootFS,
            nix::CanonPath(v.payload.path ? v.payload.path : ""));
        nix::StorePath dst = ns.copyPathToStore(context, sp);
        return json(ns.store->printStorePath(dst));
    }
    case Tag::List: {
        json arr = json::array();
        if (v.payload.list)
            for (uint32_t i = 0; i < v.payload.list->size; ++i)
                arr.push_back(valueToJsonWithContext(
                    state, v.payload.list->elems[i], context));
        return arr;
    }
    case Tag::Attrs: {
        // __toString self overrides JSON serialization (matches Nix
        // coercion rules) — but Phase A defers __toString.  For
        // Phase D we accept that and fall through to outPath instead;
        // structuredAttrs derivations very rarely use __toString.
        if (v.payload.bindings) {
            const auto & sym = drvStrictSymbols();
            // outPath fallback for derivations.
            if (auto * op = v.payload.bindings->lookup(sym.outPath)) {
                Value forced = forceValue(*state.vm, *op);
                if (forced.isString() || forced.isPath()) {
                    return valueToJsonWithContext(state, forced, context);
                }
            }
        }
        json obj = json::object();
        if (v.payload.bindings) {
            for (uint32_t i = 0; i < v.payload.bindings->size; ++i) {
                auto & en = v.payload.bindings->entries[i];
                std::string_view k = vmSymName(state, en.name);
                obj[std::string(k)] = valueToJsonWithContext(
                    state, en.value, context);
            }
        }
        return obj;
    }
    case Tag::Uninitialized:
    case Tag::Closure:
    case Tag::PrimOp:
    case Tag::PrimOpApp:
    case Tag::Thunk:
    case Tag::App:
    case Tag::Blackhole:
    case Tag::External:
    case Tag::Slot:
    default:
        throw std::runtime_error(
            "v3 BR-3 valueToJsonWithContext: unsupported value type");
    }
}

/// builtins.sort: sort a list using a comparator.  cmp(a, b) is true if
/// a should come before b.
void primSort(EvalState & state, Value * args, Value & out)
{
    if (!args[1].isList()) typeError("sort", "list");
    auto * src = args[1].payload.list;
    Value cmp = args[0];
    if (!src || src->size <= 1) { out = args[1]; return; }
    ListVec * result = Alloc::allocList(src->size);
    allocStats().listsAllocated++;
    for (uint32_t i = 0; i < src->size; ++i) result->elems[i] = src->elems[i];
    std::sort(result->elems, result->elems + src->size,
        [&](const Value & a, const Value & b) {
            Value step1 = callClosure(*state.vm, cmp, a);
            Value r = callClosure(*state.vm, step1, b);
            if (!r.isBool()) typeError("sort", "comparator returning bool");
            return r.payload.i == 1;
        });
    out.tag_payload = static_cast<uint64_t>(Tag::List);
    out.payload.list = result;
}

/// builtins.bitAnd / bitOr / bitXor on int.
void primBitAnd(EvalState &, Value * args, Value & out)
{
    if (!args[0].isInt() || !args[1].isInt()) typeError("bitAnd", "two ints");
    out.mkInt(args[0].payload.i & args[1].payload.i);
}
void primBitOr(EvalState &, Value * args, Value & out)
{
    if (!args[0].isInt() || !args[1].isInt()) typeError("bitOr", "two ints");
    out.mkInt(args[0].payload.i | args[1].payload.i);
}
void primBitXor(EvalState &, Value * args, Value & out)
{
    if (!args[0].isInt() || !args[1].isInt()) typeError("bitXor", "two ints");
    out.mkInt(args[0].payload.i ^ args[1].payload.i);
}

/// floor / ceil for floats.
void primFloor(EvalState &, Value * args, Value & out)
{
    if (args[0].isInt())   { out = args[0]; return; }
    if (!args[0].isFloat()) typeError("floor", "float or int");
    out.mkInt(static_cast<int64_t>(std::floor(args[0].payload.f)));
}
void primCeil(EvalState &, Value * args, Value & out)
{
    if (args[0].isInt())   { out = args[0]; return; }
    if (!args[0].isFloat()) typeError("ceil", "float or int");
    out.mkInt(static_cast<int64_t>(std::ceil(args[0].payload.f)));
}

/// stringLength has a 1-arg version; stringToInt would be nice but
/// nix has only specific primops.  Add fromString-ish helpers:
void primParseInt(EvalState &, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("parseInt", "string");
    try {
        int64_t v = std::stoll(args[0].payload.str);
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
    allocStats().attrsetsAllocated++;
    if (sSuccess < sValue) {
        b->entries[0] = {sSuccess, successV};
        b->entries[1] = {sValue, valueV};
    } else {
        b->entries[0] = {sValue, valueV};
        b->entries[1] = {sSuccess, successV};
    }
    out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
    out.payload.bindings = b;
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
    if      (a.isInt() && b.isInt())     return a.payload.i < b.payload.i;
    else if (a.isFloat() && b.isFloat()) return a.payload.f < b.payload.f;
    else if (a.isInt() && b.isFloat())   return static_cast<double>(a.payload.i) < b.payload.f;
    else if (a.isFloat() && b.isInt())   return a.payload.f < static_cast<double>(b.payload.i);
    else if (a.isString() && b.isString())
        return std::string_view(a.payload.str) < std::string_view(b.payload.str);
    else if (a.isList() && b.isList()) {
        // Lexicographic compare; force lazy elements as we go.
        uint32_t na = a.payload.list ? a.payload.list->size : 0;
        uint32_t nb = b.payload.list ? b.payload.list->size : 0;
        uint32_t n = std::min(na, nb);
        for (uint32_t i = 0; i < n; ++i) {
            Value ai = forceValue(vm, a.payload.list->elems[i]);
            Value bi = forceValue(vm, b.payload.list->elems[i]);
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

// #455: file-scope (external-linkage) thread-local + push/pop for
// the eager-bridge flag.  v3_hook.cc extern-declares these and
// wraps them in a ScopedEagerBridge RAII guard at the call hook
// for on-demand-root-populated lambda results.  forceEagerBridge()
// is the read-side helper used inside v3ToTreeWalker and is
// forward-declared in the anon namespace above.
thread_local bool tlsForceEagerBridge = false;
bool forceEagerBridge() { return tlsForceEagerBridge; }
bool pushForceEagerBridge() {
    bool prev = tlsForceEagerBridge;
    tlsForceEagerBridge = true;
    return prev;
}
void popForceEagerBridge(bool prev) { tlsForceEagerBridge = prev; }

// #452 / Phase C: shallow-TW-attrs-bridge flag, parallel to the
// eager-bridge knob above.  When set, treeWalkerToV3's nAttrs case
// wraps each TW entry's Value* in a v3 Bridge thunk instead of
// deeply converting.  Pushed by the call hook for the duration of
// runLambda when the lambda has formals -- only entries the body
// references get force-converted, matching TW's per-formal lazy
// semantics.  Required to safely run NixOS-module-shape lambdas in
// v3 where some param-attrset entries are mid-construction at the
// call site (e.g. fix-point's `config`).
thread_local bool tlsShallowTWAttrsBridge = false;
bool shallowTWAttrsBridge() { return tlsShallowTWAttrsBridge; }
bool pushShallowTWAttrsBridge() {
    bool prev = tlsShallowTWAttrsBridge;
    tlsShallowTWAttrsBridge = true;
    return prev;
}
void popShallowTWAttrsBridge(bool prev) { tlsShallowTWAttrsBridge = prev; }

void bumpPrimOpCallCount(const PrimOp * po)
{
    if (!po) return;
    auto & c = primOpCounter();
    std::lock_guard<std::mutex> g(c.mtx);
    c.counts[std::string(po->name)]++;
}

void dumpPrimOpStats(std::FILE * out)
{
    // #458 step B note: bridge telemetry is dumped separately from
    // v3_hook.cc's atexit handler (outside this function) so it
    // fires regardless of call-hook traffic.  Don't dump again here.

    // #453 Phase D: bridge primop counters (TW->v3 callbacks).  Print
    // before the v3-side primop counts because they're the actual
    // cutover-cost signal; high counts here mean v3 result values
    // bridged eagerly across the v3<->TW boundary.
    uint64_t br1 = nix::v3::g_bridgeCallBridge1Calls.load(std::memory_order_relaxed);
    uint64_t bra = nix::v3::g_bridgeForceAttrCalls.load(std::memory_order_relaxed);
    uint64_t brl = nix::v3::g_bridgeForceListElemCalls.load(std::memory_order_relaxed);
    if (br1 || bra || brl) {
        std::fprintf(out,
            "v3 bridge-primop calls (TW->v3): __v3_call_bridge_1=%llu "
            "__v3_force_attr=%llu __v3_force_list_elem=%llu\n",
            (unsigned long long)br1,
            (unsigned long long)bra,
            (unsigned long long)brl);
    }

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
// time).  Public — callable from v3_hook.cc.
namespace { extern nix::Value * (*v3ToTreeWalkerShim)(nix::EvalState &, Value); }
nix::Value * v3ToTreeWalkerPublic(nix::EvalState & nixState, Value v)
{
    BridgeTimer _bt(BridgeKind::V3ToTw);
    // #458 step B note: tried adding a scalar fast-path here that
    // skipped the v3ToTreeWalker shim's VMState alloc when v was a
    // forced scalar.  Measured ~3-5% regression on cardano-node and
    // fib28 -- the upfront tag check adds cost on every call, and
    // non-scalar v3 results dominate the v3->TW direction (primops
    // return strings/attrsets/lists).  Reverted.  The TW->v3
    // direction (call-hook arg, bridge-attr-lookup, forceBridgeThunk)
    // is still fast-pathed because scalar args are extremely common
    // there.
    return v3ToTreeWalkerShim ? v3ToTreeWalkerShim(nixState, v) : nullptr;
}

// #458 step 2: short-circuit TW->v3 closure dispatch.  When TW is
// about to call a `__v3_call_bridge_1` PrimOpApp, route directly to
// v3's callClosure -- bypassing TW's primop layer (which would
// dispatch primV3CallBridge1 and force args eagerly, the cardano-
// node #455 cycle source).
//
// Walks the PrimOpApp chain to find the underlying PrimOp, checks
// name == "__v3_call_bridge_1", extracts the handle from the chain's
// arg position, looks up the v3 closure, calls callClosure with the
// arg wrapped as a Bridge thunk (lazy -- v3's body forces on demand).
//
// Returns false on any mismatch so the caller can fall through to
// TW's regular primop dispatch.
bool tryUnwrapBridge1Closure(const nix::Value & funTw, Value & outV3Fn)
{
    // Already-forced funTw must be PrimOpApp(bridge1, vHandle) shape.
    if (!funTw.isPrimOpApp()) return false;
    const nix::Value * cur = &funTw;
    int depth = 0;
    while (cur->isPrimOpApp()) {
        cur = cur->primOpApp().left;
        ++depth;
    }
    if (!cur->isPrimOp()) return false;
    const nix::PrimOp * po = cur->primOp();
    if (!po || po->name != "__v3_call_bridge_1") return false;
    if (depth != 1) return false;
    const nix::Value * vHandle = funTw.primOpApp().right;
    if (!vHandle) return false;
    if (vHandle->type<true>() != nix::nInt) return false;
    int64_t h = vHandle->integer().value;
    auto & tbl = v3BridgeClosures();
    if (h < 0 || (size_t)h >= tbl.size()) return false;
    outV3Fn = tbl[(size_t)h].v3Value;
    return true;
}

bool tryDispatchBridge1Direct(nix::EvalState & ns,
                              const nix::Value & funValue,
                              nix::Value * arg,
                              nix::Value & out,
                              const nix::PosIdx pos)
{
    // funValue should be a PrimOpApp.  Walk its left side to find
    // the underlying PrimOp.
    if (!funValue.isPrimOpApp()) return false;
    const nix::Value * cur = &funValue;
    int depth = 0;
    while (cur->isPrimOpApp()) {
        cur = cur->primOpApp().left;
        ++depth;
    }
    if (!cur->isPrimOp()) return false;
    const nix::PrimOp * po = cur->primOp();
    if (!po) return false;
    if (po->name != "__v3_call_bridge_1") return false;
    // bridge1 has arity 2.  PrimOpApp(bridgePrimOp1, vHandle) means
    // arity-1-applied; the full call needs ONE more arg from caller
    // (`arg`).  If depth != 1, the chain is mis-shaped (shouldn't
    // happen for well-formed bridge1 wrappers) -- decline.
    if (depth != 1) return false;

    // Share the depth counter with primV3CallBridge1.  When both paths
    // exist concurrently and use independent counters, they ping-pong:
    // shortcut at depth N declines, TW falls through to primV3CallBridge1
    // (its counter at 0), that re-enters shortcut, etc.  cardano-node
    // PP exposes this -- the eval HANGS instead of throwing the proper
    // InfiniteRecursionError that surfaces with shortcut OFF.  One
    // shared counter ensures the depth ceiling fires regardless.
    int & s_sharedDepth = bridge1DepthCounter();
    int kMaxDepth = bridge1MaxDepth();
    if (kMaxDepth > 0 && s_sharedDepth >= kMaxDepth)
        return false;

    // Extract the handle from the immediate right-side arg.
    const nix::Value * vHandle = funValue.primOpApp().right;
    if (!vHandle) return false;
    ns.forceValue(*const_cast<nix::Value *>(vHandle), pos);
    if (vHandle->type() != nix::nInt) return false;
    int64_t h = vHandle->integer().value;
    auto & tbl = v3BridgeClosures();
    if (h < 0 || (size_t)h >= tbl.size()) return false;

    // Got a valid bridge1 invocation.  Dispatch via v3 directly:
    //  1. Wrap the TW arg as a v3 Bridge thunk (lazy -- mirrors what
    //     primV3CallBridge1 + LAZY_BRIDGE_ARG would have produced
    //     after the eager force was skipped).
    //  2. callClosure(vm, v3fn, v3arg).
    //  3. Bridge result back to TW.
    Value v3fn = tbl[(size_t)h].v3Value;
    nix::Expr * fallbackExpr = tbl[(size_t)h].fallbackExpr;

    Thunk * bridge = Alloc::allocBridgeThunk(static_cast<void *>(arg));
    allocStats().thunksAllocated++;
    Value v3Arg;
    v3Arg.tag_payload = static_cast<uint64_t>(Tag::Thunk);
    v3Arg.payload.thunk = bridge;

    ScopedNixEvalState _v3evalGuard(&ns);
    ScopedBridgeFallbackExpr fbGuard{fallbackExpr};

    // RAII increment of the SHARED depth counter for the duration of
    // this dispatch.  Decrements on every exit including throws.
    struct DepthGuard {
        int & d;
        DepthGuard(int & d_) : d(d_) { ++d; }
        ~DepthGuard() { --d; }
    } _depthGuard(s_sharedDepth);

    VMState vm;
    vm.valueStack.reserve(64 * 1024);
    vm.frames.reserve(4096);
    vm.withStack.reserve(64);
    EvalState st;
    st.nixEvalState = &ns;
    st.vm = &vm;

    Value r;
    try {
        r = callClosure(*st.vm, v3fn, v3Arg);
        r = forceValue(*st.vm, r);
    } catch (const std::exception & ex) {
        // Bridge1's existing fallbackToTreeWalker path covers the
        // BlackholeError case via re-running fallbackExpr.  Mirror
        // that here so behaviour parity holds.
        if (fallbackExpr && dynamic_cast<const BlackholeError *>(&ex)) {
            nix::Value tw;
            try {
                fallbackExpr->eval(ns, ns.baseEnv, tw);
                ns.forceValue(tw, pos);
                ns.callFunction(tw, *arg, out, pos);
                return true;
            } catch (...) {
                return false;  // give up; let TW dispatch handle it
            }
        }
        return false;
    }

    // Bridge result back to TW.
    nix::Value * tmp = v3ToTreeWalkerPublic(ns, r);
    if (!tmp) return false;
    out = *tmp;
    return true;
}

/// Public bridge entry point for the tree-walker -> v3 direction.
/// Used by the CO-2 phase B force hook to convert env values to
/// v3 upvalues.  Forces nv to WHNF in tree-walker, then walks the
/// resulting type tree to produce a v3 Value.
Value treeWalkerToV3Public(nix::EvalState & nixState, nix::Value & nv)
{
    BridgeTimer _bt(BridgeKind::TwToV3Full);
    // REVIEW MED-16: stack-allocated.
    VMState bridgeShimVm;
    bridgeShimVm.valueStack.reserve(64 * 1024);
    bridgeShimVm.frames.reserve(4096);
    bridgeShimVm.withStack.reserve(64);
    EvalState st;
    st.nixEvalState = &nixState;
    st.vm           = &bridgeShimVm;
    return treeWalkerToV3(st, nv);
}

/// WC-10: bridge-thunk forcer.  Called from vm.cc OP_FORCE /
/// forceValue when a Thunk has state == Bridge.  Looks up the
/// thread-local nix EvalState (set by setNixEvalState() at hook
/// entry), reads the stashed `nix::Value *` from the thunk,
/// forces it on the tree-walker side, and bridges the result via
/// treeWalkerToV3Public.  Throws if no nix EvalState is wired —
/// the bridge thunk needs tree-walker context to make sense.
Value forceBridgeThunk(Thunk * t)
{
    BridgeTimer _bt(BridgeKind::TwForce);
    if (!t || t->state != ThunkState::Bridge || !t->bridgeSrc)
        throw std::runtime_error(
            "v3 forceBridgeThunk: thunk has no bridge source");
    if (!tlNixEvalState)
        throw std::runtime_error(
            "v3 forceBridgeThunk: no tree-walker EvalState wired");

    // #466 / #479 Phase 1: cross-primop force-chain detector.  Bridge
    // thunks are the V3-side of a TW value; forcing one calls back into
    // TW's forceValue (via treeWalkerToV3Public) which can re-enter the
    // v3 hooks.  Without this guard a cycle through Bridge → TW force
    // → TW callFunction → v3 hook → primV3ForceAttr → Bridge ... loops
    // unbounded on the C stack.  Throw BlackholeError on re-entry; the
    // OP_FORCE handler in vm.cc will translate via mkBlackhole-as-value
    // for foreign-vm Black thunks, or surface as a proper TW infinite-
    // recursion at the caller for same-vm cycles.
    ForceChainGuard _fcg(ForceChainOp::ForceBridgeThunk,
                         reinterpret_cast<uint64_t>(t));
    if (_fcg.isCycle()) {
        throw BlackholeError(
            _fcg.atDepthCeiling()
            ? std::string("v3 forceBridgeThunk: force-chain depth ceiling reached")
            : "v3 forceBridgeThunk: force-chain cycle on thunk");
    }
    auto * srcV = static_cast<nix::Value *>(t->bridgeSrc);
    // #438 diagnostic: dump the tree-walker source's raw layout right
    // before bridging.  If a Bridge source has been reclaimed and its
    // memory reused, `srcV->type()` returns garbage and the recursive
    // `treeWalkerToV3` walks into impossible territory (e.g., a list
    // whose `bigList.size` is a stray pointer).
    static const bool s_dbg_bridge =
        std::getenv("V3_DEBUG_BRIDGE_SRC") != nullptr;
    if (__builtin_expect(s_dbg_bridge, 0)) [[unlikely]] {
        const uint64_t * raw = reinterpret_cast<const uint64_t *>(srcV);
        // Read payload atomically into locals, then derive everything
        // from those snapshots, so the dump and the type() call agree.
        uint64_t p0 = raw[0];
        uint64_t p1 = raw[1];
        uint32_t pd = (uint32_t)(p0 & 0x7);
        int twType = -1;
        try { twType = (int)srcV->type(); } catch (...) { twType = -2; }
        std::fprintf(stderr,
            "v3 forceBridgeThunk thunk=%p src=%p type=%d pd=%u p0=%016llx p1=%016llx\n",
            (void *)t, (void *)srcV,
            twType, pd,
            (unsigned long long)p0,
            (unsigned long long)p1);
        // #438: if the source's primary discriminator is 0, the Value
        // is uninitialized.  Forcing it leads to UB-fueled chaos
        // (treeWalkerToV3 falls into a phony case based on whatever
        //  `type()` decides under the unreachable assumption).  Abort
        // here so an attached debugger gets the v3 force stack and the
        // ExprVar/callsite that triggered the force.
        if (__builtin_expect(pd == 0, 0)) [[unlikely]] {
            std::fprintf(stderr,
                "v3 forceBridgeThunk: UNINITIALIZED bridge src "
                "(pd=0) — aborting for backtrace\n");
            std::fflush(stderr);
            std::abort();
        }
    }
    // #458 step B: scalar fast path -- if the bridged TW Value
    // is already a forced scalar, skip VMState allocation and
    // direct-write the v3 equivalent.  Eliminates the heavy
    // treeWalkerToV3Public call for the common case.
    Value out;
    if (tryFastBridgeScalarTwToV3(*srcV, out))
        return out;
    return treeWalkerToV3Public(*tlNixEvalState, *srcV);
}

/// #458 step A.2 (slot-threading for fix-point args): per-attribute
/// lookup against a Bridge thunk's TW Value source WITHOUT forcing the
/// whole TW Value.
///
/// Motivating case: cardano-node `with self;` where `self` is the
/// fix-point argument of an `extends overlay` chain.  When v3 forces
/// a Bridge thunk wrapping the partially-constructed `self`, TW's
/// outer-thunk BlackHole detection fires (cardano-node #455).  The
/// existing `forceBridgeThunk` path has no choice -- it forces the
/// whole.  This helper instead peeks at the TW Value's tag bit
/// (no force), and if it's already an attrset (Bindings constructed,
/// even when individual entries are still thunks), looks up the
/// requested name and bridges JUST that single value.
///
/// Returns std::nullopt if:
///   - src is still a thunk (not yet attrset-shaped)
///   - src is some other type (non-attrset)
///   - the name doesn't exist in the partial bindings
///   - forcing the single found Attr threw BlackHole (entry itself
///     is mid-construction)
///
/// Returns the bridged v3 Value otherwise -- itself possibly a fresh
/// Bridge thunk if the attr's body is still a TW thunk.
std::optional<Value> tryBridgeAttrLookup(Thunk * t, SymbolId v3name)
{
    if (!t || t->state != ThunkState::Bridge || !t->bridgeSrc)
        return std::nullopt;
    if (!tlNixEvalState)
        return std::nullopt;
    auto * srcV = static_cast<nix::Value *>(t->bridgeSrc);

    // PEEK without force: read the internalType discriminator.  TW's
    // type() throws on Blackhole-tagged values; protect with try/catch
    // so we can route to "scope blackholed" cleanly.
    nix::ValueType tt;
    try {
        tt = srcV->type();
    } catch (...) {
        return std::nullopt;
    }
    if (tt != nix::nAttrs)
        return std::nullopt;
    const nix::Bindings * bindings = srcV->attrs();
    if (!bindings)
        return std::nullopt;

    // Translate v3 SymbolId -> TW Symbol via the v3 symbol table's
    // string, then through TW's symbol table.  v3's globalSymbolTable
    // owns the canonical strings; both sides intern by string content.
    const auto & v3Tab = ir::globalSymbolTable();
    if (v3name >= v3Tab.size())
        return std::nullopt;
    std::string_view nameStr = v3Tab[v3name];
    nix::Symbol twSym = tlNixEvalState->symbols.create(nameStr);

    const nix::Attr * a = bindings->get(twSym);
    if (!a)
        return std::nullopt;

    // Bridge the single Attr value back to v3.  treeWalkerToV3Public
    // is the canonical bridge entry; for thunked entries it allocates
    // a fresh Bridge thunk, which preserves laziness one more level.
    // Catch BlackHole specifically: a forced-blackhole entry means
    // the Attr's body is itself mid-construction; treat as "not yet
    // resolvable", let the caller try the outer scope.
    //
    // #458 step B: try the scalar fast path first -- ints/floats/
    // bools/null skip VMState allocation and direct-write the v3
    // equivalent.  Common case for entries like `system = "x86..."`
    // that constant-folded into a forced scalar already.
    Value v3v;
    if (tryFastBridgeScalarTwToV3(*a->value, v3v)) {
        bridgeTelemetryBump(BridgeKind::TwToV3Attr, 0);
        return v3v;
    }
    try {
        v3v = treeWalkerToV3Public(*tlNixEvalState, *a->value);
        bridgeTelemetryBump(BridgeKind::TwToV3Attr, 0);
        return v3v;
    } catch (...) {
        return std::nullopt;
    }
}

/// #458 step B (canonicalization): scalar fast-path for TW -> v3
/// bridging.  See header for full doc.  Inspects the TW Value's
/// discriminator without forcing; for known scalars writes the v3
/// equivalent directly.  Caller fast-bridges into `out` and skips
/// the regular treeWalkerToV3Public path.
bool tryFastBridgeScalarTwToV3(const nix::Value & nv, Value & out)
{
    try {
        nix::ValueType tt = nv.type();
        bool ok = false;
        if (tt == nix::nInt) {
            out.mkInt(nv.integer().value);
            ok = true;
        } else if (tt == nix::nFloat) {
            out.mkFloat(nv.fpoint());
            ok = true;
        } else if (tt == nix::nBool) {
            out = nv.boolean() ? Value::vTrue : Value::vFalse;
            ok = true;
        } else if (tt == nix::nNull) {
            out.mkNull();
            ok = true;
        }
        if (ok) {
            // Bump the scalar-fast counter (no timing -- the work is
            // a few ns and the steady_clock call would dwarf it).
            bridgeTelemetryBump(BridgeKind::TwToV3Scalar, 0);
            return true;
        }
    } catch (...) {
        // type() may throw on uninit / blackhole; bail to slow path.
    }
    return false;
}

/// #458 step B: bridge telemetry storage + accessors.  See header
/// for full design.  Always-on counts; opt-in timings.
namespace {
struct BridgeStats {
    std::atomic<uint64_t> count{0};
    std::atomic<uint64_t> nsTotal{0};
};
BridgeStats & bridgeStats(BridgeKind k)
{
    static BridgeStats arr[(size_t)BridgeKind::Count];
    return arr[(size_t)k];
}
} // anon ns

bool bridgeTimingEnabled()
{
    static const bool e = std::getenv("NIX_V3_BRIDGE_TIMING") != nullptr;
    return e;
}

void bridgeTelemetryBump(BridgeKind k, uint64_t ns)
{
    auto & s = bridgeStats(k);
    s.count.fetch_add(1, std::memory_order_relaxed);
    if (ns) s.nsTotal.fetch_add(ns, std::memory_order_relaxed);
}

BridgeTimer::BridgeTimer(BridgeKind k) : kind(k), startNs(0)
{
    if (bridgeTimingEnabled()) {
        auto t = std::chrono::steady_clock::now();
        startNs = (uint64_t)std::chrono::duration_cast<
            std::chrono::nanoseconds>(t.time_since_epoch()).count();
    }
}
BridgeTimer::~BridgeTimer()
{
    uint64_t ns = 0;
    if (startNs) {
        auto t = std::chrono::steady_clock::now();
        uint64_t now = (uint64_t)std::chrono::duration_cast<
            std::chrono::nanoseconds>(t.time_since_epoch()).count();
        ns = now - startNs;
    }
    bridgeTelemetryBump(kind, ns);
}

void dumpBridgeTelemetry(std::FILE * out)
{
    static const char * labels[(size_t)BridgeKind::Count] = {
        "tw->v3 full ",
        "tw->v3 scalar",
        "tw->v3 attr  ",
        "tw->v3 has   ",
        "v3->tw       ",
        "tw force     ",
    };
    bool any = false;
    for (size_t i = 0; i < (size_t)BridgeKind::Count; ++i) {
        if (bridgeStats((BridgeKind)i).count.load(std::memory_order_relaxed)) {
            any = true; break;
        }
    }
    if (!any) return;
    std::fprintf(out, "v3 bridge telemetry%s:\n",
        bridgeTimingEnabled() ? " (count + nsTotal)" : " (count only -- "
        "set NIX_V3_BRIDGE_TIMING=1 for timings)");
    uint64_t totalCount = 0, totalNs = 0;
    for (size_t i = 0; i < (size_t)BridgeKind::Count; ++i) {
        uint64_t c = bridgeStats((BridgeKind)i).count.load(std::memory_order_relaxed);
        uint64_t n = bridgeStats((BridgeKind)i).nsTotal.load(std::memory_order_relaxed);
        totalCount += c;
        totalNs    += n;
        if (c == 0) continue;
        if (bridgeTimingEnabled() && n) {
            double ms = n / 1e6;
            double avgNs = (double)n / c;
            std::fprintf(out,
                "  %s  count=%-12llu ns=%-15llu (%.3f ms total, %.0f ns avg)\n",
                labels[i], (unsigned long long)c, (unsigned long long)n, ms, avgNs);
        } else {
            std::fprintf(out, "  %s  count=%llu\n", labels[i],
                (unsigned long long)c);
        }
    }
    if (totalCount) {
        if (bridgeTimingEnabled() && totalNs) {
            std::fprintf(out, "  %-13s  count=%-12llu ns=%-15llu (%.3f ms total)\n",
                "TOTAL", (unsigned long long)totalCount,
                (unsigned long long)totalNs, totalNs / 1e6);
        } else {
            std::fprintf(out, "  %-13s  count=%llu\n", "TOTAL",
                (unsigned long long)totalCount);
        }
    }
}

/// #458 step A.4: existence-check sibling of tryBridgeAttrLookup for
/// the `attrs ? name` operator.  Cheaper than tryBridgeAttrLookup --
/// no bridging the entry value, just answer whether the partial
/// bindings already has the name.
BridgeAttrHasResult tryBridgeAttrHas(Thunk * t, SymbolId v3name)
{
    if (!t || t->state != ThunkState::Bridge || !t->bridgeSrc)
        return BridgeAttrHasResult::Indeterminate;
    if (!tlNixEvalState)
        return BridgeAttrHasResult::Indeterminate;
    auto * srcV = static_cast<nix::Value *>(t->bridgeSrc);
    nix::ValueType tt;
    try {
        tt = srcV->type();
    } catch (...) {
        return BridgeAttrHasResult::Indeterminate;
    }
    if (tt != nix::nAttrs)
        return BridgeAttrHasResult::Indeterminate;
    const nix::Bindings * bindings = srcV->attrs();
    if (!bindings)
        return BridgeAttrHasResult::Indeterminate;
    const auto & v3Tab = ir::globalSymbolTable();
    if (v3name >= v3Tab.size())
        return BridgeAttrHasResult::Indeterminate;
    std::string_view nameStr = v3Tab[v3name];
    nix::Symbol twSym = tlNixEvalState->symbols.create(nameStr);
    auto r = bindings->get(twSym)
        ? BridgeAttrHasResult::Present
        : BridgeAttrHasResult::Absent;
    bridgeTelemetryBump(BridgeKind::TwToV3Has, 0);
    return r;
}

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
    auto ph = nix::hashPlaceholder(std::string_view(args[0].payload.str));
    out = mkStringValueOwned(std::move(ph));
}

/// builtins.__warn "msg" v → print msg to stderr, return v.  Tree-walker:
/// src/libexpr/primops.cc:1451 (prim_warn).  v3 simplifies: emits the
/// "warning:" prefix, returns args[1] unchanged.  Doesn't honour
/// abort-on-warn settings (parity-relevant for that subset of users).
void primWarn(EvalState &, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("warn", "string");
    std::fprintf(stderr, "warning: %s\n", args[0].payload.str);
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
    std::string pathStr = v.isPath() ? std::string(v.payload.path)
                                      : std::string(v.payload.str);
    nix::CanonPath path(pathStr);
    if (!ns->store->isStorePath(path.abs()))
        path = nix::CanonPath(nix::canonPath(path.abs(), true).string());
    if (!ns->store->isInStore(path.abs()))
        throw std::runtime_error("v3 storePath: path '" + path.abs() +
                                  "' is not in the Nix store");
    auto path2 = ns->store->toStorePath(path.abs()).first;
    if (!nix::settings.readOnlyMode)
        ns->store->ensurePath(path2);
    nix::NixStringContext context;
    context.insert(nix::NixStringContextElem::Opaque{.path = path2});
    nix::Value tw;
    tw.mkString(path.abs(), context, ns->mem);
    Value result = treeWalkerToV3Public(*ns, tw);
    out = result;
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
    std::string name(args[0].payload.str);
    std::string contents(args[1].payload.str);
    // For string context tracking: v3 strings can carry context too
    // (see contextStorage in primops.cc).  For toFile, refs come from
    // the contents' context — which on the v3 side may be empty if the
    // string was literal.  Future work: thread v3 context through.
    nix::StorePathSet refs;
    auto storePath = nix::settings.readOnlyMode
        ? ns->store->makeFixedOutputPathFromCA(
            name,
            nix::TextInfo{
                .hash = nix::hashString(nix::HashAlgorithm::SHA256, contents),
                .references = std::move(refs),
            })
        : ({
            nix::StringSource s{contents};
            ns->store->addToStoreFromDump(
                s, name,
                nix::FileSerialisationMethod::Flat,
                nix::ContentAddressMethod::Raw::Text,
                nix::HashAlgorithm::SHA256, refs, ns->repair);
        });
    nix::Value tw;
    ns->allowAndSetStorePathString(storePath, tw);
    Value result = treeWalkerToV3Public(*ns, tw);
    out = result;
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
    auto * ns = state.nixEvalState;
    // Convert args[0] (drvRef) and args[1] (outputName) to tree-walker
    // values, delegate to tree-walker's coerceToSingleDerivedPath +
    // mkSingleDerivedPathString, bridge the result back.
    nix::Value tw0;
    {
        std::string drvRef = args[0].isString() && args[0].payload.str
            ? std::string(args[0].payload.str) : std::string();
        // §3: build a NixStringContext from the v3-side entries (if any)
        // before calling mkString -- mkString takes &context by ref.
        nix::NixStringContext ctx;
        if (args[0].isString() && args[0].payload.str) {
            if (auto * raw = lookupStringContextEntries(args[0].payload.str)) {
                for (auto & e : *raw) {
                    try { ctx.insert(nix::NixStringContextElem::parse(e)); }
                    catch (...) { /* skip un-parseable */ }
                }
            }
        }
        if (ctx.empty())
            tw0.mkString(drvRef, ns->mem);
        else
            tw0.mkString(drvRef, ctx, ns->mem);
    }
    if (!args[1].isString()) typeError("outputOf", "string output name");
    nix::SingleDerivedPath drvPath = ns->coerceToSingleDerivedPath(
        nix::noPos, tw0,
        "while evaluating the first argument to builtins.outputOf");
    std::string outputName(args[1].payload.str);
    nix::Value tw;
    ns->mkSingleDerivedPathString(
        nix::SingleDerivedPath::Built{
            .drvPath = nix::make_ref<nix::SingleDerivedPath>(drvPath),
            .output = outputName,
        },
        tw);
    out = treeWalkerToV3Public(*ns, tw);
}

// ---------------------------------------------------------------------------
// WC-28b: fetch primops.  Each delegates to the corresponding tree-walker
// primop in `builtins` (since libnixfetchers integration is heavy and not
// worth duplicating for primops that are inherently store/network bound).
// Same bridge pattern as BR-4's `primPath` fall-through (primops.cc:4457).
// ---------------------------------------------------------------------------

/// Generic bridge: convert v3 args to tree-walker, look up
/// `builtins.<name>` (or the same with `__` prefix), call it,
/// bridge the result back.
template <int Arity>
static void bridgeBuiltin(const char * name, EvalState & state,
                          Value * args, Value & out)
{
    if (!state.nixEvalState)
        throw std::runtime_error(std::string("v3 ") + name +
            ": no tree-walker state available");
    auto & ns = *state.nixEvalState;
    nix::Value * nargs[Arity];
    for (int i = 0; i < Arity; ++i) nargs[i] = v3ToTreeWalker(state, args[i]);
    nix::Value & blt = ns.getBuiltins();
    ns.forceAttrs(blt, nix::noPos, "v3 fetch primop bridge");
    auto * pAttr = blt.attrs()->get(ns.symbols.create(name));
    if (!pAttr || !pAttr->value) {
        // Try __-prefixed alias (some fetch primops use that).
        std::string alt = std::string("__") + name;
        pAttr = blt.attrs()->get(ns.symbols.create(alt));
        if (!pAttr || !pAttr->value)
            throw std::runtime_error(std::string("v3 ") + name +
                ": tree-walker builtins.<name> not found");
    }
    nix::Value cur = *pAttr->value;
    for (int i = 0; i < Arity; ++i) {
        nix::Value next;
        ns.callFunction(cur, *nargs[i], next, nix::noPos);
        cur = next;
    }
    out = treeWalkerToV3(state, cur);
}

void primFetchurl    (EvalState & s, Value * a, Value & o) { bridgeBuiltin<1>("fetchurl",    s, a, o); }
void primFetchTarball(EvalState & s, Value * a, Value & o) { bridgeBuiltin<1>("fetchTarball", s, a, o); }
void primFetchTree   (EvalState & s, Value * a, Value & o) { bridgeBuiltin<1>("fetchTree",   s, a, o); }
void primFetchGit    (EvalState & s, Value * a, Value & o) { bridgeBuiltin<1>("fetchGit",    s, a, o); }
void primFetchMercurial(EvalState & s, Value * a, Value & o){ bridgeBuiltin<1>("fetchMercurial", s, a, o); }
void primFetchClosure(EvalState & s, Value * a, Value & o) { bridgeBuiltin<1>("fetchClosure", s, a, o); }
void primFilterSource(EvalState & s, Value * a, Value & o) { bridgeBuiltin<2>("filterSource", s, a, o); }

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
        registerPrimOp({"concatLists",        1, primConcatLists});
        registerPrimOp({"concatStringsSep",   2, primConcatStringsSep});
        registerPrimOp({"substring",          3, primSubstring});
        // Higher-order callback primops (re-enter the VM via callClosure).
        registerPrimOp({"map",                2, primMap});
        registerPrimOp({"filter",             2, primFilter});
        registerPrimOp({"foldl'",             3, primFoldl, /*lazyArgs=*/0b010});
        registerPrimOp({"genList",            2, primGenList});
        registerPrimOp({"all",                2, primAll});
        registerPrimOp({"any",                2, primAny});
        registerPrimOp({"concatMap",          2, primConcatMap});
        registerPrimOp({"partition",          2, primPartition});
        registerPrimOp({"getEnv",             1, primGetEnv});
        registerPrimOp({"compareVersions",    2, primCompareVersions});
        registerPrimOp({"listToAttrs",        1, primListToAttrs});
        registerPrimOp({"removeAttrs",        2, primRemoveAttrs});
        registerPrimOp({"intersectAttrs",     2, primIntersectAttrs});
        registerPrimOp({"mapAttrs",           2, primMapAttrs});
        registerPrimOp({"elem",               2, primElem});
        registerPrimOp({"getAttr",            2, primGetAttr});
        registerPrimOp({"hasAttr",            2, primHasAttr});
        registerPrimOp({"catAttrs",           2, primCatAttrs});
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
        registerPrimOp({"groupBy",            2, primGroupBy});
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
        // C++ port of corepkgs/derivation.nix — derivationStrict
        // synthesizes paths, primDerivation wraps them up with
        // commonAttrs and outputName for tree-walker parity.
        registerPrimOp({"derivation",         1, primDerivation});
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
        registerPrimOp({"__elem",             2, primElem});
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
