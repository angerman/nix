/// @file
/// FFI plan migration step 2: skeletal Evaluator + EvalScope impl.
///
/// Provides minimal definitions for the framework types declared in
/// `include/v3/ffi.hh` so they can be linked against.  Full migration
/// (handle storage, scope-bound handle invalidation, GlobalClosureHandle
/// promotion semantics) lands incrementally per the plan.
///
/// **Status:**
///   - `Evaluator` is a thin shell -- holds a per-instance scope-list
///     pointer.  Future work will lift the existing v3 globals
///     (v3HookCache, v3SubExprCache, v3BridgeClosures, ...) into
///     Evaluator member fields so multiple Evaluators can coexist
///     (e.g., for sandboxed plugin evaluation).
///   - `EvalScope` chains scopes per Evaluator.  Handle issuance and
///     invalidation will land alongside the migration of
///     `v3FormalsLambdaBridges` (today's sentinel-Env side-table) into
///     the EvalScope handle accounting.
///   - `promoteToGlobal` / `releaseGlobal` are skeletal -- the stable
///     v3BridgeClosures table already provides the storage; the wrapper
///     just makes the lifetime contract explicit at the API boundary.
///
/// Per FFI_PLAN_2026-05-06b §A1, A7, A10 (highest-priority migration
/// step 2).
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/ffi.hh"

#include "v3/value.hh"
#include "v3/vm.hh"
#include "v3/primop.hh"

#include "nix/util/source-path.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/error.hh"
#include "nix/util/canon-path.hh"          // CanonPath (coercePathToStore)
#include "nix/expr/eval.hh"   // EvalState — ffi.cc is the one TU that wraps it
#include "nix/expr/value/context.hh"       // NixStringContext(Elem) (path/ctx shims)
#include "nix/store/store-api.hh"          // Store::printStorePath

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace nix::v3 {

// EvalState shims (audit §3.4) — out-of-line wrappers; see ffi.hh.
namespace ffi {

void forceValue(nix::EvalState & state, nix::Value & v)
{
    state.forceValue(v, nix::noPos);
}

void setTreeWalkerBuiltin(nix::EvalState & state, const std::string & name, nix::Value * value)
{
    state.getBuiltin(name) = *value;  // throws if `name` isn't a builtin
}

const nix::SymbolTable & symbols(nix::EvalState & state) { return state.symbols; }
nix::PosTable &          positions(nix::EvalState & state) { return state.positions; }

// --- TW value-graph probe + bridge round-trip (audit Phase 2/3) ---------

TwType valueType(const nix::Value * v)
{
    // type<true>(): an invalid/blackholed cell maps to nThunk instead of
    // asserting — matches the `type<true>()` call sites we replaced in vm.cc.
    switch (v->type<true>()) {
        case nix::nNull:     return TwType::Null;
        case nix::nBool:     return TwType::Bool;
        case nix::nInt:      return TwType::Int;
        case nix::nFloat:    return TwType::Float;
        case nix::nString:   return TwType::String;
        case nix::nPath:     return TwType::Path;
        case nix::nList:     return TwType::List;
        case nix::nAttrs:    return TwType::Attrs;
        case nix::nFunction: return TwType::Function;
        case nix::nThunk:    return TwType::Thunk;
        case nix::nExternal: return TwType::External;
        case nix::nFailed:   return TwType::Other;  // evaluation-failed sentinel
    }
    return TwType::Other;  // unreachable; satisfies the non-void contract.
}

nix::Value * allocValue(nix::EvalState & state)
{
    return state.allocValue();
}

void callFunction(nix::EvalState & state, nix::Value & fun, nix::Value & arg, nix::Value & out)
{
    state.callFunction(fun, arg, out, nix::noPos);
}

std::string coercePathToStore(nix::EvalState & state, const std::string & path)
{
    // The local context is filled by copyPathToStore but discarded here;
    // the v3 caller re-records the Opaque entry keyed on the store path
    // (see vm.cc OP_STR_CONCAT).  Let exceptions propagate — TW raises on
    // a missing path during interpolation and v3 must match.
    nix::NixStringContext ctx;
    nix::SourcePath sp(state.rootFS, nix::CanonPath(path));
    auto storePath = state.copyPathToStore(ctx, sp);
    return state.store->printStorePath(storePath);
}

std::string coercePathToStoreName(nix::EvalState & state, const std::string & path)
{
    nix::NixStringContext ctx;
    nix::SourcePath sp(state.rootFS, nix::CanonPath(path));
    auto storePath = state.copyPathToStore(ctx, sp);
    return std::string(storePath.to_string());
}

std::string displayContextElem(nix::EvalState & state, const std::string & raw)
{
    try {
        auto elem = nix::NixStringContextElem::parse(raw);
        return elem.display(*state.store);
    } catch (...) {
        return raw;  // keep the raw form on a parse failure.
    }
}

}  // namespace ffi

// ---------------------------------------------------------------------------
// Evaluator
// ---------------------------------------------------------------------------

/// Per-EvalScope handle storage.  Each entry holds an opaque payload
/// (whatever v3-internal value the host registers) and a `valid` flag
/// flipped to false when the enclosing scope is destroyed.  The valid
/// flag persists in the table after scope destruction (until the next
/// gen rollover) so isValid() correctly returns false for stale handles.
///
/// GC_AUDIT_ROUND_2 N6 (LATENT, documented 2026-05-21): `payload` is
/// cast to `Value *` by `applyClosure` (this file ~line 271).
/// Production FFI consumers (the embedding host API) would store v3
/// `Closure *` / `Bindings *` / `ListVec *` here, any of which can be
/// nursery-resident.  Today only test code paths use
/// `allocClosureHandle`, so the absence of a scavenger walk is not
/// active.  Before opening the FFI to production embedders, add
/// `walkEvalScopeRoots(visit)` that iterates every live `ScopeNode`
/// (via `g_topScope` chain) and calls `visit(*reinterpret_cast<Value *>(&slot.payload))`
/// for each valid slot — then call it from `gc.cc::Scavenger::run()`
/// and from `postScavengeAudit`.  See
/// `lode/GC_AUDIT_ROUND_2_2026-05-21.md` §2.4 N6.
struct HandleSlot
{
    void *   payload;
    bool     valid;
};

/// Per-Evaluator scope chain head.  Each EvalScope ctor pushes a new
/// node; dtor pops.  Threadlocal -- one logical Evaluator per thread
/// today; future work may need lock-free per-instance lists.
struct ScopeNode
{
    ScopeNode * prev;
    /// Generation token for handles allocated in this scope.  Encoded
    /// in the upper 32 bits of ClosureHandle::opaque so a handle whose
    /// scope has been destroyed (and whose generation has been removed
    /// from g_liveScopes) fails the lookup -- ABA defence works because
    /// each new scope gets a fresh generation from the global counter.
    uint32_t generation;
    /// Slot vector owned by this scope; entries are flipped to valid=false
    /// in the dtor before the table is freed.
    std::vector<HandleSlot> slots;
};

namespace {
thread_local ScopeNode * g_topScope = nullptr;

/// Scope generation counter.  Atomic so concurrent threads issue
/// distinct generations even if their EvalScopes never interact.
/// Starts at 1 (0 reserved for "uninitialised handle").
std::atomic<uint32_t> g_nextScopeGen{1};

/// Live-scope index: maps generation -> ScopeNode*.  Populated on ctor,
/// erased on dtor.  Lookup-by-generation drives O(1) handle resolution.
/// Lock guards both the map and per-slot access (writers + readers).
std::mutex                                  g_scopeLock;
std::unordered_map<uint32_t, ScopeNode *>   g_liveScopes;

constexpr uint32_t kInvalidGen = 0;

/// Pack/unpack helpers for the 64-bit handle opaque.
struct PackedHandle
{
    uint32_t generation;
    uint32_t slotIdx;
};
inline uint64_t packHandle(uint32_t gen, uint32_t slot) {
    return (uint64_t(gen) << 32) | uint64_t(slot);
}
inline PackedHandle unpackHandle(uint64_t opaque) {
    return PackedHandle{
        .generation = uint32_t(opaque >> 32),
        .slotIdx    = uint32_t(opaque & 0xFFFFFFFFu),
    };
}
}

class Evaluator
{
public:
    Evaluator() = default;
    Evaluator(const Evaluator &) = delete;
    Evaluator & operator=(const Evaluator &) = delete;
};

// ---------------------------------------------------------------------------
// EvalScope
// ---------------------------------------------------------------------------

EvalScope::EvalScope(Evaluator & e)
    : m_ev(e)
{
    uint32_t gen = g_nextScopeGen.fetch_add(1, std::memory_order_relaxed);
    // Avoid handing out gen=0 (reserved as kInvalidGen).  In practice
    // this only matters at the 4-billion-scope rollover; bias once.
    if (gen == kInvalidGen)
        gen = g_nextScopeGen.fetch_add(1, std::memory_order_relaxed);

    auto * node = new ScopeNode{
        .prev       = g_topScope,
        .generation = gen,
        .slots      = {},
    };
    {
        std::lock_guard<std::mutex> lk(g_scopeLock);
        g_liveScopes.emplace(gen, node);
    }
    g_topScope = node;
    m_state    = node;
}

EvalScope::~EvalScope()
{
    auto * node = static_cast<ScopeNode *>(m_state);
    if (!node) return;

    // Invalidate every handle issued by this scope.  Marking the slots
    // (rather than just dropping the table) means a stale ClosureHandle
    // copied out by the host still resolves cleanly to "invalid" via
    // lookupClosureHandle / isValid -- they re-check the slot's valid
    // bit on every call.  After the slot vector is freed, generation
    // removal from g_liveScopes makes future lookups short-circuit.
    {
        std::lock_guard<std::mutex> lk(g_scopeLock);
        for (auto & s : node->slots) s.valid = false;
        g_liveScopes.erase(node->generation);
    }

    if (node == g_topScope) {
        g_topScope = node->prev;
        delete node;
    } else {
        // Mismatch (scope dtor running out of stack order) is a programmer
        // error.  Leak the node so subsequent dtors find their state.
        // In a debug build, an assert would fire.
    }
}

ClosureHandle allocClosureHandle(EvalScope & /*scope*/, void * payload)
{
    // EvalScope is non-copyable + RAII, so the scope passed in MUST be
    // the topmost (otherwise the caller has a stack-order bug).  Read
    // g_topScope rather than poking at EvalScope::m_state -- the API
    // contract guarantees they match.
    ScopeNode * top = g_topScope;
    if (!top) return ClosureHandle{0};

    HandleSlot newSlot{payload, true};
    uint32_t slotIdx;
    {
        std::lock_guard<std::mutex> lk(g_scopeLock);
        slotIdx = static_cast<uint32_t>(top->slots.size());
        top->slots.push_back(newSlot);
    }
    return ClosureHandle{packHandle(top->generation, slotIdx)};
}

bool isValid(ClosureHandle h)
{
    auto p = unpackHandle(h.opaque);
    if (p.generation == kInvalidGen) return false;

    std::lock_guard<std::mutex> lk(g_scopeLock);
    auto it = g_liveScopes.find(p.generation);
    if (it == g_liveScopes.end()) return false;

    ScopeNode * node = it->second;
    if (p.slotIdx >= node->slots.size()) return false;
    return node->slots[p.slotIdx].valid;
}

void * lookupClosureHandle(ClosureHandle h)
{
    auto p = unpackHandle(h.opaque);
    if (p.generation == kInvalidGen) return nullptr;

    std::lock_guard<std::mutex> lk(g_scopeLock);
    auto it = g_liveScopes.find(p.generation);
    if (it == g_liveScopes.end()) return nullptr;

    ScopeNode * node = it->second;
    if (p.slotIdx >= node->slots.size()) return nullptr;
    auto & slot = node->slots[p.slotIdx];
    return slot.valid ? slot.payload : nullptr;
}

// ---------------------------------------------------------------------------
// Global handle promotion
// ---------------------------------------------------------------------------
//
// Today: GlobalClosureHandle wraps the same storage as ClosureHandle
// (the v3BridgeClosures table -- always-rooted via traceable_allocator).
// The lifetime distinction is contract-only; the table itself never
// shrinks (per existing MED-14 bounding work).  Once we migrate
// v3FormalsLambdaBridges to use ClosureHandle, releaseGlobal will
// actually free the slot.

GlobalClosureHandle promoteToGlobal(EvalScope & /*scope*/, ClosureHandle h)
{
    GlobalClosureHandle g;
    g.opaque = h.opaque;
    return g;
}

void releaseGlobal(GlobalClosureHandle /*h*/)
{
    // No-op for now -- the underlying v3BridgeClosures table is grow-
    // only.  When we migrate to a slot-recycling registry, this will
    // free the slot and any captured values.
}

// ---------------------------------------------------------------------------
// applyClosure (Sprint priority 1, FFI plan §G)
// ---------------------------------------------------------------------------
//
// Calls a v3 closure (registered via allocClosureHandle whose payload is a
// `Value *` pointing at the closure) with the given argument.  Returns
// `Fallible<Value>`: success carries the result; failure carries a
// v3::EvalError translated from a thrown C++ exception.
//
// **Threading / VMState:** allocates a fresh VMState per call.  This is
// the same pattern bridge1 (`primops.cc:3138`) already uses for TW→v3
// transitions.  Deeper integration (one VMState per EvalScope, fiber
// hand-off, BlockingFFI<T> wrapping for Port-class blocking calls)
// lands incrementally per the FFI plan.

Fallible<Value> applyClosure(EvalScope & scope, ClosureHandle h, Value arg)
{
    void * payload = lookupClosureHandle(h);
    if (!payload) {
        EvalError err;
        err.msg = "applyClosure: invalid handle (scope expired or never existed)";
        return Fallible<Value>{err};
    }
    (void)scope;  // EvalScope ownership is the validity check above.

    Value * funPtr = static_cast<Value *>(payload);

    // Allocate a fresh VMState.  Reserve sizes mirror bridge1's
    // (primops.cc:3138-3141): enough for a typical closure call without
    // re-allocation, but heap-bounded.
    VMState vm;
    vm.valueStack.reserve(64 * 1024);
    vm.frames.reserve(4096);
    vm.withStack.reserve(64);

    try {
        Value result = callClosure(vm, *funPtr, arg);
        return Fallible<Value>{result};
    } catch (const std::exception & e) {
        EvalError err;
        err.msg = e.what();
        // primaryPos / trace / suggestions left empty for now;
        // FFI plan §A5 boundary shim catches the structured nix::EvalError
        // (when v3 throws it) and copies position + trace.  See #489.
        return Fallible<Value>{err};
    }
}

// ---------------------------------------------------------------------------
// Category C: Filesystem I/O (Sprint priority 2, FFI plan §A12)
// ---------------------------------------------------------------------------
//
// Pure-filesystem reads -- no store daemon involvement.  Each function is a
// thin wrapper around the corresponding `nix::SourcePath` method, with
// exceptions translated to v3::EvalError at the boundary.  Sandbox /
// pure-eval gating is the dispatcher's concern (PrimOp flags, Cat. K).

namespace {

/// Map a SourceAccessor::Type enumerator to a stable lower-case string.
/// Matches the names builtins.readFileType uses ("regular", "directory",
/// "symlink", "unknown").  "unknown" subsumes char/block/socket/fifo so
/// the FFI surface is small; consumers that need the precise sub-kind
/// can call lstat directly.
const char * typeToString(nix::SourceAccessor::Type t)
{
    using T = nix::SourceAccessor::Type;
    switch (t) {
        case T::tRegular:   return "regular";
        case T::tDirectory: return "directory";
        case T::tSymlink:   return "symlink";
        case T::tChar:      return "unknown";
        case T::tBlock:     return "unknown";
        case T::tSocket:    return "unknown";
        case T::tFifo:      return "unknown";
        case T::tUnknown:   return "unknown";
    }
    return "unknown";
}

/// Wrap a thrown C++ exception as an EvalError at the FFI boundary.
/// Position / trace / suggestions stay empty until the §A5 structured
/// nix::EvalError catch lands (see #489).
EvalError exceptionToEvalError(const std::exception & e)
{
    EvalError err;
    err.msg = e.what();
    return err;
}

} // namespace

Fallible<std::string> readFile(nix::SourcePath path)
{
    try {
        return Fallible<std::string>{path.readFile()};
    } catch (const std::exception & e) {
        return Fallible<std::string>{exceptionToEvalError(e)};
    }
}

Fallible<std::map<std::string, std::string>> readDir(nix::SourcePath path)
{
    try {
        auto entries = path.readDirectory();
        std::map<std::string, std::string> out;
        for (auto & [name, optType] : entries) {
            // Unknown / lazy-resolution entries (some FS layers skip the
            // type lookup) are reported as "unknown" so consumers know
            // to call lstat for the precise kind.  Mirrors TW
            // primReadDir's behaviour at primops.cc:2549-2569.
            out.emplace(name,
                optType ? typeToString(*optType) : "unknown");
        }
        return Fallible<std::map<std::string, std::string>>{std::move(out)};
    } catch (const std::exception & e) {
        return Fallible<std::map<std::string, std::string>>{
            exceptionToEvalError(e)};
    }
}

bool pathExists(nix::SourcePath path)
{
    // No Fallible -- pathExists in nix:: itself returns bool and does not
    // throw on missing-path; only on permission / I/O errors which
    // surface as exceptions.  Match that contract: false on any throw.
    try {
        return path.pathExists();
    } catch (...) {
        return false;
    }
}

// findFile is a host-environment lookup (NIX_PATH search-path resolution)
// rather than a pure-filesystem op.  Implementing it requires either
// EvalState's searchPath or an injected lookup function.  Deferred to
// the EvaluatorSettings wiring (sprint priority 3); declared in ffi.hh
// for the eventual host migration.

} // namespace nix::v3
