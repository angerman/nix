/// @file
/// v3 VM dispatch loop (switch-based for now; computed-goto comes later
/// once the opcode set is stable).
///
/// Frame model:
///   - One large valueStack of Values shared across all frames.
///   - Each CallFrame has a stackBaseOffset; frame-local slots are
///     valueStack[stackBaseOffset .. stackBaseOffset + nLocals).
///   - Operand stack scratch grows beyond locals; the next frame is laid
///     out on top of it.
///
/// OP_FORCE walks: if the value is a Suspended thunk, we push a CFF_THUNK_RETURN
/// frame that runs the thunk's bytecode; on OP_RETURN, the result is written
/// into the thunk (state -> Evaluated, evaluated = result), the thunk is
/// dropped from the side-table, and the result is left on the operand stack.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/vm.hh"
#include "v3/alloc.hh"
#include "v3/primop.hh"
#include "v3/ir.hh"
#include "v3/disasm.hh"
#include "v3/errors.hh"

#include "nix/expr/eval.hh"
#include "nix/store/store-api.hh"
#include "nix/util/canon-path.hh"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <set>
#include <unordered_set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <execinfo.h>

namespace nix::v3 {

// #456 fix: forward decls for the bridge entry points used in
// OP_CALL's Bridge-thunk branch.  Defined in v3_hook.cc / primops.cc.
nix::Value * v3ToTreeWalkerPublic(nix::EvalState & nixState, Value v);
Value treeWalkerToV3Public(nix::EvalState & nixState, nix::Value & nv);

// #483 part 4 forward decls: shallow-TW-attrs RAII helpers.  Defined
// in primops.cc.  Wrap the OP_CALL Bridge result-bridge in a shallow
// guard so that an attrset returned by the TW lambda is bridged with
// per-entry Bridge thunks instead of recursive eager force.
bool pushShallowTWAttrsBridge();
void popShallowTWAttrsBridge(bool prev);

// WC-10: forward declaration at namespace scope so the `extern` use sites
// inside the anonymous namespaces below resolve to nix::v3::forceBridgeThunk
// (defined in primops.cc) rather than to a phantom anonymous-namespace symbol.
Value forceBridgeThunk(Thunk * t);

namespace {

/// REVIEW B5 — chase-vs-call-depth limits, documented.
///
/// Two distinct iteration bounds protect the VM:
///
/// 1. `kMaxIndirectionChase` (4096): max depth of Tag::Slot →
///    Tag::Thunk(eval=Slot→…) indirection chains traversed by
///    forceValue and OP_FORCE.  Fires only on pathological
///    self-referential let-rec patterns (`let x = x; in x`,
///    `let x = y; y = x; in x`) — the Black-state check catches
///    direct recursion, but SECD-style indirection cycles can
///    chase forever without re-entering the Black thunk.  Real
///    workloads have ≤4 indirections (recref + thunkify + slot +
///    memo) so 4096 is well above the practical maximum and
///    triggers only when the chain is genuinely cyclic.
///
/// 2. `kMaxCallDepth` (5000): max number of CallFrame entries on
///    `vm.frames`.  Mirrors tree-walker's recursive C-stack guard.
///    Triggered by deeply recursive evaluation (e.g., infinite
///    `let f = x: f x; in f 0` chains that aren't tail-call-
///    optimised).  Higher than the chase limit because real
///    programs do legitimately deep call stacks (cardano-node
///    library evaluation has been observed at >2000 frames).
///
/// OP_FORCE applies BOTH bounds: it traverses Tag::Slot/Thunk
/// chains (chase), and may push frames when it triggers a thunk
/// body (call depth).  OP_CALL applies only the call-depth bound —
/// it doesn't traverse indirection chains; forceValue does that
/// before OP_CALL dispatches.  The asymmetry is intentional and
/// noted here so future reviewers don't see "4096 here, 5000 there"
/// and try to "fix" by unification.
constexpr int    kMaxIndirectionChase = 4096;
constexpr size_t kMaxCallDepth        = 5000;

// V3_DBG_TRACE_THUNK_X — file-scope thunk-creation registry.  Bumped
// at every OP_MAKE_THUNK; consulted by the OP_WITH_LOOKUP cycle dump
// so we can compare each frame's *current* `t->suspended.desc`
// against the descriptor pointer that was written at creation time.
// A mismatch = in-place mutation (descriptor table relocated, union
// overlap UB, or a different writer to suspended.desc somewhere).
struct ThunkCreationInfo {
    uint32_t funcIdx;
    uint32_t codeOff;
    std::string name;
    const LambdaDescriptor * descPtr;
    const CompilationUnit * cu;
};
static const bool g_traceThunkX =
    std::getenv("V3_DBG_TRACE_THUNK_X") != nullptr;
inline std::unordered_map<const Thunk *, ThunkCreationInfo> & thunkCreationMap()
{
    static thread_local std::unordered_map<const Thunk *, ThunkCreationInfo> m;
    return m;
}

// #548c (2026-05-10): forward-declare the partial-bindings registry
// so withLookup (defined before the registry's body at line ~1077)
// can peek into it when a with-source is a Black thunk whose
// rec-attrset construction registered its partial Bindings.  The
// peek lets withLookup find a sibling entry that has already been
// SET via OP_ATTRS_REC_SET — STG-style "selector thunk" semantics
// for `with self;` over a mid-construction recAttrs.
// External linkage so gc.cc can rewrite forwarded Thunk * keys
// after a nursery scavenge.  Definition lives below at file scope
// outside the anonymous namespace.  We close the surrounding anon
// namespace so this declaration is at `nix::v3` scope (matching the
// definition); otherwise it would silently declare a separate
// internal-linkage function inside the anon namespace and conflict
// with the real definition.
} // -- close anon for partialBindingsRegistry forward decl
/// #558 (2026-05-10) per-thunk Bindings CHAIN.
///
/// The registry maps each in-progress thunk to a list of partial
/// Bindings — one per `OP_ATTRS_REC_INIT_TAIL` event that fired while
/// this thunk was on the call stack.  Walk back-to-front on lookup;
/// the first Bindings containing the looked-up name wins.
///
/// Why a chain instead of a single Bindings*: lib.extends-style fold
/// (`prev // overlay final prev`) composes layers.  Each layer's body
/// has its own tail-return AttrSet that contributes a partial set of
/// names.  A `with self;` lookup mid-eval needs to see contributions
/// from ALL layers, with later layers shadowing earlier ones for
/// shared names — exactly the // semantics.  A single Bindings* can
/// only hold one snapshot; eager-merge into a fresh Bindings copies
/// entries by-value at merge time, missing subsequent OP_ATTRS_REC_SET
/// writes into the source bindings (Bindings is allocated upfront by
/// REC_INIT_TAIL with placeholder values, then progressively SET).
///
/// The chain stores POINTERS to the original Bindings, so SETs on a
/// chain entry's bindings (via REC_SET on the same heap object) ARE
/// observed by the lookup that walks the chain.
///
/// Lookup order (back-to-front = LATEST first): mirrors lib.extends's
/// `// overlay` semantics where later layers win on key conflicts.
using PartialBindingsChain = std::vector<Bindings *>;
std::unordered_map<Thunk *, PartialBindingsChain> & partialBindingsRegistry();

/// #558 (2026-05-11): pick the most-informative chain layer (largest
/// size).  Used by every site that needs a SINGLE Bindings from a
/// chain (STG WHNF recovery, OP_ATTRS_UPDATE collapseDeferred, etc.).
/// Mirrors lookupInPartialChain's largest-layer-wins for the
/// "collapse to one Bindings" case.
///
/// Gated by NIX_V3_NO_LARGEST_WHNF=1 (reverts to chain.back()).
inline Bindings * pickLargestLayer(const PartialBindingsChain & chain)
{
    if (chain.empty()) return nullptr;
    static const bool s_noLargestWhnf =
        std::getenv("NIX_V3_NO_LARGEST_WHNF") != nullptr;
    if (s_noLargestWhnf) return chain.back();
    Bindings * pick = chain.back();
    for (auto * b : chain) {
        if (b && (!pick || b->size > pick->size))
            pick = b;
    }
    return pick;
}

/// #558: lookup a name across all Bindings in the chain.
///
/// Strategy: prefer ENTRIES FROM THE LARGEST CHAIN LAYER that contains
/// the key.  Larger layers are more likely to be actual fix-point
/// WHNF approximations (e.g. all-packages.nix's 4831-key bindings or
/// the merged 19085-key pkgs); smaller layers are typically
/// sub-attrsets (e.g. setFunctionArgs's {__functor, __functionArgs}
/// or qt5-packages.nix's `attrs` of size 4).
///
/// Falls back to "latest registration" (back-to-front) if multiple
/// layers tie on size — preserves overlay-style override semantics
/// for layers of the same size.
///
/// STG analog: when multiple shape hints are available for an indirect,
/// prefer the most informative (largest) one.  This matches GHC's
/// pattern of preferring tighter strictness/shape info.
///
/// Gated by NIX_V3_NO_LARGEST_PEEK=1 (reverts to back-to-front).
///
/// Returns nullptr if no chain entry has the name.
inline Value * lookupInPartialChain(const PartialBindingsChain & chain,
                                     SymbolId name)
{
    static const bool s_noLargest =
        std::getenv("NIX_V3_NO_LARGEST_PEEK") != nullptr;
    if (s_noLargest) {
        // Original back-to-front (LATEST-WINS).
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            if (!*it) continue;
            if (auto * v = (*it)->lookup(name)) return v;
        }
        return nullptr;
    }
    // LARGEST-LAYER-WINS: walk all layers, pick the value from the
    // largest matching layer.  Tie-break: latest (back-to-front).
    Value * best = nullptr;
    uint32_t bestSize = 0;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (!*it) continue;
        uint32_t sz = (*it)->size;
        if (best && sz <= bestSize) continue;
        if (auto * v = (*it)->lookup(name)) {
            best = v;
            bestSize = sz;
        }
    }
    return best;
}
namespace { // -- reopen anon

// #548c (2026-05-10) per-CU registry for the alloc/force atexit dump.
// V3_DBG_ALLOC_DUMP=1 enables.  At process exit, top-N descriptors
// by (allocCount + forceCount) are dumped with file:line:col, so we
// can identify hot re-instantiation sites.  Populated lazily as
// dispatchLoop sees CUs (the first OP_MAKE_THUNK with a CU pointer
// adds it to the set).
inline std::unordered_set<const CompilationUnit *> & cuRegistry()
{
    static thread_local std::unordered_set<const CompilationUnit *> s;
    return s;
}
static const bool g_dbgAllocDump =
    std::getenv("V3_DBG_ALLOC_DUMP") != nullptr;

// #548c (2026-05-10) atexit dump.  Walks every CU registered above
// and every LambdaDescriptor in each CU's lambdas vector; emits the
// top-N by (allocCount + forceCount).  Source positions are
// resolved via the global posSnapshotPool so output rows look like
//   alloc=12345 force=12345 setType@/nix/store/.../parse.nix:64:44
// suitable for one-pass scanning.
struct AllocDumpInstaller {
    AllocDumpInstaller() {
        if (g_dbgAllocDump) {
            std::atexit([] {
                struct Row {
                    uint64_t alloc;
                    uint64_t force;
                    const LambdaDescriptor * desc;
                };
                std::vector<Row> rows;
                for (auto * cu : cuRegistry()) {
                    if (!cu) continue;
                    for (const auto & ld : cu->lambdas) {
                        if (ld.allocCount == 0 && ld.forceCount == 0)
                            continue;
                        rows.push_back({ld.allocCount, ld.forceCount, &ld});
                    }
                }
                std::sort(rows.begin(), rows.end(),
                    [](const Row & a, const Row & b) {
                        return (a.alloc + a.force) > (b.alloc + b.force);
                    });
                size_t lim = std::min<size_t>(rows.size(), 50);
                std::fprintf(stderr,
                    "\nv3 V3_DBG_ALLOC_DUMP: top %zu/%zu lambdas by (alloc+force)\n",
                    lim, rows.size());
                for (size_t i = 0; i < lim; ++i) {
                    const auto & r = rows[i];
                    const auto * d = r.desc;
                    const char * name = d->name.empty()
                        ? "<anon>" : d->name.c_str();
                    const PosSnapshot * ps = resolvePosSnapshot(d->posHandle);
                    char buf[256];
                    if (ps && !ps->file.empty()) {
                        std::snprintf(buf, sizeof buf,
                            "%s:%u:%u",
                            ps->file.c_str(), ps->line, ps->column);
                    } else {
                        std::snprintf(buf, sizeof buf,
                            "<no-pos> codeOff=%u", d->codeOffset);
                    }
                    std::fprintf(stderr,
                        "  alloc=%llu force=%llu %s @ %s\n",
                        (unsigned long long)r.alloc,
                        (unsigned long long)r.force,
                        name, buf);
                }
                std::fflush(stderr);
            });
        }
    }
};
static AllocDumpInstaller s_allocDumpInstaller;

[[gnu::always_inline]]
inline Value pop(VMState & vm)
{
    Value v = vm.valueStack.back();
    vm.valueStack.pop_back();
    return v;
}

[[gnu::always_inline]]
inline Value & top(VMState & vm) { return vm.valueStack.back(); }

/// V3_DBG_FORCE_SITE diagnostic: log "OP_FORCE@ip=N site=lower.cc:LINE"
/// for each force-flavoured opcode dispatched.  Reads the side-table
/// `cu->forceEmitSites` populated by emit.cc.  The env-var check is
/// done exactly once (static-once-init) so when the var is unset the
/// branch predictor will skip this entirely — no runtime cost in the
/// default build.
///
/// `instrIp` is the bytecode offset of the force opcode itself (i.e.
/// `ip - 1` at the OP_FORCE / OP_GET_LOCAL_FORCE / OP_GET_UPVALUE_FORCE
/// entry, before any further increments).  Lookup is via std::lower_bound
/// on the (already-sorted) side-table — O(log N) where N is the number
/// of force emit sites in the CU.
/// V3_DBG_FORCE_INSIDE_X — tightly scoped force tracer for the v3-direct
/// nixpkgs eval-order RCA.  Fires only when there's a Black thunk
/// named "x" anywhere on the frame stack (== lib.fix's x_thunk being
/// forced).  Logs the forced thunk's name + codeOffset, the forcing
/// site's bytecode IP, and the immediate enclosing thunk/closure
/// frame.  Capped at 200 entries so it doesn't flood.  Useful for
/// finding the v3-specific eager force that has no TW analog —
/// compare two traces (one v3-direct + STG, one a synthetic that
/// works) and the divergent line is the smoking gun.
[[gnu::cold]]
inline void dbgLogForceInsideX(VMState & vm, const Value * forcing)
{
    static const bool s_enabled =
        std::getenv("V3_DBG_FORCE_INSIDE_X") != nullptr;
    if (__builtin_expect(!s_enabled, 1)) return;
    static thread_local int s_logged = 0;
    if (s_logged >= 2000) return;
    bool insideX = false;
    for (const auto & f : vm.frames) {
        if (!(f.flags & CFF_THUNK_RETURN)) continue;
        if (!f.thunk) continue;
        if (f.thunk->state != ThunkState::Blackhole) continue;
        const auto * d = f.thunk->suspended.desc;
        if (d && d->name == "x") { insideX = true; break; }
    }
    if (!insideX) return;
    // Filter: only log Thunk-shaped values (where the force actually
    // does work).  WHNF values (Int/Bool/Attrs/etc.) are no-ops and
    // would flood the log.  We DO want Tag::Slot since that's how
    // captured rec / lambda-param refs reach us.
    if (!forcing) return;
    Value chased = *forcing;
    if (chased.tag() == Tag::Slot && chased.payload.slot)
        chased = *chased.payload.slot;
    if (chased.tag() != Tag::Thunk && chased.tag() != Tag::App)
        return;
    // Filter: only log Suspended thunks (the FIRST force that flips
    // state to Blackhole).  Already-Evaluated thunks are harmless
    // and just flood the log.  Bridge/Blackhole are also informative.
    if (chased.tag() == Tag::Thunk && chased.payload.thunk
        && chased.payload.thunk->state == ThunkState::Evaluated)
        return;
    // Identify the forcing site: innermost frame's name + ip.
    const char * outerName = "?";
    uint32_t outerCodeOff = 0;
    uint32_t outerIp = 0;
    if (!vm.frames.empty()) {
        const auto & f = vm.frames.back();
        const LambdaDescriptor * d = nullptr;
        if (f.closure) d = f.closure->desc;
        else if (f.thunk) d = f.thunk->suspended.desc;
        if (d && !d->name.empty()) {
            outerName = d->name.c_str();
            outerCodeOff = d->codeOffset;
        }
        outerIp = f.ip;
    }
    // Identify forcee (the thunk we're about to force).
    const char * forcedName = "?";
    uint32_t forcedCodeOff = 0;
    void * forcedThunk = nullptr;
    int forcedState = -1;
    if (chased.tag() == Tag::Thunk && chased.payload.thunk) {
        forcedThunk = (void *)chased.payload.thunk;
        forcedState = (int)chased.payload.thunk->state;
        if (chased.payload.thunk->state == ThunkState::Suspended
            && chased.payload.thunk->suspended.desc) {
            const auto * d = chased.payload.thunk->suspended.desc;
            if (!d->name.empty()) forcedName = d->name.c_str();
            forcedCodeOff = d->codeOffset;
        }
    }
    std::fprintf(stderr,
        "FORCE-IN-X[%d] outer=%s@codeOff=%u ip=%u forced=%s thunk=%p codeOff=%u state=%d frames=%zu\n",
        s_logged++,
        outerName, (unsigned)outerCodeOff, (unsigned)outerIp,
        forcedName, forcedThunk, (unsigned)forcedCodeOff,
        forcedState, vm.frames.size());
}

[[gnu::cold]]
inline void dbgLogForceSite(const CompilationUnit * cu, uint32_t instrIp,
                            const Value * forcing = nullptr)
{
    static const bool s_enabled = std::getenv("V3_DBG_FORCE_SITE") != nullptr;
    if (__builtin_expect(!s_enabled, 1)) return;
    if (!cu) return;
    const auto & tbl = cu->forceEmitSites;
    // lower_bound finds the first entry with offset >= instrIp; since
    // entries are unique per offset the equality case is what we want.
    auto it = std::lower_bound(
        tbl.begin(), tbl.end(), instrIp,
        [](const std::pair<uint32_t, const char *> & e, uint32_t v) {
            return e.first < v;
        });
    const char * site = (it != tbl.end() && it->first == instrIp)
        ? it->second
        : "<unknown>";
    // Option-1 enrichment: log the thunk pointer + creation codeOffset
    // when forcing a Thunk-shape value, so post-processing can trace
    // back which Nix expression's thunk is being forced.  pkgs.X
    // thunks created via `inherit (rec {...}) X` have stable codeOffsets
    // identifiable by name in the disasm.
    //
    // Also chase one level through Tag::Slot to surface the underlying
    // thunk pointer — useful because OP_GET_LOCAL_FORCE on a let-rec
    // slot reads through Tag::Slot first.
    const Value * v = forcing;
    Value chased{};
    if (v && v->tag() == Tag::Slot && v->payload.slot) {
        chased = *v->payload.slot;
        v = &chased;
    }
    if (v && v->tag() == Tag::Thunk && v->payload.thunk) {
        const Thunk * t = v->payload.thunk;
        uint32_t codeOff = 0;
        const char * tname = "?";
        const void * thunkCu = nullptr;
        if (t->state == ThunkState::Suspended && t->suspended.desc) {
            auto * d = t->suspended.desc;
            codeOff = d->codeOffset;
            if (!d->name.empty()) tname = d->name.c_str();
            thunkCu = (const void *)t->suspended.cu;
        }
        std::fprintf(stderr,
            "OP_FORCE@ip=%u site=%s thunk=%p name=%s codeOff=%u state=%d cu=%p caller_cu=%p\n",
            (unsigned)instrIp, site, (const void *)t, tname,
            (unsigned)codeOff, (int)t->state, thunkCu, (const void *)cu);
        return;
    }
    std::fprintf(stderr, "OP_FORCE@ip=%u site=%s\n",
                 (unsigned)instrIp, site);
}

[[gnu::always_inline]]
inline void push(VMState & vm, Value v)
{
    vm.valueStack.push_back(v);
}

/// Equality with WHNF forcing — handles lazy list/attr entries.
/// Recurses on List / Attrs after forcing each element.
///
/// `insideContainer` is true when called recursively from list/attr
/// comparison: in that case Nix's "value identity optimization" allows
/// two closures to compare equal if they share the same underlying
/// Closure pointer (matches tree-walker's `if (&v1 == &v2) return true`
/// short-circuit when sibling list/attr entries point to the same
/// in-memory Value).  Top-level `f == f` always returns false because
/// the OP_EQ stack-pop holds two distinct Value structs even when their
/// payload pointer is identical.
inline bool valueEqual(VMState & vm, Value a, Value b, bool insideContainer = false)
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
        auto * la = a.payload.list;
        auto * lb = b.payload.list;
        if (la == lb) return true;
        uint32_t na = la ? la->size : 0;
        uint32_t nb = lb ? lb->size : 0;
        if (na != nb) return false;
        for (uint32_t i = 0; i < na; ++i)
            if (!valueEqual(vm, la->elems[i], lb->elems[i], /*insideContainer=*/true)) return false;
        return true;
    }
    case Tag::Attrs: {
        auto * aa = a.payload.bindings;
        auto * bb = b.payload.bindings;
        if (aa == bb) return true;
        // Special-case derivations: if both attrsets are derivations
        // (have `type = "derivation"`), compare their `outPath` fields
        // and ignore the rest.  Matches tree-walker semantics — required
        // by `eval-okay-eq-derivations` (where `drv // { dummy = 1; }`
        // still compares equal to the bare `drv`).
        static const SymbolId tyId = ir::globalInternSymbol("type");
        static const SymbolId opId = ir::globalInternSymbol("outPath");
        auto isDrv = [&](const Bindings * b) {
            if (!b) return false;
            const Value * t = b->lookup(tyId);
            if (!t) return false;
            Value tf = forceValue(vm, *t);
            return tf.isString() && std::string_view(tf.payload.str) == "derivation";
        };
        if (isDrv(aa) && isDrv(bb)) {
            const Value * pa = aa->lookup(opId);
            const Value * pb = bb->lookup(opId);
            if (pa && pb) return valueEqual(vm, *pa, *pb, /*insideContainer=*/true);
        }
        uint32_t na = aa ? aa->size : 0;
        uint32_t nb = bb ? bb->size : 0;
        if (na != nb) return false;
        for (uint32_t i = 0; i < na; ++i) {
            if (aa->entries[i].name != bb->entries[i].name) return false;
            if (!valueEqual(vm, aa->entries[i].value, bb->entries[i].value, /*insideContainer=*/true)) return false;
        }
        return true;
    }
    case Tag::Closure:
    case Tag::PrimOp:
    case Tag::PrimOpApp:
        // Direct comparison: never equal.  Inside a container: equal iff
        // the underlying pointer matches (matches Nix's value-identity
        // optimization for sibling list/attrset entries).
        if (!insideContainer) return false;
        return a.payload.closure == b.payload.closure;
    case Tag::Uninitialized:
    case Tag::Thunk:
    case Tag::App:
    case Tag::Blackhole:
    case Tag::External:
    case Tag::Slot:
    default:          return a.payload.raw == b.payload.raw;
    }
}

inline bool valueLess(VMState & vm, const Value & a, const Value & b)
{
    if (a.isInt() && b.isInt())     return a.payload.i < b.payload.i;
    if (a.isFloat() && b.isFloat()) return a.payload.f < b.payload.f;
    if (a.isInt() && b.isFloat())   return static_cast<double>(a.payload.i) < b.payload.f;
    if (a.isFloat() && b.isInt())   return a.payload.f < static_cast<double>(b.payload.i);
    if (a.isString() && b.isString())
        return std::string_view(a.payload.str) < std::string_view(b.payload.str);
    if (a.isList() && b.isList()) {
        // Lexicographic compare; matches tree-walker.  Phase-13
        // review HIGH-3 fix: force lazy elements before recursing.
        // After WC-35, mapAttrs/map install Tag::App entries; without
        // forcing, comparing `[(map id [1]) ...]` would throw the
        // "unsupported operand types" branch even for valid lists.
        uint32_t na = a.payload.list ? a.payload.list->size : 0;
        uint32_t nb = b.payload.list ? b.payload.list->size : 0;
        uint32_t n = std::min(na, nb);
        for (uint32_t i = 0; i < n; ++i) {
            Value ai = a.payload.list->elems[i];
            Value bi = b.payload.list->elems[i];
            if (ai.tag() == Tag::Thunk || ai.tag() == Tag::App
                || ai.tag() == Tag::Slot)
                ai = forceValue(vm, ai);
            if (bi.tag() == Tag::Thunk || bi.tag() == Tag::App
                || bi.tag() == Tag::Slot)
                bi = forceValue(vm, bi);
            if (valueLess(vm, ai, bi)) return true;
            if (valueLess(vm, bi, ai)) return false;
        }
        return na < nb;
    }
    throw std::runtime_error("v3 OP_LESS: unsupported operand types");
}

inline bool isTrueValue(const Value & v)
{
    if (!v.isBool()) throw std::runtime_error("v3: expected bool");
    return v.payload.i == 1;
}

/// Coerce a Value to its string representation for OP_STR_CONCAT.
/// Bring-up subset: int / float / bool / string / path / null.  Lists,
/// attrsets, and lambdas trigger an error here for now (the AST → IR pass
/// is responsible for inserting `toString` primop calls where needed).
///
/// In interpolation context (`forceString = true`) we route Path values
/// through tree-walker's `copyPathToStore` (DryRun under
/// settings.readOnlyMode = true) so `${./foo}` produces the proper
/// `/nix/store/<32-hash>-name` representation, not the absolute file
/// path.  Required by tests like `eval-okay-context` that count on the
/// store-path prefix length.
inline std::string coerceToString(const Value & v, bool forceString)
{
    switch (v.tag()) {
    case Tag::String: return std::string(v.payload.str);
    case Tag::Path: {
        std::string p(v.payload.path ? v.payload.path : "");
        if (forceString) {
            if (auto * ns = getNixEvalState()) {
                // Let copyPathToStore exceptions propagate — tree-walker
                // raises on missing paths during interpolation, and v3
                // should match.  Note for the caller: this string carries
                // an Opaque context entry for `storePath`; the caller is
                // responsible for recording it (see OP_STR_CONCAT below).
                nix::NixStringContext ctx;
                nix::SourcePath sp(ns->rootFS, nix::CanonPath(p));
                auto storePath = ns->copyPathToStore(ctx, sp);
                return ns->store->printStorePath(storePath);
            }
        }
        return p;
    }
    case Tag::Int:    return std::to_string(v.payload.i);
    case Tag::Float:  return std::to_string(v.payload.f);
    case Tag::Bool:   return v.payload.i == 1 ? "1" : "";
    case Tag::Null:   return "";
    case Tag::Uninitialized:
    case Tag::Attrs:
    case Tag::List:
    case Tag::Closure:
    case Tag::Thunk:
    case Tag::PrimOp:
    case Tag::PrimOpApp:
    case Tag::App:
    case Tag::Blackhole:
    case Tag::External:
    case Tag::Slot:
    default:
        {
            char buf[96];
            std::snprintf(buf, sizeof buf,
                "v3 STR_CONCAT: cannot coerce type to string (tag=%u)",
                (unsigned)v.tag());
            static const bool dbg = std::getenv("V3_DBG_STRCONCAT") != nullptr;
            if (dbg) {
                std::fprintf(stderr, "%s\n", buf);
                if (v.tag() == Tag::Closure && v.payload.closure && v.payload.closure->desc) {
                    auto * d = v.payload.closure->desc;
                    std::fprintf(stderr, "  closure: %s code=[%u..) nUp=%u\n",
                        !d->name.empty() ? d->name.c_str() : "<anon>",
                        d->codeOffset, d->nUpvalues);
                }
            }
            throw std::runtime_error(buf);
        }
    }
    (void)forceString;
}

inline Bindings * mergeBindings(const Bindings * a, const Bindings * b)
{
    // Sorted-merge two attrsets (b wins on duplicate keys).  Per-attr
    // positions in attrPosTable are keyed by (Bindings*, SymbolId), so
    // when an entry is copied to the freshly-allocated `out`, we
    // forward its source position too.  Without this,
    // `builtins.unsafeGetAttrPos` on a merged attrset returns null
    // for every name (REVIEW critic §8 #4).
    const uint32_t na = a->size, nb = b->size;
    Bindings * out = Alloc::allocBindings(na + nb);
    uint32_t i = 0, j = 0, k = 0;
    auto copyA = [&]() {
        out->entries[k] = a->entries[i];
        if (uint32_t p = lookupAttrPos(a, a->entries[i].name))
            recordAttrPos(out, out->entries[k].name, p);
        ++k; ++i;
    };
    auto copyB = [&]() {
        out->entries[k] = b->entries[j];
        if (uint32_t p = lookupAttrPos(b, b->entries[j].name))
            recordAttrPos(out, out->entries[k].name, p);
        ++k; ++j;
    };
    while (i < na && j < nb) {
        if (a->entries[i].name < b->entries[j].name) {
            copyA();
        } else if (a->entries[i].name > b->entries[j].name) {
            copyB();
        } else {
            copyB();   // duplicate; b wins (incl. its position)
            ++i;
        }
    }
    while (i < na) copyA();
    while (j < nb) copyB();
    out->size = k;
    return out;
}

/// Look up `name` in the with-stack, walking from top (innermost) outward.
/// Bounded below by the current frame's `withStackBase`: a closure must
/// not see its caller's `with` scopes.  `depth` is currently unused.
///
/// Each with-stack entry is forced lazily on first access — this is the
/// "delayed-with" rule.  `with pkgs; ...` inside a recursive group that
/// also defines pkgs would blackhole if we forced eagerly at
/// OP_WITH_PUSH; instead we keep the thunk on the stack and only force
/// when an unbound name actually triggers a lookup.  The forced value
/// is written back so subsequent lookups skip the force.
inline Value withLookup(VMState & vm, SymbolId name)
{
    // REVIEW §2.8: track whether any with-stack entry blackholed.  If
    // EVERY enclosing scope blackholed, the failure is "with rec { ...
    // self-referential ... }" -- report it as infinite recursion (matching
    // tree-walker's diagnostic), not "name not found in with-scope".
    bool anyBlackholed = false;
    size_t base = vm.frames.empty() ? 0 : vm.frames.back().withStackBase;
    for (size_t i = vm.withStack.size(); i-- > base; ) {
        Value & w = vm.withStack[i];
        // Tag::Slot: deref the slot pointer.  Slots are mutated when
        // their backing let-rec body publishes a result, so reading
        // through the slot here gives us the LATEST value (matches
        // tree-walker's `state.forceValue(*v2)` on slot pointers).
        if (w.tag() == Tag::Slot) {
            Value * p = w.payload.slot;
            if (!p) continue;
            // Don't write *p back into w — leave the slot pointer in
            // place for subsequent lookups (the slot may mutate again,
            // e.g. OP_APPLY_OVERRIDES grows the bindings).  Force a
            // local copy and use it for this iteration.
            Value derefed = *p;
            // #548c STG-style partial-Bindings peek: BEFORE forcing,
            // check if `derefed` is a Black thunk whose construction
            // has registered partial Bindings via
            // publishToNearestBlackThunkFrame.  If so, peek there
            // first — this is the "selector thunk on Con cell"
            // analog that lets `with self;` find sibling entries
            // during a rec-attrset's mid-construction.  No force, no
            // blackhole error: just a Bindings::lookup on the
            // already-allocated Con cell.
            if (derefed.isThunk() && derefed.payload.thunk
                && derefed.payload.thunk->state == ThunkState::Blackhole)
            {
                auto & reg = partialBindingsRegistry();
                auto it = reg.find(derefed.payload.thunk);
                if (it != reg.end()) {
                    if (auto * v = lookupInPartialChain(it->second, name))
                        return *v;
                }
            }
            if (derefed.isThunk() || derefed.tag() == Tag::App) {
                try {
                    derefed = forceValue(vm, derefed);
                } catch (const BlackholeError &) {
                    // Last-chance peek for partial Bindings (in case
                    // a deeper chase landed on a Black thunk we hadn't
                    // seen at the top level).
                    if (derefed.isThunk() && derefed.payload.thunk) {
                        auto & reg = partialBindingsRegistry();
                        auto it = reg.find(derefed.payload.thunk);
                        if (it != reg.end()) {
                            if (auto * v = lookupInPartialChain(it->second, name))
                                return *v;
                        }
                    }
                    anyBlackholed = true;
                    continue;
                }
            }
            if (!derefed.isAttrs()) continue;
            if (auto * v = derefed.payload.bindings->lookup(name))
                return *v;
            continue;
        }
        if (w.isThunk() || w.tag() == Tag::App) {
            // #458 step A.2 (slot-threading for fix-point args):
            // before forcing the whole TW Bridge thunk, try a per-
            // attribute lookup that observes a partially-constructed
            // attrset without tripping its outer-thunk BlackHole.
            // Cardano-node `with self;` over `extends overlay self`
            // shape: the partial Bindings already has the entries we
            // need; only the OUTER thunk is mid-blackhole.
            if (w.isThunk() && w.payload.thunk
                && w.payload.thunk->state == ThunkState::Bridge) {
                if (auto v = tryBridgeAttrLookup(w.payload.thunk, name))
                    return *v;
                // not found in this scope's partial bindings, OR src
                // not yet attrset-shaped.  Don't fall through to the
                // wholesale force below in the latter case (still-thunk
                // means "not yet resolvable here, try outer scope").
                anyBlackholed = true;
                continue;
            }
            try {
                w = forceValue(vm, w);
            } catch (const BlackholeError &) {
                // Delayed-with corner case: the with-stack entry
                // references something that's still being forced from
                // a deeper frame.  Skip it so outer scopes still get a
                // chance to define `name`.  Other errors propagate.
                anyBlackholed = true;
                continue;
            }
        }
        if (!w.isAttrs()) continue;
        if (auto * v = w.payload.bindings->lookup(name))
            return *v;
    }
    // §2.8: if every enclosing scope blackholed and none defined the
    // name, the actual cause is infinite recursion (cycles in `with rec`
    // attrsets), not a typo.  Throw a typed BlackholeError so callers
    // (e.g. on-demand-root's auto-eager bridge guard) can route
    // correctly, rather than a vague "name not found" runtime_error.
    if (anyBlackholed) {
        // STG-6 (#498) diagnostic: dump with-stack contents at cycle
        // throw so we can identify which name + which black-thunk
        // shape triggers the cross-VMState fix-point cycle.  Set
        // V3_DBG_WITH_CYCLE=1 to trigger.
        static const bool s_dbgWithCycle =
            std::getenv("V3_DBG_WITH_CYCLE") != nullptr;
        if (__builtin_expect(s_dbgWithCycle, 0)) {
            const auto & sym = ir::globalSymbolTable();
            std::string nm = name < sym.size() ? sym[name] : "<?>";
            // Print source position of the firing frame so we can
            // identify which AST source location triggered the cycle.
            const auto & frFire = vm.frames.back();
            const LambdaDescriptor * dFire = nullptr;
            if (frFire.thunk && (frFire.thunk->state == ThunkState::Suspended
                || frFire.thunk->state == ThunkState::Blackhole))
                dFire = frFire.thunk->suspended.desc;
            if (!dFire && frFire.closure) dFire = frFire.closure->desc;
            const PosSnapshot * psFire =
                dFire ? resolvePosSnapshot(dFire->posHandle) : nullptr;
            std::fprintf(stderr,
                "v3 OP_WITH_LOOKUP cycle: name='%s' base=%zu top=%zu vm=%p frames=%zu pos=%s:%u:%u\n",
                nm.c_str(), base, vm.withStack.size(), (void *)&vm,
                vm.frames.size(),
                (psFire && !psFire->file.empty()) ? psFire->file.c_str() : "<no-pos>",
                psFire ? psFire->line : 0u,
                psFire ? psFire->column : 0u);
            for (size_t i = vm.withStack.size(); i-- > base; ) {
                Value w = vm.withStack[i];
                std::fprintf(stderr, "  with[%zu] tag=%u",
                    i, (unsigned)w.tag());
                if (w.tag() == Tag::Slot && w.payload.slot) {
                    Value d = *w.payload.slot;
                    std::fprintf(stderr, " -> SLOT(%p)=tag=%u",
                        (void *)w.payload.slot, (unsigned)d.tag());
                    if (d.isThunk() && d.payload.thunk) {
                        std::fprintf(stderr, "(thunk=%p state=%d)",
                            (void *)d.payload.thunk,
                            (int)d.payload.thunk->state);
                    }
                } else if (w.isThunk() && w.payload.thunk) {
                    std::fprintf(stderr, " thunk=%p state=%d",
                        (void *)w.payload.thunk,
                        (int)w.payload.thunk->state);
                }
                std::fprintf(stderr, "\n");
            }
            // #546 follow-on: dump the call-frame chain so we can
            // identify WHICH thunk is forcing the with-source.  The
            // bottom of the chain holds the OP_WITH_LOOKUP-firing
            // thunk; outer frames show the cause chain leading to it.
            std::fprintf(stderr, "  frames (top=%zu, last 12):\n",
                vm.frames.size());
            size_t lim = vm.frames.size() < 12 ? 0 : vm.frames.size() - 12;
            for (size_t i = vm.frames.size(); i-- > lim; ) {
                const auto & f = vm.frames[i];
                const char * kind = (f.flags & CFF_THUNK_RETURN) ? "thunk"
                    : f.closure ? "call" : "?";
                const LambdaDescriptor * d = nullptr;
                // BUGFIX (2026-05-09): the THUNK_RETURN flag should
                // route to f.thunk's desc, not f.closure's.  Earlier
                // version preferred f.closure unconditionally, which
                // meant a frame that had BOTH set (set by some path
                // we haven't pinpointed) showed the wrong descriptor.
                //
                // suspended.desc is union-shared with `evaluated:Value`
                // / `bridgeSrc:void*`; reading it when state isn't
                // Suspended/Blackhole returns garbage (pre-state-guard
                // version was reporting bogus codeOffsets).  Fall
                // through to the closure's desc in that case.
                bool descValid = f.thunk && (
                    f.thunk->state == ThunkState::Suspended
                    || f.thunk->state == ThunkState::Blackhole);
                if (f.flags & CFF_THUNK_RETURN) {
                    if (descValid) d = f.thunk->suspended.desc;
                    else if (f.closure) d = f.closure->desc;
                } else {
                    if (f.closure) d = f.closure->desc;
                    else if (descValid) d = f.thunk->suspended.desc;
                }
                const CompilationUnit * thunkCu =
                    (descValid && f.thunk) ? f.thunk->suspended.cu : nullptr;
                // OP_TAIL_CALL retargets cur.cu/cur.closure but leaves
                // f.thunk's descriptor pointing at the original
                // thunk-body lambda.  So `d` (the THUNK descriptor)
                // names the *outer* thunk, while f.closure->desc names
                // what the frame is *actually executing*.  Print BOTH
                // when they differ -- the executing-desc + f.cu match
                // the disassembly window below; the thunk-desc is the
                // identity that OP_RETURN will deposit the result into.
                const LambdaDescriptor * exec = f.closure ? f.closure->desc : nullptr;
                bool tailCalled = exec && d && exec != d;
                std::fprintf(stderr,
                    "    [%zu] %s ip=%u thunk-name='%s' thunk-codeOff=%u",
                    i, kind, f.ip,
                    d && !d->name.empty() ? d->name.c_str() : "<anon>",
                    d ? (unsigned)d->codeOffset : 0u);
                if (tailCalled)
                    std::fprintf(stderr, " EXEC=%s codeOff=%u",
                        !exec->name.empty() ? exec->name.c_str() : "<anon>",
                        (unsigned)exec->codeOffset);
                std::fprintf(stderr,
                    " f.cu=%p thunk.cu=%p closure=%p thunk=%p flags=%x",
                    (const void *)f.cu, (const void *)thunkCu,
                    (const void *)f.closure, (const void *)f.thunk,
                    (unsigned)f.flags);
                if (f.thunk)
                    std::fprintf(stderr, " thunk=%p state=%d",
                        (void *)f.thunk, (int)f.thunk->state);
                std::fprintf(stderr, "\n");
                // V3_DBG_TRACE_THUNK_X: cross-check current desc
                // against the descriptor pointer recorded at
                // OP_MAKE_THUNK time.  If `descPtr` differs, the
                // suspended-union has been overwritten.  If `descPtr`
                // matches but `codeOffset`/`name` differ, the
                // descriptor itself was mutated in place (or the
                // cu->lambdas vector relocated its storage).  Either
                // is a hard data-corruption signal.
                if (g_traceThunkX && f.thunk) {
                    auto & m = thunkCreationMap();
                    auto it = m.find(f.thunk);
                    if (it == m.end()) {
                        std::fprintf(stderr,
                            "      [trace-x] thunk=%p NOT in creation "
                            "map (allocated outside OP_MAKE_THUNK)\n",
                            (void *)f.thunk);
                    } else {
                        const auto & ci = it->second;
                        const LambdaDescriptor * curD =
                            (f.thunk->state == ThunkState::Suspended
                              || f.thunk->state == ThunkState::Blackhole)
                            ? f.thunk->suspended.desc : nullptr;
                        bool ptrMatch  = (curD == ci.descPtr);
                        bool codeMatch = curD && curD->codeOffset == ci.codeOff;
                        bool nameMatch = curD && curD->name == ci.name;
                        std::fprintf(stderr,
                            "      [trace-x] created: fid=%u codeOff=%u "
                            "name='%s' descPtr=%p cu=%p%s%s%s\n",
                            ci.funcIdx, ci.codeOff, ci.name.c_str(),
                            (const void *)ci.descPtr,
                            (const void *)ci.cu,
                            ptrMatch ? "" : " [DESC-PTR-CHANGED]",
                            codeMatch ? "" : " [CODEOFF-CHANGED]",
                            nameMatch ? "" : " [NAME-CHANGED]");
                    }
                }
                // Disassemble around current ip for the inner-most few
                // frames so we can see the failing IR-ops + their
                // immediate predecessors (the value that became the
                // failing with-source).
                if (i + 3 >= vm.frames.size() && f.cu) {
                    uint32_t lo = f.ip > 30 ? f.ip - 30 : 0;
                    uint32_t hi = std::min<uint32_t>(f.ip + 6,
                        static_cast<uint32_t>(f.cu->code.size()));
                    if (lo < hi) {
                        std::fprintf(stderr, "      bytecode [%u-%u):\n",
                            lo, hi);
                        disassembleWindow(stderr, *f.cu, lo, hi);
                    }
                }
                // For frames with d->name=="super" but unfamiliar codeOff,
                // dump bytecode from the function's START so we can see
                // its prologue / what kind of function it is (lambda body
                // vs let-rec entry vs hidden-from-expr thunk).
                if (d && d->name == "super" && f.cu && (f.flags & CFF_THUNK_RETURN)) {
                    uint32_t lo = d->codeOffset;
                    uint32_t hi = std::min<uint32_t>(lo + 25,
                        static_cast<uint32_t>(f.cu->code.size()));
                    if (lo < hi) {
                        std::fprintf(stderr,
                            "      'super' THUNK body-start [%u-%u):\n",
                            lo, hi);
                        disassembleWindow(stderr, *f.cu, lo, hi);
                    }
                }
            }
            std::fflush(stderr);
        }
        throw BlackholeError(
            "v3 OP_WITH_LOOKUP: cycle while resolving '"
            + std::string(name < ir::globalSymbolTable().size()
                ? ir::globalSymbolTable()[name] : "<?>") + "'");
    }
    // V3_DBG_WITH: print the missing name + the with-stack contents to
    // help diagnose pure-VM nixpkgs failures where eval-order divergence
    // causes a name to be looked up before its `with` scope is visible.
    static const bool s_dbg_with = std::getenv("V3_DBG_WITH") != nullptr;
    const auto & symTab = ir::globalSymbolTable();
    std::string nm = name < symTab.size() ? symTab[name] : "<?>";
    if (s_dbg_with) {
        std::fprintf(stderr,
            "v3 OP_WITH_LOOKUP miss: name='%s' (sid=%u) base=%zu top=%zu\n",
            nm.c_str(), (unsigned)name, base, vm.withStack.size());
        for (size_t i = vm.withStack.size(); i-- > base; ) {
            Value w = vm.withStack[i];   // copy so we can chase
            void * orig_thunk = w.isThunk() ? (void *)w.payload.thunk : nullptr;
            std::fprintf(stderr, "  with[%zu] tag=%u thunk_ptr=%p",
                i, (unsigned)w.tag(), orig_thunk);
            // Chase Evaluated thunk chains and Tag::Slot derefs to find
            // the underlying attrset (or pinpoint where the chain
            // terminates in a Black thunk / non-attrset).
            int chase_lim = 8;
            while (chase_lim-- > 0) {
                if (w.tag() == Tag::Slot) {
                    Value * p = w.payload.slot;
                    std::fprintf(stderr, " -> SLOT(%p)", (void*)p);
                    if (!p) break;
                    w = *p;
                    std::fprintf(stderr, "=tag=%u", (unsigned)w.tag());
                    if (w.isThunk())
                        std::fprintf(stderr, "(ptr=%p,state=%d)",
                            (void*)w.payload.thunk, (int)w.payload.thunk->state);
                    continue;
                }
                if (w.isThunk()
                    && w.payload.thunk->state == ThunkState::Evaluated) {
                    w = w.payload.thunk->evaluated;
                    std::fprintf(stderr, " -> tag=%u", (unsigned)w.tag());
                    if (w.isThunk())
                        std::fprintf(stderr, "(ptr=%p,state=%d)",
                            (void *)w.payload.thunk, (int)w.payload.thunk->state);
                    continue;
                }
                break;
            }
            if (w.isAttrs() && w.payload.bindings) {
                auto * b = w.payload.bindings;
                std::fprintf(stderr, " attrs size=%u {", b->size);
                for (uint32_t k = 0; k < b->size && k < 30; ++k) {
                    SymbolId s = b->entries[k].name;
                    std::fprintf(stderr, "%s%s",
                        k ? "," : "",
                        s < symTab.size() ? symTab[s].c_str() : "?");
                }
                if (b->size > 30) std::fprintf(stderr, ",...");
                std::fprintf(stderr, "}");
                // Also check if name IS in this attrset (via binary search) —
                // if it is, we have a real bug (lookup failed but it's there).
                if (auto * v = b->lookup(name)) {
                    std::fprintf(stderr, " [name-IS-here-but-missed!]");
                    (void)v;
                }
            } else if (w.isThunk()) {
                Thunk * t = w.payload.thunk;
                std::fprintf(stderr, " thunk state=%d nUp=%u",
                    (int)t->state, (unsigned)t->nUpvalues);
                if (t->state == ThunkState::Suspended) {
                    auto * d = t->suspended.desc;
                    if (d)
                        std::fprintf(stderr, " %s [%u..)",
                            !d->name.empty() ? d->name.c_str() : "<anon>",
                            d->codeOffset);
                }
            } else if (w.isClosure() && w.payload.closure
                       && w.payload.closure->desc) {
                auto * d = w.payload.closure->desc;
                std::fprintf(stderr, " closure=%s nUp=%u",
                    !d->name.empty() ? d->name.c_str() : "<anon>",
                    w.payload.closure->nUpvalues);
            }
            std::fprintf(stderr, "\n");
        }
        // Dump frame stack — the failing `with` lookup happens during a
        // specific frame's body; identifying it helps localize the source.
        size_t lim = vm.frames.size();
        std::fprintf(stderr, "  frames=%zu (showing all):\n", lim);
        for (size_t i = lim; i > 0; --i) {
            const auto & fr = vm.frames[i - 1];
            const LambdaDescriptor * d = nullptr;
            // #498 fix: only read fr.thunk->suspended.desc when state
            // is Suspended/Blackhole — Evaluated/Bridge thunks have a
            // different union active, reading suspended.desc on them
            // is undefined behaviour and aborts the diagnostic before
            // printing the rest of the stack.
            if (fr.thunk && (fr.thunk->state == ThunkState::Suspended
                          || fr.thunk->state == ThunkState::Blackhole))
                d = fr.thunk->suspended.desc;
            else if (fr.closure) d = fr.closure->desc;
            std::fprintf(stderr,
                "    frame[%zu]: %s code=[%u..) ip=%u flags=%u thunk=%p withBase=%u\n",
                i - 1,
                d && !d->name.empty() ? d->name.c_str()
                    : (d ? "<anon>" : "<root>"),
                d ? d->codeOffset : 0, fr.ip,
                (unsigned)fr.flags, (void *)fr.thunk, fr.withStackBase);
            std::fflush(stderr);
        }
        // V3_DBG_WITH_DISASM=1: also dump each frame's bytecode in a
        // window around fr.ip in fr.cu (using the frame's actual
        // executing CU, not the thunk descriptor's codeOffset which
        // may be stale after OP_TAIL_CALL retargets cu/ip).
        static const bool s_dbg_disasm =
            std::getenv("V3_DBG_WITH_DISASM") != nullptr;
        // V3_DUMP_LAMBDAS=1: dump every LambdaDescriptor in EVERY
        // unique CU on the call stack with codeOffset + name.
        static const bool s_dumpLambdas =
            std::getenv("V3_DUMP_LAMBDAS") != nullptr;
        if (__builtin_expect(s_dumpLambdas, 0)) {
            std::set<const CompilationUnit *> seenCus;
            for (size_t fi = 0; fi < vm.frames.size(); ++fi) {
                const auto & fr = vm.frames[fi];
                if (!fr.cu) continue;
                if (!seenCus.insert(fr.cu).second) continue;
                std::fprintf(stderr,
                    "  V3_DUMP_LAMBDAS: cu=%p (%zu entries)\n",
                    (const void *)fr.cu, fr.cu->lambdas.size());
                for (size_t li = 0; li < fr.cu->lambdas.size(); ++li) {
                    const auto & d = fr.cu->lambdas[li];
                    std::fprintf(stderr,
                        "    L[%zu] codeOffset=%u nUp=%u name=%s\n",
                        li, (unsigned)d.codeOffset, (unsigned)d.nUpvalues,
                        d.name.empty() ? "<anon>" : d.name.c_str());
                }
            }
        }
        // V3_DUMP_RANGE=START:END dumps bytecode for an arbitrary
        // range from the failing frame's CU.  Use to inspect thunks
        // not currently on the stack (e.g., a thunk that returned
        // through CFF_FORCE_RETRY upstream of the current frame).
        static const char * s_dumpRange = std::getenv("V3_DUMP_RANGE");
        if (const char * ranges = s_dumpRange; __builtin_expect(ranges != nullptr, 0)) {
            // Dump the range from EVERY unique CU on the call stack so
            // we don't miss thunks in CUs other than vm.frames.back().cu
            // (e.g., when force-chasing across imported files).
            std::set<const CompilationUnit *> seenCusR;
            for (size_t fi = 0; fi < vm.frames.size(); ++fi) {
                const auto & fr = vm.frames[fi];
                if (!fr.cu) continue;
                if (!seenCusR.insert(fr.cu).second) continue;
                std::string s(ranges);
                size_t pos = 0;
                while (pos < s.size()) {
                    size_t comma = s.find(',', pos);
                    std::string token = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                    size_t colon = token.find(':');
                    if (colon != std::string::npos) {
                        uint32_t lo = std::strtoul(token.substr(0, colon).c_str(), nullptr, 0);
                        uint32_t hi = std::strtoul(token.substr(colon + 1).c_str(), nullptr, 0);
                        std::fprintf(stderr, "  V3_DUMP_RANGE cu=%p [%u..%u):\n",
                            (const void *)fr.cu, lo, hi);
                        disassembleWindow(stderr, *fr.cu, lo, hi);
                    }
                    if (comma == std::string::npos) break;
                    pos = comma + 1;
                }
            }
        }
        if (s_dbg_disasm) {
            for (size_t i = lim; i > 0; --i) {
                const auto & fr = vm.frames[i - 1];
                if (!fr.cu) continue;
                uint32_t fip = fr.ip;
                uint32_t lo = fip > 32 ? fip - 32 : 0;
                uint32_t hi = fip + 32;
                if (hi <= lo) continue;
                std::fprintf(stderr,
                    "  frame[%zu] cu-disasm [%u..%u):\n", i - 1, lo, hi);
                disassembleWindow(stderr, *fr.cu, lo, hi);
            }
        }
    }
    throw std::runtime_error(
        "v3 OP_WITH_LOOKUP: name '" + nm + "' not found in with-scope");
}

/// Snapshot the current frame's visible with-stack (entries from
/// `withStackBase` to top) into a fresh ListVec.  Returns nullptr when
/// no withs are currently in scope (cheap fast-path for the common case
/// of no enclosing `with`).
inline ListVec * snapshotCurrentWiths(VMState & vm)
{
    size_t base = vm.frames.empty() ? 0 : vm.frames.back().withStackBase;
    size_t top  = vm.withStack.size();
    if (top <= base) return nullptr;
    uint32_t n = static_cast<uint32_t>(top - base);
    ListVec * out = Alloc::allocList(n);
    for (uint32_t i = 0; i < n; ++i)
        out->elems[i] = vm.withStack[base + i];
    return out;
}

/// Push a closure/thunk's captured with-stack onto vm.withStack so it
/// becomes visible to the body's OP_WITH_LOOKUPs.  The caller must have
/// already set the new frame's withStackBase to vm.withStack.size()
/// BEFORE calling this so the floor is correct.
inline void pushCapturedWiths(VMState & vm, ListVec * capturedWiths)
{
    if (!capturedWiths) return;
    for (uint32_t i = 0; i < capturedWiths->size; ++i)
        vm.withStack.push_back(capturedWiths->elems[i]);
}

} // namespace

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

namespace {

/// WC-38 architectural fix attempt: walk the frame stack from BOTTOM
/// up and publish `v` to the OUTERMOST CFF_THUNK_RETURN frame whose
/// `thunk` is in Blackhole state.  Mirrors tree-walker's
/// `v.mkAttrs(...)` semantics writing into the slot of the currently-
/// forcing thunk DURING `ExprAttrs::eval`.  Sub-thunks captured-with
/// the slot will then see the (possibly intermediate) attrset rather
/// than blackholing.
///
/// We target the OUTERMOST (deepest in the stack) Black thunk, not
/// the innermost.  Reason: the inner Black thunks (e.g., a sub-thunk
/// firing INSIDE x's body) have a different final value than x.
/// Publishing to inner thunks would corrupt their evaluated.  The
/// outermost Black thunk is the one whose `with self;` capture is in
/// scope for failing sub-thunks (e.g., x in `let x = f x; in x`).
///
/// Idempotent: the eventual `OP_RETURN` of the outer thunk frame
/// will overwrite `evaluated` with the final retVal.  Risk:
/// intermediate values may be partial.  For the lib.fix bootstrap
/// pattern, the final value IS one of the intermediate attrsets,
/// so a sub-thunk reading the intermediate is reading a valid
/// snapshot.
///
/// Gated behind `NIX_V3_EARLY_PUBLISH=1`.  Default off; verified
/// non-functional for nixpkgs WC-38 and DAMAGING (corrupts slot
/// targets with intermediate values).  Kept as scaffolding only.
///
/// Option 2 from the multi-agent synthesis (publish-to-all variant)
/// was tested and rejected: writing intermediate ExprAttrs values to
/// every Black CFF_THUNK_RETURN frame's thunk wrote tiny intermediate
/// attrsets like `{prev=...}` into the lib.fix slot, replacing the
/// thunk-being-forced with a wrong-typed value.  Each thunk has its
/// OWN final value; without per-thunk destination tracking (which
/// tree-walker has via stack-local `vCur`), v3 cannot safely publish
/// intermediate values.  Outermost-only and publish-to-all both
/// corrupt nixpkgs.
/// #457/#458 partial-bindings registry.  Side-table mapping a
/// currently-being-forced (Black) Thunk to a partial Bindings*
/// produced by OP_ATTRS_REC_INIT in its body.  Used by callers that
/// need to access individual entries of a mid-construction rec-
/// attrset (specifically OP_REC_BINDING_SLOT_REF, OP_ATTRS_SELECT,
/// OP_ATTRS_HAS) without forcing the wrapping thunk.
///
/// Why a side-table instead of EARLY_PUBLISH (which writes to
/// thunk.evaluated directly): EARLY_PUBLISH corrupts nested rec-
/// attrset constructions, since each thunk has its own final value
/// and writing the partial Bindings to an outer thunk's evaluated
/// field can leave a wrong-typed value if the outer thunk's body
/// produces something else.  The side-table is safe because the
/// thunk's actual state machine is unchanged -- the side entry is
/// just a hint that callers can use opportunistically.
///
/// Lifetime: entry added at OP_ATTRS_REC_INIT (when a thunk frame
/// is on the stack); entry removed when forceValue completes the
/// body normally (transition to Evaluated) or when an exception
/// unwinds (clearBlackMarksOnException scans and clears).
//
// Defined OUTSIDE the surrounding anonymous namespace so the symbol
// has external linkage and gc.cc can call it from another TU
// (for nursery-scavenge key rewrites).
} // -- close anon namespace for partialBindingsRegistry definition
std::unordered_map<Thunk *, PartialBindingsChain> & partialBindingsRegistry()
{
    static thread_local std::unordered_map<Thunk *, PartialBindingsChain> tbl;
    return tbl;
}
namespace { // -- reopen anon namespace

/// Publish a freshly-built attrset to the innermost Black thunk frame's
/// partial-bindings side-table and (optionally) eagerly transition outer
/// Black thunks to Evaluated.  `isRecInit` MUST be true only when `v` IS
/// the rec-attrset under construction by that thunk's body — i.e., the
/// caller is OP_ATTRS_REC_INIT.  Non-rec attrset construction
/// (OP_ATTRS_INIT, OP_ATTRS_INIT_DYN, OP_ATTRS_UPDATE) MUST pass
/// isRecInit=false and the function becomes a no-op.
///
/// #495 follow-on (2026-05-07 root-cause): publishing on every attrset
/// construction (including non-rec sub-expressions like the LEFT side
/// of `LEFT // RIGHT` inside an inherit-from from-expr) pollutes the
/// outer thunk's partialBindings entry.  When a recursive force on the
/// outer thunk consults the registry, it returns the SUB-EXPRESSION's
/// bindings instead of the outer thunk's actual (in-progress) value,
/// causing closures called from sub-expressions to receive the wrong
/// argument.  Reproduced by repro-495-broader-thunkify-bug.nix (lib.
/// systems.elaborate's `inherit ({...} // platforms.select final) ...`):
/// the LEFT-side `{linux-kernel, gcc}` was being registered as final's
/// partial bindings, so `select(final)` saw `{linux-kernel, gcc}`
/// instead of the elaborated platform record and threw on
/// `final.isx86`.
///
/// Restricting publishing to OP_ATTRS_REC_INIT preserves the legitimate
/// use case (rec attrset self-reference like `rec { x = 1; y = self.x;
/// }`) while eliminating the cross-expression contamination.  The
/// kept behaviour: when a rec attrset's body is being constructed and
/// inner code re-enters via `self.X`, partialBindings recovery returns
/// the rec's in-progress Bindings (which is the SAME pointer that
/// OP_ATTRS_REC_SET writes into, so subsequent SET writes are visible
/// to the recovery path).
inline void publishToNearestBlackThunkFrame(VMState & vm, const Value & v,
                                             bool isRecInit)
{
    // STG-1 (#498/#547): publish is disabled by default.  Each
    // thunk's slot is written ONLY by its own OP_RETURN; no outer
    // thunk write-through.  STG mode (the slot mechanism) is the
    // validated path for nixpkgs.
    //
    // 2026-05-09 (#547 Phase 2): flipped default-on after the inventory
    // matrix in lode/STG_INVENTORY_2026-05-09.md showed STG fixes
    // v3-fhook on nixpkgs (BLACKHOLE → OK) without regressing any
    // synthetic / lib workload.  The legacy publish was actively
    // corrupting outer Black thunks with wrong-shape intermediate
    // values; the slot mechanism + cell-update at OP_RETURN is the
    // architecturally-correct replacement.  Set NIX_V3_NO_STG=1 to
    // restore the legacy publish path (will be deleted in a follow-up).
    static const bool s_stgMode =
        std::getenv("NIX_V3_NO_STG") == nullptr;
    // #548c (2026-05-10) STG-style early-alloc:
    //
    // Under STG mode, we keep the eager-state-flip OFF (the slot
    // mechanism + cell-update at OP_RETURN replaces it) but ENABLE
    // the partial-bindings side-table population.  The side-table
    // is the "selector thunk" analog from GHC's STG: with the
    // Bindings allocated upfront by OP_ATTRS_REC_INIT (now also
    // emitted for non-rec attrsets — see emit.cc:emitOne(AttrSet)),
    // any sub-expression that derefs the outer Black thunk via
    // Tag::Slot can peek at the partial Bindings via withLookup
    // (see vm.cc:withLookup partial-bindings peek path).  Earlier-
    // SET entries (alphabetical sort order) are already visible to
    // later-SET entries' from-expressions.
    //
    // Only register for isRecInit=true (the rec-attrset's own
    // OP_ATTRS_REC_INIT path).  This preserves #495's fix: non-rec
    // sub-attrsets inside `LEFT // RIGHT` still don't pollute the
    // outer thunk because they go through OP_ATTRS_INIT — wait —
    // **with Phase A, ALL non-rec attrsets emit through
    // OP_ATTRS_REC_INIT now**.  This is correct because the
    // side-table is only ever read by withLookup; it doesn't flip
    // the thunk's `evaluated` field, so the #495 corruption can't
    // recur.  The "wrong-shape" pollution was specifically the
    // eager state-flip writing a sub-expression's Bindings into
    // the outer thunk's `evaluated` — that write is firmly OFF
    // under STG mode (the legacy path below `if (s_stgMode) ...`
    // is bypassed).
    if (s_stgMode) {
        if (!isRecInit) return;
        Tag t = v.tag();
        if (t != Tag::Attrs || !v.payload.bindings) return;
        // OP_ATTRS_REC_INIT (non-tail-return path).  Register with the
        // INNERMOST Black thunk frame only.  This is the conservative
        // choice for sub-expression attrsets: they aren't the
        // function's eventual return value, so registering them with
        // OUTER thunks (waiting for the function's return) would
        // falsely advertise sub-expression shapes via the partial-
        // Bindings peek path (vm.cc:withLookup).
        //
        // Tail-return AttrSets emit OP_ATTRS_REC_INIT_TAIL instead,
        // which calls publishToAllThunkFrames — see that path for the
        // architectural rationale (#558).
        for (size_t i = vm.frames.size(); i > 0; --i) {
            CallFrame & fr = vm.frames[i - 1];
            if (!(fr.flags & CFF_THUNK_RETURN)) continue;
            if (!fr.thunk) continue;
            if (fr.thunk->state != ThunkState::Blackhole) continue;
            // Sub-attrset (non-tail) registration: REPLACE the chain
            // with a single-element vector containing this Bindings.
            // Sub-attrsets shouldn't accumulate — only the latest
            // sub-attrset for this innermost-Black thunk represents
            // its currently-relevant partial state.
            auto & chain = partialBindingsRegistry()[fr.thunk];
            chain.clear();
            chain.push_back(v.payload.bindings);
            static const bool s_dbg_reg =
                std::getenv("NIX_V3_DBG_PARTIAL_BINDINGS") != nullptr;
            if (s_dbg_reg) std::fprintf(stderr,
                "v3 STG partialBindings: register thunk=%p bindings=%p size=%u\n",
                (void *)fr.thunk, (void *)v.payload.bindings,
                (unsigned)v.payload.bindings->size);
            break;
        }
        return;
    }
    // ---- Pre-STG path (legacy default) ----
    static const bool s_publishNonRec =
        std::getenv("NIX_V3_PUBLISH_NON_REC_INIT") != nullptr;
    if (!isRecInit && !s_publishNonRec) return;
    // 2026-05-06 #457/#458: was opt-in (NIX_V3_EARLY_PUBLISH=1)
    // because earlier nixpkgs runs corrupted under both outermost-only
    // and publish-to-all variants.  After the #456 chase-cycle fix
    // (vm.cc forceValue Bridge→Bridge break) and the OP_CALL Bridge-
    // thunk handler, re-tested on hello.name, git.name, vim, curl,
    // coreutils, python3, nodejs, cardano-node default — all produce
    // correct output, full regression suite green, no perf regression
    // (~1% faster on cardano-node).  Flipped default-on.  Disable
    // via NIX_V3_NO_EARLY_PUBLISH=1 if a regression surfaces.
    static const bool s_disabled =
        std::getenv("NIX_V3_NO_EARLY_PUBLISH") != nullptr;
    // Always populate the partial-bindings side-table for Tag::Attrs
    // values, even if the thunk-state EARLY_PUBLISH is disabled --
    // the side-table is a safer mechanism (doesn't corrupt nested
    // thunks).  Only the eager state-flip part of EARLY_PUBLISH
    // depends on s_disabled.
    Tag t = v.tag();
    if (t == Tag::Attrs && v.payload.bindings) {
        static const bool s_dbg_reg =
            std::getenv("NIX_V3_DBG_PARTIAL_BINDINGS") != nullptr;
        // Find the nearest Black thunk frame on the stack.  Inner-
        // most-Black gets the registry entry -- it's the one whose
        // body just ran OP_ATTRS_REC_INIT.
        for (size_t i = vm.frames.size(); i > 0; --i) {
            CallFrame & fr = vm.frames[i - 1];
            if (!(fr.flags & CFF_THUNK_RETURN)) continue;
            if (!fr.thunk) continue;
            if (fr.thunk->state != ThunkState::Blackhole) continue;
            // Legacy non-STG path: replace the chain (single-element)
            // for back-compat with the old single-Bindings registry.
            auto & chain = partialBindingsRegistry()[fr.thunk];
            chain.clear();
            chain.push_back(v.payload.bindings);
            if (s_dbg_reg) std::fprintf(stderr,
                "v3 partialBindings: register thunk=%p bindings=%p size=%u\n",
                (void *)fr.thunk, (void *)v.payload.bindings,
                (unsigned)v.payload.bindings->size);
            break;  // innermost-Black only
        }
    }
    if (s_disabled) return;
    // Only publish concrete values, not thunks/apps/blackholes.
    if (t == Tag::Thunk || t == Tag::App || t == Tag::Blackhole) return;
    static const bool s_dbg =
        std::getenv("NIX_V3_EARLY_PUBLISH_DBG") != nullptr;
    static const bool s_publishAll =
        std::getenv("NIX_V3_EARLY_PUBLISH_ALL") != nullptr;
    // Default: outermost-only.  Set NIX_V3_EARLY_PUBLISH_ALL=1 for
    // publish-to-every-Black-thunk variant (also broken on nixpkgs).
    for (size_t i = 0; i < vm.frames.size(); ++i) {
        CallFrame & fr = vm.frames[i];
        if (!(fr.flags & CFF_THUNK_RETURN)) continue;
        if (!fr.thunk) continue;
        if (fr.thunk->state != ThunkState::Blackhole) continue;
        if (s_dbg) {
            const char * tagName = "?";
            uint32_t nKeys = 0;
            if (t == Tag::Attrs) {
                tagName = "Attrs";
                if (v.payload.bindings) nKeys = v.payload.bindings->size;
            } else if (t == Tag::List) {
                tagName = "List";
                if (v.payload.list) nKeys = v.payload.list->size;
            } else {
                tagName = "Other";
            }
            std::fprintf(stderr,
                "v3 EARLY_PUBLISH: frame[%zu] thunk=%p tag=%s n=%u "
                "(at frames=%zu)\n",
                i, (void*)fr.thunk, tagName, nKeys, vm.frames.size());
        }
        fr.thunk->state = ThunkState::Evaluated;
        fr.thunk->evaluated = v;
        if (!s_publishAll) return;
    }
}

/// #558: tail-return-AttrSet publish.  Register `v`'s Bindings with
/// EVERY thunk frame on the call stack — Black AND Suspended — using
/// MERGE semantics: the new registration is `//`'d (Nix attrset
/// update) over any prior registration so accumulated multi-layer
/// contributions stay visible to the partial-Bindings peek path.
///
/// Architectural rationale: lib.fix-style fix-points produce a chain
/// of nested thunks (`final → prev_outer → ... → prev_inner →
/// super_lambda`) all conceptually waiting for super's return value.
/// When `with self;` derefs through the with-source slot, it can land
/// on ANY of these thunks.  Registering super's partial Bindings with
/// each of them lets the withLookup partial-Bindings peek find the
/// entries regardless of which thunk the slot derefs to.
///
/// Suspended-state inclusion: lib.fix's `let x = f x;` keeps `x` in
/// Suspended state for the duration of `f x`'s evaluation (v3's force
/// protocol marks Black only inside the immediate forceValue
/// dispatch).  Without registering with Suspended thunks, x's slot
/// derefs would miss the registry.
///
/// MERGE-on-conflict (lib.extends multi-layer support): when a thunk
/// already has a registered Bindings (from a deeper function's
/// tail-return), MERGE the new bindings into the existing ones.
///   reg[T] = mergeBindings(reg[T], v.bindings)  // RHS wins on dup
///
/// Why merge: the lib.extends fold (`prev // overlay final prev`)
/// composes layers.  Each layer's body has its own tail-return
/// AttrSet that contributes a partial set of names.  pkgs's eventual
/// value at thunk T is the // of all layer outputs.  A `with self;`
/// lookup mid-eval needs to see ALL contributions, not just the most
/// recent layer's.
///
/// Single-Bindings-per-thunk representation: `mergeBindings` (defined
/// at vm.cc:571) is the same primitive that OP_ATTRS_UPDATE uses to
/// implement Nix's `//` operator.  It allocates a fresh combined
/// Bindings; b's entries shadow a's on duplicate keys.  This matches
/// lib.extends's overlay-wins-on-conflict semantics exactly.
///
/// Cost: O(|reg[T]| + |v|) per REC_INIT_TAIL when `reg[T]` is already
/// populated; one Bindings allocation.  In a deep extends chain
/// (~10 layers, ~4000 entries each), that's ~40k entry copies per
/// chain-level — bounded and amortized over the whole pkgs eval.
///
/// Caveat: stale entries linger until their thunk transitions to
/// Evaluated.  withLookup's peek path gates on `state == Blackhole`
/// (vm.cc:651), so an Evaluated thunk's stale entry is unreachable.
/// No correctness issue; future work could clean up at OP_RETURN.
inline void publishToAllThunkFrames(VMState & vm, const Value & v)
{
    static const bool s_stgMode =
        std::getenv("NIX_V3_NO_STG") == nullptr;
    if (!s_stgMode) return;
    Tag t = v.tag();
    if (t != Tag::Attrs || !v.payload.bindings) return;
    static const bool s_dbg_reg =
        std::getenv("NIX_V3_DBG_PARTIAL_BINDINGS") != nullptr;
    auto & reg = partialBindingsRegistry();
    // #558 (2026-05-10) NIX_V3_TAIL_REGISTER_SCOPE controls how
    // many thunk frames to register with.
    //   "all" (default): every thunk frame on the call stack.
    //     Closes lib.fix-style cycles spanning many lib.extends layers.
    //     Risk: registering with thunks ACROSS lib.fix boundaries
    //     (e.g., this AttrSet is part of stage_n's eval, but stage_n+1's
    //     thunk is also on the stack — registering with stage_n+1 is
    //     incorrect since this AttrSet isn't part of its value).
    //   "immediate": only the innermost THUNK_RETURN frame.
    //     Conservative; matches the original innermost-Black behavior.
    //     Reverts the libsForQt5 fix.
    //   "black": every Black thunk only (skip Suspended).
    //     Avoids registering with lib.fix's outer x_thunk that's
    //     Suspended-but-currently-in-an-active-force.
    static const char * s_scope_env = std::getenv("NIX_V3_TAIL_REGISTER_SCOPE");
    static const std::string s_scope = s_scope_env ? s_scope_env : "all";
    // #558 (2026-05-10) "synthetic" detection: when the running thunk
    // is a synthetic let-binding thunk (name="<thunk>") rather than a
    // real lambda body, the AttrSet is a SUB-EXPRESSION (e.g.
    // qt5-packages.nix's `attrs = { inherit (pkgs) lib; ... }`).
    // Such sub-AttrSets aren't part of any outer thunk's WHNF — they
    // shouldn't pollute outer chains.  STG-true: only register with
    // the running thunk.
    //
    // Real lambda bodies (named, non-"<thunk>") DO represent the
    // function's WHNF, and may be tail-call propagated via Tag::Slot
    // / lib.fix; register with all THUNK_RETURN frames.
    //
    // Gated by NIX_V3_NO_SYNTH_RESTRICT=1 for bisecting.
    bool runningIsSynthetic = false;
    {
        Thunk * runningThunk = nullptr;
        for (size_t i = vm.frames.size(); i > 0; --i) {
            if ((vm.frames[i - 1].flags & CFF_THUNK_RETURN)
                && vm.frames[i - 1].thunk) {
                runningThunk = vm.frames[i - 1].thunk;
                break;
            }
        }
        if (runningThunk) {
            const auto * d =
                (runningThunk->state == ThunkState::Suspended
                 || runningThunk->state == ThunkState::Blackhole)
                ? runningThunk->suspended.desc : nullptr;
            if (d && d->name == "<thunk>") runningIsSynthetic = true;
        }
    }
    static const bool s_noSynthRestrict =
        std::getenv("NIX_V3_NO_SYNTH_RESTRICT") != nullptr;
    bool restrictToImmediate =
        runningIsSynthetic && !s_noSynthRestrict;
    // Diagnostic: print the AttrSet's source position (= the
    // currently-executing thunk's lambda position).  Helps identify
    // which AttrSet expression is being TAIL-registered.
    if (s_dbg_reg) {
        // The running thunk is the topmost thunk frame.
        Thunk * runningThunk = nullptr;
        for (size_t i = vm.frames.size(); i > 0; --i) {
            if ((vm.frames[i - 1].flags & CFF_THUNK_RETURN)
                && vm.frames[i - 1].thunk) {
                runningThunk = vm.frames[i - 1].thunk;
                break;
            }
        }
        if (runningThunk) {
            const auto * d = (runningThunk->state == ThunkState::Suspended
                              || runningThunk->state == ThunkState::Blackhole)
                ? runningThunk->suspended.desc : nullptr;
            const PosSnapshot * ps =
                d ? resolvePosSnapshot(d->posHandle) : nullptr;
            std::fprintf(stderr,
                "v3 STG TAIL ORIGIN: bindings=%p size=%u pos=%s:%u:%u\n",
                (void *)v.payload.bindings,
                (unsigned)v.payload.bindings->size,
                (ps && !ps->file.empty()) ? ps->file.c_str() : "<no-pos>",
                ps ? ps->line : 0u,
                ps ? ps->column : 0u);
        }
    }
    for (size_t i = vm.frames.size(); i > 0; --i) {
        CallFrame & fr = vm.frames[i - 1];
        if (!(fr.flags & CFF_THUNK_RETURN)) continue;
        if (!fr.thunk) continue;
        // Default: register with Black AND Suspended thunks.  Evaluated
        // thunks have a final value and would be stale registrations.
        if (fr.thunk->state != ThunkState::Blackhole
            && fr.thunk->state != ThunkState::Suspended) continue;
        if (s_scope == "black"
            && fr.thunk->state != ThunkState::Blackhole) continue;
        // Append the new tail-return Bindings to this thunk's chain.
        // Duplicates (same Bindings* registered twice for one thunk)
        // are skipped to keep the chain compact; lookup walks back so
        // a duplicate at the end is harmless but wastes work.
        auto & chain = reg[fr.thunk];
        if (chain.empty() || chain.back() != v.payload.bindings) {
            chain.push_back(v.payload.bindings);
            if (s_dbg_reg) std::fprintf(stderr,
                "v3 STG partialBindings(TAIL): register thunk=%p state=%d bindings=%p size=%u (chain depth=%zu)\n",
                (void *)fr.thunk, (int)fr.thunk->state,
                (void *)v.payload.bindings,
                (unsigned)v.payload.bindings->size,
                chain.size());
        }
        if (s_scope == "immediate" || restrictToImmediate) break;
    }
}

/// Run the dispatch loop on `vm` until either:
///   - OP_HALT is reached (top-level exit), or
///   - The frame stack is popped down to `exitDepth` (used by inner
///     re-entries from callback primops to return to the caller).
/// Returns the final value (whatever was on the operand stack at exit).
Value dispatchLoop(VMState & vm, size_t exitDepth)
{
    const CallFrame & topFrame = vm.frames.back();
    const CompilationUnit * cu = topFrame.cu;
    uint32_t ip = topFrame.ip;
    const Closure * closure = topFrame.closure;
    size_t stackBase = topFrame.stackBaseOffset;

    Value finalResult{};
    finalResult.mkNull();

    // V3_DUMP_AT_START=1: on entry, dump every lambda descriptor in
    // the top frame's CU.  Useful for profiling tools that want to
    // see the full bytecode without forcing an error condition.
    // Idempotent (we'd dump on every dispatchLoop entry but the
    // outer call gates on initial entry depth).
    static const bool s_dump_at_start =
        std::getenv("V3_DUMP_AT_START") != nullptr;
    if (__builtin_expect(s_dump_at_start, 0)) [[unlikely]] {
        if (cu) {
            static std::set<const CompilationUnit *> dumpedCus;
            if (dumpedCus.insert(cu).second) {
                std::fprintf(stderr,
                    "  V3_DUMP_AT_START: cu=%p (%zu lambdas, code.size=%zu)\n",
                    (const void *)cu, cu->lambdas.size(), cu->code.size());
                for (size_t li = 0; li < cu->lambdas.size(); ++li) {
                    const auto & d = cu->lambdas[li];
                    uint32_t end = (li + 1 < cu->lambdas.size())
                        ? cu->lambdas[li + 1].codeOffset
                        : static_cast<uint32_t>(cu->code.size());
                    std::fprintf(stderr,
                        "    L[%zu] code=[%u..%u) nUp=%u nLocals=%u nWiths=%u name=%s\n",
                        li, (unsigned)d.codeOffset, end,
                        (unsigned)d.nUpvalues, (unsigned)d.nLocals,
                        (unsigned)d.nWithTargets,
                        d.name.empty() ? "<anon>" : d.name.c_str());
                    disassembleWindow(stderr, *cu, d.codeOffset, end);
                }
            }
        }
    }

    bool running = true;
    // Gate the per-instruction counter behind an env var: it adds a
    // memory write to every instruction and is only useful for
    // profiling.  Overhead on fib32 was ~3% on first-run timings.
    static const bool s_kCountInstructions = std::getenv("NIX_VM_STATS") != nullptr;
    // V3_DBG_TRACE_THUNK_BODY: per-instruction trace gated on the
    // currently-running thunk frame having a specific (codeOffset, nUp).
    // Used to nail down WC-37 frame/thunk mismatch. Format:
    //   V3_DBG_TRACE_THUNK_BODY=1346,5  ← trace any thunk with codeOffset
    //                                     1346 and nUp=5
    static const char * s_trace_env_static = std::getenv("V3_DBG_TRACE_THUNK_BODY");
    static const uint32_t s_trace_codeoff_static =
        s_trace_env_static ? static_cast<uint32_t>(std::strtoul(s_trace_env_static, nullptr, 10)) : 0;
    static const uint16_t s_trace_nup_static = []() -> uint16_t {
        const char * e = std::getenv("V3_DBG_TRACE_THUNK_BODY");
        if (!e) return 0;
        const char * comma = std::strchr(e, ',');
        if (!comma) return 0;
        return static_cast<uint16_t>(std::strtoul(comma + 1, nullptr, 10));
    }();
    // Profile (post-#062e3c502): even with [[unlikely]], the compiler
    // kept reloading the function-local statics every iteration --
    // 478 + 277 = 755 samples on the two checks (~15% of dispatchLoop
    // time on fib38).  Promote to plain function-scope const locals
    // so the loop sees them as loop-invariant load-once values; the
    // compiler then hoists them entirely out of the inner loop and
    // the trace branch becomes a single dead-code path under -O2.
    const bool kCountInstructions = s_kCountInstructions;
    const char * const s_trace_env = s_trace_env_static;
    const uint32_t s_trace_codeoff = s_trace_codeoff_static;
    const uint16_t s_trace_nup = s_trace_nup_static;
    // Cheney nursery (#434 Phase C): scavenge gate at top-of-loop.
    // We avoid the per-iteration env-var check by promoting the gate
    // to a function-scope const.  When nursery+scavenge are both on,
    // every Nth iteration we sync `ip` back to the current frame
    // (so root-walking sees consistent state) and ask the nursery
    // whether it wants to scavenge.  If it does, we re-read the
    // dispatch locals from `vm.frames.back()` because frame
    // pointers may have been forwarded in place.
    //
    // CRITICAL: scavenge ONLY runs in the outermost dispatchLoop
    // (`exitDepth == 0`).  Inner dispatchLoops are entered from
    // C++ helpers (forceValue, runOnExistingVm, runFunction) that
    // hold v3 nursery pointers in C-stack locals across the call
    // — those locals are NOT in any walked root set.  If we
    // scavenged inside an inner dispatchLoop, the outer caller's
    // popped-but-still-used `Value`s would have stale payload
    // pointers after the call returned.  By restricting scavenge
    // to the outermost loop, every C++ local that holds a
    // potentially-nursery pointer is one of:
    //   (1) bounded by an opcode handler that runs to completion
    //       within ONE iteration (no scavenge can interleave); or
    //   (2) on `vm.valueStack` / in a `vm.frames[]` slot (which
    //       the scavenger walks).
    // Cost: re-entry chains let the nursery fill to its overflow
    // ceiling before they exit; the next outer iteration will then
    // reclaim.  Acceptable because re-entry depth is bounded by
    // the call depth, and primop callbacks return promptly.
    static const bool s_kNurseryOn_static =
        std::getenv("NIX_V3_NURSERY") != nullptr
        && std::getenv("NIX_V3_NURSERY")[0] != '0';
    static const bool s_kScavengeOn_static =
        std::getenv("NIX_V3_NURSERY_SCAVENGE") != nullptr
        && std::getenv("NIX_V3_NURSERY_SCAVENGE")[0] != '0';
    const bool kNurseryGate = s_kNurseryOn_static && s_kScavengeOn_static
                              && exitDepth == 0;
    // Cache the per-thread Nursery* once per dispatchLoop entry to
    // avoid the thread_local re-resolution per iteration (Darwin's
    // tlv_atomic_thunk is cheap but not free; on fib33 the per-
    // iteration cost was visible in -fprofile-generate runs).
    // `nullptr` when the gate is off — the check below short-circuits.
    Nursery * const nursery = kNurseryGate ? &threadNursery() : nullptr;
    while (running) {
        // Phase C scavenge trigger.  Only inspected when the gate
        // is on (kNurseryGate covers env-var + exitDepth == 0).
        // The shouldScavenge() body is a small arithmetic compare;
        // under -O2 with the [[unlikely]] hint the whole branch
        // folds to a single conditional jump on the hot path.
        if (__builtin_expect(nursery != nullptr, 0)) [[unlikely]] {
            if (nursery->shouldScavenge()) {
                // Sync ip into the frame so the scavenger walks a
                // consistent VM state.  ip is a per-iteration
                // running counter; valueStack/withStack/frames are
                // already source-of-truth.
                if (!vm.frames.empty()) vm.frames.back().ip = ip;
                if (nursery->maybeScavenge(vm)) {
                    // Frame pointers may have been forwarded.  Re-
                    // read the dispatch locals from the top frame.
                    if (!vm.frames.empty()) {
                        auto & f = vm.frames.back();
                        cu        = f.cu;
                        closure   = f.closure;
                        stackBase = f.stackBaseOffset;
                        ip        = f.ip;
                    }
                }
            }
        }
        // V3_DBG_TRACE_THUNK_BODY: print this instruction if the current
        // frame is a thunk frame matching the configured codeOffset/nUp.
        // Profile (sample on fib38) showed this branch alone consumed
        // ~10% of dispatchLoop time even though it's almost always
        // false.  Mark it unlikely so the compiler keeps the cold body
        // off the fast path and predicts the branch correctly.
        if (__builtin_expect(s_trace_env != nullptr, 0)
            && !vm.frames.empty()) [[unlikely]] {
            const auto & cur = vm.frames.back();
            if (cur.thunk && (cur.flags & CFF_THUNK_RETURN)
                && cur.thunk->nUpvalues == s_trace_nup)
            {
                const auto * d = cur.thunk->suspended.desc;
                if (d && d->codeOffset == s_trace_codeoff) {
                    Instruction peek = cu->code[ip];
                    Op pop_o = decodeOp(peek);
                    uint32_t pop_n = decodeOperand(peek);
                    // Fingerprint: pointer values of the first 3 upvalues
                    // — distinguishes different thunks that happen to share
                    // pointer (Boehm GC reuse) by their upvalue contents.
                    uint64_t fp0 = cur.thunk->tail[0].tag_payload;
                    uint64_t fp1 = cur.thunk->nUpvalues > 1 ? cur.thunk->tail[1].tag_payload : 0;
                    std::fprintf(stderr,
                        "  TRACE thunk=%p state=%d ip=%u op=0x%02x operand=%u (cu=%p) fp=[%016llx,%016llx]\n",
                        (void*)cur.thunk, (int)cur.thunk->state, ip, (unsigned)pop_o, pop_n,
                        (void*)cu, (unsigned long long)fp0, (unsigned long long)fp1);
                }
            }
        }
        Instruction instr = cu->code[ip++];
        if (__builtin_expect(kCountInstructions, 0)) [[unlikely]] {
            vm.nrInstructions++;
            allocStats().bytecodeInstructions++;
        }
        Op op = decodeOp(instr);
        uint32_t operand = decodeOperand(instr);

        // -Wswitch-enum: deliberately don't list reserved opcodes
        // (OP_NOP, OP_POP, OP_SWAP, OP_NEGATE, OP_BRANCH_TRUE, OP_POS)
        // in the case table -- they hit the default abort below by
        // design.  Previous comments listed them inline; this single
        // pragma block keeps them off the unhandled-enum diagnostic.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wswitch-enum"
        switch (op) {

        // --- Literals ---
        case OP_LIT_INT: {
            int32_t imm = decodeSignedOperand(instr);
            Value v; v.mkInt(imm); push(vm, v);
            break;
        }
        case OP_LIT_INT_BIG: { Value v; v.mkInt(cu->intConstants[operand]); push(vm, v); break; }
        case OP_LIT_FLOAT: {
            Value v; v.mkFloat(cu->floatConstants[operand]); push(vm, v); break;
        }
        case OP_LIT_STR: {
            Value v;
            v.mkString(cu->stringConstants[operand].c_str());
            push(vm, v);
            break;
        }
        case OP_LIT_PATH: {
            Value v;
            v.tag_payload = static_cast<uint64_t>(Tag::Path);
            v.payload.path = cu->stringConstants[operand].c_str();
            push(vm, v);
            break;
        }
        case OP_LIT_TRUE:  push(vm, Value::vTrue);  break;
        case OP_LIT_FALSE: push(vm, Value::vFalse); break;
        case OP_LIT_NULL:  push(vm, Value::vNull);  break;

        // --- Locals / upvalues ---
        // Note: bounds checking on GET_LOCAL is omitted — the emit pass
        // + LambdaDescriptor::nLocals + the OP_CALL resize guarantee
        // every slot a function references has been pre-allocated.  A
        // bounds violation means the bytecode is corrupt; accept the
        // UB rather than pay for the check on every read.
        case OP_GET_LOCAL: {
            push(vm, vm.valueStack[stackBase + operand]);
            break;
        }
        case OP_GET_LOCAL_FORCE: {
            // Superinstruction: GET_LOCAL + FORCE.  Push the slot value
            // and apply the FORCE fast path inline.  Hot path:
            // tag != Thunk/App/Slot -> just push.  Cold paths route
            // through op_force_slow.
            //
            // Profile-guided ordering: the diagnostic env-var checks
            // (V3_DBG_FORCE_SITE / V3_DBG_GETFORCE_TAG /
            //  NIX_V3_NO_GETFORCE_SUPER) are statically false in
            // production, so we only consult them after the hot
            // fast-path bails out.
            const Value & v = vm.valueStack[stackBase + operand];
            Tag t = v.tag();
            if (__builtin_expect(t != Tag::Thunk && t != Tag::App && t != Tag::Slot, 1)) {
                push(vm, v);
                break;
            }
            // Slow path (cold): diagnostics + the slow force.  Static
            // env-var checks live here so the fast path doesn't pay
            // the load + branch on every iteration.
            dbgLogForceSite(cu, ip - 1, &vm.valueStack[stackBase + operand]);
            dbgLogForceInsideX(vm, &vm.valueStack[stackBase + operand]);
            static const bool s_skipForce =
                std::getenv("NIX_V3_NO_GETFORCE_SUPER") != nullptr;
            if (__builtin_expect(s_skipForce, 0)) [[unlikely]] {
                push(vm, v);
                break;
            }
            {
                static const char * s_dbg_gflog =
                    std::getenv("V3_DBG_GETFORCE_TAG");
                if (__builtin_expect(s_dbg_gflog != nullptr, 0)) [[unlikely]] {
                    static const size_t depthFilter =
                        std::atoll(s_dbg_gflog);
                    if (vm.frames.size() >= depthFilter) {
                        std::fprintf(stderr,
                            "v3 GLF@d%zu slot=%u tag=%u ip=%u\n",
                            vm.frames.size(), operand, (unsigned)t, ip - 1);
                    }
                }
            }
            push(vm, v);
            goto op_force_slow;
        }
        case OP_SET_LOCAL: {
            // SET keeps an auto-grow loop because some lower paths
            // (notably tryEval / inherit-from temp slots) write to a
            // slot that wasn't reserved by the function's nLocals
            // count — see eval-okay-tryeval-failed-thunk-reeval.
            // Fast path: target slot already exists below the value
            // we're about to pop.  Use `idx + 1 < size` to avoid the
            // unsigned-underflow trap when valueStack is empty (size-1
            // would wrap to SIZE_MAX and the bound check would always
            // succeed, dereferencing past the end).
            const size_t idx = stackBase + operand;
            if (__builtin_expect(idx + 1 < vm.valueStack.size(), 1)) {
                vm.valueStack[idx] = vm.valueStack.back();
                vm.valueStack.pop_back();
            } else {
                Value v = pop(vm);
                while (idx >= vm.valueStack.size())
                    vm.valueStack.push_back(Value{});
                vm.valueStack[idx] = v;
            }
            break;
        }
        case OP_GET_UPVALUE: {
            if (!closure)
                throw std::runtime_error("v3 OP_GET_UPVALUE: no closure context");
            if (operand >= closure->nUpvalues)
                throw std::runtime_error("v3 OP_GET_UPVALUE: index out of range");
            push(vm, closure->upvalues[operand]);
            break;
        }
        case OP_GET_UPVALUE_FORCE: {
            if (!closure)
                throw std::runtime_error("v3 OP_GET_UPVALUE_FORCE: no closure context");
            // Hot path: tag != Thunk/App/Slot.  Diagnostics and the
            // NIX_V3_NO_GETFORCE_SUPER gate live below the bail-out so
            // they don't pay the load + branch on every iteration.
            const Value & v = closure->upvalues[operand];
            Tag t = v.tag();
            if (__builtin_expect(t != Tag::Thunk && t != Tag::App && t != Tag::Slot, 1)) {
                push(vm, v);
                break;
            }
            // V3_DBG_FORCE_SITE trace; see OP_GET_LOCAL_FORCE.
            dbgLogForceSite(cu, ip - 1,
                operand < closure->nUpvalues ? &closure->upvalues[operand] : nullptr);
            // See OP_GET_LOCAL_FORCE — same NIX_V3_NO_GETFORCE_SUPER gate.
            static const bool s_skipForceUv =
                std::getenv("NIX_V3_NO_GETFORCE_SUPER") != nullptr;
            if (__builtin_expect(s_skipForceUv, 0)) [[unlikely]] {
                push(vm, v);
                break;
            }
            push(vm, v);
            goto op_force_slow;
        }
        case OP_DUP:  push(vm, top(vm)); break;
        // OP_POP / OP_SWAP: bytecode values reserved (don't reuse for
        // disk-cache compatibility), but no current emit path produces
        // them, so dispatch removed.  Hits abort via the default case
        // if a stale CU contains them.

        // --- Arithmetic ---
        // Int operations check for overflow via __builtin_*_overflow:
        // tree-walker raises an integer-overflow error, and v3 should
        // match.  Float operations have no such check (NaN/Inf semantics
        // mirror IEEE-754, same as tree-walker).
        case OP_ADD: {
            // Hot path mirror of OP_EQ/OP_LESS: in-place stack mutate on
            // int-int (the dominant case for the fib/ackermann/numeric-
            // loop shape) to avoid the pop+pop+push triple.  __builtin_
            // expect(int-int, 1) keeps the slow Float / mixed paths off
            // the inner-loop hot trace.
            Value & top1 = vm.valueStack.back();
            Value & top0 = vm.valueStack[vm.valueStack.size() - 2];
            if (__builtin_expect(top0.isInt() && top1.isInt(), 1)) {
                int64_t sum;
                if (__builtin_expect(__builtin_add_overflow(
                        top0.payload.i, top1.payload.i, &sum), 0))
                    throw std::runtime_error("v3 OP_ADD: integer overflow");
                vm.valueStack.pop_back();
                vm.valueStack.back().mkInt(sum);
                break;
            }
            // Slow path: Float / mixed Int-Float.
            Value rhs = pop(vm), lhs = pop(vm);
            Value r;
            if (lhs.isFloat() && rhs.isFloat())      r.mkFloat(lhs.payload.f + rhs.payload.f);
            else if (lhs.isInt() && rhs.isFloat())   r.mkFloat(static_cast<double>(lhs.payload.i) + rhs.payload.f);
            else if (lhs.isFloat() && rhs.isInt())   r.mkFloat(lhs.payload.f + static_cast<double>(rhs.payload.i));
            else throw std::runtime_error("v3 OP_ADD: type mismatch");
            push(vm, r);
            break;
        }
        case OP_SUB: {
            Value & top1 = vm.valueStack.back();
            Value & top0 = vm.valueStack[vm.valueStack.size() - 2];
            if (__builtin_expect(top0.isInt() && top1.isInt(), 1)) {
                int64_t diff;
                if (__builtin_expect(__builtin_sub_overflow(
                        top0.payload.i, top1.payload.i, &diff), 0))
                    throw std::runtime_error("v3 OP_SUB: integer overflow");
                vm.valueStack.pop_back();
                vm.valueStack.back().mkInt(diff);
                break;
            }
            Value rhs = pop(vm), lhs = pop(vm);
            Value r;
            if (lhs.isFloat() && rhs.isFloat())      r.mkFloat(lhs.payload.f - rhs.payload.f);
            else if (lhs.isInt() && rhs.isFloat())   r.mkFloat(static_cast<double>(lhs.payload.i) - rhs.payload.f);
            else if (lhs.isFloat() && rhs.isInt())   r.mkFloat(lhs.payload.f - static_cast<double>(rhs.payload.i));
            else throw std::runtime_error("v3 OP_SUB: type mismatch");
            push(vm, r);
            break;
        }
        case OP_MUL: {
            Value & top1 = vm.valueStack.back();
            Value & top0 = vm.valueStack[vm.valueStack.size() - 2];
            if (__builtin_expect(top0.isInt() && top1.isInt(), 1)) {
                int64_t prod;
                if (__builtin_expect(__builtin_mul_overflow(
                        top0.payload.i, top1.payload.i, &prod), 0))
                    throw std::runtime_error("v3 OP_MUL: integer overflow");
                vm.valueStack.pop_back();
                vm.valueStack.back().mkInt(prod);
                break;
            }
            Value rhs = pop(vm), lhs = pop(vm);
            Value r;
            if (lhs.isFloat() && rhs.isFloat())      r.mkFloat(lhs.payload.f * rhs.payload.f);
            else if (lhs.isInt() && rhs.isFloat())   r.mkFloat(static_cast<double>(lhs.payload.i) * rhs.payload.f);
            else if (lhs.isFloat() && rhs.isInt())   r.mkFloat(lhs.payload.f * static_cast<double>(rhs.payload.i));
            else throw std::runtime_error("v3 OP_MUL: unsupported types");
            push(vm, r);
            break;
        }
        case OP_DIV: {
            Value rhs = pop(vm), lhs = pop(vm);
            Value r;
            if (lhs.isInt() && rhs.isInt()) {
                if (rhs.payload.i == 0) throw std::runtime_error("v3 OP_DIV: division by zero");
                // INT64_MIN / -1 wraps around (mathematical result is
                // INT64_MAX + 1).  Match tree-walker by raising.
                if (lhs.payload.i == std::numeric_limits<int64_t>::min() && rhs.payload.i == -1)
                    throw std::runtime_error("v3 OP_DIV: integer overflow");
                r.mkInt(lhs.payload.i / rhs.payload.i);
            } else if (lhs.isFloat() && rhs.isFloat()) {
                r.mkFloat(lhs.payload.f / rhs.payload.f);
            } else if (lhs.isInt() && rhs.isFloat()) {
                r.mkFloat(static_cast<double>(lhs.payload.i) / rhs.payload.f);
            } else if (lhs.isFloat() && rhs.isInt()) {
                r.mkFloat(lhs.payload.f / static_cast<double>(rhs.payload.i));
            } else throw std::runtime_error("v3 OP_DIV: unsupported types");
            push(vm, r);
            break;
        }
        // OP_NEGATE: bytecode value reserved (don't reuse for disk-
        // cache compatibility); never emitted by lowerExpr.  Negation
        // lowers as `0 - x` via OP_SUB.  Default-case abort catches a
        // stale CU.

        // --- Comparison ---
        // Inline fast-path for the int-int case (common: `n == 0`,
        // `n < 2` etc.).  In-place mutate the deeper slot to the bool
        // result and pop the top — no helper call, no Value temporaries.
        case OP_EQ:  {
            Value & top1 = vm.valueStack.back();
            Value & top0 = vm.valueStack[vm.valueStack.size() - 2];
            if (__builtin_expect(top0.isInt() && top1.isInt(), 1)) {
                bool eq = top0.payload.i == top1.payload.i;
                vm.valueStack.pop_back();
                vm.valueStack.back() = eq ? Value::vTrue : Value::vFalse;
                break;
            }
            Value b = pop(vm), a = pop(vm); Value r; r = valueEqual(vm, a, b) ? Value::vTrue : Value::vFalse; push(vm, r); break;
        }
        case OP_NEQ: {
            Value & top1 = vm.valueStack.back();
            Value & top0 = vm.valueStack[vm.valueStack.size() - 2];
            if (__builtin_expect(top0.isInt() && top1.isInt(), 1)) {
                bool ne = top0.payload.i != top1.payload.i;
                vm.valueStack.pop_back();
                vm.valueStack.back() = ne ? Value::vTrue : Value::vFalse;
                break;
            }
            Value b = pop(vm), a = pop(vm); Value r; r = valueEqual(vm, a, b) ? Value::vFalse : Value::vTrue; push(vm, r); break;
        }
        case OP_LESS:{
            Value & top1 = vm.valueStack.back();
            Value & top0 = vm.valueStack[vm.valueStack.size() - 2];
            if (__builtin_expect(top0.isInt() && top1.isInt(), 1)) {
                bool lt = top0.payload.i < top1.payload.i;
                vm.valueStack.pop_back();
                vm.valueStack.back() = lt ? Value::vTrue : Value::vFalse;
                break;
            }
            Value b = pop(vm), a = pop(vm); Value r; r = valueLess(vm, a, b) ? Value::vTrue : Value::vFalse; push(vm, r); break;
        }

        // --- Boolean / branches ---
        // All boolean opcodes force their operand: a function arg may be
        // a thunk whose evaluated value is the bool we need to branch on.
        // Without a force, `arg || y` would peek the thunk, fail the
        // isBool check, and incorrectly fall through into the rhs block.
        case OP_NOT: {
            Value v = pop(vm);
            // Phase 13: must also force Tag::Slot — formal-arg recref
            // lookups (lower.cc:thunkifyRecAttrSelect) used to wrap the
            // slot ref behind a Thunk wrapper, so the Thunk-only check
            // sufficed.  Under NIX_V3_INLINE_REC_SLOT we get a bare
            // Tag::Slot here; without forcing, the bool check fails
            // and the wrong branch is taken.
            if (v.isThunk() || v.tag() == Tag::App || v.tag() == Tag::Slot)
                v = forceValue(vm, v);
            push(vm, isTrueValue(v) ? Value::vFalse : Value::vTrue);
            break;
        }

        case OP_AND_BRANCH: {
            // peek; if false -> jump (keep false); if true -> pop and fall through
            Value & v = vm.valueStack.back();
            if (v.isThunk() || v.tag() == Tag::App || v.tag() == Tag::Slot)
                v = forceValue(vm, v);
            if (v.isBool() && v.payload.i == 0) ip = operand;
            else                                 vm.valueStack.pop_back();
            break;
        }
        case OP_OR_BRANCH: {
            Value & v = vm.valueStack.back();
            if (v.isThunk() || v.tag() == Tag::App || v.tag() == Tag::Slot)
                v = forceValue(vm, v);
            if (v.isBool() && v.payload.i == 1) ip = operand;
            else                                 vm.valueStack.pop_back();
            break;
        }
        case OP_IMPL_BRANCH: {
            // If lhs false -> result is true; jump.  If lhs true -> pop, fall through.
            Value v = pop(vm);
            if (v.isThunk() || v.tag() == Tag::App || v.tag() == Tag::Slot)
                v = forceValue(vm, v);
            if (v.isBool() && v.payload.i == 0) { push(vm, Value::vTrue); ip = operand; }
            break;
        }

        case OP_JUMP: ip = operand; break;
        case OP_BRANCH_FALSE: {
            Value v = pop(vm);
            if (v.isThunk() || v.tag() == Tag::App || v.tag() == Tag::Slot)
                v = forceValue(vm, v);
            if (v.isBool() && v.payload.i == 0) ip = operand;
            break;
        }
        // OP_BRANCH_TRUE: bytecode value reserved; lowerer always emits
        // OP_BRANCH_FALSE with negated condition or OP_AND/OP_OR-shaped
        // branches.  Removed dispatch; default-case abort catches stale.

        // --- Closure / call / thunk ---
        case OP_MAKE_CLOSURE: {
            uint32_t funcIdx = operand;
            uint16_t nUp = static_cast<uint16_t>(cu->code[ip++]);
            // #530 lexical-with chain — second data word is the count
            // of with-target Values pushed BELOW the upvalue block on
            // the value stack.
            uint16_t nWiths = static_cast<uint16_t>(cu->code[ip++]);
            Closure * c = Alloc::allocClosure(nUp);
            allocStats().closuresAllocated++;
            c->desc = &cu->lambdas[funcIdx];
            c->cu   = cu;
            c->nUpvalues = nUp;
            // Pop upvalues first (they sit on TOP of stack), then pop
            // the with-target block beneath.  Build capturedWiths
            // outermost-first by filling reverse into the ListVec.
            for (uint16_t i = nUp; i > 0; --i) c->upvalues[i - 1] = pop(vm);
            if (nWiths > 0) {
                ListVec * lws = Alloc::allocList(nWiths);
                for (uint16_t i = nWiths; i > 0; --i)
                    lws->elems[i - 1] = pop(vm);
                c->capturedWiths = lws;
            } else {
                // No lexical `with` enclosing this lambda — fall back
                // to runtime-snapshot path for top-level cases like
                // primop bridges where the lowerer didn't see the
                // creation site (e.g., synthesized closures from
                // setNixEvalState glue).  Concrete v3-lowered code
                // will always set nWiths==0 only when there are
                // genuinely no enclosing `with` scopes, so the
                // snapshot returns nullptr too — safe overlap.
                c->capturedWiths = snapshotCurrentWiths(vm);
            }
            // #498 v2: log ALL super lambdas being made (V3_DBG_MAKE_SUPER_ALL).
            static const bool s_dbgMakeSuperAll =
                std::getenv("V3_DBG_MAKE_SUPER_ALL") != nullptr;
            if (__builtin_expect(s_dbgMakeSuperAll, 0)
                && c->desc && c->desc->name == "super") {
                std::fprintf(stderr,
                    "v3 OP_MAKE_CLOSURE super (codeOff=%u nUp=%u): cu=%p frames=%zu\n",
                    c->desc->codeOffset, (unsigned)nUp,
                    (void *)cu, vm.frames.size());
                if (!vm.frames.empty()) {
                    const auto & fr = vm.frames.back();
                    const LambdaDescriptor * d = nullptr;
                    if (fr.thunk
                        && (fr.thunk->state == ThunkState::Suspended
                            || fr.thunk->state == ThunkState::Blackhole))
                        d = fr.thunk->suspended.desc;
                    else if (fr.closure) d = fr.closure->desc;
                    std::fprintf(stderr,
                        "  maker frame: %s ip=%u flags=%u\n",
                        d && !d->name.empty() ? d->name.c_str()
                            : (d ? "<anon>" : "<root>"),
                        fr.ip, (unsigned)fr.flags);
                    // Dump the local[0] of the maker frame (= caller's arg
                    // for OP_CALL targets).
                    if (fr.stackBaseOffset < vm.valueStack.size()) {
                        Value lv = vm.valueStack[fr.stackBaseOffset];
                        Value chase = lv;
                        for (int hops = 0; hops < 4; ++hops) {
                            if (chase.tag() == Tag::Slot && chase.payload.slot)
                                chase = *chase.payload.slot;
                            else if (chase.tag() == Tag::Thunk
                                     && chase.payload.thunk
                                     && chase.payload.thunk->state == ThunkState::Evaluated)
                                chase = chase.payload.thunk->evaluated;
                            else break;
                        }
                        std::fprintf(stderr,
                            "  maker.local[0] tag=%d", (int)lv.tag());
                        if (chase.tag() == Tag::Attrs && chase.payload.bindings) {
                            std::fprintf(stderr, " -> attrs size=%u",
                                (unsigned)chase.payload.bindings->size);
                        } else {
                            std::fprintf(stderr, " -> chased.tag=%d",
                                (int)chase.tag());
                        }
                        std::fprintf(stderr, "\n");
                    }
                }
                std::fflush(stderr);
            }
            // #498: when the closure being made is named "super" with
            // 4 upvalues (matches all-packages.nix's failing inner
            // lambda), log the captured upvalues + the current frame.
            static const bool s_dbgMakeSuper =
                std::getenv("V3_DBG_MAKE_SUPER") != nullptr;
            if (__builtin_expect(s_dbgMakeSuper, 0)
                && c->desc && c->desc->name == "super"
                && nUp == 4) {
                auto chase = [](Value v, int hops) -> Value {
                    while (hops-- > 0) {
                        if (v.tag() == Tag::Slot && v.payload.slot)
                            v = *v.payload.slot;
                        else if (v.tag() == Tag::Thunk && v.payload.thunk
                                 && v.payload.thunk->state == ThunkState::Evaluated)
                            v = v.payload.thunk->evaluated;
                        else break;
                    }
                    return v;
                };
                std::fprintf(stderr,
                    "v3 OP_MAKE_CLOSURE super: cu=%p desc.codeOffset=%u "
                    "frames=%zu\n",
                    (void *)cu, c->desc->codeOffset, vm.frames.size());
                for (uint16_t i = 0; i < nUp; ++i) {
                    Value uv = c->upvalues[i];
                    Value chased = chase(uv, 4);
                    std::fprintf(stderr,
                        "  upvalue[%u]: tag=%d", i, (int)uv.tag());
                    if (chased.tag() == Tag::Attrs && chased.payload.bindings) {
                        const auto & st = ir::globalSymbolTable();
                        auto * b = chased.payload.bindings;
                        std::fprintf(stderr,
                            " -> attrs size=%u {", (unsigned)b->size);
                        for (uint32_t k = 0; k < b->size && k < 6; ++k) {
                            SymbolId nm = b->entries[k].name;
                            std::fprintf(stderr, "%s%s", k ? "," : "",
                                nm < st.size() ? st[nm].c_str() : "?");
                        }
                        if (b->size > 6) std::fprintf(stderr, ",...");
                        std::fprintf(stderr, "}");
                    } else {
                        std::fprintf(stderr, " -> tag=%d", (int)chased.tag());
                    }
                    std::fprintf(stderr, "\n");
                }
                // Print all frames.
                for (size_t fi = vm.frames.size(); fi > 0; --fi) {
                    const auto & fr = vm.frames[fi - 1];
                    const LambdaDescriptor * d = nullptr;
                    if (fr.thunk
                        && (fr.thunk->state == ThunkState::Suspended
                            || fr.thunk->state == ThunkState::Blackhole))
                        d = fr.thunk->suspended.desc;
                    else if (fr.closure) d = fr.closure->desc;
                    std::fprintf(stderr,
                        "  maker frame[%zu]: %s ip=%u flags=%u\n",
                        fi - 1,
                        d && !d->name.empty() ? d->name.c_str()
                            : (d ? "<anon>" : "<root>"),
                        fr.ip, (unsigned)fr.flags);
                }
                // Dump bytecode of the calling frame (res's thunk body) to
                // identify what immediately preceded the call to pkgs.
                if (vm.frames.size() >= 2) {
                    const auto & callerFr = vm.frames[vm.frames.size() - 2];
                    if (callerFr.cu) {
                        uint32_t lo = (callerFr.ip > 8) ? callerFr.ip - 8 : 0;
                        uint32_t hi = callerFr.ip + 4;
                        std::fprintf(stderr,
                            "  caller frame disasm cu=%p [%u..%u):\n",
                            (void*)callerFr.cu, lo, hi);
                        disassembleWindow(stderr, *callerFr.cu, lo, hi);
                        // Find caller's containing lambda
                        const auto & cuRef = *callerFr.cu;
                        uint32_t target = callerFr.ip;
                        uint32_t bestIdx = ~0u;
                        uint32_t bestOff = 0;
                        for (uint32_t li = 0; li < cuRef.lambdas.size(); ++li) {
                            uint32_t lo2 = cuRef.lambdas[li].codeOffset;
                            if (lo2 <= target && lo2 > bestOff) {
                                bestOff = lo2;
                                bestIdx = li;
                            }
                        }
                        if (bestIdx != ~0u) {
                            const auto & ld = cuRef.lambdas[bestIdx];
                            std::fprintf(stderr,
                                "  caller lambda[%u]: name=%s codeOffset=%u nUp=%u\n",
                                bestIdx,
                                ld.name.empty() ? "<anon>" : ld.name.c_str(),
                                ld.codeOffset, ld.nUpvalues);
                            // Print full body of caller's lambda (extended)
                            std::fprintf(stderr,
                                "  caller lambda body [%u..%u):\n",
                                ld.codeOffset, callerFr.ip + 30);
                            disassembleWindow(stderr, cuRef,
                                ld.codeOffset, callerFr.ip + 30);
                        }
                    }
                }
                // Also print the maker frame's locals (especially local[0],
                // which is the formal arg `pkgs` whose value should be
                // the fix-point but appears to be `{prev}`).
                if (!vm.frames.empty()) {
                    const auto & fr = vm.frames.back();
                    uint32_t nLoc = fr.thunk
                        && (fr.thunk->state == ThunkState::Suspended
                            || fr.thunk->state == ThunkState::Blackhole)
                            ? fr.thunk->suspended.desc->nLocals
                        : (fr.closure ? fr.closure->desc->nLocals : 0);
                    std::fprintf(stderr,
                        "  maker frame.local[0..min(2,%u)]:\n", nLoc);
                    for (uint32_t li = 0; li < std::min(nLoc, 2u); ++li) {
                        Value lv = vm.valueStack[fr.stackBaseOffset + li];
                        Value chased = lv;
                        for (int hops = 0; hops < 4; ++hops) {
                            if (chased.tag() == Tag::Slot && chased.payload.slot)
                                chased = *chased.payload.slot;
                            else if (chased.tag() == Tag::Thunk
                                     && chased.payload.thunk
                                     && chased.payload.thunk->state == ThunkState::Evaluated)
                                chased = chased.payload.thunk->evaluated;
                            else break;
                        }
                        std::fprintf(stderr,
                            "    local[%u]: tag=%d", li, (int)lv.tag());
                        if (chased.tag() == Tag::Attrs && chased.payload.bindings) {
                            const auto & st = ir::globalSymbolTable();
                            auto * b = chased.payload.bindings;
                            std::fprintf(stderr,
                                " -> attrs size=%u {", (unsigned)b->size);
                            for (uint32_t k = 0; k < b->size && k < 6; ++k) {
                                SymbolId nm = b->entries[k].name;
                                std::fprintf(stderr, "%s%s", k ? "," : "",
                                    nm < st.size() ? st[nm].c_str() : "?");
                            }
                            if (b->size > 6) std::fprintf(stderr, ",...");
                            std::fprintf(stderr, "}");
                        } else {
                            std::fprintf(stderr, " -> tag=%d", (int)chased.tag());
                        }
                        std::fprintf(stderr, "\n");
                    }
                }
                std::fflush(stderr);
            }
            Value v; v.mkClosure(c); push(vm, v);
            break;
        }
        case OP_MAKE_THUNK: {
            uint32_t funcIdx = operand;
            uint16_t nUp = static_cast<uint16_t>(cu->code[ip++]);
            // #530 lexical-with chain — see OP_MAKE_CLOSURE for the
            // protocol.  Second data word is the with-target count;
            // the with-targets sit BELOW the upvalues on the stack.
            uint16_t nWiths = static_cast<uint16_t>(cu->code[ip++]);
            Thunk * t = Alloc::allocThunkSuspended(nUp);
            allocStats().thunksAllocated++;
            // The "descriptor" we use is the LambdaDescriptor for the
            // referenced function (treated as 0-arg for thunks).
            // Reuse the LambdaDescriptor pointer through suspended.desc.
            t->suspended.desc = &cu->lambdas[funcIdx];
            // #548c diagnostic: track per-descriptor allocCount so the
            // atexit dump can reveal hot re-instantiation sites.
            ++cu->lambdas[funcIdx].allocCount;
            if (__builtin_expect(g_dbgAllocDump, 0)) {
                cuRegistry().insert(cu);
            }
            t->suspended.cu = cu;
            // V3_DBG_TRACE_THUNK_X -- track creation of every thunk into
            // a process-wide map (thunk_ptr -> (funcIdx, codeOff,
            // name, descPtr, cu)).  Consumed by the cycle-dump
            // diagnostic at OP_WITH_LOOKUP-cycle to verify whether the
            // thunk's current `suspended.desc` matches what it was at
            // creation time.  A mismatch is hard evidence of in-place
            // descriptor mutation (or thunk-pointer reuse).
            if (__builtin_expect(g_traceThunkX, 0)) {
                ThunkCreationInfo info{
                    funcIdx,
                    t->suspended.desc ? t->suspended.desc->codeOffset : 0u,
                    t->suspended.desc ? t->suspended.desc->name : std::string(),
                    t->suspended.desc,
                    cu};
                thunkCreationMap()[t] = info;
            }
            // V3_DBG_MK_THUNK_ANY -- log every OP_MAKE_THUNK with funcIdx,
            // descriptor name, codeOffset.  Use to verify (1) the
            // diagnostic codepath compiles in, (2) which fids are
            // actually being thunkified at runtime, and (3) whether a
            // "super"-named thunk is ever created via OP_MAKE_THUNK.
            //
            // Filtered by codeOffset to keep the log small: only prints
            // when codeOffset < 600 (== the early functions in the cu),
            // capturing the unusual codeOff=140 'super' thunk if it's
            // created here.
            static const bool s_dbgMkThunkAny =
                std::getenv("V3_DBG_MK_THUNK_ANY") != nullptr;
            if (__builtin_expect(s_dbgMkThunkAny, 0)) {
                static thread_local int s_n = 0;
                if (t->suspended.desc
                    && t->suspended.desc->name == "super") {
                    s_n++;
                    std::fprintf(stderr,
                        "MK_THUNK super[%d] cu=%p funcIdx=%u codeOff=%u "
                        "nUp=%u nWiths=%u arity=%u hasFormals=%u\n",
                        s_n, (const void *)cu, funcIdx,
                        t->suspended.desc->codeOffset,
                        nUp, nWiths,
                        t->suspended.desc->arity,
                        t->suspended.desc->hasFormals);
                }
            }
            for (uint16_t i = nUp; i > 0; --i) t->tail[i - 1] = pop(vm);
            if (nWiths > 0) {
                ListVec * lws = Alloc::allocList(nWiths);
                for (uint16_t i = nWiths; i > 0; --i)
                    lws->elems[i - 1] = pop(vm);
                t->suspended.capturedWiths = lws;
            } else {
                // No lexical with-chain at the creation site — fall
                // back to runtime snapshot for parity with synthetic
                // make-paths (see OP_MAKE_CLOSURE comment).
                t->suspended.capturedWiths = snapshotCurrentWiths(vm);
            }
            // #498: trace MK_THUNK for thunks named "res" with nUp=4
            // (the all-packages.nix `let res = ...` thunk) and dump
            // captured upvalues to identify which freeVars[1] is.
            // Filter by upvalue[3] presence of 'conflictingAttrs' attr
            // to pin the all-packages.nix one (stage.nix's let has res
            // and conflictingAttrs).
            auto chaseFn = [](Value v, int hops) -> Value {
                while (hops-- > 0) {
                    if (v.tag() == Tag::Slot && v.payload.slot)
                        v = *v.payload.slot;
                    else if (v.tag() == Tag::Thunk && v.payload.thunk
                             && v.payload.thunk->state == ThunkState::Evaluated)
                        v = v.payload.thunk->evaluated;
                    else break;
                }
                return v;
            };
            bool isStageRes = false;
            if (t->suspended.desc && t->suspended.desc->name == "res"
                && nUp == 4) {
                Value uv3chased = chaseFn(t->tail[3], 4);
                if (uv3chased.tag() == Tag::Attrs && uv3chased.payload.bindings
                    && uv3chased.payload.bindings->size == 2) {
                    auto * b = uv3chased.payload.bindings;
                    static const SymbolId conflictSym =
                        ir::globalInternSymbol("conflictingAttrs");
                    for (uint32_t k = 0; k < b->size; ++k) {
                        if (b->entries[k].name == conflictSym) {
                            isStageRes = true; break;
                        }
                    }
                }
            }
            // #498 diagnostic: trace prev-thunk MAKE_THUNK (nUp==2,
            // name=="prev") and dump captured upvalues to verify
            // tail[1] (= captured "final") IS extends.final's local[0]
            // at MAKE_THUNK time.
            static const bool s_dbgMakePrev =
                std::getenv("V3_DBG_MAKE_PREV") != nullptr;
            if (__builtin_expect(s_dbgMakePrev, 0)
                && t->suspended.desc && t->suspended.desc->name == "prev"
                && nUp == 2) {
                // Dump the maker frame's local[0] for comparison with
                // tail[1] (which should be a snapshot of local[0]).
                {
                    const auto & fr = vm.frames.back();
                    Value uv0 = vm.valueStack[fr.stackBaseOffset + 0];
                    Value chased = chaseFn(uv0, 4);
                    const LambdaDescriptor * desc = nullptr;
                    if (fr.closure) desc = fr.closure->desc;
                    else if (fr.thunk) desc = fr.thunk->suspended.desc;
                    std::fprintf(stderr,
                        "v3 OP_MAKE_THUNK prev MAKER %s codeOff=%u.local[0].tag=%d",
                        desc && !desc->name.empty() ? desc->name.c_str() : "?",
                        (unsigned)(desc ? desc->codeOffset : 0),
                        (int)uv0.tag());
                    if (chased.tag() == Tag::Attrs && chased.payload.bindings) {
                        const auto & st = ir::globalSymbolTable();
                        auto * b = chased.payload.bindings;
                        std::fprintf(stderr, " -> attrs size=%u {",
                            (unsigned)b->size);
                        for (uint32_t k = 0; k < b->size && k < 4; ++k) {
                            SymbolId nm = b->entries[k].name;
                            std::fprintf(stderr, "%s%s", k ? "," : "",
                                nm < st.size() ? st[nm].c_str() : "?");
                        }
                        std::fprintf(stderr, "}");
                    } else {
                        std::fprintf(stderr, " -> tag=%d", (int)chased.tag());
                    }
                    std::fprintf(stderr, "\n");
                }
                std::fprintf(stderr,
                    "v3 OP_MAKE_THUNK prev: thunk=%p frames=%zu\n",
                    (void *)t, vm.frames.size());
                for (uint16_t i = 0; i < nUp; ++i) {
                    Value uv = t->tail[i];
                    Value chased = chaseFn(uv, 4);
                    std::fprintf(stderr,
                        "  upvalue[%u]: tag=%d", i, (int)uv.tag());
                    if (chased.tag() == Tag::Attrs && chased.payload.bindings) {
                        const auto & st = ir::globalSymbolTable();
                        auto * b = chased.payload.bindings;
                        std::fprintf(stderr,
                            " -> attrs size=%u {", (unsigned)b->size);
                        for (uint32_t k = 0; k < b->size && k < 6; ++k) {
                            SymbolId nm = b->entries[k].name;
                            std::fprintf(stderr, "%s%s", k ? "," : "",
                                nm < st.size() ? st[nm].c_str() : "?");
                        }
                        if (b->size > 6) std::fprintf(stderr, ",...");
                        std::fprintf(stderr, "}");
                    } else if (chased.tag() == Tag::Closure
                               && chased.payload.closure
                               && chased.payload.closure->desc) {
                        std::fprintf(stderr, " -> Closure name=%s",
                            chased.payload.closure->desc->name.c_str());
                    } else {
                        std::fprintf(stderr, " -> tag=%d", (int)chased.tag());
                    }
                    std::fprintf(stderr, "\n");
                }
                std::fflush(stderr);
            }
            static const bool s_dbgMakeRes =
                std::getenv("V3_DBG_MAKE_RES") != nullptr;
            if (__builtin_expect(s_dbgMakeRes, 0) && isStageRes) {
                std::fprintf(stderr,
                    "v3 OP_MAKE_THUNK res (stage.nix): cu=%p thunk=%p "
                    "frames=%zu\n",
                    (void *)cu, (void *)t, vm.frames.size());
                for (uint16_t i = 0; i < nUp; ++i) {
                    Value uv = t->tail[i];
                    Value chased = chaseFn(uv, 4);
                    std::fprintf(stderr,
                        "  upvalue[%u]: tag=%d", i, (int)uv.tag());
                    if (chased.tag() == Tag::Attrs && chased.payload.bindings) {
                        const auto & st = ir::globalSymbolTable();
                        auto * b = chased.payload.bindings;
                        std::fprintf(stderr,
                            " -> attrs size=%u {", (unsigned)b->size);
                        for (uint32_t k = 0; k < b->size && k < 6; ++k) {
                            SymbolId nm = b->entries[k].name;
                            std::fprintf(stderr, "%s%s", k ? "," : "",
                                nm < st.size() ? st[nm].c_str() : "?");
                        }
                        if (b->size > 6) std::fprintf(stderr, ",...");
                        std::fprintf(stderr, "}");
                    } else {
                        std::fprintf(stderr, " -> tag=%d", (int)chased.tag());
                    }
                    std::fprintf(stderr, "\n");
                }
                std::fflush(stderr);
            }
            Value v;
            v.tag_payload = static_cast<uint64_t>(Tag::Thunk);
            v.payload.thunk = t;
            push(vm, v);
            break;
        }
        case OP_CALL: {
            op_call_dispatch:
            // A non-tail call resets the tail-iteration counter — any
            // subsequent runaway recursion is bounded against the
            // 5000-frame stack guard, not the tail-call counter.
            vm.tailCallCount = 0;
            Value arg = pop(vm), fun = pop(vm);
            // V3_DBG_FINAL_CALL=1: log Apply of extends's `final:`
            // lambda OR allPackages's `self:` outer lambda — tracing
            // the broader-thunkify upvalue bug at #498.  Cache the
            // env-var lookup as `static const bool` so OP_CALL — the
            // hottest opcode on fib/ackermann/lib.foldl' — doesn't
            // pay a libc getenv call per iteration.  Same pattern as
            // s_dbgOpCall below.
            static const bool s_dbgFinalCall =
                std::getenv("V3_DBG_FINAL_CALL") != nullptr;
            if (__builtin_expect(s_dbgFinalCall, 0)
                && fun.tag() == Tag::Closure && fun.payload.closure
                && fun.payload.closure->desc
                && (fun.payload.closure->desc->name == "final"
                    || fun.payload.closure->desc->name == "self"
                    || fun.payload.closure->desc->name == "rattrs"
                    || fun.payload.closure->desc->name == "overlay")) {
                Value chase = arg;
                int hops = 0;
                while (hops < 4) {
                    if (chase.tag() == Tag::Slot && chase.payload.slot)
                        chase = *chase.payload.slot;
                    else if (chase.tag() == Tag::Thunk && chase.payload.thunk
                             && chase.payload.thunk->state == ThunkState::Evaluated)
                        chase = chase.payload.thunk->evaluated;
                    else break;
                    ++hops;
                }
                std::fprintf(stderr,
                    "v3 OP_CALL %s-lambda: arg.tag=%d",
                    fun.payload.closure->desc->name.c_str(),
                    (int)arg.tag());
                if (chase.tag() == Tag::Attrs && chase.payload.bindings) {
                    auto * b = chase.payload.bindings;
                    std::fprintf(stderr, " -> attrs size=%u", b->size);
                } else {
                    std::fprintf(stderr, " -> tag=%d", (int)chase.tag());
                }
                std::fprintf(stderr, " (frames=%zu)\n", vm.frames.size());
            }
            // V3_DBG_OP_CALL=1 logs every OP_CALL with the closure
            // name + arg shape.  Used to trace the broader-thunkify
            // upvalue bug.
            static const bool s_dbgOpCall =
                std::getenv("V3_DBG_OP_CALL") != nullptr;
            if (s_dbgOpCall) {
                const char * fname = "<?>";
                if (fun.tag() == Tag::Closure && fun.payload.closure
                    && fun.payload.closure->desc)
                    fname = fun.payload.closure->desc->name.c_str();
                int arg_tag = (int)arg.tag();
                int arg_size = -1;
                if (arg.tag() == Tag::Attrs && arg.payload.bindings)
                    arg_size = arg.payload.bindings->size;
                std::fprintf(stderr,
                    "v3 OP_CALL: name=%s fun.tag=%d arg.tag=%d size=%d frames=%zu\n",
                    fname, (int)fun.tag(), arg_tag, arg_size,
                    vm.frames.size());
            }

            // Force `fun` if it's a deferred shape (Tag::App from lazy
            // primops like mapAttrs, or a Thunk that lazy attr access
            // produced).  Tree-walker's `callFunction` does the same up
            // front; mirroring it here keeps the rest of the dispatch
            // simple and avoids the OP_RETURN-chase cycle problem (where
            // chasing while the outer thunk is still Black trips
            // infinite-recursion).  Cheap on the hot path: one tag
            // check on already-WHNF callables.
            // Fast path: a Tag::Thunk in Evaluated state caches its
            // resolved value in `evaluated`.  Most calls under
            // thunkifyRecAttrSelect (Phase-5-off rec-attr access) hit
            // this case after the first force, so chasing one Thunk
            // hop inline saves a forceValue() call + its full chase
            // setup (depth guards, iteration bound, etc.).
            if (Tag fT = fun.tag(); fT == Tag::Thunk) {
                Thunk * t = fun.payload.thunk;
                if (t->state == ThunkState::Evaluated) {
                    Value e = t->evaluated;
                    Tag eT = e.tag();
                    // If the evaluated value itself is in WHNF (the
                    // common case for rec-attr-thunk-of-Closure), we're
                    // done.  Else fall through to the full forceValue.
                    if (eT != Tag::Thunk && eT != Tag::App && eT != Tag::Slot) {
                        fun = e;
                    } else {
                        vm.frames.back().ip = ip;
                        fun = forceValue(vm, fun);
                    }
                } else {
                    vm.frames.back().ip = ip;
                    fun = forceValue(vm, fun);
                }
            } else if (fT == Tag::App || fT == Tag::Slot) {
                vm.frames.back().ip = ip;
                fun = forceValue(vm, fun);
            }

            // PrimOp / PrimOpApp partial application.
            if (fun.isPrimOp() || fun.tag() == Tag::PrimOpApp) {
                // Walk the PrimOpApp chain to find the root PrimOp and
                // collect the previously-applied args.
                Value cur = fun;
                size_t depth = 0;
                while (cur.tag() == Tag::PrimOpApp) { ++depth; cur = cur.payload.pair->left; }
                if (!cur.isPrimOp())
                    throw std::runtime_error("v3 OP_CALL: PrimOpApp chain doesn't terminate in a PrimOp");
                const PrimOp * po = cur.payload.primop;
                size_t totalArgs = depth + 1;
                if (totalArgs < po->arity) {
                    // Build a new PrimOpApp wrapping (fun, arg).
                    ValuePair * vp = Alloc::allocPair();
                    vp->left = fun;
                    vp->right = arg;
                    Value v;
                    v.tag_payload = static_cast<uint64_t>(Tag::PrimOpApp);
                    v.payload.pair = vp;
                    push(vm, v);
                    break;
                }
                if (totalArgs > po->arity)
                    throw std::runtime_error("v3 OP_CALL: too many args for primop");
                // Collect args in [arg_0, arg_1, ..., arg_{N-1}, arg] order.
                Value buf[8];
                if (po->arity > 8) throw std::runtime_error("v3 OP_CALL: primop arity > 8");
                buf[totalArgs - 1] = arg;
                Value chain = fun;
                for (size_t i = totalArgs - 1; i > 0; --i) {
                    buf[i - 1] = chain.payload.pair->right;
                    chain = chain.payload.pair->left;
                }
                vm.frames.back().ip = ip;
                // PrimOpApp accumulates args lazily — primops expect
                // WHNF, so force each here before invoking, EXCEPT for
                // args the primop has explicitly opted out of via its
                // `lazyArgs` bitmask (e.g., addErrorContext's value arg
                // — see lib/modules.nix's
                // `config = addErrorContext "..." config` cycle).
                for (uint32_t i = 0; i < po->arity; ++i) {
                    if (po->lazyArgs & (1u << i)) continue;
                    buf[i] = forceValue(vm, buf[i]);
                }
                bumpPrimOpCallCount(po);
                EvalState state; state.vm = &vm;
                Value out;
                po->fn(state, buf, out);
                push(vm, out);
                break;
            }

            // #456 fix: callee is a Bridge thunk wrapping a TW
            // lambda.  v3 can't directly execute TW lambda bytecode,
            // so route the call back through TW's callFunction.
            // Mirrors the OP_CALL flow for v3 closures, but at the
            // boundary: bridge fun + arg to TW, call, bridge result
            // back to v3.
            //
            // Without this, an OP_CALL on a Bridge thunk falls into
            // the "not a closure" error below.  This case arises
            // whenever v3 evaluates a body that calls a function
            // captured from TW (e.g. lib.foldl' under cardano-node /
            // hello.name flake-eval shapes).
            //
            // Wrap in try/catch: a TW BlackHole here means the
            // surrounding v3 evaluation tripped a fix-point cycle the
            // existing eager-bridge / call-hook fallback machinery
            // handles when allowed to bubble up.  Propagate the
            // exception so v3CallFunctionEntry's outer catch
            // (v3_hook.cc:3183) blacklists the lambda + falls back
            // to TW for the whole call.  Also catch generic
            // exceptions so we don't silently corrupt the
            // BridgeShimVm's frame state.
            if (fun.isThunk() && fun.payload.thunk
                && fun.payload.thunk->state == ThunkState::Bridge
                && fun.payload.thunk->bridgeSrc) {
                if (!getNixEvalState())
                    throw std::runtime_error(
                        "v3 OP_CALL: bridge-thunk call needs a TW EvalState");
                auto * ns = getNixEvalState();
                auto * funTw = static_cast<nix::Value *>(
                    fun.payload.thunk->bridgeSrc);
                // #466 active-v3-vm tracking: announce that this vm is
                // bridging out, so any TW callback into v3 hooks can
                // detect the re-entry and refuse cycle-prone paths
                // (lambda-skip's body_fid invocation in particular).
                ScopedActiveV3VM _activeV3VM(&vm);
                ns->forceValue(*funTw, nix::noPos);

                // #466 OP_CALL Bridge round-trip elimination.
                //
                // After forcing funTw, check if it's actually a v3
                // closure that was bridged TO TW via v3ToTreeWalker —
                // shape `mkPrimOpApp(__v3_call_bridge_1, vHandle)`.
                // If so, dispatch directly via callClosure on THIS
                // vm, skipping ns->callFunction entirely.
                //
                // Why this matters: the previous path created a fresh
                // VMState (via ns->callFunction → v3 hook → bridge1
                // shortcut → fresh VMState).  When the OUTER thunk's
                // body was Black-marked on this vm's frames, the
                // fresh VMState's force on that thunk saw Black and
                // threw — the cross-VMState force scenario at the
                // root of the lambda-skip cycle (#466 memory).
                // Staying on this vm preserves the Black ancestry so
                // the cycle is detected locally and unwinds cleanly.
                Value v3Fn;
                static const bool s_disabled =
                    std::getenv("NIX_V3_NO_OP_CALL_BRIDGE_SHORTCUT") != nullptr;
                if (!s_disabled && tryUnwrapBridge1Closure(*funTw, v3Fn)) {
                    // #466 / #479 Phase 1: participate in the cross-
                    // primop force chain.  The shortcut keeps work on
                    // THIS vm (good for cycle locality), but a chain
                    // of Bridge thunks each unwrapping to a different
                    // closure can still grow C-stack unboundedly.  Key
                    // by funTw pointer -- the same TW closure value
                    // re-appearing on the chain IS the cycle.
                    ForceChainGuard _fcg(
                        ForceChainOp::OpCallBridge,
                        reinterpret_cast<uint64_t>(funTw));
                    if (_fcg.isCycle()) {
                        throw BlackholeError(
                            _fcg.atDepthCeiling()
                            ? std::string("v3 OP_CALL bridge: force-chain depth ceiling reached")
                            : std::string("v3 OP_CALL bridge: force-chain cycle"));
                    }
                    // Local v3 dispatch on this vm.  No TW round-trip,
                    // no fresh VMState.
                    Value out = callClosure(vm, v3Fn, arg);
                    push(vm, out);
                    break;
                }

                // STG-14a (#509/#515): direct v3 dispatch for TW lambdas
                // with v3 bodies in the subCache.  Avoids the
                // `v3ToTreeWalkerPublic(arg)` step below -- which today
                // forces a v3 Tag::Slot/Tag::Thunk arg eagerly (the
                // STG-12 cycle source on nixpkgs hello.name under
                // STG_KEEP_HOOKS).  When this shortcut applies, the v3
                // arg flows through unchanged (slot identity preserved
                // all the way into the body) and runLambda dispatches
                // the bytecode locally.
                static const bool s_twLambdaShortcutDisabled =
                    std::getenv("NIX_V3_NO_TW_LAMBDA_INV3") != nullptr;
                if (!s_twLambdaShortcutDisabled) {
                    Value v3Out;
                    if (tryDispatchTWLambdaInV3(*ns, *funTw, arg, v3Out)) {
                        push(vm, v3Out);
                        break;
                    }
                }

                // Bridge arg back to TW.  v3->TW preserves identity
                // for Bridge thunks (unwraps to original) and converts
                // scalars / composites otherwise.
                nix::Value * argTw = v3ToTreeWalkerPublic(*ns, arg);
                if (!argTw)
                    throw std::runtime_error(
                        "v3 OP_CALL: bridge-thunk arg failed v3->TW bridge");
                static const bool s_dbg =
                    std::getenv("V3_DBG_OPCALL_BRIDGE") != nullptr;
                if (s_dbg) {
                    int twType = funTw->isValid()
                        ? (int)funTw->type<true>() : -1;
                    const LambdaDescriptor * d = nullptr;
                    if (vm.frames.back().closure)
                        d = vm.frames.back().closure->desc;
                    else if (vm.frames.back().thunk)
                        d = vm.frames.back().thunk->suspended.desc;
                    std::fprintf(stderr,
                        "v3 OP_CALL bridge: tw.type=%d top=%s ip=%u\n",
                        twType,
                        d && !d->name.empty() ? d->name.c_str() : "<?>",
                        vm.frames.back().ip);
                }
                // #484 STG-style address identity: outTw must be HEAP-
                // allocated, NOT stack.  TW updates value cells in
                // place when forcing thunks (the classic STG knot-tying
                // discipline); any v3 Bridge thunk we build below
                // points at outTw via `&` -- if outTw is a stack
                // local, the address dies with our frame and later
                // forces get garbage.  More subtly, even a temporary
                // heap COPY (the prior #483 part 2 approach inside
                // treeWalkerToV3) breaks address identity: TW's
                // update of the ORIGINAL thunk doesn't propagate to
                // our snapshot.  Manifested as nixpkgs by-name-
                // overlay.nix:54 `self._internalCallByNamePackageFile
                // missing` -- captured `self` was a pre-fix-point
                // snapshot copy.  Heap-allocate from the start; the
                // address we hand off is the SAME one TW will
                // update in place.
                nix::Value * outTwHeap = ns->allocValue();
                ns->callFunction(*funTw, *argTw, *outTwHeap, nix::noPos);
                Value v3out;
                if (outTwHeap->type<true>() == nix::nThunk) {
                    Thunk * bridge = Alloc::allocBridgeThunk(
                        static_cast<void *>(outTwHeap));
                    allocStats().thunksAllocated++;
                    v3out.tag_payload = static_cast<uint64_t>(Tag::Thunk);
                    v3out.payload.thunk = bridge;
                } else {
                    bool prev = pushShallowTWAttrsBridge();
                    try {
                        v3out = treeWalkerToV3Public(*ns, *outTwHeap);
                    } catch (...) {
                        popShallowTWAttrsBridge(prev);
                        throw;
                    }
                    popShallowTWAttrsBridge(prev);
                }
                push(vm, v3out);
                break;
            }

            // __functor: applying an attrset that has a `__functor`
            // attribute calls `__functor self arg` per the standard
            // Nix protocol.  Push (functor, attrset, arg) and re-enter
            // OP_CALL twice to match the curried call sequence.
            if (fun.isAttrs()) {
                static const SymbolId functorId = ir::globalInternSymbol("__functor");
                if (!fun.payload.bindings)
                    throw std::runtime_error("v3 OP_CALL: callee is an attrset without __functor");
                const Value * fn = fun.payload.bindings->lookup(functorId);
                if (!fn)
                    throw std::runtime_error("v3 OP_CALL: callee is an attrset without __functor");
                Value forced = forceValue(vm, *fn);
                // First apply functor to self (= the attrset).
                Value firstStep = callClosure(vm, forced, fun);
                // Then apply that result to the original arg.
                Value out = callClosure(vm, firstStep, arg);
                push(vm, out);
                break;
            }

            if (!fun.isClosure()) {
                static const bool dbg = std::getenv("V3_DBG_CALL") != nullptr;
                if (dbg) {
                    std::fprintf(stderr,
                        "v3 OP_CALL: callee is not a closure tag=%u "
                        "frames=%zu callerIp=%u\n",
                        (unsigned)fun.tag(), vm.frames.size(), ip - 1);
                    size_t lim = vm.frames.size();
                    for (size_t i = lim; i > 0 && i + 8 > lim; --i) {
                        const auto & fr = vm.frames[i - 1];
                        const LambdaDescriptor * desc = nullptr;
                        if (fr.thunk)
                            desc = fr.thunk->suspended.desc;
                        else if (fr.closure)
                            desc = fr.closure->desc;
                        std::fprintf(stderr,
                            "  frame[%zu]: %s code=[%u..) ip=%u flags=%u\n",
                            i - 1,
                            desc && !desc->name.empty() ? desc->name.c_str()
                                : (desc ? "<anon>" : "<closure-body>"),
                            desc ? desc->codeOffset : 0,
                            fr.ip, (unsigned)fr.flags);
                    }
                    // Dump 32 instructions before/after the failing OP_CALL.
                    if (cu) {
                        uint32_t fip = ip > 0 ? ip - 1 : 0;
                        uint32_t lo = fip > 64 ? fip - 64 : 0;
                        uint32_t hi = fip + 16;
                        std::fprintf(stderr, "  current frame disasm [%u..%u):\n", lo, hi);
                        disassembleWindow(stderr, *cu, lo, hi);
                    }
                }
                throw std::runtime_error("v3 OP_CALL: callee is not a closure");
            }
            const Closure * callee = fun.payload.closure;
            const LambdaDescriptor * desc = callee->desc;

            // #495 follow-on bisect: log OP_CALL post-force for
            // platform-named closures.  Used to trace the wrong-arg
            // capture in the broader-thunkify upvalue bug.  Also derefs
            // Tag::Slot args to show the slot's storage pointer and
            // the Value found there (chasing one indirection).  The
            // bug: arg=Tag::Slot(p), *p = Tag::Attrs{gcc, linux-kernel}
            // but should be the platform record `final` containing
            // isx86 etc.
            static const bool s_dbgOpCallPost =
                std::getenv("V3_DBG_OP_CALL_POST") != nullptr;
            if (__builtin_expect(s_dbgOpCallPost, 0)
                && desc && !desc->name.empty()
                && desc->name == "platform")
            {
                int arg_tag = (int)arg.tag();
                int arg_size = -1;
                if (arg.tag() == Tag::Attrs && arg.payload.bindings)
                    arg_size = arg.payload.bindings->size;
                std::fprintf(stderr,
                    "v3 OP_CALL platform: callee=%p desc=%p arg.tag=%d size=%d "
                    "frames=%zu\n",
                    (void*)callee, (void*)desc, arg_tag, arg_size,
                    vm.frames.size());
                // Dereference Tag::Slot indirections (chase up to 4
                // hops to handle Slot→Slot rebinding) and print the
                // ultimate Value's tag + a few attr names.
                if (arg.tag() == Tag::Slot && arg.payload.slot) {
                    Value * p = arg.payload.slot;
                    int hop = 0;
                    while (p && hop < 4) {
                        std::fprintf(stderr,
                            "  slot[%d] @ %p tag=%d", hop, (void*)p,
                            (int)p->tag());
                        if (p->tag() == Tag::Attrs && p->payload.bindings) {
                            const auto & st = ir::globalSymbolTable();
                            auto * b = p->payload.bindings;
                            std::fprintf(stderr, " bindings=%p size=%u present=[",
                                (void*)b, (unsigned)b->size);
                            for (uint32_t i = 0; i < b->size && i < 10; ++i) {
                                SymbolId nm = b->entries[i].name;
                                std::fprintf(stderr, "%s%s",
                                    i == 0 ? "" : ",",
                                    nm < st.size() ? st[nm].c_str() : "?");
                            }
                            std::fprintf(stderr, "]\n");
                            break;
                        }
                        if (p->tag() == Tag::Slot) {
                            std::fprintf(stderr, " → @ %p\n",
                                (void*)p->payload.slot);
                            p = p->payload.slot;
                            ++hop;
                            continue;
                        }
                        if (p->tag() == Tag::Thunk && p->payload.thunk) {
                            auto * th = p->payload.thunk;
                            std::fprintf(stderr,
                                " thunk=%p state=%d",
                                (void*)th, (int)th->state);
                            // Chase Evaluated thunks one hop to see the
                            // cached value (the actual Bindings the
                            // closure body will see).
                            if (th->state == ThunkState::Evaluated) {
                                Value & ev = th->evaluated;
                                std::fprintf(stderr,
                                    " evaluated.tag=%d", (int)ev.tag());
                                if (ev.tag() == Tag::Attrs && ev.payload.bindings) {
                                    auto * b = ev.payload.bindings;
                                    const auto & st = ir::globalSymbolTable();
                                    std::fprintf(stderr,
                                        " bindings=%p size=%u present=[",
                                        (void*)b, (unsigned)b->size);
                                    for (uint32_t i = 0; i < b->size && i < 10; ++i) {
                                        SymbolId nm = b->entries[i].name;
                                        std::fprintf(stderr, "%s%s",
                                            i == 0 ? "" : ",",
                                            nm < st.size() ? st[nm].c_str() : "?");
                                    }
                                    std::fprintf(stderr, "]");
                                }
                            }
                            std::fprintf(stderr, "\n");
                        } else if (p->tag() == Tag::App) {
                            std::fprintf(stderr, " app\n");
                        } else {
                            std::fprintf(stderr, "\n");
                        }
                        break;
                    }
                }
            }

            // #495: native fix-point intrinsics fast path.  Recognised
            // at lower-time (lower.cc recogniseIntrinsic), evaluated in
            // v3 without TW round-trips.  Permanent optimization: the
            // bytecode for `lib.fix` etc. never runs; we dispatch
            // directly to a v3-native impl that mirrors the canonical
            // pure-Nix definition.
            //
            // Fix:  `f: let x = f x; in x`
            //   Allocate a heap-stable Value slot (Boehm-traced),
            //   make a Tag::Slot pointing at it, push slot as arg,
            //   call f(slot), store result into slot, return result.
            //   Knot-tying via slot mutation -- exactly what TW's
            //   `let x = f x; in x;` does via Env::values[].
            //
            // Caller invariant: arg is the lambda's `f` (a callable).
            //
            // OPT-IN via NIX_V3_INTRINSIC_DISPATCH=1.  Default OFF until
            // validated against full nixpkgs lib evaluation; the simple
            // case works (`fix ext` returns the right attrset) but
            // nixpkgs lib's fix-point chain (makeExtensible' + extends
            // chain) interacts in ways not yet diagnosed.
            static const bool s_intrinsicEnable =
                std::getenv("NIX_V3_INTRINSIC_DISPATCH") != nullptr;
            if (s_intrinsicEnable && __builtin_expect(
                    desc->intrinsicKind != LambdaDescriptor::Intrinsic::None,
                    0)) {
                if (desc->intrinsicKind == LambdaDescriptor::Intrinsic::Fix) {
                    // Refuse native dispatch when the user's `f` is a
                    // Bridge thunk (a TW value bridged into v3): TW
                    // lambdas can't handle a v3 Tag::Slot as an arg.
                    // Fall through to the regular bytecode dispatch
                    // which already knows how to bridge across.
                    Value forcedArg = arg;
                    if (forcedArg.tag() == Tag::Thunk
                        && forcedArg.payload.thunk
                        && forcedArg.payload.thunk->state == ThunkState::Bridge)
                    {
                        // Skip intrinsic; fall through to bytecode path.
                        goto skip_intrinsic_fix_op_call;
                    }
                    allocStats().intrinsicFixCalls++;
                    static const bool s_dbg =
                        std::getenv("V3_DBG_INTRINSIC") != nullptr;
                    if (s_dbg) std::fprintf(stderr,
                        "v3 intrinsic Fix dispatch [#%llu]: arg.tag=%d desc=%s\n",
                        (unsigned long long)allocStats().intrinsicFixCalls,
                        (int)arg.tag(),
                        desc->name.empty() ? "<anon>" : desc->name.c_str());
                    // Heap-allocate the slot storage (GC-traced).  Initial
                    // value is Tag::Uninitialized; populated by the body's
                    // result.  Tag::Slot wraps a Value*; reading through
                    // the slot during body eval re-reads from this heap
                    // location, so the body sees the in-progress result
                    // (TW-style knot-tying).
                    Value * slotStorage = Alloc::allocValue();
                    slotStorage->tag_payload =
                        static_cast<uint64_t>(Tag::Uninitialized);
                    Value slotV;
                    slotV.tag_payload = static_cast<uint64_t>(Tag::Slot);
                    slotV.payload.slot = slotStorage;
                    // Save current ip on this frame so callClosure's
                    // re-entry into the dispatch loop can return cleanly.
                    vm.frames.back().ip = ip;
                    // Evaluate `f slotV`.  callClosure forces `fun` (the
                    // user-supplied f) and runs its body with the slot
                    // as arg.  The body may force the slot (chases via
                    // Tag::Slot deref); blackhole detection is per-thunk,
                    // not per-slot, so a self-referential `let x = f x;`
                    // shape works as long as f is sufficiently lazy
                    // (the standard Nix `lib.fix` precondition).
                    Value res = callClosure(vm, arg, slotV);
                    if (s_dbg) std::fprintf(stderr,
                        "v3 intrinsic Fix: callClosure returned tag=%d\n",
                        (int)res.tag());
                    // Don't forceValue eagerly -- TW's `let x = f x; in x`
                    // returns whatever `f` returns (could be a thunk if f
                    // is lazy).  Eager force here can drive a Tag::Slot
                    // chase through the as-yet-uninitialised slot.
                    *slotStorage = res;
                    push(vm, res);
                    break;
                }
                // STG-13c (#509/#512): native dispatch for ExtendsBody.
                // chain[2] of `extends = overlay: f: final: <body>`.
                // body: `let prev = f final; in prev // overlay final prev`.
                //
                // Closure upvalues at indices `desc->intrinsicVar0`
                // (overlay) and `intrinsicVar1` (f).  arg = final.
                if (desc->intrinsicKind == LambdaDescriptor::Intrinsic::ExtendsBody
                    && desc->intrinsicVar0 >= 0 && desc->intrinsicVar1 >= 0
                    && (uint16_t)desc->intrinsicVar0 < callee->nUpvalues
                    && (uint16_t)desc->intrinsicVar1 < callee->nUpvalues) {
                    allocStats().intrinsicExtendsCalls++;
                    static const bool s_dbg =
                        std::getenv("V3_DBG_INTRINSIC") != nullptr;
                    Value overlay = callee->upvalues[(uint16_t)desc->intrinsicVar0];
                    Value f       = callee->upvalues[(uint16_t)desc->intrinsicVar1];
                    Value final_  = arg;
                    if (s_dbg) std::fprintf(stderr,
                        "v3 intrinsic ExtendsBody dispatch [#%llu]: "
                        "overlay.tag=%d f.tag=%d final.tag=%d\n",
                        (unsigned long long)allocStats().intrinsicExtendsCalls,
                        (int)overlay.tag(), (int)f.tag(), (int)final_.tag());
                    vm.frames.back().ip = ip;
                    // prev = f(final); force to attrs WHNF.
                    Value prev = callClosure(vm, f, final_);
                    prev = forceValue(vm, prev);
                    if (!prev.isAttrs() || !prev.payload.bindings)
                        throw std::runtime_error(
                            "v3 intrinsic ExtendsBody: prev (= f final) didn't reduce to attrs");
                    // overlay_partial = overlay(final), then
                    // overlay_result = overlay_partial(prev); force to attrs.
                    Value overlay_partial = callClosure(vm, overlay, final_);
                    Value overlay_result  = callClosure(vm, overlay_partial, prev);
                    overlay_result = forceValue(vm, overlay_result);
                    if (!overlay_result.isAttrs() || !overlay_result.payload.bindings)
                        throw std::runtime_error(
                            "v3 intrinsic ExtendsBody: overlay final prev didn't reduce to attrs");
                    Bindings * merged = mergeBindings(prev.payload.bindings,
                                                      overlay_result.payload.bindings);
                    Value res;
                    res.tag_payload = static_cast<uint64_t>(Tag::Attrs);
                    res.payload.bindings = merged;
                    push(vm, res);
                    break;
                }

                // STG-13c (#509/#512): native dispatch for ComposeBody.
                // chain[3] of `composeExtensions = f: g: final: prev: <body>`.
                // body: `let fApplied = f final prev; prev' = prev //
                // fApplied; in fApplied // g final prev'`.
                //
                // Closure upvalues: intrinsicVar0 (f), intrinsicVar1 (g),
                // intrinsicVar2 (final).  arg = prev.
                if (desc->intrinsicKind == LambdaDescriptor::Intrinsic::ComposeBody
                    && desc->intrinsicVar0 >= 0 && desc->intrinsicVar1 >= 0
                    && desc->intrinsicVar2 >= 0
                    && (uint16_t)desc->intrinsicVar0 < callee->nUpvalues
                    && (uint16_t)desc->intrinsicVar1 < callee->nUpvalues
                    && (uint16_t)desc->intrinsicVar2 < callee->nUpvalues) {
                    allocStats().intrinsicComposeCalls++;
                    static const bool s_dbg =
                        std::getenv("V3_DBG_INTRINSIC") != nullptr;
                    Value f       = callee->upvalues[(uint16_t)desc->intrinsicVar0];
                    Value g       = callee->upvalues[(uint16_t)desc->intrinsicVar1];
                    Value final_  = callee->upvalues[(uint16_t)desc->intrinsicVar2];
                    Value prev_   = arg;
                    if (s_dbg) std::fprintf(stderr,
                        "v3 intrinsic ComposeBody dispatch [#%llu]: "
                        "f.tag=%d g.tag=%d final.tag=%d prev.tag=%d\n",
                        (unsigned long long)allocStats().intrinsicComposeCalls,
                        (int)f.tag(), (int)g.tag(), (int)final_.tag(),
                        (int)prev_.tag());
                    vm.frames.back().ip = ip;
                    // fApplied = f final prev; force to attrs.
                    Value f_partial = callClosure(vm, f, final_);
                    Value fApplied  = callClosure(vm, f_partial, prev_);
                    fApplied = forceValue(vm, fApplied);
                    if (!fApplied.isAttrs() || !fApplied.payload.bindings)
                        throw std::runtime_error(
                            "v3 intrinsic ComposeBody: f final prev didn't reduce to attrs");
                    // prev' = prev // fApplied (force prev_ to attrs first).
                    Value prevForced = forceValue(vm, prev_);
                    if (!prevForced.isAttrs() || !prevForced.payload.bindings)
                        throw std::runtime_error(
                            "v3 intrinsic ComposeBody: prev didn't reduce to attrs");
                    Bindings * prevPrimeB = mergeBindings(prevForced.payload.bindings,
                                                          fApplied.payload.bindings);
                    Value prevPrime;
                    prevPrime.tag_payload = static_cast<uint64_t>(Tag::Attrs);
                    prevPrime.payload.bindings = prevPrimeB;
                    // gApplied = g final prev'; force to attrs.
                    Value g_partial = callClosure(vm, g, final_);
                    Value gApplied  = callClosure(vm, g_partial, prevPrime);
                    gApplied = forceValue(vm, gApplied);
                    if (!gApplied.isAttrs() || !gApplied.payload.bindings)
                        throw std::runtime_error(
                            "v3 intrinsic ComposeBody: g final prev' didn't reduce to attrs");
                    // result = fApplied // gApplied.
                    Bindings * merged = mergeBindings(fApplied.payload.bindings,
                                                       gApplied.payload.bindings);
                    Value res;
                    res.tag_payload = static_cast<uint64_t>(Tag::Attrs);
                    res.payload.bindings = merged;
                    push(vm, res);
                    break;
                }

                // Other intrinsic kinds (Extends, ComposeExtensions, ...)
                // fall through to the regular dispatch path below.
                // Step 1's recogniseIntrinsic only sets Fix; future
                // commits add the rest.
            }
            skip_intrinsic_fix_op_call:;

            // #424: selector-lambda fast path for `\x: x.f`.  Skips
            // frame allocation + dispatch -- force arg, project the
            // recorded SymbolId, push.  Detected at emit time
            // (emit.cc; sets desc->selectorSym).  Only fires for
            // arity-1 simple-arg lambdas with no upvalues; the
            // frame's withStack invariants are unaffected since we
            // never allocate one.
            if (__builtin_expect(desc->selectorSym != 0, 0)) {
                allocStats().selectorLambdaCalls++;
                Value sArg = arg;
                if (sArg.isThunk() || sArg.tag() == Tag::App
                    || sArg.tag() == Tag::Slot) {
                    vm.frames.back().ip = ip;
                    sArg = forceValue(vm, sArg);
                }
                if (!sArg.isAttrs() || !sArg.payload.bindings)
                    throw std::runtime_error(
                        "v3 selector lambda: arg not an attrset");
                // Binary-search the attrset (entries sorted ascending
                // by SymbolId).  Mirrors what OP_ATTRS_SELECT does
                // post-IC-miss; we don't have an IC slot here since
                // there's no allocated bytecode site for the projection.
                const Value * v = sArg.payload.bindings->lookup(
                    desc->selectorSym);
                if (!v)
                    throw std::runtime_error(
                        "v3 selector lambda: missing attr");
                push(vm, *v);
                break;
            }

            // Closures from imported files own their own CompilationUnit;
            // when callee->cu differs, switch the dispatch loop to the
            // callee's bytecode/constant pools.  Falls back to the caller's
            // cu when the closure was made before cu-tracking landed.
            const CompilationUnit * calleeCu = callee->cu ? callee->cu : cu;

            // Formals validation: when a lambda has formals and no
            // ellipsis, every key in the param attrset must match a
            // declared formal name.  Tree-walker raises with the offending
            // attribute name; we mirror that message format.
            //
            // WC-38: tree-walker forces the arg attrset ALWAYS when the
            // callee has formals (eval.cc state.callFunction calls
            // forceAttrs on arg regardless of ellipsis).  v3 originally
            // skipped this when ellipsis is set, leaving the force to
            // each per-formal thunk (lower.cc:619-652).  This means
            // formal access in the body forces the arg once per access
            // — and crucially, *defers* the force until body execution.
            // For nixpkgs's chain `arg = autoArgs // userArgs` where
            // autoArgs = `intersectAttrs (functionArgs f) pkgs`, the
            // deferred force fires while pkgs (= lib.fix slot) is still
            // Black, causing the WC-38 `with`-lookup miss.
            //
            // NIX_V3_EAGER_ARG_FORCE=1 enables tree-walker semantics:
            // force the arg attrset at call time.  Default off until
            // validated against full lang + wc-laziness suites.
            if (desc->hasFormals) {
                static const bool s_eagerArgForce =
                    std::getenv("NIX_V3_EAGER_ARG_FORCE") != nullptr;
                bool needForce = !desc->ellipsis || s_eagerArgForce;
                if (needForce) {
                    // STG-12 (#498) diagnostic: see what we're about to
                    // force at OP_CALL.  V3_DBG_OPCALL_FORCE=1 to enable.
                    static const bool s_dbg_callforce =
                        std::getenv("V3_DBG_OPCALL_FORCE") != nullptr;
                    if (s_dbg_callforce) {
                        Value chase = arg;
                        Thunk * blackOnFrames = nullptr;
                        for (int hops = 0; hops < 16; ++hops) {
                            if (chase.tag() == Tag::Slot && chase.payload.slot)
                                chase = *chase.payload.slot;
                            else if (chase.tag() == Tag::Thunk
                                     && chase.payload.thunk) {
                                Thunk * th = chase.payload.thunk;
                                if (th->state == ThunkState::Evaluated) {
                                    chase = th->evaluated;
                                } else if (th->state == ThunkState::Blackhole) {
                                    for (size_t i = 0; i < vm.frames.size(); ++i)
                                        if (vm.frames[i].thunk == th) {
                                            blackOnFrames = th; break;
                                        }
                                    break;
                                } else break;
                            } else break;
                        }
                        if (blackOnFrames) {
                            const auto & curFr = vm.frames.back();
                            const LambdaDescriptor * cd = nullptr;
                            if (curFr.thunk
                                && (curFr.thunk->state == ThunkState::Suspended
                                    || curFr.thunk->state == ThunkState::Blackhole))
                                cd = curFr.thunk->suspended.desc;
                            else if (curFr.closure) cd = curFr.closure->desc;
                            const LambdaDescriptor * bd = blackOnFrames->suspended.desc;
                            std::fprintf(stderr,
                                "v3 OP_CALL force-arg → BLACK arg-thunk=%s "
                                "callee=%s formals=%zu ellipsis=%d "
                                "from-frame=%s ip=%u nUpvalues=%u\n",
                                bd && !bd->name.empty() ? bd->name.c_str() : "<?>",
                                desc->name.empty() ? "<?>" : desc->name.c_str(),
                                desc->formals.size(),
                                (int)desc->ellipsis,
                                cd && !cd->name.empty() ? cd->name.c_str() : "<?>",
                                curFr.ip, (unsigned)desc->nUpvalues);
                            const auto & tbl = ir::globalSymbolTable();
                            std::fprintf(stderr, "  callee formals: ");
                            for (size_t i = 0; i < desc->formals.size() && i < 12; ++i) {
                                uint32_t nm = desc->formals[i].name;
                                std::fprintf(stderr, "%s%s", i ? "," : "",
                                    nm < tbl.size() ? tbl[nm].c_str() : "?");
                            }
                            if (desc->formals.size() > 12) std::fprintf(stderr, ",...");
                            std::fprintf(stderr, "\n");
                        }
                    }
                    Value forcedArg = forceValue(vm, arg);
                    if (!desc->ellipsis && forcedArg.isAttrs() && forcedArg.payload.bindings) {
                        // Validation: no extra args for non-ellipsis lambdas.
                        const Bindings * b = forcedArg.payload.bindings;
                        for (uint32_t i = 0; i < b->size; ++i) {
                            SymbolId name = b->entries[i].name;
                            bool found = false;
                            for (auto & f : desc->formals)
                                if (f.name == name) { found = true; break; }
                            if (!found) {
                                const auto & tbl = ir::globalSymbolTable();
                                std::string nm = (name < tbl.size()) ? tbl[name] : "?";
                                throw std::runtime_error("v3 OP_CALL: function "
                                    "called with unexpected argument '" + nm + "'");
                            }
                        }
                    }
                    arg = forcedArg;
                }
            }

            // Max call-depth check — guards `(x: x x) (x: x x)` and
            // similar non-thunk-mediated infinite recursion.  Tree-walker
            // defaults to 5000; we match that via kMaxCallDepth (see
            // anonymous namespace at top of file).  Cheap O(1) check.
            if (__builtin_expect(vm.frames.size() >= kMaxCallDepth, 0))
                throw std::runtime_error("v3 OP_CALL: stack overflow; call depth exceeded "
                                          + std::to_string(kMaxCallDepth));

            vm.frames.back().ip = ip;

            size_t newBase = vm.valueStack.size();
            vm.valueStack.resize(newBase + desc->nLocals);
            vm.valueStack[newBase + 0] = arg;

            // #498 frame-entry diagnostic: log every OP_CALL closure entry
            // with name + local[0] shape.  V3_DBG_FRAME_ENTRY=<name> filters
            // by closure name (e.g. "final" or "self").
            {
                static const char * s_filter =
                    std::getenv("V3_DBG_FRAME_ENTRY");
                if (s_filter && desc && desc->name == s_filter) {
                    Value chase = arg;
                    int hops = 0;
                    while (hops < 4) {
                        if (chase.tag() == Tag::Slot && chase.payload.slot)
                            chase = *chase.payload.slot;
                        else if (chase.tag() == Tag::Thunk && chase.payload.thunk
                                 && chase.payload.thunk->state == ThunkState::Evaluated)
                            chase = chase.payload.thunk->evaluated;
                        else break;
                        ++hops;
                    }
                    std::fprintf(stderr,
                        "v3 FRAME_ENTRY OP_CALL %s codeOff=%u: local[0].tag=%d",
                        desc->name.c_str(), (unsigned)desc->codeOffset,
                        (int)arg.tag());
                    if (chase.tag() == Tag::Attrs && chase.payload.bindings) {
                        auto * b = chase.payload.bindings;
                        std::fprintf(stderr, " -> attrs size=%u {",
                            b->size);
                        const auto & tbl = ir::globalSymbolTable();
                        for (uint32_t i = 0; i < b->size && i < 3; ++i) {
                            uint32_t nm = b->entries[i].name;
                            std::fprintf(stderr, "%s%s", i ? "," : "",
                                nm < tbl.size() ? tbl[nm].c_str() : "?");
                        }
                        if (b->size > 3) std::fprintf(stderr, ",...");
                        std::fprintf(stderr, "}");
                    } else {
                        std::fprintf(stderr, " -> tag=%d", (int)chase.tag());
                    }
                    std::fprintf(stderr, " (frames=%zu)\n", vm.frames.size());
                }
            }

            uint32_t newWithBase = static_cast<uint32_t>(vm.withStack.size());
            // Push the new frame in a single move-construct: lets the
            // compiler initialize the trailing 40 bytes inline at the
            // back of the vector rather than emplace_back + 7 separate
            // field stores.  Frames are pre-reserved so push_back never
            // reallocates on the hot path.
            vm.frames.push_back(CallFrame{
                .cu = calleeCu,
                .closure = callee,
                .thunk = nullptr,
                .ip = desc->codeOffset,
                .stackBaseOffset = static_cast<uint32_t>(newBase),
                .withStackBase = newWithBase,
                .flags = 0,
            });
            pushCapturedWiths(vm, callee->capturedWiths);

            ip = desc->codeOffset;
            cu  = calleeCu;
            closure = callee;
            stackBase = newBase;
            break;
        }
        case OP_TAIL_CALL: {
            // Tail call: same semantics as OP_CALL but reuses the
            // current frame — no frame push.  Lets long recursive
            // chains run in O(1) frame stack space.
            //
            // Tail-iteration guard: catch infinite tail recursion
            // (`(x: x x) (x: x x)`) which the frame-stack limit
            // can't see because we don't grow the stack.  Tree-walker
            // catches it via C-stack overflow.  We bound at 10^7
            // iterations between frame-stack changes; ~99% headroom
            // over any real-world deep tail recursion.
            constexpr size_t kMaxTailCalls = 10'000'000;
            if (__builtin_expect(++vm.tailCallCount >= kMaxTailCalls, 0)) {
                vm.tailCallCount = 0;
                throw std::runtime_error("v3 OP_TAIL_CALL: tail-call iteration limit exceeded "
                                          + std::to_string(kMaxTailCalls)
                                          + " (likely infinite recursion)");
            }

            // Falls back to OP_CALL behaviour for non-closure callees
            // (primops, __functor, partial application) since those
            // need the full OP_CALL machinery.  We jump back into the
            // OP_CALL case via goto.
            Value arg = pop(vm), fun = pop(vm);
            // STG-12 diagnostic: when the topmost prev thunk dispatches
            // TAIL_CALL on a Black-chasing arg, log the fun's tag and
            // (if closure) name + hasFormals.
            static const bool s_dbgTcPre =
                std::getenv("V3_DBG_TC_PRE") != nullptr;
            if (__builtin_expect(s_dbgTcPre, 0)) {
                Value chase = arg;
                Thunk * blackOnFrames = nullptr;
                for (int hops = 0; hops < 16; ++hops) {
                    if (chase.tag() == Tag::Slot && chase.payload.slot)
                        chase = *chase.payload.slot;
                    else if (chase.tag() == Tag::Thunk && chase.payload.thunk) {
                        Thunk * th = chase.payload.thunk;
                        if (th->state == ThunkState::Evaluated)
                            chase = th->evaluated;
                        else if (th->state == ThunkState::Blackhole) {
                            for (size_t i = 0; i < vm.frames.size(); ++i)
                                if (vm.frames[i].thunk == th) {
                                    blackOnFrames = th; break;
                                }
                            break;
                        } else break;
                    } else break;
                }
                if (blackOnFrames) {
                    const char * funName = "<?>";
                    int funIsClosure = (int)fun.isClosure();
                    int funHasFormals = -1;
                    int funEllipsis = -1;
                    int funThunkState = -1;
                    const char * thunkName = "";
                    if (fun.isClosure() && fun.payload.closure
                        && fun.payload.closure->desc) {
                        funName = fun.payload.closure->desc->name.c_str();
                        funHasFormals = (int)fun.payload.closure->desc->hasFormals;
                        funEllipsis = (int)fun.payload.closure->desc->ellipsis;
                    } else if (fun.tag() == Tag::Thunk && fun.payload.thunk) {
                        funThunkState = (int)fun.payload.thunk->state;
                        if (fun.payload.thunk->state == ThunkState::Suspended
                            || fun.payload.thunk->state == ThunkState::Blackhole) {
                            const auto * d = fun.payload.thunk->suspended.desc;
                            if (d) thunkName = d->name.c_str();
                        }
                    }
                    std::fprintf(stderr,
                        "v3 OP_TAIL_CALL pre-dispatch BLACK arg: "
                        "fun.tag=%d ptr=%p isClosure=%d name=%s hasFormals=%d ellipsis=%d "
                        "thunkState=%d thunkName=%s\n",
                        (int)fun.tag(), fun.payload.thunk,
                        funIsClosure, funName,
                        funHasFormals, funEllipsis,
                        funThunkState, thunkName);
                }
            }
            if (!fun.isClosure()) {
                // Push back and replay through OP_CALL.
                push(vm, fun);
                push(vm, arg);
                goto op_call_dispatch;
            }
            const Closure * tcCallee = fun.payload.closure;
            const LambdaDescriptor * tcDesc = tcCallee->desc;
            const CompilationUnit * tcCalleeCu = tcCallee->cu ? tcCallee->cu : cu;

            // Same eager-arg-force as OP_CALL — see WC-38 explanation above.
            if (tcDesc->hasFormals) {
                static const bool s_eagerArgForce =
                    std::getenv("NIX_V3_EAGER_ARG_FORCE") != nullptr;
                bool needForce = !tcDesc->ellipsis || s_eagerArgForce;
                if (needForce) {
                    // STG-12 (#498) diagnostic: see what we're about to
                    // force.  V3_DBG_TAIL_FORCE=1 to enable.
                    static const bool s_dbg_tcforce =
                        std::getenv("V3_DBG_TAIL_FORCE") != nullptr;
                    if (s_dbg_tcforce) {
                        Value chase = arg;
                        Thunk * blackOnFrames = nullptr;
                        for (int hops = 0; hops < 16; ++hops) {
                            if (chase.tag() == Tag::Slot && chase.payload.slot)
                                chase = *chase.payload.slot;
                            else if (chase.tag() == Tag::Thunk
                                     && chase.payload.thunk) {
                                Thunk * th = chase.payload.thunk;
                                if (th->state == ThunkState::Evaluated) {
                                    chase = th->evaluated;
                                } else if (th->state == ThunkState::Blackhole) {
                                    for (size_t i = 0; i < vm.frames.size(); ++i)
                                        if (vm.frames[i].thunk == th) {
                                            blackOnFrames = th; break;
                                        }
                                    break;
                                } else break;
                            } else break;
                        }
                        if (blackOnFrames) {
                            const auto & curFr = vm.frames.back();
                            const LambdaDescriptor * cd = nullptr;
                            if (curFr.thunk
                                && (curFr.thunk->state == ThunkState::Suspended
                                    || curFr.thunk->state == ThunkState::Blackhole))
                                cd = curFr.thunk->suspended.desc;
                            else if (curFr.closure) cd = curFr.closure->desc;
                            const LambdaDescriptor * bd = blackOnFrames->suspended.desc;
                            std::fprintf(stderr,
                                "v3 OP_TAIL_CALL force-arg → BLACK arg-thunk=%s "
                                "callee=%s formals.size=%zu ellipsis=%d "
                                "from-frame=%s ip=%u (cu=%p code-off=%u)\n",
                                bd && !bd->name.empty() ? bd->name.c_str() : "<?>",
                                tcDesc->name.empty() ? "<?>" : tcDesc->name.c_str(),
                                tcDesc->formals.size(),
                                (int)tcDesc->ellipsis,
                                cd && !cd->name.empty() ? cd->name.c_str() : "<?>",
                                curFr.ip, (const void *)curFr.cu,
                                cd ? cd->codeOffset : 0u);
                            // Print formals names.
                            const auto & tbl = ir::globalSymbolTable();
                            std::fprintf(stderr, "  callee formals: ");
                            for (size_t i = 0; i < tcDesc->formals.size() && i < 8; ++i) {
                                uint32_t nm = tcDesc->formals[i].name;
                                std::fprintf(stderr, "%s%s", i ? "," : "",
                                    nm < tbl.size() ? tbl[nm].c_str() : "?");
                            }
                            if (tcDesc->formals.size() > 8) std::fprintf(stderr, ",...");
                            std::fprintf(stderr, "\n");
                        }
                    }
                    Value forcedArg = forceValue(vm, arg);
                    if (!tcDesc->ellipsis && forcedArg.isAttrs() && forcedArg.payload.bindings) {
                        const Bindings * b = forcedArg.payload.bindings;
                        for (uint32_t i = 0; i < b->size; ++i) {
                            SymbolId name = b->entries[i].name;
                            bool found = false;
                            for (auto & f : tcDesc->formals)
                                if (f.name == name) { found = true; break; }
                            if (!found) {
                                const auto & tbl = ir::globalSymbolTable();
                                std::string nm = (name < tbl.size()) ? tbl[name] : "?";
                                throw std::runtime_error("v3 OP_TAIL_CALL: function "
                                    "called with unexpected argument '" + nm + "'");
                            }
                        }
                    }
                    arg = forcedArg;
                }
            }

            // Reuse the current frame: shrink valueStack down to our
            // stackBase, then resize for the callee's locals.  The
            // outer-frame's stackBaseOffset and CallFrame stay put;
            // we just retarget cu/closure/ip and overwrite locals.
            vm.valueStack.resize(stackBase + tcDesc->nLocals);
            vm.valueStack[stackBase + 0] = arg;

            // #498 frame-entry diagnostic for OP_TAIL_CALL.
            {
                static const char * s_filter =
                    std::getenv("V3_DBG_FRAME_ENTRY");
                if (s_filter && tcDesc && tcDesc->name == s_filter) {
                    Value chase = arg;
                    int hops = 0;
                    while (hops < 4) {
                        if (chase.tag() == Tag::Slot && chase.payload.slot)
                            chase = *chase.payload.slot;
                        else if (chase.tag() == Tag::Thunk && chase.payload.thunk
                                 && chase.payload.thunk->state == ThunkState::Evaluated)
                            chase = chase.payload.thunk->evaluated;
                        else break;
                        ++hops;
                    }
                    std::fprintf(stderr,
                        "v3 FRAME_ENTRY OP_TAIL_CALL %s codeOff=%u: local[0].tag=%d",
                        tcDesc->name.c_str(),
                        (unsigned)tcDesc->codeOffset, (int)arg.tag());
                    if (chase.tag() == Tag::Attrs && chase.payload.bindings) {
                        auto * b = chase.payload.bindings;
                        std::fprintf(stderr, " -> attrs size=%u {",
                            b->size);
                        const auto & tbl = ir::globalSymbolTable();
                        for (uint32_t i = 0; i < b->size && i < 4; ++i) {
                            uint32_t nm = b->entries[i].name;
                            std::fprintf(stderr, "%s%s", i ? "," : "",
                                nm < tbl.size() ? tbl[nm].c_str() : "?");
                        }
                        if (b->size > 4) std::fprintf(stderr, ",...");
                        std::fprintf(stderr, "}");
                    } else {
                        std::fprintf(stderr, " -> tag=%d", (int)chase.tag());
                    }
                    std::fprintf(stderr, " (frames=%zu)\n", vm.frames.size());
                }
            }

            // Update the existing frame in place (don't push a new one).
            CallFrame & cur = vm.frames.back();
            // V3_DBG_STORE_PREVSTAGE: trace when OP_TAIL_CALL retargets
            // a frame that has CFF_THUNK_RETURN — this is the path that
            // can corrupt thunk evaluated values (WC-37 hypothesis).
            {
                static const bool s_dbg_tc =
                    std::getenv("V3_DBG_STORE_PREVSTAGE") != nullptr;
                if (s_dbg_tc && (cur.flags & CFF_THUNK_RETURN) && cur.thunk) {
                    std::fprintf(stderr,
                        "v3 OP_TAIL_CALL on thunk-frame: thunk %p nUp=%u "
                        "old_cu=%p old_ip=%u -> new_cu=%p new_ip=%u "
                        "callee=%s nUp=%u\n",
                        (void*)cur.thunk,
                        (unsigned)cur.thunk->nUpvalues,
                        (void*)cur.cu, cur.ip,
                        (void*)tcCalleeCu, tcDesc->codeOffset,
                        !tcDesc->name.empty() ? tcDesc->name.c_str() : "<anon>",
                        tcDesc->nUpvalues);
                }
            }
            cur.cu = tcCalleeCu;
            cur.closure = tcCallee;
            // thunk stays whatever it was — if we're inside a thunk
            // re-entry frame, the thunk should still be set when
            // we eventually OP_RETURN.
            cur.ip = tcDesc->codeOffset;
            // stackBaseOffset is unchanged: we reuse the same
            // operand-stack window.
            //
            // Phase-13 review HIGH-1 fix: the callee gets its OWN
            // with-scope, so we MUST truncate the with-stack and
            // reset withStackBase to the new size *before* pushing
            // the callee's captured withs.  Previously we kept the
            // outer's withs on the stack, which leaked names from
            // the caller's `with` chain into the tail-callee's
            // OP_WITH_LOOKUP scope (cross-closure tail call).  Self-
            // recursive TC was unaffected because the captures match.
            if (vm.withStack.size() > cur.withStackBase)
                vm.withStack.resize(cur.withStackBase);
            cur.withStackBase = static_cast<uint32_t>(vm.withStack.size());

            // Push the callee's captured-withs on top of the now-
            // truncated with-stack.  They get popped at OP_RETURN
            // since withStackBase tracks the new floor.
            pushCapturedWiths(vm, tcCallee->capturedWiths);

            ip = tcDesc->codeOffset;
            cu = tcCalleeCu;
            closure = tcCallee;
            // stackBase unchanged.
            break;
        }
        case OP_RETURN: {
            // Reset the tail-call counter — once we return out of a
            // tail-recursive burst, subsequent tail calls in a
            // different chain start fresh.
            vm.tailCallCount = 0;
            Value retVal = pop(vm);
            // Capture only the fields we need across the pop_back —
            // copying the whole CallFrame is the per-recursion-call
            // hot path on fib/ack benchmarks.
            const CallFrame & frRef = vm.frames.back();
            const uint32_t fStackBase    = frRef.stackBaseOffset;
            const uint32_t fWithBase     = frRef.withStackBase;
            const uint32_t fFlags        = frRef.flags;
            Thunk *        fThunk        = frRef.thunk;
            vm.valueStack.resize(fStackBase);
            vm.withStack.resize(fWithBase);
            vm.frames.pop_back();
            CallFrame fr;  // referenced by name later — only thunk + flags matter.
            fr.flags = fFlags;
            fr.thunk = fThunk;
            if (fFlags & CFF_THUNK_RETURN) {
                // Chase Evaluated chains so the thunk caches the
                // ultimate WHNF and not an intermediate thunk.
                //
                // Tag::App is intentionally NOT chased here: chasing
                // would call callClosure while `fr.thunk` is still
                // Blackhole, and any transitive force of fr.thunk in
                // the App's body would trip "infinite recursion".  The
                // App is left in `evaluated`; downstream consumers
                // (OP_CALL, OP_FORCE, callClosure) all force-on-receive
                // and chase Apps through forceValue's own loop, by
                // which time `fr.thunk->state` is Evaluated and any
                // re-entry just reads the cached App and chases it
                // again (idempotent — the App's left/right don't
                // change).
                {
                    // Diagnostic: trace the chase chain for OP_RETURN
                    // self-cycle root-cause analysis.  Gated on
                    // V3_DBG_RET_CHASE=1.
                    static const bool s_dbgRetChase =
                        std::getenv("V3_DBG_RET_CHASE") != nullptr;
                    if (s_dbgRetChase && retVal.isThunk() && fr.thunk) {
                        Value chase = retVal;
                        std::fprintf(stderr,
                            "v3 OP_RETURN chase: fr.thunk=%p initial=%p\n",
                            (void *)fr.thunk,
                            (void *)retVal.payload.thunk);
                        int hops = 0;
                        while (chase.isThunk() && hops < 32) {
                            Thunk * th = chase.payload.thunk;
                            const auto * d = (th->state == ThunkState::Suspended
                                              || th->state == ThunkState::Blackhole)
                                ? th->suspended.desc : nullptr;
                            const PosSnapshot * ps =
                                d ? resolvePosSnapshot(d->posHandle) : nullptr;
                            std::fprintf(stderr,
                                "  hop=%d thunk=%p state=%d name='%s' pos=%s:%u:%u%s\n",
                                hops, (void *)th, (int)th->state,
                                d && !d->name.empty() ? d->name.c_str() : "<?>",
                                (ps && !ps->file.empty()) ? ps->file.c_str() : "<no-pos>",
                                ps ? ps->line : 0u,
                                ps ? ps->column : 0u,
                                th == fr.thunk ? " <-- SELF" : "");
                            if (th->state != ThunkState::Evaluated) break;
                            chase = th->evaluated;
                            ++hops;
                        }
                        std::fflush(stderr);
                    }
                }
                while (retVal.isThunk() && retVal.payload.thunk->state == ThunkState::Evaluated)
                    retVal = retVal.payload.thunk->evaluated;
                // Self-reference detection: `let x = x; in x` makes the
                // thunk's body return the thunk itself (the chase above
                // can't catch this since we hit a Blackhole-state thunk
                // which isn't ThunkState::Evaluated until we're about
                // to assign).  Storing self into evaluated would make
                // subsequent forceValue calls spin forever in the
                // chase loop above.  Match tree-walker by raising.
                if (retVal.isThunk() && retVal.payload.thunk == fr.thunk) {
                    // Diagnostic: gated on V3_DBG_RETURN_SELF=1.
                    if (std::getenv("V3_DBG_RETURN_SELF")) {
                        const LambdaDescriptor * d = fr.thunk
                            && (fr.thunk->state == ThunkState::Suspended
                                || fr.thunk->state == ThunkState::Blackhole)
                            ? fr.thunk->suspended.desc : nullptr;
                        const PosSnapshot * ps =
                            d ? resolvePosSnapshot(d->posHandle) : nullptr;
                        std::fprintf(stderr,
                            "v3 OP_RETURN self-cycle: thunk=%p name='%s' pos=%s:%u:%u state=%d retTag=%d\n",
                            (void *)fr.thunk,
                            d && !d->name.empty() ? d->name.c_str() : "<?>",
                            (ps && !ps->file.empty()) ? ps->file.c_str() : "<no-pos>",
                            ps ? ps->line : 0u,
                            ps ? ps->column : 0u,
                            (int)fr.thunk->state,
                            (int)retVal.tag());
                    }
                    // #558 (2026-05-10) STG indirect-chain recovery:
                    //
                    // 1. If THIS thunk has a registered partial Bindings
                    //    (its body's REC_INIT_TAIL fired earlier — the
                    //    STG "reached WHNF" point), recover with that.
                    // 2. Otherwise, return vBlackhole (the deferred-
                    //    value marker).  The chain from retVal's thunk
                    //    back to self traversed Evaluated indirections
                    //    that resolved circularly.  Returning
                    //    vBlackhole lets the consumer see "value not
                    //    yet known" and propagate that lazily — same
                    //    protocol v3 uses for cross-stack Black thunk
                    //    accesses (see line ~7355).
                    //
                    // Rationale: the original throw assumed self-return
                    // was a real `let x = x; in x;` infinite-loop.
                    // Under STG-style partial-Bindings recovery, a
                    // Black thunk's "value" is the partial Bindings
                    // (or vBlackhole until WHNF reached); a chase
                    // resolving back to self via indirections is NOT
                    // a true loop — it's the chain unwinding through
                    // a self-reference that's still mid-construction.
                    auto & reg = partialBindingsRegistry();
                    auto pIt = reg.find(fr.thunk);
                    if (pIt != reg.end() && !pIt->second.empty()) {
                        Value recovered;
                        recovered.tag_payload =
                            static_cast<uint64_t>(Tag::Attrs);
                        recovered.payload.bindings =
                            pickLargestLayer(pIt->second);
                        retVal = recovered;
                    } else {
                        // Defer: return vBlackhole.  Caller sees a
                        // marker that propagates until a consumer
                        // demands a concrete value — at which point
                        // the surrounding fix-point will likely have
                        // settled.
                        static const bool s_dbgVBHProd =
                            std::getenv("V3_DBG_VBH_PROD") != nullptr;
                        if (s_dbgVBHProd) {
                            const auto * d = (fr.thunk
                                && (fr.thunk->state == ThunkState::Suspended
                                    || fr.thunk->state == ThunkState::Blackhole))
                                ? fr.thunk->suspended.desc : nullptr;
                            const PosSnapshot * ps =
                                d ? resolvePosSnapshot(d->posHandle) : nullptr;
                            std::fprintf(stderr,
                                "v3 OP_RETURN→vBlackhole defer: thunk=%p name='%s' pos=%s:%u:%u\n",
                                (void *)fr.thunk,
                                d && !d->name.empty() ? d->name.c_str() : "<?>",
                                (ps && !ps->file.empty()) ? ps->file.c_str() : "<no-pos>",
                                ps ? ps->line : 0u,
                                ps ? ps->column : 0u);
                        }
                        retVal = Value::vBlackhole;
                    }
                }
                // V3_DBG_STORE_PREVSTAGE: trace any thunk that gets
                // evaluated to a Closure whose desc is "prevStage" and
                // codeOffset 3099, nUp=0 — used to isolate WC-37.
                {
                    static const bool s_dbg_pv =
                        std::getenv("V3_DBG_STORE_PREVSTAGE") != nullptr;
                    if (s_dbg_pv && retVal.tag() == Tag::Closure
                        && retVal.payload.closure
                        && retVal.payload.closure->desc
                        && retVal.payload.closure->nUpvalues == 0
                        && retVal.payload.closure->desc->name == "prevStage")
                    {
                        const auto * d = fr.thunk->suspended.desc;
                        std::fprintf(stderr,
                            "v3 OP_RETURN: storing prevStage(nUp=0) into thunk "
                            "%p desc=%s codeOffset=%u nUp=%u; cu=%p ip=%u\n",
                            (void*)fr.thunk,
                            d && !d->name.empty() ? d->name.c_str()
                                : (d ? "<anon>" : "<no-desc>"),
                            d ? d->codeOffset : 0,
                            (unsigned)fr.thunk->nUpvalues,
                            (void*)cu,
                            ip - 1);
                        // Dump frame stack to help locate caller.
                        size_t lim2 = vm.frames.size();
                        for (size_t i = lim2; i > 0 && i + 6 > lim2; --i) {
                            const auto & fr2 = vm.frames[i - 1];
                            const LambdaDescriptor * d2 = nullptr;
                            if (fr2.thunk) d2 = fr2.thunk->suspended.desc;
                            else if (fr2.closure) d2 = fr2.closure->desc;
                            std::fprintf(stderr,
                                "  frame[%zu]: %s code=[%u..) ip=%u flags=%u cu=%p\n",
                                i - 1,
                                d2 && !d2->name.empty() ? d2->name.c_str()
                                    : (d2 ? "<anon>" : "<closure-body>"),
                                d2 ? d2->codeOffset : 0, fr2.ip,
                                (unsigned)fr2.flags, (void*)fr2.cu);
                        }
                        // Dump bytecode around ip-1 in the popped frame's cu
                        // — verifies that ip-1 is actually OP_RETURN.
                        if (cu && ip > 1) {
                            uint32_t lo = ip > 6 ? ip - 6 : 0;
                            uint32_t hi = ip + 4;
                            std::fprintf(stderr, "  popped frame cu=%p disasm [%u..%u):\n",
                                (void*)cu, lo, hi);
                            disassembleWindow(stderr, *cu, lo, hi);
                            // Also disasm the descriptor's codeOffset region
                            // — this is what the body SHOULD have started at.
                            if (d && d->codeOffset != ip - 1) {
                                std::fprintf(stderr,
                                    "  desc.codeOffset=%u disasm [%u..%u):\n",
                                    d->codeOffset, d->codeOffset, d->codeOffset + 200);
                                disassembleWindow(stderr, *cu,
                                    d->codeOffset, d->codeOffset + 200);
                            }
                            // Find the lambda whose codeOffset is closest
                            // to (ip-1), going backwards.  Tells us which
                            // function we actually returned from.
                            uint32_t target_off = ip - 1;
                            uint32_t best_idx = ~0u;
                            uint32_t best_off = 0;
                            for (uint32_t li = 0; li < cu->lambdas.size(); ++li) {
                                uint32_t lo2 = cu->lambdas[li].codeOffset;
                                if (lo2 <= target_off && lo2 > best_off) {
                                    best_off = lo2;
                                    best_idx = li;
                                }
                            }
                            if (best_idx != ~0u) {
                                const auto & ld = cu->lambdas[best_idx];
                                std::fprintf(stderr,
                                    "  ip-1=%u falls inside lambdas[%u]"
                                    " (name=%s codeOffset=%u nUp=%u nLocals=%u)\n",
                                    target_off, best_idx,
                                    !ld.name.empty() ? ld.name.c_str() : "<anon>",
                                    ld.codeOffset, ld.nUpvalues, ld.nLocals);
                            }
                            // Also dump the thunk pointer's `tail` (its
                            // upvalues) so we can identify which specific
                            // thunk instance.
                            std::fprintf(stderr,
                                "  fr.thunk->tail upvalues (nUp=%u):\n",
                                (unsigned)fr.thunk->nUpvalues);
                            for (uint16_t ui = 0; ui < fr.thunk->nUpvalues && ui < 8; ++ui) {
                                const Value & uv = fr.thunk->tail[ui];
                                std::fprintf(stderr,
                                    "    [%u] tag=%u\n", ui, (unsigned)uv.tag());
                            }
                        }
                    }
                }
                // EVAL-COMP §4.4 / §8.2: drop upvalue references on
                // evaluation.  Once the thunk's body has returned, its
                // upvalues are no longer needed -- the cached
                // `evaluated` value is the only useful state.  Zeroing
                // tail[] lets Boehm reclaim transitive references that
                // would otherwise be retained for the thunk's
                // lifetime.  This is the GHC selector-thunk pattern
                // generalised: every Nix thunk gets selector-thunk
                // memory behaviour.  Particularly important for
                // `let pkgs = import <nixpkgs> {}; in pkgs.foo.bar`
                // patterns where pkgs holds a giant attrset that's
                // otherwise pinned by every per-attr selector thunk.
                {
                    Thunk * t = fr.thunk;
                    for (uint16_t ui = 0; ui < t->nUpvalues; ++ui)
                        t->tail[ui] = Value{};
                    // Don't reset nUpvalues -- the FAM size was set at
                    // alloc time; reusing the slot would require the
                    // count.  Leaving it preserves alloc-time
                    // invariants (Bridge thunks etc.).
                }
                // #558 (2026-05-11) Taint check: if this thunk's body
                // used STG WHNF recovery (CFF_TAINTED set on this
                // frame), the retVal is an APPROXIMATE result derived
                // from chain.back() — a partial fix-point shape.
                // Don't memoize: keep state Suspended so future forces
                // re-run the body with whatever chain.back() is then.
                //
                // The retVal is still returned to the caller (so this
                // access gets the partial-but-best-current result),
                // but no Evaluated transition.
                //
                // STG analog: a thunk that observed an in-flight
                // indirection must re-evaluate.  Mirrors GHC's
                // re-entrancy of selector thunks that observed
                // BLACKHOLE.
                if (fFlags & CFF_TAINTED) {
                    // Don't memoize.  retVal stays on the value-stack
                    // (already pushed earlier in OP_RETURN's pop).
                    // No cell update either — would corrupt the slot
                    // with stale data.
                    fr.thunk->state = ThunkState::Suspended;
                } else {
                    fr.thunk->state = ThunkState::Evaluated;
                    fr.thunk->evaluated = retVal;
                    // STG-8 (#498): cell update.  If this thunk was stored
                    // at a heap-stable cell (recorded at OP_ATTRS_REC_SET
                    // time), overwrite the cell's contents with the body's
                    // final result.  This mirrors tree-walker's in-place
                    // `forceValue` update — slots / sub-thunks that
                    // captured a Tag::Slot pointing at the cell now read
                    // the result via single deref, and foreign VMState
                    // observers stop seeing the leaked Black thunk.
                    //
                    // Read-and-clear: we want the write to fire exactly
                    // once per cell binding.  Idempotent on re-entry
                    // (cell becomes nullptr after first OP_RETURN).
                    if (Value * cell = fr.thunk->cell) {
                        *cell = retVal;
                        fr.thunk->cell = nullptr;
                    }
                    // #558 Phase 1.5: also update shapeCell with the
                    // final value, then clear it.  This makes a final
                    // forceValue-after-body see the actual result
                    // through shapeCell, mirroring the cell semantics.
                    if (Value * sc = fr.thunk->shapeCell) {
                        *sc = retVal;
                        fr.thunk->shapeCell = nullptr;
                    }
                }
                // #457/#458: clear the partial-Bindings registry
                // entry now that the thunk's final value is set.
                {
                    auto & reg = partialBindingsRegistry();
                    auto it = reg.find(fr.thunk);
                    if (it != reg.end()) reg.erase(it);
                }
                // #558 (2026-05-10) Cross-chain cleanup.  When the
                // thunk's body returns a Bindings via REC_INIT_TAIL,
                // that Bindings was registered with MULTIPLE thunks'
                // chains (publishToAllThunkFrames).  Once THIS thunk
                // is Evaluated, the Bindings is no longer "in
                // construction" — it's a finalized value.  Other
                // thunks' chains shouldn't keep this Bindings as
                // their "partial WHNF" approximation.
                //
                // Concrete bug this fixes: qt5-packages.nix:35's
                // `attrs = { inherit (pkgs) lib fetchurl; ... }`.
                // When attrs's REC_INIT_TAIL fires, attrs's bindings
                // gets registered with pkgs's chain (because pkgs's
                // thunk is on the call stack as Suspended/Black).
                // attrs's bindings has `lib` -> T_lib (the
                // inherit-from select thunk) which itself reads
                // pkgs.lib.  Without this cleanup, after attrs's
                // OP_RETURN, pkgs's chain.back() = attrs's bindings,
                // and a future T_lib force triggering STG WHNF
                // recovery on pkgs returns attrs's bindings → select
                // lib → T_lib (cycle).
                //
                // STG analog: when an indirect thunk forwards to
                // another thunk's value, the indirect's "last seen
                // shape" is updated; old shapes are no longer
                // visible.  For us, "old shapes" are stale partial
                // bindings registered cross-thunk.
                //
                // Gated by NIX_V3_NO_CROSS_CHAIN_CLEANUP=1 for
                // bisecting any regression.
                if (retVal.tag() == Tag::Attrs && retVal.payload.bindings) {
                    static const bool s_noCleanup =
                        std::getenv("NIX_V3_NO_CROSS_CHAIN_CLEANUP") != nullptr;
                    if (!s_noCleanup) {
                        Bindings * b = retVal.payload.bindings;
                        auto & reg = partialBindingsRegistry();
                        for (auto & [t, chain] : reg) {
                            chain.erase(
                                std::remove(chain.begin(), chain.end(), b),
                                chain.end());
                        }
                    }
                }

                // WC-38: the legacy "return-chain push" -- eagerly
                // forcing the next thunk if the outer's body returned
                // a Suspended thunk -- has been removed.  forceValue's
                // chase loop already resolves the chain on the
                // consumer's pull, and OP_RETURN's caller-resume path
                // re-runs op_force_slow when CFF_FORCE_RETRY is set.
            }
            if (vm.frames.size() == exitDepth) {
                finalResult = retVal;
                running = false;
                break;
            }
            {
                // WC-38: GHC STG-style force-retry.  If the caller frame
                // was marked CFF_FORCE_RETRY (set by OP_FORCE /
                // OP_GET_LOCAL_FORCE / OP_GET_UPVALUE_FORCE before
                // pushing the now-popped thunk frame) AND retVal is
                // still a Thunk/App (= the body returned a forwarding
                // pointer to another unforced value), re-enter
                // op_force_slow to drive the chain.
                //
                // ALSO (WC-38 part 2 / "early publish"): if the popped
                // frame was a CLOSURE call frame (NOT CFF_THUNK_RETURN)
                // and the caller frame is a thunk frame in Black state,
                // EARLY-PUBLISH retVal to the caller's thunk.evaluated.
                // This mirrors tree-walker's `v.mkAttrs(...)` writing
                // to the slot DURING expr->eval (not at body return),
                // so sub-thunks captured-with that fire DURING the
                // outer's body see the published value rather than
                // hitting the Black state and throwing.
                CallFrame & caller = vm.frames.back();
                cu = caller.cu;
                ip = caller.ip;
                closure = caller.closure;
                stackBase = caller.stackBaseOffset;

                // Early publish: only fires when popped frame was a
                // closure call (not a thunk frame), retVal is fully
                // resolved (non-thunk/app), and caller is a Black
                // thunk frame.  Idempotent — the eventual OP_RETURN
                // of the caller's thunk frame will overwrite with the
                // FINAL retVal.
                //
                // Disabled by default — set NIX_V3_EARLY_PUBLISH=1
                // to enable.  Currently doesn't fully fix WC-38 but
                // is kept for experimentation.
                static const bool s_early_publish =
                    std::getenv("NIX_V3_EARLY_PUBLISH") != nullptr;
                if (s_early_publish
                    && !(fFlags & CFF_THUNK_RETURN)
                    && (caller.flags & CFF_THUNK_RETURN)
                    && caller.thunk
                    && caller.thunk->state == ThunkState::Blackhole
                    && retVal.tag() != Tag::Thunk
                    && retVal.tag() != Tag::App
                    && retVal.tag() != Tag::Blackhole)
                {
                    caller.thunk->state = ThunkState::Evaluated;
                    caller.thunk->evaluated = retVal;
                }

                bool retry = (caller.flags & CFF_FORCE_RETRY)
                    && (retVal.tag() == Tag::Thunk
                        || retVal.tag() == Tag::App
                        || retVal.tag() == Tag::Slot);
                // Clear the retry flag — it's a one-shot per
                // OP_FORCE.  The next OP_FORCE will re-set it.
                caller.flags &= ~CFF_FORCE_RETRY;
                // STG-11 (#498): diagnostic — log the retry value's
                // shape so we can see what's about to be re-forced.
                // V3_DBG_RETRY=1 dumps each retry's retVal tag + chase.
                // V3_DBG_RETRY_BLACK=1 only logs when the retry's
                // chain would hit a Black thunk (= the cycle source).
                static const bool s_dbgRetry =
                    std::getenv("V3_DBG_RETRY") != nullptr;
                static const bool s_dbgRetryBlack =
                    std::getenv("V3_DBG_RETRY_BLACK") != nullptr;
                if (retry && (s_dbgRetry || s_dbgRetryBlack))
                {
                    Value chase = retVal;
                    int hops = 0;
                    Thunk * retryThunk = nullptr;
                    Thunk * blackHit = nullptr;
                    while (hops < 16) {
                        if (chase.tag() == Tag::Slot && chase.payload.slot) {
                            chase = *chase.payload.slot;
                        } else if (chase.tag() == Tag::Thunk
                                   && chase.payload.thunk) {
                            Thunk * th = chase.payload.thunk;
                            if (th->state == ThunkState::Evaluated) {
                                chase = th->evaluated;
                            } else if (th->state == ThunkState::Blackhole) {
                                blackHit = th;
                                break;
                            } else {
                                break;
                            }
                        } else break;
                        ++hops;
                    }
                    if (retVal.tag() == Tag::Thunk
                        && retVal.payload.thunk)
                        retryThunk = retVal.payload.thunk;
                    else if (retVal.tag() == Tag::Slot
                             && retVal.payload.slot
                             && retVal.payload.slot->tag() == Tag::Thunk)
                        retryThunk = retVal.payload.slot->payload.thunk;
                    bool onlyBlack = s_dbgRetryBlack;
                    if (!onlyBlack || blackHit) {
                        std::fprintf(stderr,
                            "v3 OP_RETURN retry: retVal.tag=%d chase.tag=%d hops=%d",
                            (int)retVal.tag(), (int)chase.tag(), hops);
                        if (blackHit) {
                            const LambdaDescriptor * bd =
                                blackHit->suspended.desc;
                            std::fprintf(stderr,
                                " BLACK thunk=%p name=%s code=%u",
                                (void *)blackHit,
                                bd && !bd->name.empty() ? bd->name.c_str() : "<?>",
                                bd ? bd->codeOffset : 0);
                        }
                        if (retryThunk) {
                            const LambdaDescriptor * d =
                                retryThunk->state == ThunkState::Suspended
                                || retryThunk->state == ThunkState::Blackhole
                                    ? retryThunk->suspended.desc : nullptr;
                            std::fprintf(stderr,
                                " retryThunk=%p state=%d name=%s",
                                (void *)retryThunk,
                                (int)retryThunk->state,
                                d && !d->name.empty() ? d->name.c_str() : "<?>");
                        }
                        // Dump caller frame name so we know where the
                        // retry fires from.
                        const LambdaDescriptor * cd = nullptr;
                        if (caller.thunk
                            && (caller.thunk->state == ThunkState::Suspended
                                || caller.thunk->state == ThunkState::Blackhole))
                            cd = caller.thunk->suspended.desc;
                        else if (caller.closure)
                            cd = caller.closure->desc;
                        std::fprintf(stderr,
                            " caller.name=%s frames=%zu\n",
                            cd && !cd->name.empty() ? cd->name.c_str() : "<?>",
                            vm.frames.size());
                    }
                }
                push(vm, retVal);
                if (retry)
                    goto op_force_slow;
            }
            break;
        }
        case OP_FORCE: {
            // Fast path: peek at the top of the stack.  The vast majority
            // of OP_FORCE calls hit values already in WHNF (Int / Bool /
            // String / Attrs / List / Closure / Path / Null / Float /
            // PrimOp / PrimOpApp).  Skip the pop+push for those.
            // Diagnostics live BELOW the bail-out so the fast path
            // doesn't compute the address argument when disabled.
            {
                Value & topRef = vm.valueStack.back();
                Tag t = topRef.tag();
                if (t != Tag::Thunk && t != Tag::App && t != Tag::Slot) break;
            }
            // V3_DBG_FORCE_SITE trace; see dbgLogForceSite().
            dbgLogForceSite(cu, ip - 1,
                vm.valueStack.empty() ? nullptr : &vm.valueStack.back());
            dbgLogForceInsideX(vm,
                vm.valueStack.empty() ? nullptr : &vm.valueStack.back());
            // Slow path: shared with OP_GET_LOCAL_FORCE / OP_GET_UPVALUE_FORCE
            // which push the value first and then jump here.
            op_force_slow:
            Value v = pop(vm);
            // Chase Evaluated chains, deref Tag::Slot, and resolve
            // Tag::App deferred calls (used by mapAttrs et al. for
            // lazy entries).
            //
            // Same iteration bound as forceValue() to detect
            // SECD-style indirection cycles (`let x = x; in x` after
            // the Phase 5 slot-pointer rewrite).  See forceValue
            // comment for rationale.
            {
            int forceChaseIters = 0;
            while (true) {
                if (__builtin_expect(++forceChaseIters > kMaxIndirectionChase, 0))
                    throw std::runtime_error(
                        "v3 OP_FORCE: infinite recursion (chase cycle through "
                        "Tag::Slot/Tag::Thunk indirections)");
                if (v.tag() == Tag::Slot) {
                    Value * p = v.payload.slot;
                    if (!p) throw std::runtime_error(
                        "v3 OP_FORCE: null slot pointer");
                    v = *p;
                    continue;
                }
                if (v.tag() == Tag::App) {
                    // REVIEW MED-18: walk the App spine iteratively
                    // to find the leaf function + collected args.
                    // Pre-fix recursed through forceValue per App level
                    // (`left = forceValue(vm, left)`), blowing C-stack
                    // on long mapAttrs / map chains in nixpkgs.  Now we
                    // chase down the left side without recursion,
                    // accumulating rights into a small vector, then
                    // apply once.  The leaf force call is non-App, so
                    // any forceValue recursion bottoms out at the leaf
                    // rather than at every App level.
                    std::vector<Value> rights;
                    rights.reserve(8);
                    while (v.tag() == Tag::App) {
                        rights.push_back(v.payload.pair->right);
                        v = v.payload.pair->left;
                    }
                    vm.frames.back().ip = ip;
                    if (v.tag() == Tag::Slot
                        || v.tag() == Tag::Thunk
                        || v.tag() == Tag::App)
                        v = forceValue(vm, v);
                    // Apply rights in source order (we collected
                    // outermost-first while walking; reverse on apply).
                    for (size_t i = rights.size(); i > 0; --i) {
                        v = callClosure(vm, v, rights[i - 1]);
                    }
                    continue;
                }
                if (!v.isThunk()) break;
                if (v.payload.thunk->state == ThunkState::Evaluated) {
                    v = v.payload.thunk->evaluated;
                    continue;
                }
                break;
            }
            } // end forceChaseIters scope
            if (!v.isThunk()) { push(vm, v); break; }
            Thunk * t = v.payload.thunk;
            if (t->state == ThunkState::Blackhole) {
                // WC-17.1 diagnostic: dump the v3 frame stack with
                // function names + IP deltas when V3_DBG_OPCYCLE=1.
                // The `name` field on LambdaDescriptor (populated by
                // emit() from ir::Function::name) lets us correlate
                // cycle frames back to source-level rec-attrset attr
                // names — invaluable for diagnosing the closure-bridge
                // cycle without a full bytecode disassembler.
                static const bool s_dbg = std::getenv("V3_DBG_OPCYCLE") != nullptr;
                if (s_dbg) {
                    auto frameInfo = [&](Thunk * th, const Closure * cl, uint32_t fip) -> std::string {
                        const LambdaDescriptor * desc = nullptr;
                        if (th) desc = th->suspended.desc;
                        else if (cl) desc = cl->desc;
                        if (!desc) return "<closure-body>";
                        char buf[256];
                        std::snprintf(buf, sizeof buf,
                            "%s code=[%u..) nUp=%u nLocals=%u",
                            !desc->name.empty() ? desc->name.c_str() : "<anon>",
                            desc->codeOffset, desc->nUpvalues, desc->nLocals);
                        return buf;
                    };
                    std::fprintf(stderr,
                        "v3 OP_FORCE Black thunk=%p frames=%zu callerIp=%u\n",
                        (void*)t, vm.frames.size(), ip - 1);
                    size_t lim = vm.frames.size();
                    for (size_t i = lim; i > 0 && i + 8 > lim; --i) {
                        const auto & fr = vm.frames[i - 1];
                        std::fprintf(stderr,
                            "  frame[%zu]: %s flags=%u ip=%u thunk=%p\n",
                            i - 1, frameInfo(fr.thunk, fr.closure, fr.ip).c_str(),
                            (unsigned)fr.flags, fr.ip, (void*)fr.thunk);
                    }
                    // WC-32 disassembler: when V3_DBG_OPCYCLE_DISASM=1,
                    // also dump 8 instructions surrounding each frame's ip.
                    static const bool s_dbg_disasm =
                        std::getenv("V3_DBG_OPCYCLE_DISASM") != nullptr;
                    if (s_dbg_disasm) {
                        // Expanded: dump top 12 frames (was 8) for WC-38
                        // investigation.
                        for (size_t i = lim; i > 0 && i + 12 > lim; --i) {
                            const auto & fr = vm.frames[i - 1];
                            if (!fr.cu) continue;
                            uint32_t fip = fr.ip;
                            uint32_t lo = fip > 16 ? fip - 16 : 0;
                            uint32_t hi = fip + 16;
                            std::fprintf(stderr,
                                "  frame[%zu] disasm [%u..%u):\n",
                                i - 1, lo, hi);
                            disassembleWindow(stderr, *fr.cu, lo, hi);
                        }
                        // Also dump the prologue of each frame's lambda
                        // (where the body STARTS) for context.
                        std::fprintf(stderr, "  --- frame prologues ---\n");
                        for (size_t i = lim; i > 0 && i + 8 > lim; --i) {
                            const auto & fr = vm.frames[i - 1];
                            if (!fr.cu) continue;
                            const LambdaDescriptor * desc = nullptr;
                            if (fr.thunk)
                                desc = fr.thunk->suspended.desc;
                            else if (fr.closure)
                                desc = fr.closure->desc;
                            if (!desc) continue;
                            uint32_t prologueStart = desc->codeOffset;
                            uint32_t prologueEnd = prologueStart + 16;
                            std::fprintf(stderr,
                                "  frame[%zu] prologue [%u..%u):\n",
                                i - 1, prologueStart, prologueEnd);
                            disassembleWindow(stderr, *fr.cu, prologueStart, prologueEnd);
                        }
                    }
                }
                // #466 error-as-value (GHC-style mkBlackHole) — see
                // forceValue's matching block for full rationale.
                {
                    static const bool s_blackholeAsValue =
                        std::getenv("NIX_V3_NO_BLACKHOLE_AS_VALUE") == nullptr;
                    if (s_blackholeAsValue) {
                        bool onMyFrames = false;
                        for (size_t i = 0; i < vm.frames.size(); ++i) {
                            if (vm.frames[i].thunk == t) {
                                onMyFrames = true; break;
                            }
                        }
                        if (!onMyFrames) {
                            push(vm, Value::vBlackhole);
                            break;
                        }
                        // #558 (2026-05-10) STG WHNF deferral in OP_FORCE.
                        //
                        // When the Black thunk is on our own frames AND
                        // has registered partial Bindings, leave the
                        // value on stack UNCHANGED (still Tag::Thunk
                        // Black).  Consumers (OP_ATTRS_SELECT,
                        // OP_WITH_LOOKUP) detect Tag::Thunk Black and
                        // use the chain peek mechanism — walks all chain
                        // layers via lookupInPartialChain, finding the
                        // key in whichever layer has it.
                        //
                        // Why not return Tag::Attrs (chain.back() or
                        // any single layer): chain.back() is the LATEST
                        // registered AttrSet, which may be a small
                        // sub-attrset (e.g. {__functor, __functionArgs}
                        // from setFunctionArgs) that doesn't have the
                        // looked-up key.  Returning a single layer
                        // collapses the chain — we lose access to
                        // OTHER layers' keys.
                        //
                        // STG analog: when forcing a Black thunk that
                        // already has partial WHNF info, the forcing
                        // is idempotent — return the thunk identifier
                        // and let the consumer project from it.
                        static const bool s_noStgWhnfFp =
                            std::getenv("NIX_V3_NO_STG_WHNF") != nullptr;
                        if (!s_noStgWhnfFp) {
                            auto & reg = partialBindingsRegistry();
                            auto pIt = reg.find(t);
                            if (pIt != reg.end() && !pIt->second.empty()) {
                                Value recovered;
                                recovered.tag_payload =
                                    static_cast<uint64_t>(Tag::Attrs);
                                recovered.payload.bindings =
                                    pickLargestLayer(pIt->second);
                                vm.valueStack.back() = recovered;
                                break;
                            }
                        }
                    }
                }
                throw BlackholeError("v3 OP_FORCE: infinite recursion (blackhole)");
            }
            // WC-10: Bridge thunk — call into tree-walker for the
            // single nix::Value*, then bridge the already-forced
            // result.  Defined in primops.cc so vm.cc stays free of
            // nix:: includes.
            if (t->state == ThunkState::Bridge) {
                ++t->forces;
                ++allocStats().bridgeThunksForced;
                // #466 / STG-7 (#498): forceBridgeThunk reaches into TW
                // (ns->forceValue), and TW may re-enter v3 via the eval
                // hook on whatever Expr it ends up driving.  Without
                // ScopedActiveV3VM here the re-entry guard
                // (v3EvalEntry's `activeV3VM() != nullptr` check) stays
                // inactive, so the inner v3 call spawns a fresh VMState
                // that black-marks v3 thunks already mid-flight on this
                // outer vm — exactly the cross-VMState fresh-VMState
                // cycle that surfaces under STG-mode + KEEP_HOOKS=1.
                // Mirror the forceValue Bridge handler at line 5539.
                ScopedActiveV3VM _activeV3VM(&vm);
                Value resolved = forceBridgeThunk(t);
                // Self-Bridge guard (#520): forceBridgeThunk goes
                // through getOrAllocBridgeThunkCached, which is
                // pointer-keyed on `nix::Value *` (== bridgeSrc).
                // When bridgeSrc is an nFunction TW value, the
                // treeWalkerToV3 nFunction case calls
                // getOrAllocBridgeThunkCached(&nv) and the cache
                // hits — returning Tag::Thunk{t} (the SAME bridge
                // back).  If we then `t->state = Evaluated;
                // t->evaluated = Tag::Thunk{t}`, the chase loop's
                // Evaluated branch follows t->evaluated → t →
                // t->evaluated → ... ad infinitum.  Treat the
                // self-Bridge as canonical WHNF (a Bridge wrapping a
                // Function IS the v3 representation; consumers
                // unwrap via v3ToTreeWalker to recover the TW
                // lambda).  Leave state == Bridge so future forces
                // re-resolve harmlessly and the cache still hits.
                if (resolved.tag() == Tag::Thunk
                    && resolved.payload.thunk == t) {
                    push(vm, resolved);
                    break;
                }
                t->state = ThunkState::Evaluated;
                t->evaluated = resolved;
                // STG-14b option (a): cell update protocol on Bridge
                // thunks.  Mirrors STG-8's OP_RETURN cell-update for
                // Suspended thunks.  When prepHookUpvaluesAndWiths
                // builds a per-Bindings-entry Bridge with
                // bridge->cell = &entries[i].value, this single write
                // propagates the resolved TW value into every consumer
                // observing entries[i].value (inner+outer call-hook
                // entries that share the recBuildCache Bindings).
                if (Value * cell = t->cell) {
                    *cell = resolved;
                    t->cell = nullptr;
                }
                push(vm, resolved);
                break;
            }
            // Suspended: blackhole and run.
            // We treat suspended.desc as a LambdaDescriptor* (see OP_MAKE_THUNK).
            const LambdaDescriptor * desc = t->suspended.desc;
            // Phase 13 instrumentation: bump per-thunk + per-descriptor +
            // global counters at the Suspended → Blackhole gate.  Each
            // thunk should transition exactly once per lifetime, so
            // `t->forces` should never grow past 1 unless something
            // re-suspends a previously-blackholed thunk.  Per-descriptor
            // count tells us how many thunks share the same body
            // (over-allocation indicator: when 1 expression yields N
            // thunks because the binding it captures isn't shared).
            ++t->forces;
            ++desc->forceCount;
            ++allocStats().thunksForced;
            // #558 (2026-05-11) Focused trace: log when a thunk with a
            // specific name is forced for the first time.  Used to
            // diagnose v3-specific eager forces vs TW.  Set
            // V3_DBG_FORCE_NAME=libsForQt5 to trace.  Also matches
            // by file:line if name doesn't match — set V3_DBG_FORCE_POS=8390
            // to match by line number.
            {
                static const char * s_focusName =
                    std::getenv("V3_DBG_FORCE_NAME");
                static const char * s_focusPos =
                    std::getenv("V3_DBG_FORCE_POS");
                // V3_DBG_FORCE_FILE: optional file-name substring filter
                // (combine with V3_DBG_FORCE_POS to match by file+line).
                // E.g., V3_DBG_FORCE_FILE=darwin/default.nix V3_DBG_FORCE_POS=232.
                static const char * s_focusFile =
                    std::getenv("V3_DBG_FORCE_FILE");
                bool nameMatch = s_focusName && desc
                    && desc->name == s_focusName;
                bool posMatch = false;
                if (s_focusPos && desc && desc->posHandle) {
                    const PosSnapshot * ps = resolvePosSnapshot(desc->posHandle);
                    if (ps) {
                        uint32_t want = std::strtoul(s_focusPos, nullptr, 10);
                        bool lineOk = ps->line == want;
                        bool fileOk = !s_focusFile
                            || (ps->file.find(s_focusFile) != std::string::npos);
                        if (lineOk && fileOk) posMatch = true;
                    }
                }
                if (__builtin_expect((nameMatch || posMatch)
                                     && desc && desc->forceCount == 1, 0)) {
                    if (true)
                    {
                        const PosSnapshot * ps =
                            resolvePosSnapshot(desc->posHandle);
                        std::fprintf(stderr,
                            "v3 FORCE-NAME-FIRST: '%s' thunk=%p pos=%s:%u:%u frames=%zu\n",
                            desc->name.c_str(), (void *)t,
                            (ps && !ps->file.empty()) ? ps->file.c_str() : "?",
                            ps ? ps->line : 0u, ps ? ps->column : 0u,
                            vm.frames.size());
                        size_t lim = vm.frames.size();
                        for (size_t fi = lim; fi > 0 && fi + 20 > lim; --fi) {
                            const auto & fr = vm.frames[fi - 1];
                            const LambdaDescriptor * d = nullptr;
                            if (fr.thunk
                                && (fr.thunk->state == ThunkState::Suspended
                                    || fr.thunk->state == ThunkState::Blackhole))
                                d = fr.thunk->suspended.desc;
                            else if (fr.closure)
                                d = fr.closure->desc;
                            const PosSnapshot * ps2 =
                                d ? resolvePosSnapshot(d->posHandle) : nullptr;
                            std::fprintf(stderr,
                                "  [%zu] %s ip=%u flags=%u %s:%u:%u\n",
                                fi - 1,
                                d && !d->name.empty() ? d->name.c_str() : "<?>",
                                fr.ip, (unsigned)fr.flags,
                                (ps2 && !ps2->file.empty()) ? ps2->file.c_str() : "?",
                                ps2 ? ps2->line : 0u, ps2 ? ps2->column : 0u);
                        }
                        std::fflush(stderr);
                    }
                }
            }
            // Phase 13: live periodic stats dump.  When V3_DBG_FORCES is
            // set, every N millionth force emits a one-line snapshot to
            // stderr.  Lets us watch a runaway eval without waiting for
            // atexit (which doesn't fire under SIGKILL / SIGXCPU).
            // Default N = 10M; override via V3_DBG_FORCE_STRIDE.
            {
                static const bool s_periodic =
                    std::getenv("V3_DBG_FORCES") != nullptr;
                if (__builtin_expect(s_periodic, 0)) {
                    static const uint64_t s_stride = []() -> uint64_t {
                        const char * e = std::getenv("V3_DBG_FORCE_STRIDE");
                        return e ? std::strtoull(e, nullptr, 10)
                                 : uint64_t(10) * 1000 * 1000;
                    }();
                    auto & a = allocStats();
                    if (s_stride && (a.thunksForced % s_stride) == 0) {
                        const PosSnapshot * ps = resolvePosSnapshot(desc->posHandle);
                        char posBuf[256] = "";
                        if (ps && !ps->file.empty())
                            std::snprintf(posBuf, sizeof posBuf,
                                " at=%s:%u:%u", ps->file.c_str(),
                                ps->line, ps->column);
                        std::fprintf(stderr,
                            "v3 PROGRESS: forced=%llu allocated=%llu "
                            "ratio=%.3f frames=%zu arena=%lluMB hot=%s/%llu%s\n",
                            (unsigned long long)a.thunksForced,
                            (unsigned long long)a.thunksAllocated,
                            a.thunksAllocated
                                ? double(a.thunksForced) / double(a.thunksAllocated)
                                : 0.0,
                            vm.frames.size(),
                            (unsigned long long)(threadArena().bytesAllocated() >> 20),
                            !desc->name.empty() ? desc->name.c_str() : "<anon>",
                            (unsigned long long)desc->forceCount,
                            posBuf);
                        // #548c (2026-05-10): if V3_DBG_ALLOC_DUMP is
                        // also set, list the top-10 descriptors by
                        // (alloc + force) right here — atexit doesn't
                        // fire on `timeout` SIGKILL, so emitting at
                        // each progress tick guarantees we capture the
                        // hot pattern before the run terminates.
                        if (g_dbgAllocDump) {
                            struct R { uint64_t a, f; const LambdaDescriptor * d; };
                            std::vector<R> rows;
                            for (auto * cui : cuRegistry()) {
                                if (!cui) continue;
                                for (const auto & ld : cui->lambdas) {
                                    if (ld.allocCount + ld.forceCount < 1000)
                                        continue;
                                    rows.push_back({ld.allocCount, ld.forceCount, &ld});
                                }
                            }
                            std::sort(rows.begin(), rows.end(),
                                [](const R & x, const R & y) {
                                    return (x.a + x.f) > (y.a + y.f);
                                });
                            size_t lim = std::min<size_t>(rows.size(), 10);
                            std::fprintf(stderr,
                                "  top-10 hot descriptors:\n");
                            for (size_t i = 0; i < lim; ++i) {
                                const auto & r = rows[i];
                                const PosSnapshot * pps = resolvePosSnapshot(r.d->posHandle);
                                char b[256];
                                if (pps && !pps->file.empty())
                                    std::snprintf(b, sizeof b,
                                        "%s:%u:%u",
                                        pps->file.c_str(), pps->line, pps->column);
                                else
                                    std::snprintf(b, sizeof b,
                                        "<no-pos> codeOff=%u",
                                        r.d->codeOffset);
                                std::fprintf(stderr,
                                    "    a=%llu f=%llu %s @ %s\n",
                                    (unsigned long long)r.a,
                                    (unsigned long long)r.f,
                                    !r.d->name.empty() ? r.d->name.c_str() : "<anon>",
                                    b);
                            }
                        }
                    }
                }
            }
            // Synthesize a closure-like view for OP_GET_UPVALUE: we set
            // `closure` to a fake Closure pointer crafted from the thunk
            // tail.  Instead of allocating a temporary Closure, we build
            // one on the heap (cheap; thunk forcing is uncommon enough).
            Closure * fakeClo = Alloc::allocClosure(t->nUpvalues);
            fakeClo->desc = desc;
            fakeClo->nUpvalues = t->nUpvalues;
            fakeClo->capturedWiths = t->suspended.capturedWiths;
            fakeClo->cu = t->suspended.cu;
            for (uint16_t i = 0; i < t->nUpvalues; ++i) fakeClo->upvalues[i] = t->tail[i];

            ListVec * thunkWiths = t->suspended.capturedWiths;
            const CompilationUnit * thunkCu = t->suspended.cu ? t->suspended.cu : cu;

            // Same call-depth guard as OP_CALL — catches blackhole-style
            // recursion that doesn't go through OP_CALL (e.g. `let x = x;
            // in x`, where every reference to x re-enters via OP_FORCE).
            if (__builtin_expect(vm.frames.size() >= kMaxCallDepth, 0))
                throw std::runtime_error("v3 OP_FORCE: stack overflow; call depth exceeded "
                                          + std::to_string(kMaxCallDepth));

            // REVIEW §3: window between `t->state = Blackhole` and the
            // frame-push could leak orphan Black thunks if any step in
            // between threw bad_alloc (valueStack.resize, withStack
            // reads, position-pool accesses).  Snapshot prior state so
            // the catch path can revert.  Single thunk per OP_FORCE so
            // the snapshot is one ThunkState.
            ThunkState priorState = t->state;
            t->state = ThunkState::Blackhole;

            // ip on caller frame must be saved BEFORE the resize too,
            // so the catch path can leave it unchanged-but-correct.
            uint32_t priorCallerIp = vm.frames.back().ip;
            uint32_t priorCallerFlags = vm.frames.back().flags;
            vm.frames.back().ip = ip;
            // WC-38: mark the caller frame for force-retry. When the
            // pushed thunk's body returns, OP_RETURN's caller-resume
            // path will re-enter op_force_slow if retVal is still a
            // Thunk/App.  This replaces the over-eager OP_RETURN
            // chain push with GHC STG-style consumer-driven chase.
            vm.frames.back().flags |= CFF_FORCE_RETRY;

            size_t newBase = vm.valueStack.size();
            uint32_t newWithBase;
            try {
                vm.valueStack.resize(newBase + desc->nLocals);
                newWithBase = static_cast<uint32_t>(vm.withStack.size());
            } catch (...) {
                // Revert: thunk back to Suspended, caller frame back
                // to its prior ip/flags.  Re-throw -- the v3 force
                // hook / outer eval will catch and route via
                // phaseBFailureCount or fallbackToTreeWalker.
                t->state = priorState;
                vm.frames.back().ip = priorCallerIp;
                vm.frames.back().flags = priorCallerFlags;
                throw;
            }

            // V3_DBG_STORE_PREVSTAGE: trace OP_FORCE pushes for thunks
            // with nUp=5 to verify their codeOffset before body runs.
            {
                static const bool s_dbg_force =
                    std::getenv("V3_DBG_STORE_PREVSTAGE") != nullptr;
                if (s_dbg_force && t->nUpvalues == 5) {
                    std::fprintf(stderr,
                        "v3 OP_FORCE: pushing thunk %p desc=%s codeOffset=%u "
                        "nUp=%u cu=%p\n",
                        (void*)t,
                        !desc->name.empty() ? desc->name.c_str() : "<anon>",
                        desc->codeOffset, (unsigned)t->nUpvalues,
                        (void*)thunkCu);
                }
            }
            // WC-38: V3_DBG_FORCE_TRACE=DEPTH — log every OP_FORCE
            // pushed at frame-stack depth >= DEPTH.  Used to identify
            // the eager-force divergence between v3 and tree-walker.
            // Frame depth filter avoids spam — only deep forces inside
            // pkgs's body are interesting.
            {
                static const char * s_dbg_force_trace =
                    std::getenv("V3_DBG_FORCE_TRACE");
                if (s_dbg_force_trace) {
                    static const size_t depthFilter =
                        std::atoll(s_dbg_force_trace);
                    if (vm.frames.size() >= depthFilter) {
                        // Resolve source position for direct trace-diff
                        // against tree-walker's TW_DBG_FORCE output.
                        const PosSnapshot * ps = resolvePosSnapshot(desc->posHandle);
                        if (ps && !ps->file.empty()) {
                            std::fprintf(stderr,
                                "v3 FORCE: %s:%u:%u\n",
                                ps->file.c_str(), ps->line, ps->column);
                        } else {
                            std::fprintf(stderr,
                                "v3 FORCE: <?nopos> name=%s codeOff=%u\n",
                                !desc->name.empty() ? desc->name.c_str() : "<anon>",
                                desc->codeOffset);
                        }
                    }
                }
            }

            vm.frames.push_back(CallFrame{
                .cu = thunkCu,
                .closure = fakeClo,
                .thunk = t,
                .ip = desc->codeOffset,
                .stackBaseOffset = static_cast<uint32_t>(newBase),
                .withStackBase = newWithBase,
                .flags = CFF_THUNK_RETURN,
            });
            pushCapturedWiths(vm, thunkWiths);
            cu = thunkCu;

            ip = desc->codeOffset;
            closure = fakeClo;
            stackBase = newBase;
            break;
        }

        // --- Lists ---
        case OP_LIST_INIT: {
            uint32_t n = operand;
            // Empty list: skip the alloc, push the singleton.  Common
            // for default formals (`xs ? []`) and branch results.
            if (n == 0) {
                push(vm, Value::vEmptyList);
                break;
            }
            ListVec * l = Alloc::allocList(n);
            allocStats().listsAllocated++;
            for (uint32_t i = n; i > 0; --i) l->elems[i - 1] = pop(vm);
            Value v;
            v.tag_payload = static_cast<uint64_t>(Tag::List);
            v.payload.list = l;
            push(vm, v);
            break;
        }
        case OP_LIST_CONCAT: {
            Value rhs = pop(vm), lhs = pop(vm);
            // Force-on-receive: lazy values (Tag::App from mapAttrs/
            // map/zipAttrsWith, Tag::Thunk from chained AttrSelects)
            // must be forced before shape-checking.  See WC-35.
            if (lhs.tag() == Tag::App || lhs.tag() == Tag::Thunk || lhs.tag() == Tag::Slot) {
                vm.frames.back().ip = ip;
                lhs = forceValue(vm, lhs);
            }
            if (rhs.tag() == Tag::App || rhs.tag() == Tag::Thunk || rhs.tag() == Tag::Slot) {
                vm.frames.back().ip = ip;
                rhs = forceValue(vm, rhs);
            }
            if (!lhs.isList() || !rhs.isList())
                throw std::runtime_error("v3 OP_LIST_CONCAT: not lists");
            uint32_t n = lhs.payload.list->size + rhs.payload.list->size;
            ListVec * out = Alloc::allocList(n);
            allocStats().listsAllocated++;
            uint32_t k = 0;
            for (uint32_t i = 0; i < lhs.payload.list->size; ++i) out->elems[k++] = lhs.payload.list->elems[i];
            for (uint32_t i = 0; i < rhs.payload.list->size; ++i) out->elems[k++] = rhs.payload.list->elems[i];
            Value v;
            v.tag_payload = static_cast<uint64_t>(Tag::List);
            v.payload.list = out;
            push(vm, v);
            break;
        }

        // --- Attrsets ---
        case OP_ATTRS_INIT: {
            uint32_t n = operand;
            // Empty attrset: skip the alloc entirely, push the singleton.
            // Real-world Nix code creates many empty attrsets (default
            // formal `... ? {}`, branch results etc.) — not allocating
            // them is cheap and reduces GC pressure.
            if (n == 0) {
                push(vm, Value::vEmptyAttrs);
                break;
            }
            // REVIEW MED-6: build entries directly into either a stack
            // buffer (most attrsets are small) or a heap fallback when
            // n exceeds kSmall.  Pre-fix used four std::vectors per
            // call (names/poses/values/entries); now zero allocations
            // for n <= kSmall and one for the heap fallback.
            struct Entry { SymbolId name; Value value; uint32_t pos; };
            constexpr uint32_t kSmall = 16;
            Entry smallBuf[kSmall];
            std::vector<Entry> bigBuf;
            Entry * entries;
            if (n <= kSmall) {
                entries = smallBuf;
            } else {
                bigBuf.resize(n);
                entries = bigBuf.data();
            }
            // Each entry is (SymbolId, PosIdx) inlined as 2 code words
            // followed by n popped values.  PosIdx feeds the per-attr
            // position side-table backing builtins.unsafeGetAttrPos.
            for (uint32_t i = 0; i < n; ++i) {
                entries[i].name = static_cast<SymbolId>(cu->code[ip + 2 * i]);
                entries[i].pos  = cu->code[ip + 2 * i + 1];
            }
            ip += 2 * n;
            for (uint32_t i = n; i > 0; --i) entries[i - 1].value = pop(vm);
            // Sort by name; duplicates become adjacent.
            std::sort(entries, entries + n,
                      [](const Entry & a, const Entry & b) { return a.name < b.name; });
            for (uint32_t i = 1; i < n; ++i) {
                if (entries[i].name == entries[i - 1].name) {
                    const auto & tbl = ir::globalSymbolTable();
                    SymbolId nm = entries[i].name;
                    std::string s = (nm < tbl.size()) ? tbl[nm] : "?";
                    throw std::runtime_error("v3 OP_ATTRS_INIT: attribute '" + s +
                                              "' already defined");
                }
            }
            Bindings * b = Alloc::allocBindings(n);
            allocStats().attrsetsAllocated++;
            for (uint32_t i = 0; i < n; ++i) {
                b->entries[i].name  = entries[i].name;
                b->entries[i].value = entries[i].value;
                recordAttrPos(b, entries[i].name, entries[i].pos);
            }
            Value v;
            v.tag_payload = static_cast<uint64_t>(Tag::Attrs);
            v.payload.bindings = b;
            publishToNearestBlackThunkFrame(vm, v, /*isRecInit=*/false);
            push(vm, v);
            break;
        }
        case OP_ATTRS_INIT_DYN: {
            uint32_t nStatic = (operand >> 12) & 0xFFFu;
            uint32_t nDyn    = operand & 0xFFFu;
            // Stack layout (bottom-up): [static values...][dyn name+value pairs...]
            uint32_t totalDynVals = nDyn * 2;
            std::vector<Value> dynPairs(totalDynVals);
            for (uint32_t i = totalDynVals; i > 0; --i) dynPairs[i - 1] = pop(vm);
            std::vector<Value> staticVals(nStatic);
            for (uint32_t i = nStatic; i > 0; --i) staticVals[i - 1] = pop(vm);
            // Inline layout: nStatic*(name, pos) pairs followed by nDyn
            // pos words for the dynamic entries.
            std::vector<SymbolId> staticNames(nStatic);
            std::vector<uint32_t> staticPoses(nStatic);
            for (uint32_t i = 0; i < nStatic; ++i) {
                staticNames[i] = static_cast<SymbolId>(cu->code[ip + 2 * i]);
                staticPoses[i] = cu->code[ip + 2 * i + 1];
            }
            ip += 2 * nStatic;
            std::vector<uint32_t> dynPoses(nDyn);
            for (uint32_t i = 0; i < nDyn; ++i)
                dynPoses[i] = cu->code[ip + i];
            ip += nDyn;

            std::vector<std::tuple<SymbolId, Value, uint32_t>> entries;
            entries.reserve(nStatic + nDyn);
            for (uint32_t i = 0; i < nStatic; ++i)
                entries.emplace_back(staticNames[i], staticVals[i], staticPoses[i]);
            for (uint32_t i = 0; i < nDyn; ++i) {
                Value & nameV = dynPairs[i * 2];
                Value & valV  = dynPairs[i * 2 + 1];
                // null-named dynamic attrs are silently dropped — Nix
                // semantics so things like `{ ${if cond then "k" else null}
                // = v; }` work as a conditional add.
                if (nameV.isNull()) continue;
                if (!nameV.isString())
                    throw std::runtime_error("v3 OP_ATTRS_INIT_DYN: dynamic name must be a string");
                // Use the global symbol table — IDs from any CU stay
                // consistent so attrset lookups across CUs work.
                SymbolId id = ir::globalInternSymbol(nameV.payload.str);
                entries.emplace_back(id, valV, dynPoses[i]);
            }
            std::sort(entries.begin(), entries.end(),
                      [](auto & a, auto & b) { return std::get<0>(a) < std::get<0>(b); });
            // Dup-attr detection: after sort, duplicates are adjacent.
            for (size_t i = 1; i < entries.size(); ++i) {
                if (std::get<0>(entries[i]) == std::get<0>(entries[i - 1])) {
                    const auto & tbl = ir::globalSymbolTable();
                    SymbolId nm = std::get<0>(entries[i]);
                    std::string s = (nm < tbl.size()) ? tbl[nm] : "?";
                    throw std::runtime_error("v3 OP_ATTRS_INIT_DYN: attribute '" + s +
                                              "' already defined");
                }
            }
            Bindings * b = Alloc::allocBindings(static_cast<uint32_t>(entries.size()));
            allocStats().attrsetsAllocated++;
            for (size_t i = 0; i < entries.size(); ++i) {
                b->entries[i].name  = std::get<0>(entries[i]);
                b->entries[i].value = std::get<1>(entries[i]);
                recordAttrPos(b, std::get<0>(entries[i]), std::get<2>(entries[i]));
            }
            Value v;
            v.tag_payload = static_cast<uint64_t>(Tag::Attrs);
            v.payload.bindings = b;
            publishToNearestBlackThunkFrame(vm, v, /*isRecInit=*/false);
            push(vm, v);
            break;
        }
        case OP_ATTRS_REC_INIT: {
            // #498 diagnostic: log local[0] of the current frame at
            // OP_ATTRS_REC_INIT for "final"-named maker frames.
            static const bool s_dbgRecInitLocal0 =
                std::getenv("V3_DBG_REC_INIT_LOCAL0") != nullptr;
            if (__builtin_expect(s_dbgRecInitLocal0, 0)) {
                const auto & fr = vm.frames.back();
                const LambdaDescriptor * d = nullptr;
                if (fr.closure) d = fr.closure->desc;
                else if (fr.thunk) d = fr.thunk->suspended.desc;
                if (d && d->name == "final") {
                    Value v = vm.valueStack[fr.stackBaseOffset + 0];
                    Value chase = v;
                    int hops = 0;
                    while (hops < 4) {
                        if (chase.tag() == Tag::Slot && chase.payload.slot)
                            chase = *chase.payload.slot;
                        else if (chase.tag() == Tag::Thunk && chase.payload.thunk
                                 && chase.payload.thunk->state == ThunkState::Evaluated)
                            chase = chase.payload.thunk->evaluated;
                        else break;
                        ++hops;
                    }
                    std::fprintf(stderr,
                        "v3 OP_ATTRS_REC_INIT in final codeOff=%u: local[0].tag=%d",
                        (unsigned)d->codeOffset, (int)v.tag());
                    if (chase.tag() == Tag::Attrs && chase.payload.bindings) {
                        const auto & st = ir::globalSymbolTable();
                        auto * b = chase.payload.bindings;
                        std::fprintf(stderr, " -> attrs size=%u {",
                            (unsigned)b->size);
                        for (uint32_t k = 0; k < b->size && k < 4; ++k) {
                            SymbolId nm = b->entries[k].name;
                            std::fprintf(stderr, "%s%s", k ? "," : "",
                                nm < st.size() ? st[nm].c_str() : "?");
                        }
                        std::fprintf(stderr, "}");
                    } else {
                        std::fprintf(stderr, " -> tag=%d", (int)chase.tag());
                    }
                    std::fprintf(stderr, " (frames=%zu)\n", vm.frames.size());
                }
            }
            // Allocate a Bindings(n) with placeholder values; values
            // are written later by OP_ATTRS_REC_SET[slot].  Names come
            // pre-sorted from emit (LetRec emit sorts entries by
            // SymbolId before writing the data words and rewrites the
            // REC_SET operand to the sorted slot).  Each entry is
            // (SymbolId, PosIdx) — the PosIdx feeds the per-attr
            // position side-table.
            uint32_t n = operand;
            Bindings * b = Alloc::allocBindings(n);
            allocStats().attrsetsAllocated++;
            for (uint32_t i = 0; i < n; ++i) {
                SymbolId nm = static_cast<SymbolId>(cu->code[ip + 2 * i]);
                uint32_t ps = cu->code[ip + 2 * i + 1];
                b->entries[i].name = nm;
                b->entries[i].value.mkNull();
                recordAttrPos(b, nm, ps);
            }
            ip += 2 * n;
            Value v;
            v.tag_payload = static_cast<uint64_t>(Tag::Attrs);
            v.payload.bindings = b;
            // OP_ATTRS_REC_INIT is the ONLY callsite with isRecInit=true.
            // The rec attrset's Bindings* registered here is the same
            // pointer that subsequent OP_ATTRS_REC_SET writes into, so
            // self-reference recovery via partialBindings observes the
            // entries as they are filled in.
            publishToNearestBlackThunkFrame(vm, v, /*isRecInit=*/true);
            // #558 Phase 1.5 (2026-05-12) Cell-Update Everywhere:
            // publish the in-progress Bindings to the innermost
            // THUNK_RETURN frame's shapeCell.  Consumers that
            // forceValue this Black thunk can read *shapeCell to
            // get the partial state — without consulting the
            // cross-thunk partial-Bindings registry that causes
            // the #558 isFromBootstrapFiles cascade.
            //
            // Only the INNERMOST THUNK_RETURN frame is updated:
            // this Bindings is the running thunk's own in-progress
            // value, not the outer frames'.  (For tail-position
            // results, OP_ATTRS_REC_INIT_TAIL / OP_RETURN propagate
            // the final value up through the cell chain.)
            //
            // Gated by NIX_V3_CELL_EVERYWHERE=1 for safe rollout.
            {
                static const bool s_cellEverywhere =
                    std::getenv("NIX_V3_CELL_EVERYWHERE") != nullptr;
                if (__builtin_expect(s_cellEverywhere, 0)) {
                    for (size_t fi = vm.frames.size(); fi > 0; --fi) {
                        auto & fr = vm.frames[fi - 1];
                        if (!(fr.flags & CFF_THUNK_RETURN)) continue;
                        if (!fr.thunk) continue;
                        if (!fr.thunk->shapeCell) continue;
                        *fr.thunk->shapeCell = v;
                        break;  // innermost THUNK_RETURN only
                    }
                }
            }
            push(vm, v);
            break;
        }
        case OP_ATTRS_LET_REC_INIT: {
            // Bytecode-identical body to OP_ATTRS_REC_INIT (allocate a
            // placeholder rec-attrset with the n trailing (name, pos)
            // pairs; entries are filled in by following OP_ATTRS_REC_SET
            // ops sharing the same n-slot layout).
            //
            // KEY DIFFERENCE: no publishToNearestBlackThunkFrame.  The
            // emitter selects this opcode for `let ... in body` shapes
            // (lowerLet -> lowerLetRecCapture with hasBody=true), where
            // the rec-attrset is INTERMEDIATE state -- the surrounding
            // thunk's eventual return value is `body`'s evaluation,
            // NOT the recAttrs.  Publishing the placeholder recAttrs
            // (or its in-progress fill) to the surrounding Black thunk
            // sets thunk->state=Evaluated with a wrong-shape value,
            // which later participates in with-scope lookups and
            // produces "name X not found in with-scope" errors when
            // the with-source dereferences to that wrong shape.
            //
            // Specifically surfaces in lib.extends's body
            //   `final: let prev = f final; in prev // overlay final prev`
            // where `let prev = ...` ran inside lib.fix's `x`-thunk
            // body and published `{prev}` (size 1) onto `x`'s thunk.
            // Inner closures that captured `pkgs = self = final = x`
            // then saw `{prev}` instead of the full pkgs attrset and
            // failed to resolve `with pkgs; callPackage`.  See
            // CALLPACKAGE_BUG_2026-05-09.md for the full analysis.
            uint32_t n = operand;
            Bindings * b = Alloc::allocBindings(n);
            allocStats().attrsetsAllocated++;
            for (uint32_t i = 0; i < n; ++i) {
                SymbolId nm = static_cast<SymbolId>(cu->code[ip + 2 * i]);
                uint32_t ps = cu->code[ip + 2 * i + 1];
                b->entries[i].name = nm;
                b->entries[i].value.mkNull();
                recordAttrPos(b, nm, ps);
            }
            ip += 2 * n;
            Value v;
            v.tag_payload = static_cast<uint64_t>(Tag::Attrs);
            v.payload.bindings = b;
            push(vm, v);
            break;
        }
        case OP_ATTRS_REC_INIT_TAIL: {
            // #558 (2026-05-10): tail-return-AttrSet variant.
            // Bytecode-identical to OP_ATTRS_REC_INIT (allocates a
            // Bindings(n) with placeholder values, n trailing (name,
            // pos) pairs in the same layout) BUT registers the partial
            // Bindings with EVERY thunk frame on the call stack —
            // Black AND Suspended — using FIRST-WINS semantics.
            //
            // Emitted by the lowerer when the AttrSet IR's
            // `isFunctionReturn` flag is true — i.e. this AttrSet IS
            // the function body's tail-return value.  Outer thunks
            // currently waiting for this function's return are
            // therefore conceptually waiting for THIS AttrSet's
            // value, so registering with all of them lets `with self;`
            // / `with pkgs;`-style lookups find the in-progress
            // entries via the partial-Bindings peek path
            // (vm.cc:withLookup).
            //
            // See bytecode.hh OP_ATTRS_REC_INIT_TAIL doc + ir.hh
            // AttrSet::isFunctionReturn doc for the complete design
            // rationale.
            uint32_t n = operand;
            Bindings * b = Alloc::allocBindings(n);
            allocStats().attrsetsAllocated++;
            for (uint32_t i = 0; i < n; ++i) {
                SymbolId nm = static_cast<SymbolId>(cu->code[ip + 2 * i]);
                uint32_t ps = cu->code[ip + 2 * i + 1];
                b->entries[i].name = nm;
                b->entries[i].value.mkNull();
                recordAttrPos(b, nm, ps);
            }
            ip += 2 * n;
            Value v;
            v.tag_payload = static_cast<uint64_t>(Tag::Attrs);
            v.payload.bindings = b;
            publishToAllThunkFrames(vm, v);
            // #558 Phase 1.5 (2026-05-12) Cell-Update Everywhere
            // TAIL variant: tail-position result IS the function's
            // (and tail-call ancestors') value.  Update ALL outer
            // THUNK_RETURN frames' shapeCells — each gets its OWN
            // shapeCell updated, no cross-thunk pollution at
            // lookup time (consumers only read their own thunk's
            // shapeCell, never others').  Mirrors
            // publishToAllThunkFrames but per-thunk cell-targeted.
            {
                static const bool s_cellEverywhere =
                    std::getenv("NIX_V3_CELL_EVERYWHERE") != nullptr;
                if (__builtin_expect(s_cellEverywhere, 0)) {
                    for (size_t fi = vm.frames.size(); fi > 0; --fi) {
                        auto & fr = vm.frames[fi - 1];
                        if (!(fr.flags & CFF_THUNK_RETURN)) continue;
                        if (!fr.thunk) continue;
                        if (!fr.thunk->shapeCell) continue;
                        *fr.thunk->shapeCell = v;
                        // Note: NO break — update ALL outer frames
                        // (tail-position propagation).
                    }
                }
            }
            push(vm, v);
            break;
        }
        case OP_APPLY_OVERRIDES: {
            // Peek the attrset on top of stack.  If it has __overrides,
            // force it and merge each (name, value) into the rec attrs:
            //   - Names already present are overwritten in place (so
            //     OP_ATTRS_SELECT inside the rec body sees the new value).
            //   - New names cause a Bindings grow + re-sort so the result
            //     attrset visible to the outer scope contains them.
            //   This matches tree-walker semantics.
            Value & top = vm.valueStack.back();
            if (!top.isAttrs() || !top.payload.bindings) break;
            static const SymbolId ovId = ir::globalInternSymbol("__overrides");
            const Value * ovRaw = top.payload.bindings->lookup(ovId);
            if (!ovRaw) break;
            Value ov = forceValue(vm, *ovRaw);
            // Tree-walker raises if __overrides is present but not an
            // attrset; v3 silently ignored.
            if (!ov.isAttrs())
                throw std::runtime_error("v3 OP_APPLY_OVERRIDES: __overrides must be an attrset");
            if (!ov.payload.bindings) break;
            auto * dst = top.payload.bindings;
            const auto * src = ov.payload.bindings;
            // First pass: overwrite existing entries; collect names to add.
            std::vector<std::pair<SymbolId, Value>> toAdd;
            for (uint32_t i = 0; i < src->size; ++i) {
                SymbolId k = src->entries[i].name;
                const Value * existing = dst->lookup(k);
                if (existing) {
                    // Mutate in place via const_cast — `lookup` returns a
                    // pointer to the actual storage and we own this Bindings.
                    const_cast<Value &>(*existing) = src->entries[i].value;
                } else {
                    toAdd.emplace_back(k, src->entries[i].value);
                }
            }
            if (!toAdd.empty()) {
                Bindings * grown = Alloc::allocBindings(dst->size + toAdd.size());
                allocStats().attrsetsAllocated++;
                std::vector<std::pair<SymbolId, Value>> all;
                all.reserve(dst->size + toAdd.size());
                for (uint32_t i = 0; i < dst->size; ++i)
                    all.emplace_back(dst->entries[i].name, dst->entries[i].value);
                for (auto & e : toAdd) all.push_back(e);
                std::sort(all.begin(), all.end(),
                    [](auto & a, auto & b) { return a.first < b.first; });
                for (size_t i = 0; i < all.size(); ++i) {
                    grown->entries[i].name  = all[i].first;
                    grown->entries[i].value = all[i].second;
                }
                top.payload.bindings = grown;
            }
            break;
        }
        case OP_ATTRS_SELECT: {
            Value attrs = pop(vm);
            // #458 step A.3: same per-attr peek as OP_WITH_LOOKUP for
            // Bridge thunks.  When attrs is a TW Value bridged into v3
            // and the outer thunk's type is already nAttrs (Bindings
            // built; entries may still be thunks), look up just our
            // operand symbol and bridge the single Attr -- avoiding
            // deep `treeWalkerToV3Public` conversion that would walk
            // every entry and risk fix-point cycles.  Works in concert
            // with A.2 for the broader slot-threading-for-fix-points
            // story.  IC cache update is skipped on this path -- the
            // per-attr bridge result has no v3-side Bindings to cache
            // against (and the IC's purpose is amortizing v3-internal
            // Bindings* shape-keyed lookups).
            if (attrs.isThunk() && attrs.payload.thunk
                && attrs.payload.thunk->state == ThunkState::Bridge) {
                // The opcode's 24-bit operand is the SymbolId.
                if (auto v = tryBridgeAttrLookup(
                        attrs.payload.thunk,
                        static_cast<SymbolId>(operand))) {
                    push(vm, *v);
                    ip++;  // consume the icIdx operand word we'd
                           // otherwise read at line below
                    break;
                }
                // peek didn't resolve -- fall through to wholesale
                // force.  May still succeed (if not in a cycle) or
                // throw BlackholeError that propagates correctly.
            }
            // Force lazy shapes (Tag::App from mapAttrs entries, Thunks
            // from chained AttrSelects).  Same rationale as OP_CALL —
            // tree-walker forces target before AttrSelect; v3's lower
            // emits an explicit OP_FORCE most of the time, but App/Thunk
            // values can sneak through via OP_RETURN's no-chase
            // semantics.  Cheap on already-forced values.
            //
            // #558 (2026-05-10) partial-Bindings peek for OP_ATTRS_SELECT:
            // when the source is a Black thunk in mid-construction (the
            // canonical lib.fix `let x = f x; in x` shape, where x is
            // currently being forced and an inner `self.X` access tries
            // to select through it), peek the partial-Bindings registry
            // chain BEFORE forcing.  If the chain has an entry for the
            // looked-up name, return it — avoids the BlackholeError that
            // forceValue would throw on the Black thunk.
            //
            // Mirror of the OP_WITH_LOOKUP partial-Bindings peek path
            // (vm.cc:withLookup) but for direct Select access.  Both
            // paths share the same registry chain (populated by
            // OP_ATTRS_REC_INIT_TAIL via publishToAllThunkFrames).
            // #558 (2026-05-10) Chase Evaluated thunks to find a
            // potentially-Black target.  When `attrs` is a thunk that
            // was Evaluated to another thunk (e.g., the inherit-from
            // cache thunk's `evaluated` was set to a recovered
            // Tag::Thunk for the outer Black fix-point), the chain
            // peek path SHOULD fire on the chased target.  Without
            // this chase, we'd see Tag::Attrs (from STG WHNF's
            // recovery in the cache thunk's body) which doesn't have
            // all the chain layers' keys.
            {
                Value chase = attrs;
                int hops = 0;
                while (hops < 8
                       && chase.isThunk()
                       && chase.payload.thunk
                       && chase.payload.thunk->state == ThunkState::Evaluated
                       && chase.payload.thunk->evaluated.isThunk())
                {
                    chase = chase.payload.thunk->evaluated;
                    ++hops;
                }
                if (chase.isThunk()
                    && chase.payload.thunk
                    && chase.payload.thunk->state == ThunkState::Blackhole)
                {
                    auto & reg = partialBindingsRegistry();
                    auto it = reg.find(chase.payload.thunk);
                    if (it != reg.end()) {
                        if (auto * v = lookupInPartialChain(
                                it->second,
                                static_cast<SymbolId>(operand))) {
                            push(vm, *v);
                            ip++;
                            break;
                        }
                    }
                }
            }
            if (attrs.isThunk() && attrs.payload.thunk
                && attrs.payload.thunk->state == ThunkState::Blackhole)
            {
                auto & reg = partialBindingsRegistry();
                auto it = reg.find(attrs.payload.thunk);
                if (it != reg.end()) {
                    if (auto * v = lookupInPartialChain(
                            it->second,
                            static_cast<SymbolId>(operand))) {
                        // Diagnostic: when chain peek returns a thunk
                        // value (potentially the cycle-creating value),
                        // log the source thunk + bindings.
                        // V3_DBG_PEEK_THUNK=1.
                        static const bool s_dbgPeekThunk =
                            std::getenv("V3_DBG_PEEK_THUNK") != nullptr;
                        if (s_dbgPeekThunk && v->isThunk()) {
                            const auto & st = ir::globalSymbolTable();
                            std::fprintf(stderr,
                                "v3 OP_ATTRS_SELECT chain-peek: source=%p sym='%s' result-tag=%d result-thunk=%p chain-depth=%zu\n",
                                (void *)attrs.payload.thunk,
                                operand < st.size() ? st[operand].c_str() : "?",
                                (int)v->tag(),
                                (void *)v->payload.thunk,
                                it->second.size());
                            for (size_t li = 0; li < it->second.size(); ++li) {
                                Bindings * b = it->second[li];
                                std::fprintf(stderr,
                                    "  layer[%zu] bindings=%p size=%u\n",
                                    li, (void *)b, b ? b->size : 0);
                            }
                        }
                        push(vm, *v);
                        ip++;  // consume the icIdx operand word
                        break;
                    }
                }
            }
            if (attrs.tag() == Tag::App || attrs.tag() == Tag::Thunk || attrs.tag() == Tag::Slot) {
                vm.frames.back().ip = ip;
                try {
                    attrs = forceValue(vm, attrs);
                } catch (const BlackholeError &) {
                    // Last-chance peek for partial Bindings (in case
                    // the chase landed on a Black thunk we hadn't
                    // seen at the top level).  Mirrors the
                    // OP_WITH_LOOKUP catch-and-peek pattern.
                    if (attrs.isThunk() && attrs.payload.thunk) {
                        auto & reg = partialBindingsRegistry();
                        auto it = reg.find(attrs.payload.thunk);
                        if (it != reg.end()) {
                            if (auto * v = lookupInPartialChain(
                                    it->second,
                                    static_cast<SymbolId>(operand))) {
                                push(vm, *v);
                                ip++;
                                break;
                            }
                        }
                    }
                    throw;
                }
                // #558 (2026-05-10) Post-force chain peek.  When
                // forceValue returns Tag::Thunk Black (under the
                // Tag::Thunk-deferral STG WHNF semantics), use chain
                // peek to walk all chain layers.  forceValue defers
                // instead of collapsing to chain.back() so that
                // consumers see the full chain.
                if (attrs.isThunk() && attrs.payload.thunk
                    && attrs.payload.thunk->state == ThunkState::Blackhole)
                {
                    auto & reg = partialBindingsRegistry();
                    auto it = reg.find(attrs.payload.thunk);
                    if (it != reg.end()) {
                        if (auto * v = lookupInPartialChain(
                                it->second,
                                static_cast<SymbolId>(operand))) {
                            push(vm, *v);
                            ip++;
                            break;
                        }
                    }
                }
            }
            if (!attrs.isAttrs()) {
                // #558 (2026-05-10) diagnostic: log tag + symbol + frame
                // chain when a Select fails on a non-attrs value.  Used
                // to root-cause the v3-vs-TW divergence on nixpkgs.
                static const bool s_dbgSelFail =
                    std::getenv("V3_DBG_SELECT_FAIL") != nullptr;
                if (s_dbgSelFail) {
                    const auto & st = ir::globalSymbolTable();
                    std::fprintf(stderr,
                        "v3 OP_ATTRS_SELECT: not an attrset (tag=%d) "
                        "looking up '%s' (frames=%zu)\n",
                        (int)attrs.tag(),
                        operand < st.size() ? st[operand].c_str() : "?",
                        vm.frames.size());
                    size_t lim = vm.frames.size();
                    for (size_t fi = lim; fi > 0 && fi + 12 > lim; --fi) {
                        const auto & frD = vm.frames[fi - 1];
                        const LambdaDescriptor * d = nullptr;
                        if (frD.thunk
                            && (frD.thunk->state == ThunkState::Suspended
                                || frD.thunk->state == ThunkState::Blackhole))
                            d = frD.thunk->suspended.desc;
                        else if (frD.closure)
                            d = frD.closure->desc;
                        const PosSnapshot * ps =
                            d ? resolvePosSnapshot(d->posHandle) : nullptr;
                        std::fprintf(stderr,
                            "  [%zu] %s ip=%u thunk=%p flags=%u %s:%u:%u\n",
                            fi - 1,
                            d && !d->name.empty() ? d->name.c_str() : "<?>",
                            frD.ip,
                            (void *)frD.thunk,
                            (unsigned)frD.flags,
                            (ps && !ps->file.empty()) ? ps->file.c_str() : "?",
                            ps ? ps->line : 0u,
                            ps ? ps->column : 0u);
                    }
                    std::fflush(stderr);
                }
                throw std::runtime_error("v3 OP_ATTRS_SELECT: not an attrset");
            }
            uint32_t icIdx = cu->code[ip++];
            // REVIEW §3: IC entries key on (Bindings* shape pointer +
            // slot index + sym).  Safe because Bindings::entries is a
            // FAM allocated alongside Bindings -- the pointer to a
            // specific entry doesn't move once the Bindings is built.
            // OP_APPLY_OVERRIDES grows the entries vector via realloc
            // (see vm.cc:2301+), but that's a different Bindings* so
            // the IC entry doesn't alias.  If a future op were to
            // mutate an existing Bindings in-place (resize entries[]),
            // every cached entry pointer would dangle -- update this
            // comment to add the assertion.
            auto & ic = cu->attrSelectCache[icIdx];
            // Phase 13.3: non-const so we can write back the resolved
            // value of a Tag::App entry — mapAttrs et al. install lazy
            // App(App(fn,name),val) entries that, without memoization,
            // re-apply the function on every access.
            auto * b = attrs.payload.bindings;
            // V3_DBG_PREHOOK diagnostic: log every ATTRS_SELECT preHook
            // attempt with what value it returns.  Used to localize WC-37.
            static const bool s_dbg_prehook = std::getenv("V3_DBG_PREHOOK") != nullptr;
            if (s_dbg_prehook) {
                static const SymbolId preHookSym = ir::globalInternSymbol("preHook");
                if (operand == preHookSym) {
                    static int call_n = 0;
                    ++call_n;
                    std::fprintf(stderr,
                        "v3 ATTRS_SELECT preHook (#%d): bindings=%p size=%u attrs:\n",
                        call_n, (void*)b, b ? b->size : 0);
                    if (b) {
                        const auto & st = ir::globalSymbolTable();
                        for (uint32_t i = 0; i < b->size && i < 30; ++i) {
                            SymbolId nm = b->entries[i].name;
                            const Value & vv = b->entries[i].value;
                            Tag vtag = vv.tag();
                            std::fprintf(stderr, "  [%u] %s tag=%u",
                                i, nm < st.size() ? st[nm].c_str() : "?",
                                (unsigned)vtag);
                            if (vtag == Tag::Thunk && vv.payload.thunk) {
                                Thunk * t = vv.payload.thunk;
                                const LambdaDescriptor * d = nullptr;
                                if (t->state == ThunkState::Suspended)
                                    d = t->suspended.desc;
                                std::fprintf(stderr, " state=%d nUp=%u",
                                    (int)t->state, (unsigned)t->nUpvalues);
                                if (d)
                                    std::fprintf(stderr, " %s [%u..)",
                                        !d->name.empty() ? d->name.c_str() : "<anon>",
                                        d->codeOffset);
                                // For preHook entry [0], also dump the
                                // thunk's body bytecode + upvalue tags.
                                if (i == 0 && t->state == ThunkState::Suspended && d) {
                                    std::fprintf(stderr, "\n    body [%u..%u):\n",
                                        d->codeOffset, d->codeOffset + 200);
                                    if (t->suspended.cu)
                                        disassembleWindow(stderr, *t->suspended.cu,
                                            d->codeOffset, d->codeOffset + 200);
                                    // Dump the FUNCTION DESCRIPTORS of every
                                    // MAKE_THUNK target in this body — names
                                    // like "recref-X" tell us what each
                                    // upvalue resolves to in source.
                                    if (t->suspended.cu) {
                                        const auto & cu2 = *t->suspended.cu;
                                        std::fprintf(stderr, "    referenced functions:\n");
                                        for (uint32_t cur = d->codeOffset;
                                             cur < d->codeOffset + 80 && cur < cu2.code.size(); ) {
                                            Op op = decodeOp(cu2.code[cur]);
                                            uint32_t operand = decodeOperand(cu2.code[cur]);
                                            if (op == OP_MAKE_THUNK || op == OP_MAKE_CLOSURE) {
                                                if (operand < cu2.lambdas.size()) {
                                                    const auto & d3 = cu2.lambdas[operand];
                                                    std::fprintf(stderr,
                                                        "      [%u] -> fn[%u] (%s, nUp=%u, code=[%u..))\n",
                                                        cur, operand,
                                                        !d3.name.empty() ? d3.name.c_str() : "<anon>",
                                                        d3.nUpvalues, d3.codeOffset);
                                                    // Also dump the body of recref- thunks
                                                    if (d3.name.find("recref-") == 0) {
                                                        std::fprintf(stderr, "        body:\n");
                                                        disassembleWindow(stderr, cu2,
                                                            d3.codeOffset, d3.codeOffset + 6);
                                                    }
                                                }
                                                // #530: encode word + nUpvalues + nWithTargets.
                                                cur += 3;
                                            } else {
                                                cur++;
                                            }
                                        }
                                    }
                                    std::fprintf(stderr, "    upvalues:\n");
                                    for (uint16_t u = 0; u < t->nUpvalues && u < 8; ++u) {
                                        const Value & uv = t->tail[u];
                                        Tag ut = uv.tag();
                                        std::fprintf(stderr, "      [%u] tag=%u", u, (unsigned)ut);
                                        if (ut == Tag::Closure && uv.payload.closure
                                            && uv.payload.closure->desc) {
                                            auto * cd = uv.payload.closure->desc;
                                            std::fprintf(stderr, " closure=%s [%u..) nUp=%u",
                                                !cd->name.empty() ? cd->name.c_str() : "<anon>",
                                                cd->codeOffset, uv.payload.closure->nUpvalues);
                                        } else if (ut == Tag::Thunk && uv.payload.thunk) {
                                            Thunk * ut2 = uv.payload.thunk;
                                            std::fprintf(stderr, " state=%d nUp=%u",
                                                (int)ut2->state, (unsigned)ut2->nUpvalues);
                                            if (ut2->state == ThunkState::Suspended) {
                                                auto * d2 = ut2->suspended.desc;
                                                if (d2)
                                                    std::fprintf(stderr, " %s [%u..)",
                                                        !d2->name.empty() ? d2->name.c_str() : "<anon>",
                                                        d2->codeOffset);
                                            } else if (ut2->state == ThunkState::Evaluated) {
                                                Tag et = ut2->evaluated.tag();
                                                std::fprintf(stderr, " EVAL=tag%u", (unsigned)et);
                                                if (et == Tag::Closure && ut2->evaluated.payload.closure
                                                    && ut2->evaluated.payload.closure->desc) {
                                                    auto * cd = ut2->evaluated.payload.closure->desc;
                                                    std::fprintf(stderr, "(%s [%u..) nUp=%u)",
                                                        !cd->name.empty() ? cd->name.c_str() : "<anon>",
                                                        cd->codeOffset, ut2->evaluated.payload.closure->nUpvalues);
                                                }
                                            }
                                        } else if (ut == Tag::Attrs && uv.payload.bindings) {
                                            auto * b2 = uv.payload.bindings;
                                            std::fprintf(stderr, " attrs size=%u {", b2->size);
                                            const auto & st2 = ir::globalSymbolTable();
                                            for (uint32_t k = 0; k < b2->size && k < 30; ++k) {
                                                SymbolId nm = b2->entries[k].name;
                                                std::fprintf(stderr, "%s%s",
                                                    k ? "," : "",
                                                    nm < st2.size() ? st2[nm].c_str() : "?");
                                            }
                                            std::fprintf(stderr, "}");
                                        }
                                        std::fprintf(stderr, "\n");
                                    }
                                }
                                if (t->state == ThunkState::Evaluated) {
                                    Tag etag = t->evaluated.tag();
                                    std::fprintf(stderr, " EVAL=tag%u", (unsigned)etag);
                                    if (etag == Tag::Closure && t->evaluated.payload.closure
                                        && t->evaluated.payload.closure->desc) {
                                        auto * ed = t->evaluated.payload.closure->desc;
                                        std::fprintf(stderr, "(%s [%u..) nUp=%u)",
                                            !ed->name.empty() ? ed->name.c_str() : "<anon>",
                                            ed->codeOffset,
                                            t->evaluated.payload.closure->nUpvalues);
                                    } else if (etag == Tag::String && t->evaluated.payload.str) {
                                        std::fprintf(stderr, "(\"%.40s\")",
                                            t->evaluated.payload.str);
                                    }
                                }
                            } else if (vtag == Tag::Closure && vv.payload.closure
                                && vv.payload.closure->desc) {
                                auto * d = vv.payload.closure->desc;
                                std::fprintf(stderr, " %s [%u..) nUp=%u",
                                    !d->name.empty() ? d->name.c_str() : "<anon>",
                                    d->codeOffset, vv.payload.closure->nUpvalues);
                            }
                            std::fprintf(stderr, "\n");
                        }
                    }
                }
            }
            // EVAL-COMP §8.1: 4-way polymorphic IC fast path.  Walk
            // the small entries array; on hit, read the cached slot
            // directly.  Hit at any way is O(kWays) compares, vs.
            // O(log n) binary search on miss.
            //
            // V3_DBG_NO_IC disables the fast-path -- bisect aid for
            // suspected IC corruption.
            static const bool s_no_ic = std::getenv("V3_DBG_NO_IC") != nullptr;
            uint32_t hitSlot = UINT32_MAX;
            if (!s_no_ic) {
                for (int w = 0; w < cu->attrSelectCache[icIdx].kWays; ++w) {
                    auto & e = ic.entries[w];
                    if (e.bindings == b
                        && e.slot < b->size
                        && b->entries[e.slot].name == static_cast<SymbolId>(operand))
                    {
                        hitSlot = e.slot;
                        break;
                    }
                }
            }
            if (hitSlot != UINT32_MAX) {
                Value & slot = b->entries[hitSlot].value;
                // Phase 13.3 mapAttrs memo (IC fast path).  Without
                // writeback, every access to a mapAttrs entry re-applies
                // its function -- confirmed via per-descriptor force
                // counter (parse.nix:60:44 = 625K forces on a 2-stage
                // probe).  Tree-walker mutates the slot via
                // `forceValue(*v)`; mirror that here.
                if (__builtin_expect(slot.tag() == Tag::App, 0)) {
                    vm.frames.back().ip = ip;
                    Value resolved = forceValue(vm, slot);
                    slot = resolved;
                    push(vm, resolved);
                } else {
                    push(vm, slot);
                }
            } else {
                // Manual binary search inlined to also recover the
                // matched slot index, so we can install in the cache.
                uint32_t lo = 0, hi = b->size;
                while (lo < hi) {
                    uint32_t mid = (lo + hi) >> 1;
                    SymbolId midName = b->entries[mid].name;
                    if (midName == static_cast<SymbolId>(operand)) { lo = mid; break; }
                    if (midName < static_cast<SymbolId>(operand)) lo = mid + 1; else hi = mid;
                }
                if (lo >= b->size || b->entries[lo].name != static_cast<SymbolId>(operand)) {
                    // #558 (2026-05-10) Registry-wide chain peek
                    // recovery.  When the lookup misses on this
                    // single Bindings, search the partial-Bindings
                    // registry for any thunk whose chain contains
                    // THIS Bindings as a layer.  If found, walk that
                    // thunk's full chain (lookupInPartialChain) for
                    // the symbol — recovers access to OTHER chain
                    // layers that the chain.back()-collapse in OP_FORCE
                    // / forceValue lost.
                    //
                    // STG analog: when forcing collapsed an indirect
                    // chain to a single shape, the lookup may need
                    // to chase through other shapes that aren't
                    // visible from the collapsed result.
                    //
                    // Cost: O(chains * layers) on each miss.  In
                    // practice misses are rare (most lookups hit
                    // directly), so this is acceptable.
                    //
                    // Gated by NIX_V3_NO_REGISTRY_PEEK=1 for bisecting.
                    static const bool s_noRegistryPeek =
                        std::getenv("NIX_V3_NO_REGISTRY_PEEK") != nullptr;
                    if (!s_noRegistryPeek) {
                        auto & reg = partialBindingsRegistry();
                        static const bool s_dbgRegPeek =
                            std::getenv("V3_DBG_REGISTRY_PEEK") != nullptr;
                        size_t regSize = reg.size();
                        size_t matchedThunks = 0;
                        for (auto & kv : reg) {
                            const auto & chain = kv.second;
                            // Check if THIS Bindings is in the chain.
                            bool match = false;
                            for (auto * cb : chain) {
                                if (cb == b) { match = true; break; }
                            }
                            if (!match) continue;
                            ++matchedThunks;
                            // Walk chain for the symbol.
                            if (auto * v = lookupInPartialChain(
                                    chain, static_cast<SymbolId>(operand))) {
                                if (s_dbgRegPeek) std::fprintf(stderr,
                                    "v3 registry-peek HIT: bindings=%p sym=%u found in chain (depth=%zu)\n",
                                    (void *)b, operand, chain.size());
                                push(vm, *v);
                                goto attrs_select_done;
                            }
                        }
                        if (s_dbgRegPeek) std::fprintf(stderr,
                            "v3 registry-peek MISS: bindings=%p sym=%u registry-size=%zu matched-thunks=%zu\n",
                            (void *)b, operand, regSize, matchedThunks);
                    }
                    // WC-21 diagnostic: dump requested attr + present
                    // attr names to help root-cause closure-bridge
                    // attr-shape divergences.  Off by default.
                    static const bool dbg = std::getenv("V3_DBG_ATTRS_SELECT") != nullptr;
                    if (dbg) {
                        auto & symTab = ir::globalSymbolTable();
                        SymbolId want = static_cast<SymbolId>(operand);
                        std::fprintf(stderr,
                            "v3 OP_ATTRS_SELECT miss: want sid=%u name=\"%s\" "
                            "bindings=%p size=%u ip=%u present=[",
                            (unsigned)want,
                            want < symTab.size() ? symTab[want].c_str() : "?",
                            (void*)b, (unsigned)b->size, (unsigned)ip);
                        for (uint32_t i = 0; i < b->size && i < 20; ++i) {
                            SymbolId nm = b->entries[i].name;
                            std::fprintf(stderr, "%s%s",
                                i ? "," : "",
                                nm < symTab.size() ? symTab[nm].c_str() : "?");
                        }
                        if (b->size > 20) std::fprintf(stderr, ",...");
                        std::fprintf(stderr, "]\n");
                        // Frame stack so we can identify WHICH function
                        // emitted this OP_ATTRS_SELECT.
                        std::fprintf(stderr, "  frame stack size=%zu (top first):\n",
                            vm.frames.size());
                        for (size_t fi = vm.frames.size(); fi > 0; --fi) {
                            const auto & fr = vm.frames[fi - 1];
                            const LambdaDescriptor * d = nullptr;
                            if (fr.thunk && fr.thunk->state == ThunkState::Blackhole)
                                d = fr.thunk->suspended.desc;
                            else if (fr.closure)
                                d = fr.closure->desc;
                            std::fprintf(stderr,
                                "    [%zu] %s ip=%u thunk=%p closure=%p flags=%u\n",
                                fi - 1,
                                d && !d->name.empty() ? d->name.c_str() : "<?>",
                                fr.ip,
                                (void*)fr.thunk, (void*)fr.closure,
                                (unsigned)fr.flags);
                        }
                        if (cu) {
                            uint32_t lo = ip > 16 ? ip - 16 : 0;
                            uint32_t hi = ip + 8;
                            std::fprintf(stderr,
                                "  failing-frame disasm [%u..%u):\n", lo, hi);
                            disassembleWindow(stderr, *cu, lo, hi);
                        }
                        std::fflush(stderr);
                    }
                    throw std::runtime_error("v3 OP_ATTRS_SELECT: attribute not found");
                }
                // Install at the next eviction slot (round-robin).
                auto & evicted = ic.entries[ic.evictIdx];
                evicted.bindings = b;
                evicted.slot     = lo;
                ic.evictIdx = (ic.evictIdx + 1)
                    % CompilationUnit::AttrSelectIC::kWays;
                Value & slot = b->entries[lo].value;
                if (__builtin_expect(slot.tag() == Tag::App, 0)) {
                    vm.frames.back().ip = ip;
                    Value resolved = forceValue(vm, slot);
                    slot = resolved;
                    push(vm, resolved);
                } else {
                    push(vm, slot);
                }
            }
        attrs_select_done:
            break;
        }
        case OP_ATTRS_SELECT_DYN: {
            Value name = pop(vm), attrs = pop(vm);
            // Force lazy `name` too — attrs.${dynKey} where dynKey is
            // `formal.cpu` (now lazy via mapAttrs Tag::App entries) was
            // landing in OP_ATTRS_SELECT_DYN with name still in App form
            // and tripping `not a string`.
            if (name.tag() == Tag::App || name.tag() == Tag::Thunk || name.tag() == Tag::Slot) {
                vm.frames.back().ip = ip;
                name = forceValue(vm, name);
            }
            if (attrs.tag() == Tag::App || attrs.tag() == Tag::Thunk || attrs.tag() == Tag::Slot) {
                vm.frames.back().ip = ip;
                attrs = forceValue(vm, attrs);
            }
            if (!name.isString() || !attrs.isAttrs())
                throw std::runtime_error("v3 OP_ATTRS_SELECT_DYN: type error");
            // Intern via the global table so the SymbolId matches the
            // ones the attrset's bindings were built with.
            SymbolId id = ir::globalInternSymbol(name.payload.str);
            Value * found = attrs.payload.bindings->lookup(id);
            if (!found)
                throw std::runtime_error("v3 OP_ATTRS_SELECT_DYN: attribute not found");
            // Phase 13.3 mapAttrs memo (dynamic-name path).
            if (__builtin_expect(found->tag() == Tag::App, 0)) {
                vm.frames.back().ip = ip;
                Value resolved = forceValue(vm, *found);
                *found = resolved;
                push(vm, resolved);
            } else {
                push(vm, *found);
            }
            break;
        }
        case OP_ATTRS_HAS: {
            Value attrs = pop(vm);
            // #458 step A.4: per-attr peek for the Bridge thunk case.
            // Cheaper than tryBridgeAttrLookup -- no bridge of the value
            // is needed, just an existence check on the partial Bindings.
            if (attrs.isThunk() && attrs.payload.thunk
                && attrs.payload.thunk->state == ThunkState::Bridge) {
                auto r = tryBridgeAttrHas(
                    attrs.payload.thunk, static_cast<SymbolId>(operand));
                if (r == BridgeAttrHasResult::Present) {
                    push(vm, Value::vTrue);
                    break;
                }
                if (r == BridgeAttrHasResult::Absent) {
                    push(vm, Value::vFalse);
                    break;
                }
                // Indeterminate: src still thunk-shaped, fall through.
            }
            if (attrs.tag() == Tag::App || attrs.tag() == Tag::Thunk || attrs.tag() == Tag::Slot) {
                vm.frames.back().ip = ip;
                attrs = forceValue(vm, attrs);
            }
            bool hasIt = (attrs.isAttrs() && attrs.payload.bindings->has(operand));
            // #558 (2026-05-12): V3_DBG_ATTRS_HAS_KEY filter — trace
            // every OP_ATTRS_HAS that matches a target SymbolId.  Used
            // to verify the hypothesis that v3's partial-Bindings
            // recovery makes `pkg.passthru.isFromBootstrapFiles or
            // false` flip vs. TW.  Set to the SymbolId or name to
            // filter (we just match on the name string via the global
            // symbol table).
            {
                static const char * s_dbgKey =
                    std::getenv("V3_DBG_ATTRS_HAS_KEY");
                if (__builtin_expect(s_dbgKey != nullptr, 0)) [[unlikely]] {
                    const auto & st = ir::globalSymbolTable();
                    SymbolId sid = static_cast<SymbolId>(operand);
                    const char * nm = (sid < st.size()) ? st[sid].c_str() : "?";
                    if (std::strcmp(nm, s_dbgKey) == 0) {
                        Bindings * b = attrs.isAttrs()
                            ? attrs.payload.bindings : nullptr;
                        // Caller frame pos for context.
                        const LambdaDescriptor * dC = nullptr;
                        if (!vm.frames.empty()) {
                            const auto & cfr = vm.frames.back();
                            if (cfr.thunk
                                && (cfr.thunk->state == ThunkState::Suspended
                                    || cfr.thunk->state == ThunkState::Blackhole))
                                dC = cfr.thunk->suspended.desc;
                            else if (cfr.closure) dC = cfr.closure->desc;
                        }
                        const PosSnapshot * psC =
                            dC ? resolvePosSnapshot(dC->posHandle) : nullptr;
                        std::fprintf(stderr,
                            "v3 OP_ATTRS_HAS '%s' result=%s bindings=%p size=%u "
                            "caller='%s' pos=%s:%u:%u\n",
                            nm, hasIt ? "TRUE" : "FALSE",
                            (void *)b, b ? b->size : 0,
                            dC && !dC->name.empty() ? dC->name.c_str() : "<?>",
                            (psC && !psC->file.empty()) ? psC->file.c_str() : "<no-pos>",
                            psC ? psC->line : 0u,
                            psC ? psC->column : 0u);
                    }
                }
            }
            push(vm, hasIt ? Value::vTrue : Value::vFalse);
            break;
        }
        case OP_ATTRS_HAS_DYN: {
            Value name = pop(vm), attrs = pop(vm);
            if (name.tag() == Tag::App || name.tag() == Tag::Thunk || name.tag() == Tag::Slot) {
                vm.frames.back().ip = ip;
                name = forceValue(vm, name);
            }
            // #458 step A.4: same per-attr peek for the dyn variant.
            // The name was forced above; if it's a string, intern and
            // try the bridge-has shortcut on the partial bindings.
            if (name.isString()
                && attrs.isThunk() && attrs.payload.thunk
                && attrs.payload.thunk->state == ThunkState::Bridge) {
                SymbolId id = ir::globalInternSymbol(name.payload.str);
                auto r = tryBridgeAttrHas(attrs.payload.thunk, id);
                if (r == BridgeAttrHasResult::Present) {
                    push(vm, Value::vTrue);
                    break;
                }
                if (r == BridgeAttrHasResult::Absent) {
                    push(vm, Value::vFalse);
                    break;
                }
            }
            if (attrs.tag() == Tag::App || attrs.tag() == Tag::Thunk || attrs.tag() == Tag::Slot) {
                vm.frames.back().ip = ip;
                attrs = forceValue(vm, attrs);
            }
            if (!name.isString() || !attrs.isAttrs()) { push(vm, Value::vFalse); break; }
            SymbolId id = ir::globalInternSymbol(name.payload.str);
            push(vm, attrs.payload.bindings->has(id)
                ? Value::vTrue : Value::vFalse);
            break;
        }
        case OP_ATTRS_UPDATE: {
            Value rhs = pop(vm), lhs = pop(vm);
            if (lhs.tag() == Tag::App || lhs.tag() == Tag::Thunk || lhs.tag() == Tag::Slot) {
                vm.frames.back().ip = ip;
                lhs = forceValue(vm, lhs);
            }
            if (rhs.tag() == Tag::App || rhs.tag() == Tag::Thunk || rhs.tag() == Tag::Slot) {
                vm.frames.back().ip = ip;
                rhs = forceValue(vm, rhs);
            }
            // #558 (2026-05-10): collapse Tag::Thunk Black operands
            // (returned by forceValue's deferral path) to chain.back()
            // for the merge.  // semantics need a Bindings.
            auto collapseDeferred = [](Value & v) {
                if (v.isThunk() && v.payload.thunk
                    && v.payload.thunk->state == ThunkState::Blackhole)
                {
                    auto & reg = partialBindingsRegistry();
                    auto it = reg.find(v.payload.thunk);
                    if (it != reg.end() && !it->second.empty()) {
                        Value collapsed;
                        collapsed.tag_payload = static_cast<uint64_t>(Tag::Attrs);
                        collapsed.payload.bindings =
                            pickLargestLayer(it->second);
                        v = collapsed;
                    }
                }
            };
            collapseDeferred(lhs);
            collapseDeferred(rhs);
            if (!lhs.isAttrs() || !rhs.isAttrs())
                throw std::runtime_error("v3 OP_ATTRS_UPDATE: not attrsets");
            Bindings * out = mergeBindings(lhs.payload.bindings, rhs.payload.bindings);
            allocStats().attrsetsAllocated++;
            Value v;
            v.tag_payload = static_cast<uint64_t>(Tag::Attrs);
            v.payload.bindings = out;
            publishToNearestBlackThunkFrame(vm, v, /*isRecInit=*/false);
            push(vm, v);
            break;
        }
        case OP_ATTRS_UPDATE_TAIL: {
            // #558 (2026-05-10): tail-return // operation.  Same as
            // OP_ATTRS_UPDATE but additionally publishes the merged
            // Bindings to all THUNK_RETURN frames via
            // publishToAllThunkFrames.  STG analog of "constructor
            // allocation reaches WHNF" — when a function's tail
            // expression is `lhs // rhs`, the merged Bindings IS the
            // function's WHNF (and transitively, every tail-call
            // ancestor's).  The most-correct partial-WHNF approximation
            // for nested fix-points (lib.fix's `let x = f x; in x`
            // with f producing a // chain in tail position).
            Value rhs = pop(vm), lhs = pop(vm);
            if (lhs.tag() == Tag::App || lhs.tag() == Tag::Thunk || lhs.tag() == Tag::Slot) {
                vm.frames.back().ip = ip;
                lhs = forceValue(vm, lhs);
            }
            if (rhs.tag() == Tag::App || rhs.tag() == Tag::Thunk || rhs.tag() == Tag::Slot) {
                vm.frames.back().ip = ip;
                rhs = forceValue(vm, rhs);
            }
            // #558: if forceValue deferred (returned Tag::Thunk Black
            // with chain), collapse to chain.back() for the merge.
            // // semantics need a Bindings; the chain peek approach
            // doesn't apply to // operands directly.  Approximation:
            // use the latest chain entry as the operand.
            auto collapseDeferred = [](Value & v) {
                if (v.isThunk() && v.payload.thunk
                    && v.payload.thunk->state == ThunkState::Blackhole)
                {
                    auto & reg = partialBindingsRegistry();
                    auto it = reg.find(v.payload.thunk);
                    if (it != reg.end() && !it->second.empty()) {
                        Value collapsed;
                        collapsed.tag_payload = static_cast<uint64_t>(Tag::Attrs);
                        collapsed.payload.bindings =
                            pickLargestLayer(it->second);
                        v = collapsed;
                    }
                }
            };
            collapseDeferred(lhs);
            collapseDeferred(rhs);
            if (!lhs.isAttrs() || !rhs.isAttrs()) {
                static const bool s_dbg =
                    std::getenv("V3_DBG_UPDATE_FAIL") != nullptr;
                if (s_dbg) {
                    std::fprintf(stderr,
                        "v3 OP_ATTRS_UPDATE_TAIL: not attrsets lhs.tag=%d rhs.tag=%d\n",
                        (int)lhs.tag(), (int)rhs.tag());
                    if (lhs.isThunk() && lhs.payload.thunk)
                        std::fprintf(stderr,
                            "  lhs thunk=%p state=%d\n",
                            (void *)lhs.payload.thunk,
                            (int)lhs.payload.thunk->state);
                    if (rhs.isThunk() && rhs.payload.thunk)
                        std::fprintf(stderr,
                            "  rhs thunk=%p state=%d\n",
                            (void *)rhs.payload.thunk,
                            (int)rhs.payload.thunk->state);
                }
                throw std::runtime_error("v3 OP_ATTRS_UPDATE_TAIL: not attrsets");
            }
            Bindings * out = mergeBindings(lhs.payload.bindings, rhs.payload.bindings);
            allocStats().attrsetsAllocated++;
            Value v;
            v.tag_payload = static_cast<uint64_t>(Tag::Attrs);
            v.payload.bindings = out;
            publishToAllThunkFrames(vm, v);
            // #558 Phase 1.5: tail-position // result.  Same
            // cell-everywhere propagation as OP_ATTRS_REC_INIT_TAIL.
            {
                static const bool s_cellEverywhere =
                    std::getenv("NIX_V3_CELL_EVERYWHERE") != nullptr;
                if (__builtin_expect(s_cellEverywhere, 0)) {
                    for (size_t fi = vm.frames.size(); fi > 0; --fi) {
                        auto & fr = vm.frames[fi - 1];
                        if (!(fr.flags & CFF_THUNK_RETURN)) continue;
                        if (!fr.thunk) continue;
                        if (!fr.thunk->shapeCell) continue;
                        *fr.thunk->shapeCell = v;
                    }
                }
            }
            push(vm, v);
            break;
        }

        // --- With ---
        case OP_WITH_PUSH: {
            Value v = pop(vm);
            // #498 diagnostic: trace OP_WITH_PUSH that pushes a 1-attr
            // attrset whose only attr is "prev" — the bisect symptom
            // that surfaces under broader thunkify.  Logs the pushing
            // frame, ip, and chase through Tag::Slot/Tag::Thunk.
            static const bool s_dbgWithPushPrev =
                std::getenv("V3_DBG_WITH_PUSH_PREV") != nullptr;
            if (__builtin_expect(s_dbgWithPushPrev, 0)) {
                Value chase = v;
                int hops = 0;
                while (hops < 4) {
                    if (chase.tag() == Tag::Slot && chase.payload.slot) {
                        chase = *chase.payload.slot;
                    } else if (chase.tag() == Tag::Thunk
                               && chase.payload.thunk
                               && chase.payload.thunk->state == ThunkState::Evaluated) {
                        chase = chase.payload.thunk->evaluated;
                    } else break;
                    ++hops;
                }
                if (chase.tag() == Tag::Attrs && chase.payload.bindings
                    && chase.payload.bindings->size == 1) {
                    SymbolId nm = chase.payload.bindings->entries[0].name;
                    const auto & st = ir::globalSymbolTable();
                    std::string s = nm < st.size() ? st[nm] : "<?>";
                    if (s == "prev") {
                        std::fprintf(stderr,
                            "v3 OP_WITH_PUSH {prev}: cu=%p ip=%u frames=%zu\n",
                            (void *)cu, ip - 1, vm.frames.size());
                        // Dump bytecode window around the push.
                        if (cu) {
                            uint32_t lo = (ip > 16) ? ip - 16 : 0;
                            uint32_t hi = ip + 8;
                            std::fprintf(stderr,
                                "  pushing-frame disasm [%u..%u):\n",
                                lo, hi);
                            disassembleWindow(stderr, *cu, lo, hi);
                            // Find the LambdaDescriptor whose codeOffset
                            // is closest BELOW the failing IP -- the
                            // function whose body contains this push.
                            uint32_t target = ip - 1;
                            uint32_t bestIdx = ~0u;
                            uint32_t bestOff = 0;
                            for (uint32_t li = 0; li < cu->lambdas.size(); ++li) {
                                uint32_t lo2 = cu->lambdas[li].codeOffset;
                                if (lo2 <= target && lo2 > bestOff) {
                                    bestOff = lo2;
                                    bestIdx = li;
                                }
                            }
                            if (bestIdx != ~0u) {
                                const auto & ld = cu->lambdas[bestIdx];
                                std::fprintf(stderr,
                                    "  containing lambdas[%u]: "
                                    "codeOffset=%u nUp=%u nLocals=%u name=%s\n",
                                    bestIdx, ld.codeOffset,
                                    ld.nUpvalues, ld.nLocals,
                                    ld.name.empty() ? "<anon>"
                                                    : ld.name.c_str());
                                // Dump full body of that lambda
                                uint32_t bodyLo = ld.codeOffset;
                                uint32_t bodyHi = ip + 8;
                                std::fprintf(stderr,
                                    "  containing lambda body [%u..%u):\n",
                                    bodyLo, bodyHi);
                                disassembleWindow(stderr, *cu, bodyLo, bodyHi);
                            }
                        }
                        for (size_t fi = vm.frames.size(); fi > 0; --fi) {
                            const auto & fr = vm.frames[fi - 1];
                            const LambdaDescriptor * d = nullptr;
                            if (fr.thunk
                                && (fr.thunk->state == ThunkState::Suspended
                                    || fr.thunk->state == ThunkState::Blackhole))
                                d = fr.thunk->suspended.desc;
                            else if (fr.closure) d = fr.closure->desc;
                            std::fprintf(stderr,
                                "  frame[%zu]: %s ip=%u flags=%u\n",
                                fi - 1,
                                d && !d->name.empty() ? d->name.c_str()
                                    : (d ? "<anon>" : "<root>"),
                                fr.ip, (unsigned)fr.flags);
                        }
                        std::fflush(stderr);
                    }
                }
            }
            vm.withStack.push_back(v);
            break;
        }
        case OP_WITH_POP:  vm.withStack.pop_back(); break;
        case OP_REC_SLOT_PUBLISH: {
            // #458 step 1/6 — heap-stable rec-attrset slot publish.
            //
            // Peek the rec-attrset Tag::Attrs at top of stack (built
            // by the OP_ATTRS_REC_INIT immediately preceding), allocate
            // a fresh GC-managed Value*, copy the Tag::Attrs INTO that
            // slot, and push a Tag::Slot pointing at it ON TOP.  The
            // slot is heap-stable for the lifetime of any closure
            // capturing the Tag::Slot via its freeVars vector (Boehm
            // GC handles reachability automatically).
            //
            // Why peek-and-copy instead of move: the rec-attrset Value
            // payload is a Bindings* — copying the Value is cheap and
            // the Bindings is already heap-allocated, so OP_ATTRS_REC_SET
            // mutations to its entries[] are visible through both the
            // original Tag::Attrs (still on the stack, used by the rest
            // of the LetRec emit) and the slot's Tag::Attrs (captured
            // by inner closures).
            if (vm.valueStack.empty()) {
                throw std::runtime_error(
                    "v3 OP_REC_SLOT_PUBLISH: empty operand stack");
            }
            const Value & top = vm.valueStack.back();
            if (!top.isAttrs()) {
                throw std::runtime_error(
                    "v3 OP_REC_SLOT_PUBLISH: top of stack is not Tag::Attrs");
            }
            Value * heapSlot = Alloc::allocValue();
            *heapSlot = top;
            Value slotRef;
            slotRef.mkSlot(heapSlot);
            push(vm, slotRef);
            break;
        }
        case OP_THUNK_SET_LOCAL_THROUGH_CELL: {
            // STG-14b (#516/#517): pop a Tag::Thunk, allocate a heap
            // cell holding it, attach the cell as the thunk's
            // OP_RETURN-update target, and write a Tag::Slot{cell}
            // into the local at [slot:24].
            //
            // Used by emit.cc:594-601 for hidden-from-expr thunks
            // (the `inherit (X // Y) ...` lowering's inheritFromExpr
            // thunks).  Before this opcode, hidden thunks were stored
            // as Tag::Thunk in their slots; per-attr thunks captured
            // them via emitVarRef, holding a stale Black thunk pointer
            // when the hidden was forced mid-construction (the
            // STG_KEEP_HOOKS hang on `(import <nixpkgs> {}).lib`).
            //
            // After this opcode, the slot holds Tag::Slot{cell}; the
            // cell initially contains the Tag::Thunk and -- on the
            // hidden thunk's OP_RETURN -- gets `*cell = retVal`
            // applied (vm.cc:3003 cell-update path).  Captures
            // observing the slot deref through the cell to the
            // Evaluated value.
            if (vm.valueStack.empty()) {
                throw std::runtime_error(
                    "v3 OP_THUNK_SET_LOCAL_THROUGH_CELL: empty operand stack");
            }
            Value top = vm.valueStack.back();
            vm.valueStack.pop_back();
            if (top.tag() != Tag::Thunk || !top.payload.thunk) {
                throw std::runtime_error(
                    "v3 OP_THUNK_SET_LOCAL_THROUGH_CELL: top of stack is not Tag::Thunk");
            }
            // Heap-stable cell: holds the thunk Value initially; the
            // thunk's OP_RETURN cell-update will overwrite it with
            // the evaluated value.
            Value * cell = Alloc::allocValue();
            *cell = top;
            // Attach cell to the thunk so OP_RETURN's CFF_THUNK_RETURN
            // handler at vm.cc:3003 fires `*cell = retVal`.  Only
            // attach if the thunk is fresh (Suspended with no cell
            // yet); otherwise we'd clobber an existing cell binding
            // (e.g., from OP_ATTRS_REC_SET).  Fresh OP_MAKE_THUNK
            // produces Suspended with cell == nullptr by construction
            // (alloc.hh:317-348), so this is the expected branch.
            if (top.payload.thunk->state == ThunkState::Suspended
                && top.payload.thunk->cell == nullptr) {
                top.payload.thunk->cell = cell;
            }
            // Slot pointing at cell -- captures see the slot, deref
            // resolves through cell to the (eventually Evaluated) value.
            Value slotRef;
            slotRef.mkSlot(cell);
            uint16_t slot = static_cast<uint16_t>(operand);
            if (stackBase + slot >= vm.valueStack.size()) {
                vm.valueStack.resize(stackBase + slot + 1);
            }
            vm.valueStack[stackBase + slot] = slotRef;
            break;
        }
        case OP_REC_BINDING_SLOT_REF: {
            // Pop a Tag::Attrs (forced earlier), look up the entry by
            // SymbolId in operand, push a Tag::Slot Value pointing at
            // `&entries[i].value`.  Heap-stable because Bindings live
            // on the v3 heap (Alloc::allocBindings), not on the
            // value-stack.
            Value attrs = pop(vm);
            // #437: count and tag-distribute OP_REC_BINDING_SLOT_REF fires.
            {
                static const bool s_dbg_p5 =
                    std::getenv("V3_DBG_P5") != nullptr;
                if (__builtin_expect(s_dbg_p5, 0)) [[unlikely]] {
                    static thread_local uint64_t total = 0;
                    static thread_local uint64_t byTag[16] = {0};
                    static thread_local uint64_t bridgeFires = 0;
                    static thread_local uint64_t logged = 0;
                    ++total;
                    Tag at = attrs.tag();
                    if ((unsigned)at < 16) byTag[(unsigned)at]++;
                    bool isBridge = (at == Tag::Thunk
                        && attrs.payload.thunk
                        && attrs.payload.thunk->state == ThunkState::Bridge);
                    if (isBridge) ++bridgeFires;
                    // Print first 5 cases & every power of 10 thereafter.
                    if (logged < 5
                        || (total > 10 && (total & (total - 1)) == 0)) {
                        const auto & tbl = ir::globalSymbolTable();
                        SymbolId sym = static_cast<SymbolId>(operand);
                        std::string nm = (sym < tbl.size()) ? tbl[sym] : "?";
                        std::fprintf(stderr,
                            "v3 P5#%llu: tag=%d sym='%s' frames=%zu bridges=%llu byTag=[",
                            (unsigned long long)total, (int)at, nm.c_str(),
                            vm.frames.size(),
                            (unsigned long long)bridgeFires);
                        for (int i = 0; i < 16; ++i)
                            if (byTag[i]) std::fprintf(stderr, "%d:%llu,", i,
                                (unsigned long long)byTag[i]);
                        std::fprintf(stderr, "]\n");
                        ++logged;
                    }
                }
            }
            if (attrs.tag() == Tag::App || attrs.tag() == Tag::Thunk || attrs.tag() == Tag::Slot) {
                vm.frames.back().ip = ip;
                // #457/#458: tolerant force.  If the source thunk is
                // currently being forced (Black) elsewhere on the
                // stack, forceValue throws BlackHole.  But the
                // partial-Bindings side-table may have an entry for
                // it (populated by OP_ATTRS_REC_INIT when the thunk's
                // body ran).  Use that to recover the partial Bindings
                // and proceed.  Allows mid-construction rec-attrset
                // self-reference to work without the structural
                // closure-capture redesign.
                // STG-2 (#547): side-table recovery is disabled by
                // default.  STG semantics: a Black thunk access is a
                // cycle and must throw, not be papered over with
                // whatever the publish-walk happened to register.
                // NIX_V3_NO_STG=1 restores the legacy recovery path.
                static const bool s_stgMode_recref =
                    std::getenv("NIX_V3_NO_STG") == nullptr;
                Bindings * recoveredBindings = nullptr;
                if (!s_stgMode_recref
                    && attrs.tag() == Tag::Thunk && attrs.payload.thunk
                    && attrs.payload.thunk->state == ThunkState::Blackhole) {
                    auto & reg = partialBindingsRegistry();
                    auto it = reg.find(attrs.payload.thunk);
                    if (it != reg.end() && !it->second.empty()) {
                        // Use the latest (back) entry as the
                        // recovered bindings.  The chain's other
                        // entries represent older layer
                        // contributions; for whole-attrset recovery,
                        // the latest-layer's view is closest to the
                        // thunk's eventual value.
                        recoveredBindings = it->second.back();
                    }
                }
                if (recoveredBindings) {
                    Value recovered;
                    recovered.tag_payload = static_cast<uint64_t>(Tag::Attrs);
                    recovered.payload.bindings = recoveredBindings;
                    attrs = recovered;
                } else {
                    try {
                        attrs = forceValue(vm, attrs);
                    } catch (const BlackholeError &) {
                        // Last-ditch: try the registry again (the
                        // thunk's force might have transitioned but
                        // the top-of-stack v3 thunk it's wrapping is
                        // black).  STG-2: skip under NIX_V3_STG=1.
                        if (!s_stgMode_recref
                            && attrs.tag() == Tag::Thunk
                            && attrs.payload.thunk) {
                            auto & reg = partialBindingsRegistry();
                            auto it = reg.find(attrs.payload.thunk);
                            if (it != reg.end() && !it->second.empty()) {
                                Value recovered;
                                recovered.tag_payload =
                                    static_cast<uint64_t>(Tag::Attrs);
                                recovered.payload.bindings = it->second.back();
                                attrs = recovered;
                            } else {
                                throw;
                            }
                        } else {
                            throw;
                        }
                    }
                }
            }
            if (!attrs.isAttrs() || !attrs.payload.bindings) {
                throw std::runtime_error(
                    "v3 OP_REC_BINDING_SLOT_REF: source is not a forced attrset");
            }
            SymbolId sym = static_cast<SymbolId>(operand);
            // Binary search the sorted entries for `sym`.
            Bindings * b = attrs.payload.bindings;
            uint32_t lo = 0, hi = b->size;
            Value * found = nullptr;
            while (lo < hi) {
                uint32_t mid = (lo + hi) >> 1;
                SymbolId midName = b->entries[mid].name;
                if (midName == sym) { found = &b->entries[mid].value; break; }
                if (midName < sym) lo = mid + 1;
                else               hi = mid;
            }
            if (!found) {
                const auto & tbl = ir::globalSymbolTable();
                std::string nm = (sym < tbl.size()) ? tbl[sym] : "?";
                throw std::runtime_error(
                    "v3 OP_REC_BINDING_SLOT_REF: name '" + nm
                    + "' not found in source attrset");
            }
            // #558 (2026-05-10) diagnostic: V3_DBG_SLOT_REF=1 shows
            // what slot was selected and its current value's tag.
            {
                static const bool s_dbgSlotRef =
                    std::getenv("V3_DBG_SLOT_REF") != nullptr;
                if (__builtin_expect(s_dbgSlotRef, 0)) {
                    const auto & tbl = ir::globalSymbolTable();
                    std::string nm = (sym < tbl.size()) ? tbl[sym] : "?";
                    if (found->isThunk() && found->payload.thunk) {
                        Thunk * t = found->payload.thunk;
                        const auto * d =
                            (t->state == ThunkState::Suspended
                             || t->state == ThunkState::Blackhole)
                            ? t->suspended.desc : nullptr;
                        const PosSnapshot * ps =
                            d ? resolvePosSnapshot(d->posHandle) : nullptr;
                        std::fprintf(stderr,
                            "v3 SLOT_REF: bindings=%p size=%u sym='%s' "
                            "slot-tag=Thunk thunk=%p state=%d desc-name='%s' pos=%s:%u:%u\n",
                            (void *)b, b->size, nm.c_str(),
                            (void *)t, (int)t->state,
                            d && !d->name.empty() ? d->name.c_str() : "<?>",
                            (ps && !ps->file.empty()) ? ps->file.c_str() : "<no-pos>",
                            ps ? ps->line : 0u, ps ? ps->column : 0u);
                    } else {
                        std::fprintf(stderr,
                            "v3 SLOT_REF: bindings=%p size=%u sym='%s' slot-tag=%d\n",
                            (void *)b, b->size, nm.c_str(), (int)found->tag());
                    }
                }
            }
            // #437 diagnostic: track repeated slot derefs on the same
            // (bindings, sym) pair within a single eval.  Under
            // NIX_V3_INLINE_REC_SLOT, the inline path lacks the Thunk
            // identity that pins recursion via Blackhole; if cardano-
            // node hits this opcode N>>1 times for the same key, the
            // hypothesis from the agent analysis is confirmed.  Gated
            // on V3_DBG_INLINE_REC=1; thread-local state is fine since
            // the VM is single-threaded.  Reports when re-entry count
            // for a key crosses 4.
            {
                static const bool s_dbg_inline_rec =
                    std::getenv("V3_DBG_INLINE_REC") != nullptr;
                if (__builtin_expect(s_dbg_inline_rec, 0)) [[unlikely]] {
                    static thread_local std::unordered_map<
                        uint64_t, uint32_t> reentries;
                    uint64_t key = (reinterpret_cast<uint64_t>(b) << 24)
                        ^ static_cast<uint64_t>(sym);
                    auto & cnt = reentries[key];
                    ++cnt;
                    if (cnt > 4 && (cnt & (cnt - 1)) == 0) {
                        const auto & tbl = ir::globalSymbolTable();
                        std::string nm = (sym < tbl.size()) ? tbl[sym] : "?";
                        std::fprintf(stderr,
                            "v3 inline-rec re-entry #%u: bindings=%p sym='%s' frames=%zu ip=%u\n",
                            cnt, (void *)b, nm.c_str(),
                            vm.frames.size(), ip - 1);
                    }
                }
            }
            Value v;
            v.mkSlot(found);
            push(vm, v);
            break;
        }
        case OP_WITH_LOOKUP: {
            // Sync local ip into the top frame BEFORE withLookup may
            // throw — otherwise the cycle dump's frame[top].ip is
            // stale (still showing the value last written at
            // OP_FORCE / OP_CALL push time, which is often the body
            // start) and the disasm window misses the failing
            // OP_WITH_LOOKUP itself.  Cheap on the hot path: one
            // store per OP_WITH_LOOKUP, only adds a memory write
            // ahead of an opcode that already does an STL hashmap
            // lookup.
            vm.frames.back().ip = ip;
            push(vm, withLookup(vm, static_cast<SymbolId>(operand)));
            break;
        }

        // --- Strings / pos / assert ---
        case OP_STR_CONCAT: {
            uint32_t n = operand >> 1;
            bool forceStr = (operand & 1u) != 0;
            // Ultra-fast path: 2 ints with no forceStr — covers every
            // arithmetic `a + b` over ints, which is the dominant case
            // on compute-bound benchmarks like fib.  Skip the small[]
            // setup, the loop, and the per-part type checks.  Match
            // tree-walker by raising on overflow.
            if (!forceStr && n == 2) {
                Value & top1 = vm.valueStack.back();
                Value & top0 = vm.valueStack[vm.valueStack.size() - 2];
                if (top0.isInt() && top1.isInt()) {
                    int64_t sum;
                    if (__builtin_add_overflow(top0.payload.i, top1.payload.i, &sum))
                        throw std::runtime_error("v3 OP_STR_CONCAT: integer overflow");
                    vm.valueStack.pop_back();
                    vm.valueStack.back().mkInt(sum);
                    break;
                }
            }
            // Hot path on every Nix-level `a + b` (which the parser
            // lowers to ConcatStrings).  Avoid allocating a heap
            // vector for the common 2-part case — most ConcatStrings
            // expressions are exactly two operands.
            constexpr uint32_t kSmall = 8;
            Value small[kSmall];
            std::vector<Value> overflow;
            Value * parts = small;
            if (n > kSmall) {
                overflow.resize(n);
                parts = overflow.data();
            }
            for (uint32_t i = n; i > 0; --i) parts[i - 1] = pop(vm);

            // Force lazy parts (Tag::App from mapAttrs/zipAttrsWith,
            // Tag::Thunk from lazy attr values).  Without this, a
            // string interpolation like `"${(map f xs)[0]}"` blows up
            // because map's entries are now Tag::App after the WC-35
            // fix.  Cheap on already-WHNF values.
            for (uint32_t i = 0; i < n; ++i) {
                Tag t = parts[i].tag();
                if (t == Tag::App || t == Tag::Thunk) {
                    vm.frames.back().ip = ip;
                    parts[i] = forceValue(vm, parts[i]);
                }
            }
            // V3_DBG_STRCONCAT: when a Closure leaks into STR_CONCAT
            // (which happens when v3's eval-order divergence forces a
            // function value where tree-walker keeps it lazy), dump
            // the call stack to localise the source.
            {
                static const bool s_dbg = std::getenv("V3_DBG_STRCONCAT") != nullptr;
                if (s_dbg) {
                    bool hasUncoercible = false;
                    for (uint32_t i = 0; i < n; ++i) {
                        Tag t = parts[i].tag();
                        if (t == Tag::Closure || t == Tag::PrimOp || t == Tag::PrimOpApp || t == Tag::List)
                            { hasUncoercible = true; break; }
                    }
                    if (hasUncoercible) {
                        std::fprintf(stderr,
                            "v3 OP_STR_CONCAT pre-trace tags=[");
                        for (uint32_t i = 0; i < n; ++i)
                            std::fprintf(stderr, "%s%u", i ? "," : "", (unsigned)parts[i].tag());
                        std::fprintf(stderr, "] forceStr=%d frames=%zu callerIp=%u\n",
                            forceStr ? 1 : 0, vm.frames.size(), ip - 1);
                        // Dump current frame's upvalues / closure context.
                        if (!vm.frames.empty()) {
                            const auto & fr = vm.frames.back();
                            const Closure * cl = fr.closure;
                            const Thunk * th = fr.thunk;
                            uint16_t nUp = 0;
                            const Value * uvs = nullptr;
                            if (cl) { nUp = cl->nUpvalues; uvs = cl->upvalues; }
                            else if (th) { nUp = th->nUpvalues; uvs = th->tail; }
                            std::fprintf(stderr,
                                "  current-frame nUpvalues=%u\n", nUp);
                            for (uint16_t i = 0; i < nUp && i < 8; ++i) {
                                std::fprintf(stderr,
                                    "    upvalue[%u] tag=%u",
                                    i, (unsigned)uvs[i].tag());
                                if (uvs[i].tag() == Tag::Closure
                                    && uvs[i].payload.closure
                                    && uvs[i].payload.closure->desc) {
                                    std::fprintf(stderr, " closure=%s nUp=%u",
                                        !uvs[i].payload.closure->desc->name.empty()
                                            ? uvs[i].payload.closure->desc->name.c_str()
                                            : "<anon>",
                                        uvs[i].payload.closure->nUpvalues);
                                } else if (uvs[i].tag() == Tag::Attrs
                                           && uvs[i].payload.bindings) {
                                    auto * b2 = uvs[i].payload.bindings;
                                    std::fprintf(stderr, " attrs size=%u {",
                                        (unsigned)b2->size);
                                    const auto & st2 = ir::globalSymbolTable();
                                    for (uint32_t k = 0; k < b2->size && k < 30; ++k) {
                                        SymbolId nm = b2->entries[k].name;
                                        std::fprintf(stderr, "%s%s",
                                            k ? "," : "",
                                            nm < st2.size() ? st2[nm].c_str() : "?");
                                    }
                                    std::fprintf(stderr, "}");
                                    // For each attrs upvalue, also dump
                                    // tag of `preHook` slot (the bug
                                    // chases this attribute specifically).
                                    static const SymbolId preHookSym2 =
                                        ir::globalInternSymbol("preHook");
                                    const Value * ph = b2->lookup(preHookSym2);
                                    if (ph) {
                                        Tag pht = ph->tag();
                                        std::fprintf(stderr,
                                            " preHook=tag%u", (unsigned)pht);
                                        if (pht == Tag::Closure
                                            && ph->payload.closure
                                            && ph->payload.closure->desc) {
                                            auto * d3 = ph->payload.closure->desc;
                                            std::fprintf(stderr, "(%s [%u..) nUp=%u)",
                                                !d3->name.empty() ? d3->name.c_str() : "<anon>",
                                                d3->codeOffset,
                                                ph->payload.closure->nUpvalues);
                                        } else if (pht == Tag::Thunk && ph->payload.thunk) {
                                            Thunk * pt = ph->payload.thunk;
                                            std::fprintf(stderr, "(state=%d nUp=%u",
                                                (int)pt->state, (unsigned)pt->nUpvalues);
                                            if (pt->state == ThunkState::Suspended) {
                                                auto * d3 = pt->suspended.desc;
                                                if (d3)
                                                    std::fprintf(stderr, " %s [%u..)",
                                                        !d3->name.empty() ? d3->name.c_str() : "<anon>",
                                                        d3->codeOffset);
                                            } else if (pt->state == ThunkState::Evaluated) {
                                                std::fprintf(stderr, " EVAL=tag%u",
                                                    (unsigned)pt->evaluated.tag());
                                                // Recurse one level deep
                                                if (pt->evaluated.tag() == Tag::Thunk
                                                    && pt->evaluated.payload.thunk) {
                                                    Thunk * pt2 = pt->evaluated.payload.thunk;
                                                    std::fprintf(stderr, "(state=%d nUp=%u",
                                                        (int)pt2->state, (unsigned)pt2->nUpvalues);
                                                    if (pt2->state == ThunkState::Suspended) {
                                                        auto * d4 = pt2->suspended.desc;
                                                        if (d4)
                                                            std::fprintf(stderr, " %s [%u..)",
                                                                !d4->name.empty() ? d4->name.c_str() : "<anon>",
                                                                d4->codeOffset);
                                                    } else if (pt2->state == ThunkState::Evaluated) {
                                                        std::fprintf(stderr, " EVAL=tag%u",
                                                            (unsigned)pt2->evaluated.tag());
                                                        if (pt2->evaluated.tag() == Tag::Closure
                                                            && pt2->evaluated.payload.closure
                                                            && pt2->evaluated.payload.closure->desc) {
                                                            auto * d5 = pt2->evaluated.payload.closure->desc;
                                                            std::fprintf(stderr, "(closure=%s [%u..) nUp=%u)",
                                                                !d5->name.empty() ? d5->name.c_str() : "<anon>",
                                                                d5->codeOffset,
                                                                pt2->evaluated.payload.closure->nUpvalues);
                                                        } else if (pt2->evaluated.tag() == Tag::Thunk
                                                            && pt2->evaluated.payload.thunk) {
                                                            Thunk * pt3 = pt2->evaluated.payload.thunk;
                                                            std::fprintf(stderr, "(state=%d nUp=%u",
                                                                (int)pt3->state, (unsigned)pt3->nUpvalues);
                                                            if (pt3->state == ThunkState::Suspended) {
                                                                auto * d6 = pt3->suspended.desc;
                                                                if (d6)
                                                                    std::fprintf(stderr, " %s [%u..)",
                                                                        !d6->name.empty() ? d6->name.c_str() : "<anon>",
                                                                        d6->codeOffset);
                                                            }
                                                            std::fprintf(stderr, ")");
                                                        }
                                                    }
                                                    std::fprintf(stderr, ")");
                                                }
                                            }
                                            std::fprintf(stderr, ")");
                                        }
                                    }
                                }
                                std::fprintf(stderr, "\n");
                            }
                        }
                        size_t lim = vm.frames.size();
                        for (size_t i = lim; i > 0 && i + 8 > lim; --i) {
                            const auto & fr = vm.frames[i - 1];
                            const LambdaDescriptor * d = nullptr;
                            if (fr.thunk) d = fr.thunk->suspended.desc;
                            else if (fr.closure) d = fr.closure->desc;
                            std::fprintf(stderr,
                                "  frame[%zu]: %s code=[%u..) ip=%u flags=%u\n",
                                i - 1,
                                d && !d->name.empty() ? d->name.c_str()
                                    : (d ? "<anon>" : "<closure-body>"),
                                d ? d->codeOffset : 0, fr.ip,
                                (unsigned)fr.flags);
                        }
                        // Disasm from the frame's prologue to the failing
                        // OP_STR_CONCAT — full body lets us trace slot
                        // assignments back to their source.
                        if (cu && !vm.frames.empty()) {
                            const auto & fr = vm.frames.back();
                            const LambdaDescriptor * d = nullptr;
                            if (fr.thunk) d = fr.thunk->suspended.desc;
                            else if (fr.closure) d = fr.closure->desc;
                            uint32_t lo = d ? d->codeOffset : (ip > 32 ? ip - 32 : 0);
                            uint32_t hi = ip + 4;
                            std::fprintf(stderr,
                                "  current frame disasm [%u..%u) (prologue→ip):\n", lo, hi);
                            disassembleWindow(stderr, *cu, lo, hi);
                            // Also disasm functions referenced by MAKE_THUNK
                            // / MAKE_CLOSURE in the prologue — these are
                            // the inner thunks that produce slot values.
                            std::fprintf(stderr, "  --- referenced functions ---\n");
                            for (uint32_t cur = lo; cur < hi - 1; ) {
                                Op op = decodeOp(cu->code[cur]);
                                uint32_t operand = decodeOperand(cu->code[cur]);
                                if (op == OP_MAKE_THUNK || op == OP_MAKE_CLOSURE) {
                                    if (operand < cu->lambdas.size()) {
                                        const LambdaDescriptor & d2 = cu->lambdas[operand];
                                        uint32_t flo = d2.codeOffset;
                                        uint32_t fhi = flo + 24;
                                        std::fprintf(stderr,
                                            "  fn[%u] (%s, nUp=%u) [%u..%u):\n",
                                            operand,
                                            !d2.name.empty() ? d2.name.c_str() : "<anon>",
                                            d2.nUpvalues, flo, fhi);
                                        disassembleWindow(stderr, *cu, flo, fhi);
                                    }
                                    // #530: op + nUp + nWiths data.
                                    cur += 3;
                                } else {
                                    cur++;
                                }
                            }
                        }
                    }
                }
            }

            // nix `+` semantics: if forceString=false and the first operand
            // is numeric (Int/Float), perform arithmetic addition; otherwise
            // do string concatenation.  forceString=true (e.g. "${foo}")
            // always coerces to string.
            if (!forceStr && n > 0 && (parts[0].isInt() || parts[0].isFloat())) {
                bool allInt = true;
                for (uint32_t i = 0; i < n; ++i) if (!parts[i].isInt()) { allInt = false; break; }
                Value r;
                if (allInt) {
                    int64_t sum = 0;
                    for (uint32_t i = 0; i < n; ++i) {
                        if (__builtin_add_overflow(sum, parts[i].payload.i, &sum))
                            throw std::runtime_error("v3 OP_STR_CONCAT: integer overflow");
                    }
                    r.mkInt(sum);
                } else {
                    double sum = 0.0;
                    for (uint32_t i = 0; i < n; ++i) {
                        const Value & p = parts[i];
                        if (p.isInt())   sum += static_cast<double>(p.payload.i);
                        else if (p.isFloat()) sum += p.payload.f;
                        else throw std::runtime_error("v3 OP_STR_CONCAT: mixed numeric and non-numeric");
                    }
                    r.mkFloat(sum);
                }
                push(vm, r);
                break;
            }

            std::string out;
            // Accumulate string contexts from all parts.  Path parts
            // produce a fresh Opaque entry (the store path of the
            // copied content); String parts inherit any context their
            // payload buffer was tagged with.  Attrset parts coerce
            // via __toString/outPath like before — we treat the
            // resulting string identically.
            std::vector<std::string> ctxAccum;
            auto addCtx = [&](const std::vector<std::string> * v) {
                if (!v) return;
                for (auto & s : *v) ctxAccum.push_back(s);
            };
            for (uint32_t i = 0; i < n; ++i) {
                const Value & p = parts[i];
                // Attrset coercion: __toString self  or  outPath.
                // Matches tree-walker's coerceToString behaviour for
                // attrsets (used to interpolate derivation values).
                if (p.isAttrs() && p.payload.bindings) {
                    static const SymbolId tsId  = ir::globalInternSymbol("__toString");
                    static const SymbolId outId = ir::globalInternSymbol("outPath");
                    if (auto * fn = p.payload.bindings->lookup(tsId)) {
                        Value forced = forceValue(vm, *fn);
                        Value s = callClosure(vm, forced, p);
                        s = forceValue(vm, s);
                        if (s.isString()) {
                            out.append(s.payload.str);
                            addCtx(lookupStringContextEntries(s.payload.str));
                            continue;
                        }
                    }
                    if (auto * op = p.payload.bindings->lookup(outId)) {
                        Value forced = forceValue(vm, *op);
                        if (forced.isString()) {
                            out.append(forced.payload.str);
                            addCtx(lookupStringContextEntries(forced.payload.str));
                            continue;
                        }
                        if (forced.isPath())   { out.append(forced.payload.path); continue; }
                    }
                }
                if (p.isString())
                    addCtx(lookupStringContextEntries(p.payload.str));
                if (p.isPath() && forceStr) {
                    // coerceToString will copy this path to the store and
                    // produce its `/nix/store/...` representation; tag the
                    // resulting string with that store path as an Opaque
                    // context entry.  Encoded form is the StorePath's
                    // basename (`<hash>-<name>`) — what
                    // NixStringContextElem::to_string()/parse roundtrip.
                    // Exceptions propagate: tree-walker raises on missing
                    // paths during interpolation, so v3 must too.
                    if (auto * ns = getNixEvalState()) {
                        nix::NixStringContext tmp;
                        nix::SourcePath sp(ns->rootFS,
                                            nix::CanonPath(p.payload.path ? p.payload.path : ""));
                        auto storePath = ns->copyPathToStore(tmp, sp);
                        ctxAccum.push_back(std::string(storePath.to_string()));
                    }
                }
                out.append(coerceToString(p, forceStr));
            }
            // Path + string semantics: when the first operand is a Path
            // and we're in plain `+` mode (not interpolation), the result
            // is a Path (lexically normalized), not a String.  Required
            // by `dirOf p + ""` and by string-test concat patterns like
            // `/foo/bar + "/../xyzzy/."` which must collapse to /foo/xyzzy.
            bool resultIsPath = !forceStr && n > 0 && parts[0].isPath();
            if (resultIsPath) {
                std::string normalized =
                    std::filesystem::path(out).lexically_normal().string();
                // lexically_normal leaves a trailing "/." for inputs
                // like "/a/b/." — strip it so output matches Nix.
                while (normalized.size() > 1 && normalized.back() == '/')
                    normalized.pop_back();
                out = std::move(normalized);
            }
            // CRIT-4: arena allocation for long-lived string/path
            // payload (was std::malloc + leak).
            char * buf = Alloc::allocChars(out.size() + 1);
            std::memcpy(buf, out.data(), out.size());
            buf[out.size()] = '\0';
            Value v;
            if (resultIsPath) {
                v.tag_payload = static_cast<uint64_t>(Tag::Path);
                v.payload.path = buf;
            } else {
                v.mkString(buf);
                // De-duplicate context entries (a sorted-unique pass) and
                // record on the new buffer.  Empty input → no entry left.
                if (!ctxAccum.empty()) {
                    std::sort(ctxAccum.begin(), ctxAccum.end());
                    ctxAccum.erase(std::unique(ctxAccum.begin(), ctxAccum.end()),
                                   ctxAccum.end());
                    setStringContextEntries(buf, std::move(ctxAccum));
                }
            }
            push(vm, v);
            break;
        }
        case OP_ASSERT: {
            Value c = pop(vm);
            if (!isTrueValue(c)) throw AssertionError("v3 OP_ASSERT: assertion failed");
            break;
        }
        // OP_POS: bytecode value reserved; never emitted (lowerExpr
        // skips ExprPos in v3).  Removed dispatch; default-case abort
        // catches stale.

        case OP_LIT_PRIMOP: {
            const PrimOp * po = cu->primops[operand];
            Value v;
            v.tag_payload = static_cast<uint64_t>(Tag::PrimOp);
            v.payload.primop = po;
            push(vm, v);
            break;
        }

        case OP_LIT_BUILTINS: {
            push(vm, getBuiltinsValue());
            break;
        }

        case OP_CALL_PRIMOP: {
            uint32_t nArgs = operand;
            uint32_t poIdx = cu->code[ip++];
            const PrimOp * po = cu->primops[poIdx];
            // Profiling counter (gated on NIX_VM_STATS at process exit).
            // The bump is unconditional — the primop dispatch already
            // does substantially more work, so the cost is invisible.
            bumpPrimOpCallCount(po);
            Value args[8];
            if (nArgs > 8) throw std::runtime_error("v3 OP_CALL_PRIMOP: arity > 8 not supported");
            for (uint32_t i = nArgs; i > 0; --i) args[i - 1] = pop(vm);
            // WC-38 fix: force non-lazy strict args at runtime via
            // the C-recursive forceValue helper.  This replaced the
            // compile-time force at lower.cc:787 (`forceVal(lowerExpr
            // (*it))`), which emitted inline OP_GET_LOCAL_FORCE / OP_
            // GET_UPVALUE_FORCE / OP_FORCE in the calling bytecode
            // stream.  The bytecode-level force fired thunk frames
            // inside the caller's CFF_FORCE_RETRY chain — driving
            // sub-thunk evaluation deeper than tree-walker's
            // recursive C-stack — and during nixpkgs's `lib.fix x`
            // body it fired the inner `callPackages ../llvm { }`
            // thunk while pkgs (lib.fix x slot) was still Black.
            //
            // The C-recursive forceValue does NOT set CFF_FORCE_
            // RETRY, so each Suspended thunk fully resolves (and
            // becomes Evaluated) BEFORE the next runs — matching
            // tree-walker's call-stack semantics exactly.
            //
            // `po->lazyArgs` bit i set ⇒ arg i is passed lazily;
            // primops with lazy args (tryEval, foldl', seq, deepSeq,
            // addErrorContext) force inside their bodies inside any
            // try/catch they need.  Mirrors OP_CALL's primop branch
            // at vm.cc:1155-1158 (which handles PrimOpApp partial-
            // application chains).
            for (uint32_t i = 0; i < nArgs; ++i) {
                if (po->lazyArgs & (1u << i)) continue;
                args[i] = forceValue(vm, args[i]);
            }
            // Save current frame state in case the primop calls back
            // into the VM via callClosure().
            vm.frames.back().ip = ip;
            // Wire the EvalState to this VM so callback primops can
            // re-enter the dispatcher; also propagate the (optional)
            // nix EvalState so primops like `import` can parse files.
            EvalState state;
            state.vm = &vm;
            state.nixEvalState = getNixEvalState();
            Value out;
            po->fn(state, args, out);
            push(vm, out);
            break;
        }

        // ---- #428 fast-path primop opcodes ----------------------------
        // Each opcode is a bug-compatible inline of the corresponding C
        // primop (primops.cc): same forcing, same throws, same return
        // shape.  Emitted by the lowerer in lieu of OP_CALL_PRIMOP when
        // the called primop is one of the targeted ones; the primop
        // itself stays registered for first-class uses.

        // -Werror=switch-enum on the outer dispatch makes a single
        // multi-tag inner switch awkward (requires every enum value
        // listed), so each predicate gets its own self-contained
        // case body using a small lambda to share the force-then-test
        // pattern.
        //
        // #493: forceValue may break early on Tag::Thunk in Bridge
        // state when the underlying TW Value is a Function (the #456
        // chase-break: forceBridgeThunk would re-wrap as another
        // Bridge ad infinitum).  Result: v stays Tag::Thunk even
        // though the WHNF type is whatever TW says.  The naive
        // predicate `v.isClosure() || ...` answers false for these
        // bridged-TW-function values, mis-routing nixpkgs loadModule's
        // `if isFunction m then ... else import m` to import.  Peek
        // through Bridge thunks for the TW ValueType.  Cheap; only
        // fires on the Bridge case.
        #define V3_BRIDGE_PEEK_OR(v, twTypePred, fallback) \
            (((v).tag() == Tag::Thunk && (v).payload.thunk \
                && (v).payload.thunk->state == ThunkState::Bridge \
                && (v).payload.thunk->bridgeSrc) \
                ? ([&]() -> bool { \
                    try { \
                        auto * src = static_cast<nix::Value *>( \
                            (v).payload.thunk->bridgeSrc); \
                        nix::ValueType tt = src->type(); \
                        return (twTypePred); \
                    } catch (...) { return (fallback); } \
                  }()) \
                : (fallback))
        #define V3_IS_OP(op_name, predExpr) \
            case op_name: { \
                Value v = pop(vm); \
                if (v.isThunk() || v.tag() == Tag::App \
                    || v.tag() == Tag::Slot) { \
                    vm.frames.back().ip = ip; \
                    v = forceValue(vm, v); \
                } \
                push(vm, (predExpr) ? Value::vTrue : Value::vFalse); \
                break; \
            }
        V3_IS_OP(OP_IS_NULL,
            V3_BRIDGE_PEEK_OR(v, tt == nix::nNull,    v.isNull()))
        V3_IS_OP(OP_IS_BOOL,
            V3_BRIDGE_PEEK_OR(v, tt == nix::nBool,    v.isBool()))
        V3_IS_OP(OP_IS_INT,
            V3_BRIDGE_PEEK_OR(v, tt == nix::nInt,     v.isInt()))
        V3_IS_OP(OP_IS_FLOAT,
            V3_BRIDGE_PEEK_OR(v, tt == nix::nFloat,   v.isFloat()))
        V3_IS_OP(OP_IS_STRING,
            V3_BRIDGE_PEEK_OR(v, tt == nix::nString,  v.isString()))
        V3_IS_OP(OP_IS_PATH,
            V3_BRIDGE_PEEK_OR(v, tt == nix::nPath,    v.isPath()))
        V3_IS_OP(OP_IS_LIST,
            V3_BRIDGE_PEEK_OR(v, tt == nix::nList,    v.isList()))
        V3_IS_OP(OP_IS_ATTRS,
            V3_BRIDGE_PEEK_OR(v, tt == nix::nAttrs,   v.isAttrs()))
        V3_IS_OP(OP_IS_FUNCTION,
            V3_BRIDGE_PEEK_OR(v, tt == nix::nFunction,
                v.isClosure() || v.isPrimOp() || v.tag() == Tag::PrimOpApp))
        #undef V3_IS_OP
        #undef V3_BRIDGE_PEEK_OR

        case OP_HEAD: {
            // Mirror primHead in primops.cc:272-278.
            Value v = pop(vm);
            if (v.isThunk() || v.tag() == Tag::App
                || v.tag() == Tag::Slot) {
                vm.frames.back().ip = ip;
                v = forceValue(vm, v);
            }
            if (!v.isList() || !v.payload.list || v.payload.list->size == 0)
                throw std::runtime_error("v3 primop head: empty list or wrong type");
            push(vm, v.payload.list->elems[0]);
            break;
        }

        case OP_TAIL: {
            // Mirror primTail in primops.cc:280-292.
            Value v = pop(vm);
            if (v.isThunk() || v.tag() == Tag::App
                || v.tag() == Tag::Slot) {
                vm.frames.back().ip = ip;
                v = forceValue(vm, v);
            }
            if (!v.isList() || !v.payload.list || v.payload.list->size == 0)
                throw std::runtime_error("v3 primop tail: empty list or wrong type");
            uint32_t n = v.payload.list->size;
            ListVec * out_l = Alloc::allocList(n - 1);
            allocStats().listsAllocated++;
            for (uint32_t i = 1; i < n; ++i)
                out_l->elems[i - 1] = v.payload.list->elems[i];
            Value r;
            r.tag_payload = static_cast<uint64_t>(Tag::List);
            r.payload.list = out_l;
            push(vm, r);
            break;
        }

        case OP_LENGTH: {
            // Mirror primLength in primops.cc:262-270.  Handles list
            // OR string; throws otherwise with the same message.
            Value v = pop(vm);
            if (v.isThunk() || v.tag() == Tag::App
                || v.tag() == Tag::Slot) {
                vm.frames.back().ip = ip;
                v = forceValue(vm, v);
            }
            int64_t n = 0;
            if (v.isList())
                n = v.payload.list ? v.payload.list->size : 0;
            else if (v.isString())
                n = static_cast<int64_t>(std::strlen(v.payload.str));
            else
                throw std::runtime_error("v3 primop length: expected list or string");
            Value r; r.mkInt(n);
            push(vm, r);
            break;
        }

        case OP_ELEM_AT: {
            // Mirror primElemAt in primops.cc:294-303.  Pops idx, then list.
            Value idx = pop(vm);
            Value lst = pop(vm);
            if (idx.isThunk() || idx.tag() == Tag::App
                || idx.tag() == Tag::Slot) {
                vm.frames.back().ip = ip;
                idx = forceValue(vm, idx);
            }
            if (lst.isThunk() || lst.tag() == Tag::App
                || lst.tag() == Tag::Slot) {
                vm.frames.back().ip = ip;
                lst = forceValue(vm, lst);
            }
            if (!lst.isList() || !idx.isInt())
                throw std::runtime_error("v3 primop elemAt: expected list and int");
            uint32_t n = lst.payload.list ? lst.payload.list->size : 0;
            if (idx.payload.i < 0 || static_cast<uint64_t>(idx.payload.i) >= n)
                throw std::runtime_error("v3 primop elemAt: index out of range");
            push(vm, lst.payload.list->elems[idx.payload.i]);
            break;
        }

        case OP_ATTRS_REC_SET: {
            uint32_t i = operand;
            Value v = pop(vm);
            // Peek at the rec bindings (top of stack now) and write into entry i.
            Value & recAttrs = top(vm);
            if (!recAttrs.isAttrs())
                throw std::runtime_error("v3 OP_ATTRS_REC_SET: top is not an attrset");
            if (!recAttrs.payload.bindings || i >= recAttrs.payload.bindings->size)
                throw std::runtime_error("v3 OP_ATTRS_REC_SET: index out of range");
            recAttrs.payload.bindings->entries[i].value = v;
            // STG-8 (#498): if this entry's value is a Suspended thunk
            // (the common case from the LetRec emit's per-attr thunks),
            // record &entries[i].value as the thunk's heap-stable cell.
            // OP_RETURN's CFF_THUNK_RETURN handler will write the body's
            // result back to *cell at completion, mirroring TW's in-
            // place forceValue update.  Sub-thunks captured-with a
            // Tag::Slot to this cell then read the result via single
            // deref instead of the legacy `Slot -> Thunk -> Evaluated`
            // chase, AND foreign-VM observers (cross-VMState fresh
            // VMStates the eval/call hooks spawn) stop seeing a leaked
            // Black thunk after the body completes -- the cell holds
            // the final value.
            //
            // We only set cell when the entry IS a fresh Suspended
            // thunk (avoid clobbering a previously-set cell from a
            // shared thunk, and skip non-thunk entries entirely).
            if (v.isThunk() && v.payload.thunk
                && v.payload.thunk->state == ThunkState::Suspended
                && v.payload.thunk->cell == nullptr)
            {
                v.payload.thunk->cell =
                    &recAttrs.payload.bindings->entries[i].value;
            }
            break;
        }

        case OP_HALT: {
            // Defensive: chase Tag::Thunk/App/Slot before exiting so the
            // caller never receives an unforced value if a future bytecode
            // change drops the trailing OP_FORCE.  Today the entry-function
            // code emitter inserts that force, so this is a tail-handling
            // safety net rather than a hot path.  REVIEW MED-3.
            finalResult = pop(vm);
            if (finalResult.tag() == Tag::Thunk
                || finalResult.tag() == Tag::App
                || finalResult.tag() == Tag::Slot) {
                vm.frames.back().ip = ip;
                finalResult = forceValue(vm, finalResult);
            }
            running = false;
            break;
        }

        default:
            std::fprintf(stderr, "v3 VM: unhandled opcode 0x%02x at ip=%u\n",
                static_cast<int>(op), ip - 1);
            std::abort();
        }
#pragma clang diagnostic pop
    }

    return finalResult;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

// WC-5: clear Black marks on any thunk frames currently in the VM.
// Used as the exception-recovery hook around dispatchLoop calls.
// Tree-walker's mkFailed stores the exception and re-throws; we
// take the cheaper-but-still-correct path of reverting Black to
// Suspended so the next force re-runs (idempotent throws then
// re-throw the same error).
static void clearBlackMarksOnException(VMState & vm, size_t exitDepth)
{
    auto & reg = partialBindingsRegistry();
    for (size_t i = vm.frames.size(); i > exitDepth; --i) {
        auto & fr = vm.frames[i - 1];
        if ((fr.flags & CFF_THUNK_RETURN) && fr.thunk
            && fr.thunk->state == ThunkState::Blackhole) {
            fr.thunk->state = ThunkState::Suspended;
            // #457/#458: drop any partial-Bindings registry entry
            // tied to this thunk -- the body didn't complete, so
            // the partial Bindings is incomplete and must not leak
            // to subsequent forces.
            auto it = reg.find(fr.thunk);
            if (it != reg.end()) reg.erase(it);
        }
    }
    // WC-37: also unwind the leftover frames pushed by the failed
    // dispatchLoop call.  Without this, a thunk frame that was on
    // the stack when the exception was thrown remains as a "ghost
    // frame" — when a subsequent OP_RETURN in an outer dispatchLoop
    // pops vm.frames.back(), it pops the ghost frame, finds
    // CFF_THUNK_RETURN + a thunk pointer, and stores whatever was on
    // the value stack into that thunk's evaluated slot.  In the
    // pure-VM nixpkgs case this corrupts the +chain preHook thunk
    // (codeOffset=1346, nUp=5) by storing a closure (e.g.
    // bintoolsPackages's body's MAKE_CLOSURE result) where a string
    // was expected.  Resize valueStack & withStack to the FIRST
    // popped frame's bases (= the stack/with depth when that frame
    // was pushed), preserving everything below.
    if (vm.frames.size() > exitDepth) {
        const auto & firstPopped = vm.frames[exitDepth];
        uint32_t targetStackBase = firstPopped.stackBaseOffset;
        uint32_t targetWithBase  = firstPopped.withStackBase;
        vm.frames.resize(exitDepth);
        if (vm.valueStack.size() > targetStackBase)
            vm.valueStack.resize(targetStackBase);
        if (vm.withStack.size() > targetWithBase)
            vm.withStack.resize(targetWithBase);
    }
    // REVIEW MED-3 + critic §8 #2: clear CFF_FORCE_RETRY on the
    // topmost surviving frame.  The flag is a one-shot set by OP_FORCE
    // before pushing a thunk frame, consumed by the caller's
    // OP_RETURN.  If the pushed thunk threw, the flag stayed on the
    // surviving caller; the next OP_RETURN would interpret a stale
    // value as a thunk-force result and trigger spurious retry.
    if (!vm.frames.empty())
        vm.frames.back().flags &= ~CFF_FORCE_RETRY;
}

/// #425: get the `builtins` attrset singleton (lazily built once,
/// reused process-wide).  Exposed publicly so the v3 force/call
/// hook can materialise an upvalue Value for sub-Exprs that
/// captured `builtins` (via `LitBuiltins`) as a freeVar -- there's
/// no env-side counterpart to walk to, so we just hand back the
/// singleton.  Same Value pushed by OP_LIT_BUILTINS at runtime.
Value getBuiltinsValue() noexcept
{
    static Value vBuiltins = []{
        const auto & reg = allRegisteredPrimOps();
        // REVIEW_2026-05-04 §6.1 follow-up: tree-walker's `addConstant`
        // registers `__currentSystem` etc into the BASE ENV but adds
        // only the stripped name (`currentSystem`) to `builtins`.
        // `RegisterPrimOp` uses the bare name throughout.  In both
        // cases, `__`-prefixed primops never appear as `builtins.X`.
        // Tree-walker test `eval-okay-builtins` enforces this:
        //   `assert !builtins ? __currentSystem;`
        // v3 used to violate the rule by exposing every registered
        // name (including `__add` etc) in `builtins`.  Filter them
        // out here so the `__` aliases serve only their base-env
        // resolution role.
        uint32_t nVisible = 0;
        for (auto & [poName, po] : reg) {
            if (poName.size() >= 2 && poName[0] == '_' && poName[1] == '_')
                continue;
            ++nVisible;
        }
        Bindings * b = Alloc::allocBindings(nVisible);
        uint32_t i = 0;
        for (auto & [poName, po] : reg) {
            if (poName.size() >= 2 && poName[0] == '_' && poName[1] == '_')
                continue;
            Value v;
            v.tag_payload = static_cast<uint64_t>(Tag::PrimOp);
            v.payload.primop = &po;
            SymbolId sid = ir::globalInternSymbol(poName);
            b->entries[i] = { sid, v };
            ++i;
        }
        // Bindings expects entries sorted by SymbolId for binary search.
        std::sort(&b->entries[0], &b->entries[b->size],
            [](const auto & a, const auto & b){ return a.name < b.name; });
        Value v;
        v.tag_payload = static_cast<uint64_t>(Tag::Attrs);
        v.payload.bindings = b;
        return v;
    }();
    return vBuiltins;
}

/// REVIEW §3 fold: shared dispatch wrapper.  Every entry point below
/// (run / runFunction / runFunctionWithUpvalues / runLambda) ends with
/// `try { dispatchLoop; clearBlackMarksOnException; return r; } catch
/// (...) { clearBlackMarksOnException; throw; }`.  Centralise so the
/// pattern is in one place; if the cleanup ever needs more steps,
/// they're added once.
[[gnu::always_inline]] inline Value dispatchAndClear(VMState & vm)
{
    try {
        Value r = dispatchLoop(vm, /*exitDepth=*/0);
        // WC-15 defensive: even on success, residual Black marks can
        // persist on the frame stack from incomplete sub-evals (e.g.
        // transitive thunk chains where intermediate frames don't
        // reach OP_RETURN).  Reset them so subsequent forces don't
        // see a stale Black mark.
        clearBlackMarksOnException(vm, 0);
        return r;
    } catch (...) {
        clearBlackMarksOnException(vm, 0);
        throw;
    }
}

/// STG-10 (#498) single-VM evaluation: when `activeV3VM()` is non-null
/// (= we're being re-entered from inside an outer v3 dispatchLoop via
/// a TW callback into the eval/call hook), push a frame onto that
/// existing VM and re-enter dispatchLoop with `exitDepth` set to the
/// frame count BEFORE we pushed.  When the new frame returns
/// (OP_RETURN brings frames back to exitDepth), dispatchLoop exits and
/// returns the body's result.
///
/// This eliminates fresh-VMState spawning at TW→v3 boundaries.  All v3
/// evaluation runs on a single VM per thread; thunks Black-marked by
/// any nested call are visible to all frames on the same VM (so the
/// existing forceValue cycle detection works correctly), and the
/// cross-VMState fresh-VMState pattern that surfaced in nixpkgs
/// hello.name under STG_KEEP_HOOKS=1 disappears at the source.
///
/// Mirrors the OP_CALL frame-setup contract: caller-provided arg goes
/// at slot 0 (when `arg` is non-null); caller-provided upvalues go on
/// a synthetic Closure; capturedWiths are pushed AFTER the frame's
/// withStackBase is set, so OP_WITH_LOOKUP picks them up.  Any
/// exception thrown from the body is caught here, the inner frames
/// are unwound via clearBlackMarksOnException scoped to exitDepth (so
/// the outer's existing Black marks are preserved), and the exception
/// is re-thrown to the caller of run/runFunction/runLambda.
inline Value runOnExistingVm(VMState & vm,
                              const CompilationUnit & cu,
                              const LambdaDescriptor & desc,
                              const Value * upvalues,
                              uint32_t nUpvalues,
                              ListVec * capturedWiths,
                              const Value * arg)
{
    const size_t exitDepth = vm.frames.size();

    // Synthesize a Closure (carries upvalues + captured-withs through
    // the frame for OP_GET_UPVALUE / pushCapturedWiths).  Allocated on
    // the v3 heap (Boehm GC); lives as long as the frame needs it.
    Closure * fakeClo = nullptr;
    if (nUpvalues > 0 || arg != nullptr) {
        // Even arg-only frames (no upvalues) need a Closure so the
        // dispatch loop's `closure` register has a valid descriptor
        // to query (e.g. for capturedWiths or selector fast-paths).
        fakeClo = Alloc::allocClosure(nUpvalues);
        fakeClo->desc = &desc;
        fakeClo->cu   = &cu;
        fakeClo->capturedWiths = capturedWiths;
        fakeClo->nUpvalues = static_cast<uint16_t>(nUpvalues);
        for (uint32_t i = 0; i < nUpvalues; ++i)
            fakeClo->upvalues[i] = upvalues[i];
    }

    // Frame-setup mirrors OP_CALL (vm.cc:2316+) for arg-bearing calls
    // and runFunction's outer-with carriage layering for the no-arg
    // case (#416 layering rationale).
    const size_t base = vm.valueStack.size();
    vm.valueStack.resize(base + desc.nLocals);
    if (arg != nullptr)
        vm.valueStack[base] = *arg;

    const uint32_t newWithBase =
        static_cast<uint32_t>(vm.withStack.size());

    vm.frames.push_back(CallFrame{
        .cu = &cu,
        .closure = fakeClo,
        .thunk = nullptr,
        .ip = desc.codeOffset,
        .stackBaseOffset = static_cast<uint32_t>(base),
        .withStackBase = newWithBase,
        .flags = 0,
    });
    pushCapturedWiths(vm, capturedWiths);

    // Re-enter dispatchLoop on the SAME VM, with exitDepth set so the
    // new frame's OP_RETURN unwinds dispatchLoop back to the caller.
    // Black-mark cleanup is scoped to exitDepth so outer frames'
    // existing Black marks are preserved on exception (they belong to
    // the outer dispatchLoop's frames, which we MUST NOT touch).
    try {
        Value r = dispatchLoop(vm, exitDepth);
        clearBlackMarksOnException(vm, exitDepth);
        return r;
    } catch (...) {
        clearBlackMarksOnException(vm, exitDepth);
        throw;
    }
}

Value run(const CompilationUnit & rootCu)
{
    // STG-10: re-use an active VM if available (TW→v3 re-entry while a
    // v3 dispatchLoop is on the C-stack).  `run` is the top-level
    // entry, so no arg / no upvalues; the body opens at
    // `rootCu.entryOffset` which lambdas[0]'s codeOffset points at by
    // construction (see CompilationUnit::entryOffset documentation).
    if (VMState * activeVm = activeV3VM(); activeVm && !rootCu.lambdas.empty()) {
        // The top-level "lambda" is the synthetic entry function the
        // emitter generates for the whole program; its codeOffset is
        // `rootCu.entryOffset` (verified equal in the emitter).  Push
        // a frame at that offset onto the existing VM.
        return runOnExistingVm(*activeVm, rootCu, rootCu.lambdas[0],
                                /*upvalues*/nullptr, /*nUp*/0,
                                /*capturedWiths*/nullptr,
                                /*arg*/nullptr);
    }

    VMState vm;
    // Generous initial reservations: deep-recursive workloads (fib,
    // ackermann, large fold chains) churn the value/frame stacks
    // many times.  Avoiding reallocation through the hot path is a
    // measurable win.
    vm.valueStack.reserve(64 * 1024);
    vm.frames.reserve(4096);
    vm.withStack.reserve(64);

    vm.frames.push_back(CallFrame{
        .cu = &rootCu,
        .closure = nullptr,
        .thunk = nullptr,
        .ip = rootCu.entryOffset,
        .stackBaseOffset = 0,
        .withStackBase = 0,
        .flags = 0,
    });

    if (!rootCu.lambdas.empty())
        vm.valueStack.resize(rootCu.lambdas[0].nLocals);

    return dispatchAndClear(vm);
}

/// CO-3: run an arbitrary FuncId in `cu` as if it were a thunk body.
/// No upvalues, no args.  Used by the forceValue cutover hook for
/// per-thunk-body Functions whose `nUpvalues == 0` — i.e., closed
/// thunks the lowerer recorded in `Module::subExprFuncs`.
Value runFunction(const CompilationUnit & cu, uint32_t funcIdx,
                   ListVec * capturedWiths)
{
    if (funcIdx >= cu.lambdas.size())
        throw std::runtime_error("v3 runFunction: funcIdx out of range");
    const auto & desc = cu.lambdas[funcIdx];
    if (desc.nUpvalues != 0)
        throw std::runtime_error("v3 runFunction: function expects upvalues; use runFunctionWithUpvalues");

    // STG-10: re-use the active VM if we're being re-entered from
    // inside an outer v3 dispatchLoop (TW→v3 re-entry).  Avoids
    // spawning a fresh VMState, which would create cross-VMState
    // Black-mark leaks for thunks visited by both VMs.
    if (VMState * activeVm = activeV3VM()) {
        return runOnExistingVm(*activeVm, cu, desc,
                                /*upvalues*/nullptr, /*nUp*/0,
                                capturedWiths, /*arg*/nullptr);
    }

    VMState vm;
    vm.valueStack.reserve(64 * 1024);
    vm.frames.reserve(4096);
    vm.withStack.reserve(64);

    // #416: outer with-stack carriage.  Set the frame's withStackBase
    // BELOW the captured chain (i.e., at the current vm.withStack.size,
    // which is 0 here), then push.  withLookup walks from top of stack
    // DOWN to withStackBase (vm.cc:381), so anything ABOVE the base is
    // visible -- exactly what we want for the captured outer withs.
    // The function's own ir::With blocks push/pop on top of these.
    vm.frames.push_back(CallFrame{
        .cu = &cu,
        .closure = nullptr,
        .thunk = nullptr,
        .ip = desc.codeOffset,
        .stackBaseOffset = 0,
        .withStackBase = static_cast<uint32_t>(vm.withStack.size()),
        .flags = 0,
    });
    pushCapturedWiths(vm, capturedWiths);

    vm.valueStack.resize(desc.nLocals);

    return dispatchAndClear(vm);
}

/// CO-2 phase B: run a per-thunk Function with caller-provided
/// upvalues.  The forceValue cutover walks tree-walker's Env to
/// collect upvalue values, then calls here.  We synthesize a
/// Closure on the heap (allocated via Boehm GC; lives as long as
/// the call's frame), point the frame's closure to it, and run.
Value runFunctionWithUpvalues(const CompilationUnit & cu, uint32_t funcIdx,
                               const Value * upvalues, uint32_t nUpvalues,
                               ListVec * capturedWiths)
{
    if (funcIdx >= cu.lambdas.size())
        throw std::runtime_error("v3 runFunctionWithUpvalues: funcIdx out of range");
    const auto & desc = cu.lambdas[funcIdx];
    if (desc.nUpvalues != nUpvalues)
        throw std::runtime_error("v3 runFunctionWithUpvalues: nUpvalues mismatch");

    // STG-10: re-use the active VM if available (TW→v3 re-entry).
    if (VMState * activeVm = activeV3VM()) {
        return runOnExistingVm(*activeVm, cu, desc,
                                upvalues, nUpvalues,
                                capturedWiths, /*arg*/nullptr);
    }

    Closure * fakeClo = Alloc::allocClosure(nUpvalues);
    fakeClo->desc = &desc;
    fakeClo->cu   = &cu;
    // #416: also publish the captured chain on the closure so any
    // sub-call that re-uses fakeClo (e.g. via tail calls in the body)
    // sees the outer withs through pushCapturedWiths().
    fakeClo->capturedWiths = capturedWiths;
    fakeClo->nUpvalues = static_cast<uint16_t>(nUpvalues);
    for (uint32_t i = 0; i < nUpvalues; ++i)
        fakeClo->upvalues[i] = upvalues[i];

    VMState vm;
    vm.valueStack.reserve(64 * 1024);
    vm.frames.reserve(4096);
    vm.withStack.reserve(64);

    // #416: see runFunction() for the layering rationale -- frame's
    // withStackBase below the captured chain, captured chain pushed
    // on top, so OP_WITH_LOOKUP picks it up while the function's own
    // ir::With pushes layer above.
    vm.frames.push_back(CallFrame{
        .cu = &cu,
        .closure = fakeClo,
        .thunk = nullptr,
        .ip = desc.codeOffset,
        .stackBaseOffset = 0,
        .withStackBase = static_cast<uint32_t>(vm.withStack.size()),
        .flags = 0,
    });
    pushCapturedWiths(vm, capturedWiths);

    vm.valueStack.resize(desc.nLocals);

    return dispatchAndClear(vm);
}

/// #426: invoke a v3 lambda body Function with one supplied argument.
/// Mirrors runFunctionWithUpvalues but seeds slot 0 with `arg` so the
/// body's OP_GET_LOCAL 0 reads the caller-supplied value -- exactly
/// matching what OP_CALL does at vm.cc:1323-1325.
Value runLambda(const CompilationUnit & cu, uint32_t funcIdx,
                Value arg,
                const Value * upvalues, uint32_t nUpvalues,
                ListVec * capturedWiths)
{
    if (funcIdx >= cu.lambdas.size())
        throw std::runtime_error("v3 runLambda: funcIdx out of range");
    const auto & desc = cu.lambdas[funcIdx];
    if (desc.nUpvalues != nUpvalues)
        throw std::runtime_error("v3 runLambda: nUpvalues mismatch");

    // #424: selector-lambda fast path also fires when the call hook
    // routes here (bypassing OP_CALL).  Same shape -- force arg,
    // project, return.  Skips the entire frame setup + dispatch loop.
    if (__builtin_expect(desc.selectorSym != 0, 0)) {
        allocStats().selectorLambdaCalls++;
        // The arg may still be a Thunk/App/Slot; force first.
        //
        // STG-10 (#498): re-use the active VM's forceValue rather than
        // a throwaway VMState.  The throwaway pattern was the original
        // source of cross-VMState Black-mark leaks: if `arg` is a slot
        // pointing at a thunk currently being forced on the outer VM,
        // forcing on a fresh VM throws BlackholeError that the outer
        // VM has no way to recover from.  Sharing the outer VM means
        // the cycle detection sees the in-flight thunk on the same
        // frame stack and the existing chase logic (vm.cc:5293+)
        // handles it correctly.
        //
        // REVIEW §3: wrap forceValue in try/catch + clearBlackMarks
        // so a thrown forceValue doesn't leave Black marks on the
        // (potentially throwaway) VMState's frames.  Mirror what the
        // main dispatchLoop does below.
        Value sArg = arg;
        if (sArg.isThunk() || sArg.tag() == Tag::App
            || sArg.tag() == Tag::Slot) {
            if (VMState * activeVm = activeV3VM()) {
                size_t exitDepth = activeVm->frames.size();
                try {
                    sArg = forceValue(*activeVm, sArg);
                } catch (...) {
                    clearBlackMarksOnException(*activeVm, exitDepth);
                    throw;
                }
                clearBlackMarksOnException(*activeVm, exitDepth);
            } else {
                VMState forceVm;
                forceVm.valueStack.reserve(64);
                forceVm.frames.reserve(64);
                forceVm.withStack.reserve(8);
                try {
                    sArg = forceValue(forceVm, sArg);
                } catch (...) {
                    clearBlackMarksOnException(forceVm, 0);
                    throw;
                }
                clearBlackMarksOnException(forceVm, 0);
            }
        }
        if (!sArg.isAttrs() || !sArg.payload.bindings)
            throw std::runtime_error(
                "v3 selector lambda: arg not an attrset");
        const Value * v = sArg.payload.bindings->lookup(desc.selectorSym);
        if (!v)
            throw std::runtime_error(
                "v3 selector lambda: missing attr");
        return *v;
    }

    // STG-10: re-use the active VM if available (TW→v3 re-entry).
    // Slot identity in `arg` is preserved through runOnExistingVm's
    // OP_CALL-shaped frame setup.
    if (VMState * activeVm = activeV3VM()) {
        return runOnExistingVm(*activeVm, cu, desc,
                                upvalues, nUpvalues,
                                capturedWiths, /*arg*/&arg);
    }

    Closure * fakeClo = Alloc::allocClosure(nUpvalues);
    fakeClo->desc = &desc;
    fakeClo->cu   = &cu;
    fakeClo->capturedWiths = capturedWiths;
    fakeClo->nUpvalues = static_cast<uint16_t>(nUpvalues);
    for (uint32_t i = 0; i < nUpvalues; ++i)
        fakeClo->upvalues[i] = upvalues[i];

    VMState vm;
    vm.valueStack.reserve(64 * 1024);
    vm.frames.reserve(4096);
    vm.withStack.reserve(64);

    // Mirror OP_CALL's frame setup: nLocals slots reserved, slot 0 = arg.
    size_t base = vm.valueStack.size();
    vm.valueStack.resize(base + desc.nLocals);
    vm.valueStack[base] = arg;

    vm.frames.push_back(CallFrame{
        .cu = &cu,
        .closure = fakeClo,
        .thunk = nullptr,
        .ip = desc.codeOffset,
        .stackBaseOffset = static_cast<uint32_t>(base),
        .withStackBase = static_cast<uint32_t>(vm.withStack.size()),
        .flags = 0,
    });
    pushCapturedWiths(vm, capturedWiths);

    return dispatchAndClear(vm);
}

Value forceValue(VMState & vm, Value v)
{
    // STG-12 (#498) diagnostic: log the call site (current top frame
    // CU + ip) when forceValue is invoked with an input that, after
    // chase, lands on a Black thunk on this VM's frames.  That tells
    // us which opcode handler is calling forceValue with the cycle
    // source.  V3_DBG_FORCE_CALLSITE=1 to enable.  Cached because
    // forceValue is called on every OP_FORCE / get-local-force-on-
    // thunk slow path; the per-call getenv was visible in profiling.
    static const bool s_dbgForceCallsite =
        std::getenv("V3_DBG_FORCE_CALLSITE") != nullptr;
    if (__builtin_expect(s_dbgForceCallsite, 0)) {
        Value chase = v;
        Thunk * blackOnFrames = nullptr;
        // Record chase trace for printing.
        constexpr int kTraceMax = 8;
        Tag traceTag[kTraceMax] = {};
        void * tracePtr[kTraceMax] = {};
        int traceCount = 0;
        auto recordHop = [&](Value val) {
            if (traceCount < kTraceMax) {
                traceTag[traceCount] = val.tag();
                tracePtr[traceCount] = val.payload.thunk;  // any pointer
                ++traceCount;
            }
        };
        recordHop(chase);
        for (int hops = 0; hops < 16; ++hops) {
            if (chase.tag() == Tag::Slot && chase.payload.slot) {
                chase = *chase.payload.slot;
                recordHop(chase);
            } else if (chase.tag() == Tag::Thunk
                       && chase.payload.thunk) {
                Thunk * th = chase.payload.thunk;
                if (th->state == ThunkState::Evaluated) {
                    chase = th->evaluated;
                    recordHop(chase);
                } else if (th->state == ThunkState::Blackhole) {
                    // Black on this VM's frames?
                    for (size_t i = 0; i < vm.frames.size(); ++i) {
                        if (vm.frames[i].thunk == th) {
                            blackOnFrames = th; break;
                        }
                    }
                    break;
                } else {
                    break;
                }
            } else break;
        }
        if (blackOnFrames) {
            const LambdaDescriptor * bd = blackOnFrames->suspended.desc;
            void * caller = __builtin_return_address(0);
            std::fprintf(stderr,
                "v3 forceValue → BLACK on-frames thunk=%p name=%s frames=%zu caller=%p\n",
                (void *)blackOnFrames,
                bd && !bd->name.empty() ? bd->name.c_str() : "<?>",
                vm.frames.size(), caller);
            // C-stack backtrace via execinfo so we can see who called
            // public forceValue.
            {
                void * cstack[32];
                int nFrames = ::backtrace(cstack, 32);
                char ** syms = ::backtrace_symbols(cstack, nFrames);
                std::fprintf(stderr, "  C-stack (%d frames):\n", nFrames);
                for (int k = 0; k < nFrames && k < 12; ++k)
                    std::fprintf(stderr, "    %s\n", syms[k]);
                if (syms) std::free(syms);
            }
            std::fprintf(stderr, "  chase trace (%d hops):", traceCount);
            for (int k = 0; k < traceCount; ++k) {
                std::fprintf(stderr, " [%d:tag=%d ptr=%p]",
                    k, (int)traceTag[k], tracePtr[k]);
            }
            std::fprintf(stderr, "\n");
            // Find which frame holds the BLACK thunk so we can show
            // it explicitly in the trace.
            size_t blackIdx = (size_t)-1;
            for (size_t i = 0; i < vm.frames.size(); ++i) {
                if (vm.frames[i].thunk == blackOnFrames) {
                    blackIdx = i; break;
                }
            }
            std::fprintf(stderr, "  blackIdx=%zd\n", (ssize_t)blackIdx);
            // Backtrace: show the top 12 frames so we can identify the
            // forceValue caller chain.  Always include the BLACK frame
            // even if outside the window.
            size_t n = vm.frames.size();
            size_t lo = n > 12 ? n - 12 : 0;
            if (blackIdx != (size_t)-1 && blackIdx < lo) lo = blackIdx;
            for (size_t i = n; i-- > lo;) {
                const auto & fr = vm.frames[i];
                const LambdaDescriptor * d = nullptr;
                if (fr.thunk
                    && (fr.thunk->state == ThunkState::Suspended
                        || fr.thunk->state == ThunkState::Blackhole))
                    d = fr.thunk->suspended.desc;
                else if (fr.closure) d = fr.closure->desc;
                Instruction prev = (fr.cu && fr.ip > 0
                                    && fr.ip <= fr.cu->code.size())
                    ? fr.cu->code[fr.ip - 1] : 0;
                std::fprintf(stderr,
                    "  fr[%zu]: %s ip=%u prev-op=0x%02x cu=%p flags=0x%x thunk=%p"
                    " codeOff=%u%s\n",
                    i,
                    d && !d->name.empty() ? d->name.c_str() : "<?>",
                    fr.ip, (unsigned)((prev >> 24) & 0xFF),
                    (const void *)fr.cu,
                    (unsigned)fr.flags, (void *)fr.thunk,
                    d ? d->codeOffset : 0u,
                    fr.thunk == blackOnFrames ? " ← BLACK" : "");
                if (fr.cu && fr.ip > 0
                    && fr.ip <= fr.cu->code.size()) {
                    // Find the enclosing LambdaDescriptor by scanning the
                    // cu's funcs (codeOffset closest to but not exceeding
                    // fr.ip).
                    const LambdaDescriptor * encl = nullptr;
                    for (const auto & ld : fr.cu->lambdas) {
                        if (ld.codeOffset <= fr.ip
                            && (!encl || ld.codeOffset > encl->codeOffset))
                            encl = &ld;
                    }
                    std::fprintf(stderr, "    enclosing-lambda: %s codeOff=%u nL=%u\n",
                        encl
                            ? (encl->name.empty() ? "<?>" : encl->name.c_str())
                            : "<no-funcs>",
                        encl ? encl->codeOffset : 0u,
                        encl ? encl->nLocals : 0u);
                    // For fr[16] only: dump the full body of the
                    // enclosing lambda so we can see where ip=157 sits.
                    static bool dumped_full = false;
                    if (encl && i == n - 2 && !dumped_full) {
                        dumped_full = true;
                        // Find end of this lambda (start of next lambda
                        // by codeOffset).
                        uint32_t bodyEnd = (uint32_t)fr.cu->code.size();
                        for (const auto & ld : fr.cu->lambdas) {
                            if (ld.codeOffset > encl->codeOffset
                                && ld.codeOffset < bodyEnd)
                                bodyEnd = ld.codeOffset;
                        }
                        std::fprintf(stderr,
                            "    full body codeOff=%u..%u:\n",
                            encl->codeOffset, bodyEnd);
                        for (uint32_t k = encl->codeOffset; k < bodyEnd; ++k) {
                            Instruction w = fr.cu->code[k];
                            std::fprintf(stderr,
                                "      [%u:%02x %06x]%s\n",
                                k, (unsigned)((w >> 24) & 0xFF),
                                (unsigned)(w & 0xFFFFFF),
                                k == fr.ip ? "  <-- ip" : "");
                        }
                        // Dump symbols 1176 (prev) and 138 (the one referenced
                        // in x's body — see fr[5]'s code).
                        const auto & tbl = ir::globalSymbolTable();
                        std::fprintf(stderr, "    sym 1176 = %s   sym 138 = %s\n",
                            1176u < tbl.size() ? tbl[1176].c_str() : "<?>",
                            138u < tbl.size() ? tbl[138].c_str() : "<?>");
                    }
                    int lo = (int)fr.ip - 6; if (lo < 0) lo = 0;
                    int hi = (int)fr.ip + 4;
                    if (hi > (int)fr.cu->code.size())
                        hi = (int)fr.cu->code.size();
                    std::fprintf(stderr, "    code: ");
                    for (int k = lo; k < hi; ++k) {
                        Instruction w = fr.cu->code[k];
                        std::fprintf(stderr, "[%d:%02x %06x]%s",
                            k, (unsigned)((w >> 24) & 0xFF),
                            (unsigned)(w & 0xFFFFFF),
                            k == (int)fr.ip ? "*" : " ");
                    }
                    std::fprintf(stderr, "\n");
                }
            }
            const auto & fr = vm.frames.back();
            const LambdaDescriptor * d = nullptr;
            if (fr.thunk
                && (fr.thunk->state == ThunkState::Suspended
                    || fr.thunk->state == ThunkState::Blackhole))
                d = fr.thunk->suspended.desc;
            else if (fr.closure) d = fr.closure->desc;
            // Print the opcode at ip-1 (the op that was just running)
            // and ip (next op).
            if (fr.cu && fr.ip > 0
                && fr.ip <= fr.cu->code.size()) {
                // Opcode = top 8 bits of the 32-bit Instruction word.
                Instruction prev = fr.ip > 0 ? fr.cu->code[fr.ip - 1] : 0;
                Instruction curr = fr.ip < fr.cu->code.size()
                    ? fr.cu->code[fr.ip] : 0;
                std::fprintf(stderr,
                    "  prev op (ip-1=%u) = 0x%02x  next op (ip=%u) = 0x%02x\n",
                    fr.ip - 1, (unsigned)((prev >> 24) & 0xFF),
                    fr.ip, (unsigned)((curr >> 24) & 0xFF));
                // Also dump a small window so we can see the surrounding
                // instructions if op alone isn't enough.
                std::fprintf(stderr, "  ip window: ");
                int lo = (int)fr.ip - 3; if (lo < 0) lo = 0;
                int hi = (int)fr.ip + 3;
                if (hi > (int)fr.cu->code.size())
                    hi = (int)fr.cu->code.size();
                for (int k = lo; k < hi; ++k) {
                    Instruction w = fr.cu->code[k];
                    std::fprintf(stderr, "[%d:%02x %06x]%s",
                        k, (unsigned)((w >> 24) & 0xFF),
                        (unsigned)(w & 0xFFFFFF),
                        k == (int)fr.ip ? "*" : " ");
                }
                std::fprintf(stderr, "\n");
            }
        }
    }
    // Track the FIRST slot we passed through so we can memoize the
    // final result back into it.  Mirrors tree-walker's behavior:
    // `state.forceValue(*v2)` mutates the slot directly, so a future
    // read of the same slot sees the resolved value (no need to walk
    // the thunk chain again).  Without memoization, every withLookup
    // through a Tag::Slot would re-force the underlying thunk.
    Value * memoSlot = nullptr;
    // Iteration bound: detect infinite chases through Tag::Slot →
    // Tag::Thunk(Eval=Slot→...) cycles that arise from self-referential
    // let-rec patterns like `let x = x; in x` or `let x = y; y = x; in x`.
    // The Black-state check normally catches direct recursion, but
    // SECD-style indirections (Slot→Eval→Slot) can chase forever
    // without re-entering the Black thunk.  16 iterations is well
    // beyond any realistic indirection chain (≤4 in practice for
    // recref+thunkify+slot+memo) and only fires on pathological
    // cycles.
    // See kMaxIndirectionChase / kMaxCallDepth at the top of the
    // anonymous namespace for rationale.
    int chaseIters = 0;
    // V3_DBG_CHASE: optional ring-buffer of recent (tag, ptr) pairs so
    // we can dump the chain shape if the limit fires.  Cheap when
    // disabled (single static check on hot path).
    static const bool s_dbg_chase = std::getenv("V3_DBG_CHASE") != nullptr;
    constexpr int kRingSize = 32;
    Tag ringTag[kRingSize] = {};
    void * ringPtr[kRingSize] = {};
    int ringIdx = 0;
    // Loop until WHNF: a thunk's body might itself yield a thunk
    // (e.g., `let inherit outer; in outer` returns the outer thunk),
    // and we want to chase the chain until we land on a real value.
    while (true) {
        if (__builtin_expect(s_dbg_chase, 0)) {
            ringTag[ringIdx % kRingSize] = v.tag();
            void * p = nullptr;
            if (v.tag() == Tag::Thunk) p = v.payload.thunk;
            else if (v.tag() == Tag::Slot) p = v.payload.slot;
            else if (v.tag() == Tag::App) p = v.payload.pair;
            ringPtr[ringIdx % kRingSize] = p;
            ringIdx++;
        }
        if (__builtin_expect(++chaseIters > kMaxIndirectionChase, 0)) {
            if (s_dbg_chase) {
                std::fprintf(stderr,
                    "v3 chase-cycle limit %d hit; last %d steps:\n",
                    kMaxIndirectionChase, kRingSize);
                int start = ringIdx > kRingSize ? ringIdx - kRingSize : 0;
                for (int i = start; i < ringIdx; ++i) {
                    int slot = i % kRingSize;
                    std::fprintf(stderr,
                        "  step[%d]: tag=%d ptr=%p", i,
                        (int)ringTag[slot], ringPtr[slot]);
                    // For Thunks, additionally show their state +
                    // evaluated tag so we can see "Evaluated → Thunk →
                    // Evaluated → ...".
                    if (ringTag[slot] == Tag::Thunk && ringPtr[slot]) {
                        auto * t = static_cast<Thunk *>(ringPtr[slot]);
                        std::fprintf(stderr, " state=%d", (int)t->state);
                        if (t->state == ThunkState::Evaluated)
                            std::fprintf(stderr, " evaluated.tag=%d",
                                (int)t->evaluated.tag());
                    }
                    std::fprintf(stderr, "\n");
                }
            }
            throw std::runtime_error(
                "v3 forceValue: infinite recursion (chase cycle through "
                "Tag::Slot/Tag::Thunk indirections)");
        }
        // Same call-depth guard — `let x = x; in x` lands here in
        // a C++ recursion via dispatchLoop → forceValue → dispatchLoop
        // and never grows through the bytecode-level OP_CALL/OP_FORCE
        // guards.  Match those guards.
        if (__builtin_expect(vm.frames.size() >= kMaxCallDepth, 0))
            throw std::runtime_error("v3 forceValue: stack overflow; call depth exceeded "
                                      + std::to_string(kMaxCallDepth));
        // Tag::Slot — SECD-style indirection.  The slot pointer
        // references another stable Value that gets mutated when its
        // let-rec body publishes a result.  Dereference and continue
        // chasing.  See `with self;` semantics in Tag::Slot's docstring
        // for why this matters: sub-thunks captured under `with self;`
        // must observe the latest slot contents at force time, not a
        // snapshot from when the with-stack was pushed.
        if (v.tag() == Tag::Slot) {
            Value * p = v.payload.slot;
            if (!p) throw std::runtime_error("v3 forceValue: null slot pointer");
            // Remember the OUTERMOST slot for memoization.  If we
            // pass through multiple Tag::Slot indirections (chained),
            // memoize at the first one — its slot is what consumers
            // hold.  Inner slots get memoized by their own future
            // forceValue calls.
            if (!memoSlot) memoSlot = p;
            v = *p;
            continue;
        }
        // Tag::App is a deferred application — force it by actually
        // applying.  Used by primops like mapAttrs that build lazy
        // entries: each entry is `App(fn, arg)` and we materialize on
        // demand.  `left` may itself be an App / Thunk (e.g. mapAttrs
        // builds App(App(fn, name), value)) — force the spine first.
        if (v.tag() == Tag::App) {
            Value left  = v.payload.pair->left;
            Value right = v.payload.pair->right;
            left = forceValue(vm, left);
            v = callClosure(vm, left, right);
            continue;
        }
        if (!v.isThunk()) break;
        Thunk * t = v.payload.thunk;
        if (t->state == ThunkState::Evaluated) { v = t->evaluated; continue; }
        if (t->state == ThunkState::Blackhole) {
            // #458 lambda-skip leaked-Black recovery (opt-in).  When the
            // thunk's Black state was set by a previous VMState that has
            // since unwound, the current VMState's frame stack does NOT
            // contain the thunk.  Treat this as a leaked mark — reset to
            // Suspended and let the chase fall through to the Suspended
            // handler below to re-run the body idempotently.  Gated
            // behind NIX_V3_LEAKED_BLACK_RECOVER=1 because the prior
            // attempt (memory file) turned BlackHole into a chase cycle
            // when the leak interpretation was wrong on a real cycle.
            // With the slot-capture redesign + RecBuildSlot in place,
            // genuine self-reference cycles are sidestepped earlier (via
            // Tag::Slot deref instead of forcing a wrap thunk), so the
            // leak interpretation should be correct in more cases.
            static const bool s_recover =
                std::getenv("NIX_V3_LEAKED_BLACK_RECOVER") != nullptr;
            if (s_recover) {
                bool onCurrentFrames = false;
                for (size_t i = 0; i < vm.frames.size(); ++i) {
                    if (vm.frames[i].thunk == t) { onCurrentFrames = true; break; }
                }
                if (!onCurrentFrames) {
                    // Per-(VMState, Thunk) re-entry counter: if recovery
                    // recurs on the same thunk pointer N times in a row
                    // from the same VMState, the leak interpretation is
                    // wrong (it's a real cycle).  Throw the BlackholeError
                    // through the normal path instead of looping.
                    //
                    // REVIEW_2026-05-06b C1: was a thread_local static
                    // map keyed only by Thunk*.  Across multiple top-level
                    // evals on the same thread, stale counts could leak
                    // (a Thunk* freed in eval A whose address gets
                    // reused for a NEW thunk in eval B inherits A's
                    // count, mis-classifying a fresh leak as a "real
                    // cycle").  Now keyed by (VMState*, Thunk*) so each
                    // VMState has its own counter space.  Bounded blow-
                    // up: stale entries from destroyed VMStates linger
                    // until they hit the 4-attempt cap and are erased.
                    struct KeyHash {
                        size_t operator()(const std::pair<const void *, Thunk *> & p) const noexcept {
                            return std::hash<const void *>{}(p.first)
                                 ^ (std::hash<Thunk *>{}(p.second) << 1);
                        }
                    };
                    static thread_local std::unordered_map<
                        std::pair<const void *, Thunk *>, int, KeyHash>
                        reentryCount;
                    auto key = std::make_pair(
                        static_cast<const void *>(&vm), t);
                    int & cnt = reentryCount[key];
                    if (++cnt > 4) {
                        reentryCount.erase(key);
                        // Fall through to throw below.
                    } else {
                        static const bool s_dbgRec =
                            std::getenv("V3_DBG_LEAKED_BLACK") != nullptr;
                        if (s_dbgRec) std::fprintf(stderr,
                            "v3 forceValue: leaked-Black recover thunk=%p "
                            "(vm=%p frames=%zu, not on stack, attempt %d) -> Suspended\n",
                            (void*)t, (void*)&vm, vm.frames.size(), cnt);
                        t->state = ThunkState::Suspended;
                        continue;
                    }
                }
            }
            // Same diagnostic as OP_FORCE's blackhole path — V3_DBG_OPCYCLE
            // dumps the frame stack so the cycle source is visible.
            static const bool s_dbg = std::getenv("V3_DBG_OPCYCLE") != nullptr;
            if (s_dbg) {
                // suspended.desc is only valid for Suspended/Blackhole
                // thunks — reading it on Bridge/Evaluated thunks accesses
                // the wrong union variant and the resulting `desc->name`
                // segfaults silently, terminating the dump after one
                // frame.  Guard the read.
                auto frameInfo = [&](Thunk * th, const Closure * cl, uint32_t fip) -> std::string {
                    const LambdaDescriptor * desc = nullptr;
                    if (th && (th->state == ThunkState::Suspended
                            || th->state == ThunkState::Blackhole))
                        desc = th->suspended.desc;
                    else if (cl) desc = cl->desc;
                    if (!desc) return "<closure-body>";
                    char buf[256];
                    std::snprintf(buf, sizeof buf,
                        "%s code=[%u..) nUp=%u nLocals=%u",
                        !desc->name.empty() ? desc->name.c_str() : "<anon>",
                        desc->codeOffset, desc->nUpvalues, desc->nLocals);
                    return buf;
                };
                const LambdaDescriptor * tdesc =
                    (t && (t->state == ThunkState::Suspended
                        || t->state == ThunkState::Blackhole))
                    ? t->suspended.desc : nullptr;
                std::fprintf(stderr,
                    "v3 forceValue Black thunk=%p frames=%zu desc.name=%s desc.code=%u\n",
                    (void*)t, vm.frames.size(),
                    (tdesc && !tdesc->name.empty()) ? tdesc->name.c_str() : "<anon>",
                    tdesc ? tdesc->codeOffset : 0);
                size_t lim = vm.frames.size();
                ssize_t blackIdx = -1;
                for (size_t i = lim; i > 0; --i) {
                    const auto & fr = vm.frames[i - 1];
                    bool isBlack = (fr.thunk == t);
                    if (isBlack) blackIdx = (ssize_t)(i - 1);
                    std::fprintf(stderr,
                        "  frame[%zu]:%s %s flags=%u ip=%u thunk=%p\n",
                        i - 1, isBlack ? " <-BLACK" : "",
                        frameInfo(fr.thunk, fr.closure, fr.ip).c_str(),
                        (unsigned)fr.flags, fr.ip, (void*)fr.thunk);
                }
                static const bool s_dbg_disasm =
                    std::getenv("V3_DBG_OPCYCLE_DISASM") != nullptr;
                if (s_dbg_disasm && blackIdx >= 0) {
                    // Dump the BLACK frame's prologue (start of body)
                    // through current ip — captures every OP_FORCE the
                    // body ran before re-entering itself.
                    const auto & fr = vm.frames[blackIdx];
                    if (fr.cu) {
                        const LambdaDescriptor * desc = nullptr;
                        if (fr.thunk)
                            desc = fr.thunk->suspended.desc;
                        else if (fr.closure)
                            desc = fr.closure->desc;
                        if (desc) {
                            uint32_t lo = desc->codeOffset;
                            uint32_t hi = fr.ip + 8;
                            std::fprintf(stderr,
                                "  BLACK frame[%zd] disasm [%u..%u) (prologue→ip):\n",
                                blackIdx, lo, hi);
                            disassembleWindow(stderr, *fr.cu, lo, hi);
                        }
                    }
                    // Also dump the innermost frame's prologue → ip.
                    const auto & inner = vm.frames.back();
                    if (inner.cu) {
                        const LambdaDescriptor * idesc = nullptr;
                        if (inner.thunk)
                            idesc = inner.thunk->suspended.desc;
                        else if (inner.closure)
                            idesc = inner.closure->desc;
                        if (idesc) {
                            uint32_t lo = idesc->codeOffset;
                            uint32_t hi = inner.ip + 8;
                            std::fprintf(stderr,
                                "  INNER frame[%zu] disasm [%u..%u) (prologue→ip):\n",
                                lim - 1, lo, hi);
                            disassembleWindow(stderr, *inner.cu, lo, hi);
                        }
                    }
                    // Also dump frame[33] — caller of innermost.  Often
                    // the App's `left` was a closure call return, which
                    // is the actual divergence source.
                    if (lim >= 2) {
                        const auto & f33 = vm.frames[lim - 2];
                        if (f33.cu) {
                            const LambdaDescriptor * d33 = nullptr;
                            if (f33.thunk)
                                d33 = f33.thunk->suspended.desc;
                            else if (f33.closure)
                                d33 = f33.closure->desc;
                            if (d33) {
                                uint32_t lo = d33->codeOffset;
                                uint32_t hi = f33.ip + 8;
                                std::fprintf(stderr,
                                    "  CALLER frame[%zu] disasm [%u..%u) (prologue→ip):\n",
                                    lim - 2, lo, hi);
                                disassembleWindow(stderr, *f33.cu, lo, hi);
                            }
                        }
                    }
                }
            }
            // #457/#458: before throwing, consult the partial-Bindings
            // registry.  If this Black thunk's body has run
            // OP_ATTRS_REC_INIT and registered a partial Bindings,
            // return that as the resolved value.  This lets self-
            // referential `with self;` and similar mid-construction
            // attribute access work without forcing the wrapping
            // thunk to completion (which is exactly what TW does via
            // its lazy attr access on partial Bindings).
            //
            // STG-3 (#547): partialBindings recovery is disabled by
            // default.  Real cycles must throw under STG semantics so
            // the consumer sees the typed exception, not a wrong
            // sub-attrset.  NIX_V3_NO_STG=1 restores the legacy
            // recovery path.
            //
            // Disable via NIX_V3_NO_PARTIAL_BINDINGS_RECOVER=1 if the
            // legacy path is restored and misclassifies a real cycle.
            {
                static const bool s_stgMode_recover =
                    std::getenv("NIX_V3_NO_STG") == nullptr;
                static const bool s_disabled =
                    std::getenv("NIX_V3_NO_PARTIAL_BINDINGS_RECOVER") != nullptr;
                static const bool s_dbg_reg =
                    std::getenv("NIX_V3_DBG_PARTIAL_BINDINGS") != nullptr;
                if (!s_disabled && !s_stgMode_recover) {
                    auto & reg = partialBindingsRegistry();
                    auto it = reg.find(t);
                    if (it != reg.end() && !it->second.empty()) {
                        // Use the latest (back) entry for recovery.
                        // This is the legacy non-STG path; the chain
                        // typically has only one entry under non-STG
                        // mode (single-element replacement).
                        if (s_dbg_reg) std::fprintf(stderr,
                            "v3 partialBindings: RECOVER thunk=%p bindings=%p\n",
                            (void *)t, (void *)it->second.back());
                        Value out;
                        out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
                        out.payload.bindings = it->second.back();
                        return out;
                    }
                    if (s_dbg_reg) std::fprintf(stderr,
                        "v3 partialBindings: NO RECOVERY for thunk=%p (registry size=%zu)\n",
                        (void *)t, reg.size());
                }
            }

            // #466 error-as-value (GHC-style mkBlackHole).
            //
            // If the Black thunk is on THIS vm's frames, this is a
            // genuine local cycle (`let x = x; in x` shape) — throw
            // BlackholeError as before so tryEval / consumer error
            // paths see the typed exception.
            //
            // If the Black thunk is on a FOREIGN vm's frames (typical
            // under lambda-skip + bridge primop chains), the thunk is
            // genuinely mid-construction in another VMState; throwing
            // here triggers fallbackToTreeWalker retry chains that
            // re-create fresh VMStates and grow the C-stack
            // unboundedly.  Instead, return the Tag::Blackhole singleton
            // as a propagating sentinel value.  Most consumers (OP_CALL,
            // OP_ATTRS_*, OP_ADD, etc.) fail naturally on Blackhole
            // operands with regular type errors, which propagate
            // cleanly without retry cycles.  Bridge primops convert the
            // Blackhole back to TW's mkBlackHole sentinel so TW's
            // existing infinite-recursion protocol takes over.
            //
            // Mirrors GHC's blackhole-as-value protocol (rts/sm/Evac.c
            // eval_thunk_selector and friends): when forcing a thunk
            // that's already under evaluation by another stack, return
            // a marker rather than blocking or throwing immediately;
            // let the marker propagate through the operation chain
            // until something concrete tries to use it.
            //
            // Default-on; opt out via NIX_V3_NO_BLACKHOLE_AS_VALUE=1
            // for bisecting any regression.
            {
                static const bool s_blackholeAsValue =
                    std::getenv("NIX_V3_NO_BLACKHOLE_AS_VALUE") == nullptr;
                if (s_blackholeAsValue) {
                    bool onMyFrames = false;
                    for (size_t i = 0; i < vm.frames.size(); ++i) {
                        if (vm.frames[i].thunk == t) {
                            onMyFrames = true; break;
                        }
                    }
                    if (!onMyFrames) {
                        static const bool s_dbgBhv =
                            std::getenv("V3_DBG_BLACKHOLE_AS_VALUE") != nullptr;
                        if (s_dbgBhv) {
                            static thread_local uint64_t hits = 0;
                            if (++hits == 1 || (hits & (hits - 1)) == 0)
                                std::fprintf(stderr,
                                    "v3 blackhole-as-value: thunk=%p (vm=%p, "
                                    "frames=%zu) returning vBlackhole "
                                    "(hits=%llu)\n",
                                    (void *)t, (void *)&vm,
                                    vm.frames.size(),
                                    (unsigned long long)hits);
                        }
                        static const bool s_dbgVBHProd2 =
                            std::getenv("V3_DBG_VBH_PROD") != nullptr;
                        if (s_dbgVBHProd2) {
                            const auto * d = (t->state == ThunkState::Suspended
                                              || t->state == ThunkState::Blackhole)
                                ? t->suspended.desc : nullptr;
                            const PosSnapshot * ps =
                                d ? resolvePosSnapshot(d->posHandle) : nullptr;
                            std::fprintf(stderr,
                                "v3 forceValue→vBlackhole(cross-stack): thunk=%p name='%s' pos=%s:%u:%u\n",
                                (void *)t,
                                d && !d->name.empty() ? d->name.c_str() : "<?>",
                                (ps && !ps->file.empty()) ? ps->file.c_str() : "<no-pos>",
                                ps ? ps->line : 0u,
                                ps ? ps->column : 0u);
                        }
                        return Value::vBlackhole;
                    }
                    // #558 (2026-05-10) STG-style "reached WHNF" recovery.
                    //
                    // Gated by NIX_V3_NO_STG_WHNF=1 (bisect kill switch).
                    //
                    // The thunk IS on our own call stack — a real cycle
                    // from forceValue's perspective.  But if the thunk
                    // has reached WHNF (its `OP_ATTRS_REC_INIT_TAIL`
                    // already fired and registered the partial Bindings
                    // shape), we can RETURN that shape as the thunk's
                    // value — the entries are progressively filled in
                    // place via OP_ATTRS_REC_SET, so consumers see the
                    // currently-known fields through the same Bindings*.
                    //
                    // STG analog: a constructor allocation reaches WHNF
                    // even when the constructor's lazy fields are still
                    // unevaluated.  Forcing the thunk again returns the
                    // already-allocated cell.  For Nix attrsets,
                    // OP_ATTRS_REC_INIT_TAIL plays the role of the
                    // constructor allocation; its trailer (sorted name
                    // list) defines the SHAPE.
                    //
                    // Without this, lib.fix's `let x = f x; in x` cycles
                    // when something deep inside f's body re-projects
                    // through x — projections would force x → throws.
                    // With this, the projection sees x's currently-known
                    // shape (the merged // result so far) and proceeds.
                    // #558 Phase 1.5 (2026-05-12) Cell-Update Everywhere:
                    // BEFORE consulting the partial-Bindings registry,
                    // check the thunk's shapeCell.  shapeCell is a
                    // dedicated heap-stable Value* allocated at
                    // MAKE_THUNK time; *shapeCell starts as Tag::Thunk(t)
                    // (sentinel "not updated yet") and gets overwritten
                    // by OP_ATTRS_REC_INIT inside the body.
                    //
                    // If shapeCell has been updated past the sentinel,
                    // return its contents — this is the precise per-
                    // thunk in-progress state, free of the cross-thunk
                    // pollution that the registry-wide peek introduces.
                    //
                    // Gated by NIX_V3_CELL_EVERYWHERE=1.  When validated,
                    // the partial-Bindings registry path (below) can be
                    // retired.
                    static const bool s_cellEverywhere =
                        std::getenv("NIX_V3_CELL_EVERYWHERE") != nullptr;
                    if (__builtin_expect(s_cellEverywhere, 0)
                        && t->shapeCell != nullptr)
                    {
                        Value shapeVal = *t->shapeCell;
                        if (!(shapeVal.tag() == Tag::Thunk
                              && shapeVal.payload.thunk == t)) {
                            static const bool s_dbgCell =
                                std::getenv("V3_DBG_CELL_EVERYWHERE") != nullptr;
                            if (__builtin_expect(s_dbgCell, 0)) {
                                std::fprintf(stderr,
                                    "v3 shapeCell recovery: thunk=%p "
                                    "shapeCell=%p shapeVal.tag=%d\n",
                                    (void *)t, (void *)t->shapeCell,
                                    (int)shapeVal.tag());
                            }
                            return shapeVal;
                        }
                    }
                    static const bool s_noStgWhnf =
                        std::getenv("NIX_V3_NO_STG_WHNF") != nullptr;
                    if (!s_noStgWhnf) {
                        auto & reg = partialBindingsRegistry();
                        auto it = reg.find(t);
                        if (it != reg.end() && !it->second.empty()) {
                            // Use the LATEST chain entry — represents the
                            // most recent layer's contribution to t's
                            // eventual value.  Pointers (not copies), so
                            // subsequent SETs into the chain entry's
                            // bindings are visible.
                            static const bool s_dbgWhnf =
                                std::getenv("V3_DBG_WHNF_RECOVERY") != nullptr;
                            if (s_dbgWhnf) {
                                const auto & st = ir::globalSymbolTable();
                                std::fprintf(stderr,
                                    "v3 STG WHNF recovery: thunk=%p chain-depth=%zu\n",
                                    (void *)t,
                                    it->second.size());
                                for (size_t li = 0; li < it->second.size(); ++li) {
                                    Bindings * b = it->second[li];
                                    std::fprintf(stderr,
                                        "  layer[%zu] bindings=%p size=%u keys=[",
                                        li, (void *)b, b ? b->size : 0);
                                    if (b) {
                                        for (uint32_t i = 0; i < b->size && i < 8; ++i) {
                                            SymbolId nm = b->entries[i].name;
                                            std::fprintf(stderr, "%s%s",
                                                i ? "," : "",
                                                nm < st.size() ? st[nm].c_str() : "?");
                                        }
                                        if (b->size > 8) std::fprintf(stderr, ",...");
                                    }
                                    std::fprintf(stderr, "]\n");
                                }
                            }
                            static const bool s_dbgBhv =
                                std::getenv("V3_DBG_BLACKHOLE_AS_VALUE") != nullptr;
                            if (s_dbgBhv) {
                                static thread_local uint64_t hits = 0;
                                if (++hits == 1 || (hits & (hits - 1)) == 0) {
                                    // #558: enrich with the thunk's name +
                                    // source pos + caller frame.  Tells us
                                    // WHICH thunk is being recovered and
                                    // (via caller) what code path triggered
                                    // the force.
                                    const auto * dT =
                                        (t->state == ThunkState::Suspended
                                         || t->state == ThunkState::Blackhole)
                                        ? t->suspended.desc : nullptr;
                                    const PosSnapshot * psT =
                                        dT ? resolvePosSnapshot(dT->posHandle)
                                           : nullptr;
                                    const LambdaDescriptor * dC = nullptr;
                                    uint32_t cIp = 0;
                                    if (!vm.frames.empty()) {
                                        const auto & cfr = vm.frames.back();
                                        cIp = cfr.ip;
                                        if (cfr.thunk
                                            && (cfr.thunk->state == ThunkState::Suspended
                                                || cfr.thunk->state == ThunkState::Blackhole))
                                            dC = cfr.thunk->suspended.desc;
                                        else if (cfr.closure)
                                            dC = cfr.closure->desc;
                                    }
                                    const PosSnapshot * psC =
                                        dC ? resolvePosSnapshot(dC->posHandle)
                                           : nullptr;
                                    std::fprintf(stderr,
                                        "v3 blackhole-as-WHNF (self-frame): thunk=%p "
                                        "bindings=%p size=%u (hits=%llu)\n",
                                        (void *)t,
                                        (void *)it->second.back(),
                                        (unsigned)it->second.back()->size,
                                        (unsigned long long)hits);
                                    std::fprintf(stderr,
                                        "  thunk-name='%s' thunk-pos=%s:%u:%u\n",
                                        dT && !dT->name.empty() ? dT->name.c_str() : "<?>",
                                        (psT && !psT->file.empty()) ? psT->file.c_str() : "<no-pos>",
                                        psT ? psT->line : 0u,
                                        psT ? psT->column : 0u);
                                    std::fprintf(stderr,
                                        "  caller-frame: name='%s' pos=%s:%u:%u ip=%u\n",
                                        dC && !dC->name.empty() ? dC->name.c_str() : "<?>",
                                        (psC && !psC->file.empty()) ? psC->file.c_str() : "<no-pos>",
                                        psC ? psC->line : 0u,
                                        psC ? psC->column : 0u,
                                        cIp);
                                    // #558: also dump the full caller stack
                                    // so we can see the WHOLE chain that led
                                    // to the force.  hits=1 alone tells us
                                    // the entry point; deeper context tells
                                    // us how we got there.
                                    size_t nFrames = vm.frames.size();
                                    size_t lo = 0;
                                    for (size_t i = nFrames; i-- > lo;) {
                                        const auto & cfr2 = vm.frames[i];
                                        const LambdaDescriptor * dd = nullptr;
                                        if (cfr2.thunk
                                            && (cfr2.thunk->state == ThunkState::Suspended
                                                || cfr2.thunk->state == ThunkState::Blackhole))
                                            dd = cfr2.thunk->suspended.desc;
                                        else if (cfr2.closure)
                                            dd = cfr2.closure->desc;
                                        const PosSnapshot * pps =
                                            dd ? resolvePosSnapshot(dd->posHandle) : nullptr;
                                        std::fprintf(stderr,
                                            "    [%zu] %s ip=%u pos=%s:%u:%u flags=%u\n",
                                            i,
                                            dd && !dd->name.empty() ? dd->name.c_str() : "<?>",
                                            cfr2.ip,
                                            (pps && !pps->file.empty()) ? pps->file.c_str() : "<no-pos>",
                                            pps ? pps->line : 0u,
                                            pps ? pps->column : 0u,
                                            (unsigned)cfr2.flags);
                                    }
                                }
                            }
                            // #558 (2026-05-10) STG WHNF: return
                            // chain.back() as a single-layer Bindings.
                            // Consumers may need to peek the chain
                            // separately for full layer access.
                            //
                            // #558 (2026-05-11) Taint the calling
                            // frame: this access returned an
                            // APPROXIMATE WHNF.  If the frame is a
                            // THUNK_RETURN, the thunk's body computed
                            // a result derived from this approximation
                            // — that result shouldn't be memoized
                            // (since the chain may grow more-accurate
                            // entries).  See CFF_TAINTED docs.
                            //
                            // Gated by NIX_V3_NO_TAINT=1 for bisecting.
                            static const bool s_noTaint =
                                std::getenv("NIX_V3_NO_TAINT") != nullptr;
                            if (!s_noTaint) {
                                for (size_t i = vm.frames.size(); i > 0; --i) {
                                    auto & fr = vm.frames[i - 1];
                                    if (fr.flags & CFF_THUNK_RETURN) {
                                        fr.flags |= CFF_TAINTED;
                                        static const bool s_dbgTaint =
                                            std::getenv("V3_DBG_TAINT") != nullptr;
                                        if (s_dbgTaint) {
                                            const auto * dd = (fr.thunk
                                                && (fr.thunk->state == ThunkState::Suspended
                                                    || fr.thunk->state == ThunkState::Blackhole))
                                                ? fr.thunk->suspended.desc : nullptr;
                                            const PosSnapshot * pps =
                                                dd ? resolvePosSnapshot(dd->posHandle) : nullptr;
                                            std::fprintf(stderr,
                                                "v3 TAINT: thunk=%p name='%s' pos=%s:%u:%u\n",
                                                (void *)fr.thunk,
                                                dd && !dd->name.empty() ? dd->name.c_str() : "<?>",
                                                (pps && !pps->file.empty()) ? pps->file.c_str() : "?",
                                                pps ? pps->line : 0u, pps ? pps->column : 0u);
                                        }
                                        break;
                                    }
                                }
                            }
                            // #558 (2026-05-11) Largest-layer-wins for
                            // STG WHNF return: pick the most-informative
                            // chain entry (largest size) rather than
                            // chain.back().  Mirrors lookupInPartialChain's
                            // largest-layer-wins.  When forcing a Black
                            // thunk to a single Bindings (e.g. the lhs
                            // of an //), the largest layer is the most
                            // accurate WHNF approximation — typically
                            // the outermost overlay's merged result.
                            //
                            // STG analog: indirection chains prefer the
                            // most-resolved cell at each access.
                            //
                            // Gated by NIX_V3_NO_LARGEST_WHNF=1 (reverts
                            // to chain.back() if needed for bisecting).
                            Value recovered;
                            recovered.tag_payload =
                                static_cast<uint64_t>(Tag::Attrs);
                            recovered.payload.bindings =
                                pickLargestLayer(it->second);
                            return recovered;
                        }
                    }
                    // Local cycle without registered partial Bindings —
                    // fall through to throw.
                }
            }

            // #497 diagnostic: dump frame stack + identify Black thunk
            // when V3_DBG_BLACKHOLE_TRACE=1.  Used to investigate
            // post-#496 BlackholeError shape under broader thunkify.
            static const bool s_dbgBlackholeTrace =
                std::getenv("V3_DBG_BLACKHOLE_TRACE") != nullptr;
            if (__builtin_expect(s_dbgBlackholeTrace, 0)) {
                std::fprintf(stderr,
                    "v3 BLACKHOLE thunk=%p forces=%u (frames=%zu):\n",
                    (void *)t, (unsigned)t->forces, vm.frames.size());
                for (size_t fi = vm.frames.size(); fi > 0; --fi) {
                    const auto & fr = vm.frames[fi - 1];
                    const LambdaDescriptor * d = nullptr;
                    if (fr.thunk && fr.thunk->state == ThunkState::Blackhole)
                        d = fr.thunk->suspended.desc;
                    else if (fr.closure)
                        d = fr.closure->desc;
                    std::fprintf(stderr,
                        "  [%zu] %s ip=%u thunk=%p closure=%p flags=%u%s\n",
                        fi - 1,
                        d && !d->name.empty() ? d->name.c_str() : "<?>",
                        fr.ip,
                        (void *)fr.thunk, (void *)fr.closure,
                        (unsigned)fr.flags,
                        fr.thunk == t ? " <-- TARGET" : "");
                }
                std::fflush(stderr);
            }
            throw BlackholeError("v3 forceValue: infinite recursion (blackhole)");
        }
        if (t->state == ThunkState::Bridge) {
            // #466 active-v3-vm tracking: forceBridgeThunk goes
            // through TW (treeWalkerToV3 → ns->forceValue), so any v3
            // hook re-entered from that TW work sees this vm as the
            // active outer.  Lets v3CallFunctionEntry refuse cycle-
            // prone re-entries (lambda-skip's body_fid).
            ScopedActiveV3VM _activeV3VM(&vm);
            v = forceBridgeThunk(t);
            // Self-Bridge guard (#520): see OP_FORCE Bridge handler
            // above.  Returns Tag::Thunk{t} (cache hit on same
            // nix::Value*) when bridgeSrc is an nFunction; setting
            // t->evaluated = Tag::Thunk{t} would make the chase loop
            // (state=Evaluated → evaluated → self) infinite.  Leave
            // state == Bridge and let the break below catch us as a
            // Bridge-wrapping-Function WHNF.
            if (v.tag() == Tag::Thunk && v.payload.thunk == t) {
                break;
            }
            t->state = ThunkState::Evaluated;
            t->evaluated = v;
            // STG-14b option (a): cell update protocol on Bridge
            // thunks (mirror of OP_FORCE Bridge handler above).
            // When the Bridge was built with a cell pointing at a
            // Bindings entry slot, this write propagates the resolved
            // TW value into all observers of that entry.
            if (Value * cell = t->cell) {
                *cell = v;
                t->cell = nullptr;
            }
            // #456 fix: treat a TW-Function-bridged Thunk as WHNF.
            // forceBridgeThunk -> treeWalkerToV3 wraps an nFunction
            // TW Value as ANOTHER Bridge thunk (Tag::Thunk in Bridge
            // state) for round-trip identity preservation
            // (primops.cc treeWalkerToV3 nFunction case).  Without
            // this break, the chase loop forces the new Bridge,
            // forceBridgeThunk allocates ANOTHER Bridge for the
            // same TW Value, set t->evaluated = newer Bridge, repeat
            // ad infinitum.  Each iteration allocates a fresh Thunk
            // pointer; the chain extends forever; kMaxIndirectionChase
            // limit fires.  V3_DBG_CHASE confirms the pattern: every
            // step is a Thunk in state=Evaluated whose evaluated.tag
            // is Thunk again, ending at the freshly-allocated state=
            // Bridge thunk (the next round's seed).
            //
            // The Bridge-thunk-wrapping-Function IS canonical WHNF
            // from v3's perspective: there's nothing to reduce.  The
            // consumer (TW caller via v3ToTreeWalker) will unwrap
            // the Bridge to recover the original TW lambda for a
            // call.  Break out so this Bridge thunk IS the result.
            if (v.tag() == Tag::Thunk && v.payload.thunk
                && v.payload.thunk->state == ThunkState::Bridge)
                break;
            continue;
        }

        const LambdaDescriptor * desc = t->suspended.desc;
        Closure * fakeClo = Alloc::allocClosure(t->nUpvalues);
        fakeClo->desc = desc;
        fakeClo->nUpvalues = t->nUpvalues;
        fakeClo->capturedWiths = t->suspended.capturedWiths;
        fakeClo->cu = t->suspended.cu;
        for (uint16_t i = 0; i < t->nUpvalues; ++i) fakeClo->upvalues[i] = t->tail[i];
        ListVec * thunkWiths = t->suspended.capturedWiths;
        const CompilationUnit * thunkCu = t->suspended.cu
            ? t->suspended.cu
            : vm.frames.back().cu;
        t->state = ThunkState::Blackhole;

        size_t exitDepth = vm.frames.size();
        size_t newBase = vm.valueStack.size();
        vm.valueStack.resize(newBase + desc->nLocals);
        uint32_t newWithBase = static_cast<uint32_t>(vm.withStack.size());

        {
            static const bool s_dbg_fv =
                std::getenv("V3_DBG_STORE_PREVSTAGE") != nullptr;
            if (s_dbg_fv && t->nUpvalues == 5) {
                std::fprintf(stderr,
                    "v3 forceValue: pushing thunk %p desc=%s codeOffset=%u nUp=%u cu=%p\n",
                    (void*)t,
                    !desc->name.empty() ? desc->name.c_str() : "<anon>",
                    desc->codeOffset, (unsigned)t->nUpvalues,
                    (void*)thunkCu);
            }
        }

        vm.frames.push_back(CallFrame{
            .cu = thunkCu,
            .closure = fakeClo,
            .thunk = t,
            .ip = desc->codeOffset,
            .stackBaseOffset = static_cast<uint32_t>(newBase),
            .withStackBase = newWithBase,
            .flags = CFF_THUNK_RETURN,
        });
        pushCapturedWiths(vm, thunkWiths);

        // WC-5: if dispatchLoop throws, every thunk frame we'd unwind
        // is currently marked Blackhole.  Without cleanup, a later
        // force of the same thunk (e.g. when tree-walker takes over
        // and accesses the same lib attr) would hit the stale mark
        // and report "infinite recursion (blackhole)" — masking the
        // real error.  Tree-walker's mkFailed stores the exception
        // and re-throws on subsequent forces (eval-inline.hh:125);
        // the bare-minimum equivalent here is to revert each frame's
        // thunk back to Suspended so the next force re-runs.
        //
        // We don't store the exception (would need a Failed state
        // and re-throw machinery), so the next force simply re-runs
        // the body — slow but correct, and idempotent throws will
        // re-throw the same error consistently.
        try {
            v = dispatchLoop(vm, exitDepth);
        } catch (...) {
            // WC-37: clear blackmarks AND unwind the leftover frames
            // so they can't become "ghost frames" picked up by a later
            // OP_RETURN in an outer dispatchLoop (which would corrupt
            // the thunk pointed-to by the ghost frame).
            //
            // #557 hardening: wrap clearBlackMarksOnException in its
            // own try/catch.  If it ever throws during the outer
            // exception handling (e.g. partialBindingsRegistry hash
            // operation, or accessing a freed thunk pointer), the
            // C++ runtime would call terminate() — surfacing as the
            // brk #0x1 / EXC_BREAKPOINT trap observed when the
            // libsForQt5 cycle bypass diverges into a cascading
            // exception loop.  Swallowing here lets the original
            // exception propagate normally.
            try {
                clearBlackMarksOnException(vm, exitDepth);
            } catch (...) { /* swallow secondary throws during cleanup */ }
            // Also clear the outer Black mark we set just above.
            if (t->state == ThunkState::Blackhole)
                t->state = ThunkState::Suspended;
            throw;
        }
        // WC-14.5 success-path defensive cleanup: if the outer thunk
        // somehow remains Black after a successful dispatchLoop
        // (theoretical impossibility per the invariant, but observed
        // in cross-VMState bridge scenarios where another VMState's
        // frames interleave with this one), revert it to Suspended
        // so subsequent forces re-run idempotently rather than
        // throwing "infinite recursion (blackhole)" on a stale mark.
        if (t->state == ThunkState::Blackhole) {
            static const bool s_dbg = std::getenv("V3_DBG_BLACK") != nullptr;
            if (s_dbg) std::fprintf(stderr,
                "v3 forceValue: SUCCESS-path Black leak; reverting "
                "thunk=%p Suspended\n", (void*)t);
            t->state = ThunkState::Suspended;
        }
    }
    // SECD-style slot memoization: write the resolved value back into
    // the slot we entered through.  Future Tag::Slot derefs through
    // the same slot will see the resolved value directly.  Mirrors
    // tree-walker's `state.forceValue(*v2)` which mutates the slot
    // in-place; sub-thunks observing the slot see the mutation.
    // Important: only write back if v is a concrete WHNF value (not
    // another Slot/Thunk/App that we somehow exited the loop with —
    // shouldn't happen, but be safe).
    if (memoSlot && v.tag() != Tag::Slot)
        *memoSlot = v;
    return v;
}

Value callClosure(VMState & vm, Value fun, Value arg)
{
    // Mirror tree-walker's `callFunction`: callable values must be in
    // WHNF before we dispatch on shape.  Most callers force first
    // (OP_CALL's preceding OP_FORCE; OP_RETURN's transitive chase),
    // but a few internal paths (the __functor recursion below; primop
    // map-style App entries forced inline) leave a Tag::Thunk or
    // Tag::App on `fun`.  forceValue is idempotent on already-WHNF
    // values, so the cost is one tag check on the hot path.
    fun = forceValue(vm, fun);
    // V3_DBG_CALL_CLOSURE=1 prints every call: closure name + arg
    // shape.  Used to trace the broader-thunkify upvalue bug.
    static const bool s_dbgCallClosure =
        std::getenv("V3_DBG_CALL_CLOSURE") != nullptr;
    if (s_dbgCallClosure) {
        const char * nm = "<?>";
        if (fun.tag() == Tag::Closure && fun.payload.closure
            && fun.payload.closure->desc)
            nm = fun.payload.closure->desc->name.c_str();
        int arg_tag = (int)arg.tag();
        int arg_size = -1;
        if (arg.tag() == Tag::Attrs && arg.payload.bindings)
            arg_size = arg.payload.bindings->size;
        std::fprintf(stderr,
            "v3 callClosure: fun.tag=%d name=%s arg.tag=%d size=%d\n",
            (int)fun.tag(), nm, arg_tag, arg_size);
    }
    // PrimOp / PrimOpApp: build a partial application or invoke once
    // we have all the args.  Mirrors the OP_CALL primop branch.
    if (fun.isPrimOp() || fun.tag() == Tag::PrimOpApp) {
        Value cur = fun;
        size_t depth = 0;
        while (cur.tag() == Tag::PrimOpApp) { ++depth; cur = cur.payload.pair->left; }
        if (!cur.isPrimOp())
            throw std::runtime_error("v3 callClosure: PrimOpApp chain doesn't terminate in a PrimOp");
        const PrimOp * po = cur.payload.primop;
        size_t totalArgs = depth + 1;
        if (totalArgs < po->arity) {
            ValuePair * vp = Alloc::allocPair();
            vp->left = fun;
            vp->right = arg;
            Value v;
            v.tag_payload = static_cast<uint64_t>(Tag::PrimOpApp);
            v.payload.pair = vp;
            return v;
        }
        if (totalArgs > po->arity)
            throw std::runtime_error("v3 callClosure: too many args for primop");
        Value buf[8];
        if (po->arity > 8) throw std::runtime_error("v3 callClosure: primop arity > 8");
        buf[totalArgs - 1] = arg;
        Value chain = fun;
        for (size_t i = totalArgs - 1; i > 0; --i) {
            buf[i - 1] = chain.payload.pair->right;
            chain = chain.payload.pair->left;
        }
        for (uint32_t i = 0; i < po->arity; ++i) {
            if (po->lazyArgs & (1u << i)) continue;
            buf[i] = forceValue(vm, buf[i]);
        }
        EvalState state; state.vm = &vm; state.nixEvalState = getNixEvalState();
        Value out;
        po->fn(state, buf, out);
        return out;
    }

    // Attrset with __functor: apply functor self arg.
    if (fun.isAttrs() && fun.payload.bindings) {
        static const SymbolId functorId = ir::globalInternSymbol("__functor");
        if (auto * fn = fun.payload.bindings->lookup(functorId)) {
            Value forced = forceValue(vm, *fn);
            Value firstStep = callClosure(vm, forced, fun);
            return callClosure(vm, firstStep, arg);
        }
    }

    // #483 part 3: Bridge thunk callee.  Per the #456 fix, forceValue
    // can RETURN a Tag::Thunk in Bridge state (when forceBridgeThunk's
    // result is itself a Bridge thunk wrapping a TW Function/List for
    // round-trip identity preservation).  Without handling here,
    // callClosure throws "not callable" on a perfectly valid bridged
    // TW function.  Mirror OP_CALL's Bridge branch: call TW's
    // callFunction with the original TW value, then bridge the result
    // back to v3.  Surfaces under lambda-skip when v3 invokes a TW
    // function passed in as an arg (e.g. via the imap1+dfold pattern
    // in pkgs/stdenv/booter.nix).
    if (fun.isThunk() && fun.payload.thunk
        && fun.payload.thunk->state == ThunkState::Bridge
        && fun.payload.thunk->bridgeSrc) {
        if (auto * ns = getNixEvalState()) {
            auto * funTw = static_cast<nix::Value *>(
                fun.payload.thunk->bridgeSrc);
            ns->forceValue(*funTw, nix::noPos);
            // Try the bridge1 shortcut to keep work on this vm.
            Value v3Fn;
            static const bool s_disabled =
                std::getenv("NIX_V3_NO_OP_CALL_BRIDGE_SHORTCUT") != nullptr;
            if (!s_disabled && tryUnwrapBridge1Closure(*funTw, v3Fn))
                return callClosure(vm, v3Fn, arg);
            // STG-14a (#509/#515): mirror the OP_CALL Bridge handler's
            // direct-v3-dispatch shortcut here so callClosure's recursive
            // path (e.g. native ExtendsBody/ComposeBody calling f as
            // Bridge) also bypasses v3ToTreeWalkerPublic.
            static const bool s_twLambdaShortcutDisabled =
                std::getenv("NIX_V3_NO_TW_LAMBDA_INV3") != nullptr;
            if (!s_twLambdaShortcutDisabled) {
                Value v3Out;
                if (tryDispatchTWLambdaInV3(*ns, *funTw, arg, v3Out))
                    return v3Out;
            }
            nix::Value * argTw = v3ToTreeWalkerPublic(*ns, arg);
            if (!argTw)
                throw std::runtime_error(
                    "v3 callClosure: bridge-thunk arg failed v3->TW bridge");
            // #484 STG-style address identity: heap-allocate outTw
            // (see OP_CALL Bridge handler comment).  Preserves TW's
            // in-place thunk update across the bridge.
            nix::Value * outTwHeap = ns->allocValue();
            ns->callFunction(*funTw, *argTw, *outTwHeap, nix::noPos);
            if (outTwHeap->type<true>() == nix::nThunk) {
                Thunk * bridge = Alloc::allocBridgeThunk(
                    static_cast<void *>(outTwHeap));
                allocStats().thunksAllocated++;
                Value v3out;
                v3out.tag_payload = static_cast<uint64_t>(Tag::Thunk);
                v3out.payload.thunk = bridge;
                return v3out;
            }
            bool prev = pushShallowTWAttrsBridge();
            try {
                Value r = treeWalkerToV3Public(*ns, *outTwHeap);
                popShallowTWAttrsBridge(prev);
                return r;
            } catch (...) {
                popShallowTWAttrsBridge(prev);
                throw;
            }
        }
    }

    if (!fun.isClosure()) {
        static const bool dbg = std::getenv("V3_DBG_CALL") != nullptr;
        if (dbg) {
            std::fprintf(stderr,
                "v3 callClosure: not callable tag=%u frames=%zu\n",
                (unsigned)fun.tag(), vm.frames.size());
            size_t lim = vm.frames.size();
            for (size_t i = lim; i > 0 && i + 8 > lim; --i) {
                const auto & fr = vm.frames[i - 1];
                const LambdaDescriptor * desc = nullptr;
                if (fr.thunk)
                    desc = fr.thunk->suspended.desc;
                else if (fr.closure)
                    desc = fr.closure->desc;
                std::fprintf(stderr,
                    "  frame[%zu]: %s code=[%u..) ip=%u flags=%u\n",
                    i - 1,
                    desc && !desc->name.empty() ? desc->name.c_str()
                        : (desc ? "<anon>" : "<closure-body>"),
                    desc ? desc->codeOffset : 0,
                    fr.ip, (unsigned)fr.flags);
            }
        }
        throw std::runtime_error("v3 callClosure: not callable");
    }

    const Closure * callee = fun.payload.closure;
    const LambdaDescriptor * desc = callee->desc;

    // #495: native fix-point intrinsic -- mirrored from OP_CALL.
    // callClosure is the entry point primops + bridges use; the
    // intrinsic check must fire here too or recognised lambdas
    // dispatched via this path silently take the bytecode body.
    static const bool s_intrinsicEnable =
        std::getenv("NIX_V3_INTRINSIC_DISPATCH") != nullptr;
    if (s_intrinsicEnable && __builtin_expect(
            desc->intrinsicKind != LambdaDescriptor::Intrinsic::None, 0)) {
        if (desc->intrinsicKind == LambdaDescriptor::Intrinsic::Fix) {
            // Refuse native dispatch when the user's `f` is a Bridge
            // thunk -- TW lambdas can't handle v3 Tag::Slot.  Fall
            // through to bytecode which knows the bridge dance.
            bool argIsBridge = arg.tag() == Tag::Thunk
                && arg.payload.thunk
                && arg.payload.thunk->state == ThunkState::Bridge;
            if (!argIsBridge) {
                allocStats().intrinsicFixCalls++;
                static const bool s_dbg =
                    std::getenv("V3_DBG_INTRINSIC") != nullptr;
                if (s_dbg) std::fprintf(stderr,
                    "v3 callClosure intrinsic Fix [#%llu]: arg.tag=%d\n",
                    (unsigned long long)allocStats().intrinsicFixCalls,
                    (int)arg.tag());
                Value * slotStorage = Alloc::allocValue();
                slotStorage->tag_payload =
                    static_cast<uint64_t>(Tag::Uninitialized);
                Value slotV;
                slotV.tag_payload = static_cast<uint64_t>(Tag::Slot);
                slotV.payload.slot = slotStorage;
                Value res = callClosure(vm, arg, slotV);
                *slotStorage = res;
                return res;
            }
        }

        // STG-13c (#509/#512): native dispatch for ExtendsBody --
        // mirrored from OP_CALL.  callClosure is the entry point for
        // primops + bridges, so the intrinsic check must fire here too
        // or recognised lambdas dispatched via this path silently take
        // the bytecode body.
        if (desc->intrinsicKind == LambdaDescriptor::Intrinsic::ExtendsBody
            && desc->intrinsicVar0 >= 0 && desc->intrinsicVar1 >= 0
            && (uint16_t)desc->intrinsicVar0 < callee->nUpvalues
            && (uint16_t)desc->intrinsicVar1 < callee->nUpvalues) {
            allocStats().intrinsicExtendsCalls++;
            Value overlay = callee->upvalues[(uint16_t)desc->intrinsicVar0];
            Value f       = callee->upvalues[(uint16_t)desc->intrinsicVar1];
            Value final_  = arg;
            Value prev = callClosure(vm, f, final_);
            prev = forceValue(vm, prev);
            if (!prev.isAttrs() || !prev.payload.bindings)
                throw std::runtime_error(
                    "v3 callClosure intrinsic ExtendsBody: prev not attrs");
            Value overlay_partial = callClosure(vm, overlay, final_);
            Value overlay_result  = callClosure(vm, overlay_partial, prev);
            overlay_result = forceValue(vm, overlay_result);
            if (!overlay_result.isAttrs() || !overlay_result.payload.bindings)
                throw std::runtime_error(
                    "v3 callClosure intrinsic ExtendsBody: overlay-result not attrs");
            Bindings * merged = mergeBindings(prev.payload.bindings,
                                               overlay_result.payload.bindings);
            Value res;
            res.tag_payload = static_cast<uint64_t>(Tag::Attrs);
            res.payload.bindings = merged;
            return res;
        }

        // STG-13c (#509/#512): native dispatch for ComposeBody.
        if (desc->intrinsicKind == LambdaDescriptor::Intrinsic::ComposeBody
            && desc->intrinsicVar0 >= 0 && desc->intrinsicVar1 >= 0
            && desc->intrinsicVar2 >= 0
            && (uint16_t)desc->intrinsicVar0 < callee->nUpvalues
            && (uint16_t)desc->intrinsicVar1 < callee->nUpvalues
            && (uint16_t)desc->intrinsicVar2 < callee->nUpvalues) {
            allocStats().intrinsicComposeCalls++;
            Value f       = callee->upvalues[(uint16_t)desc->intrinsicVar0];
            Value g       = callee->upvalues[(uint16_t)desc->intrinsicVar1];
            Value final_  = callee->upvalues[(uint16_t)desc->intrinsicVar2];
            Value prev_   = arg;
            Value f_partial = callClosure(vm, f, final_);
            Value fApplied  = callClosure(vm, f_partial, prev_);
            fApplied = forceValue(vm, fApplied);
            if (!fApplied.isAttrs() || !fApplied.payload.bindings)
                throw std::runtime_error(
                    "v3 callClosure intrinsic ComposeBody: fApplied not attrs");
            Value prevForced = forceValue(vm, prev_);
            if (!prevForced.isAttrs() || !prevForced.payload.bindings)
                throw std::runtime_error(
                    "v3 callClosure intrinsic ComposeBody: prev not attrs");
            Bindings * prevPrimeB = mergeBindings(prevForced.payload.bindings,
                                                   fApplied.payload.bindings);
            Value prevPrime;
            prevPrime.tag_payload = static_cast<uint64_t>(Tag::Attrs);
            prevPrime.payload.bindings = prevPrimeB;
            Value g_partial = callClosure(vm, g, final_);
            Value gApplied  = callClosure(vm, g_partial, prevPrime);
            gApplied = forceValue(vm, gApplied);
            if (!gApplied.isAttrs() || !gApplied.payload.bindings)
                throw std::runtime_error(
                    "v3 callClosure intrinsic ComposeBody: gApplied not attrs");
            Bindings * merged = mergeBindings(fApplied.payload.bindings,
                                               gApplied.payload.bindings);
            Value res;
            res.tag_payload = static_cast<uint64_t>(Tag::Attrs);
            res.payload.bindings = merged;
            return res;
        }
    }

    // #424: selector-lambda fast path -- mirrored from OP_CALL.
    // callClosure is the entry point primops use for callback lambdas
    // (map, filter, foldl', etc.), so this fires on the dominant
    // `(p: p.name)`-style nixpkgs callbacks.
    if (__builtin_expect(desc->selectorSym != 0, 0)) {
        allocStats().selectorLambdaCalls++;
        Value sArg = arg;
        if (sArg.isThunk() || sArg.tag() == Tag::App
            || sArg.tag() == Tag::Slot) {
            sArg = forceValue(vm, sArg);
        }
        if (!sArg.isAttrs() || !sArg.payload.bindings)
            throw std::runtime_error(
                "v3 selector lambda: arg not an attrset");
        const Value * v = sArg.payload.bindings->lookup(desc->selectorSym);
        if (!v)
            throw std::runtime_error(
                "v3 selector lambda: missing attr");
        return *v;
    }

    // Cross-CU calls (e.g., calling a closure returned from
    // builtins.import): use the closure's own CU when available.
    const CompilationUnit * cu = callee->cu ? callee->cu : vm.frames.back().cu;

    // Push a CALL frame for the callee — mirrors OP_CALL.
    size_t exitDepth = vm.frames.size();
    size_t newBase = vm.valueStack.size();
    vm.valueStack.resize(newBase + desc->nLocals);
    vm.valueStack[newBase + 0] = arg;

    // #498 frame-entry diagnostic for callClosure path.
    {
        static const char * s_filter =
            std::getenv("V3_DBG_FRAME_ENTRY");
        if (s_filter && desc && desc->name == s_filter) {
            Value chase = arg;
            int hops = 0;
            while (hops < 4) {
                if (chase.tag() == Tag::Slot && chase.payload.slot)
                    chase = *chase.payload.slot;
                else if (chase.tag() == Tag::Thunk && chase.payload.thunk
                         && chase.payload.thunk->state == ThunkState::Evaluated)
                    chase = chase.payload.thunk->evaluated;
                else break;
                ++hops;
            }
            std::fprintf(stderr,
                "v3 FRAME_ENTRY callClosure %s codeOff=%u: local[0].tag=%d",
                desc->name.c_str(), (unsigned)desc->codeOffset,
                (int)arg.tag());
            if (chase.tag() == Tag::Attrs && chase.payload.bindings) {
                auto * b = chase.payload.bindings;
                std::fprintf(stderr, " -> attrs size=%u {", b->size);
                const auto & tbl = ir::globalSymbolTable();
                for (uint32_t i = 0; i < b->size && i < 4; ++i) {
                    uint32_t nm = b->entries[i].name;
                    std::fprintf(stderr, "%s%s", i ? "," : "",
                        nm < tbl.size() ? tbl[nm].c_str() : "?");
                }
                if (b->size > 4) std::fprintf(stderr, ",...");
                std::fprintf(stderr, "}");
            } else {
                std::fprintf(stderr, " -> tag=%d", (int)chase.tag());
            }
            std::fprintf(stderr, " (frames=%zu)\n", vm.frames.size());
        }
    }

    uint32_t newWithBase = static_cast<uint32_t>(vm.withStack.size());

    vm.frames.push_back(CallFrame{
        .cu = cu,
        .closure = callee,
        .thunk = nullptr,
        .ip = desc->codeOffset,
        .stackBaseOffset = static_cast<uint32_t>(newBase),
        .withStackBase = newWithBase,
        .flags = 0,
    });
    pushCapturedWiths(vm, callee->capturedWiths);

    try {
        return dispatchLoop(vm, exitDepth);
    } catch (...) {
        clearBlackMarksOnException(vm, exitDepth);
        throw;
    }
}

} // namespace nix::v3
