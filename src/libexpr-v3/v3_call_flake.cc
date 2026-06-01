/// @file
/// V3-native callFlake — load + compile + run `call-flake.nix` on v3's
/// VM instead of delegating to TW's evaluator.
///
/// See `lode/V3_NATIVE_CALL_FLAKE_DESIGN_2026-05-20.md` for the
/// architectural rationale.  TL;DR: post-#697 the bridge-side perf
/// gap is closed, but TW still EVALUATES call-flake.nix when v3
/// calls `builtins.getFlake`.  That's a V3-NATIVE violation: pure
/// Nix code (no FFI inside call-flake.nix) should run on v3's VM.
///
/// **Phase 2 (this file)**: load call-flake.nix from the canonical
/// libflake source (via the shared generated header), parse it with
/// TW's parser (parsing IS TW's responsibility per V3-NATIVE — v3
/// only owns lower → bytecode → run), lower into v3 IR, optimise,
/// compile to a CompilationUnit, run to obtain the top-level
/// 3-arg lambda closure.  The CU is held alive via a static.
///
/// `callFlakeV3` itself (the integration point) is Phase 3: it will
/// build TW args via libflake helpers, bridge to v3, and apply via
/// callClosure × 3.  For now `callFlakeV3` invokes the cache to
/// verify the compilation pipeline works end-to-end, then throws a
/// PHASE-3 marker.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
/// Input Output Group.  SPDX-License-Identifier: Apache-2.0

#include "v3/primop.hh"
#include "v3/closure.hh"
#include "v3/lower.hh"
#include "v3/ir.hh"
#include "v3/vm.hh"
#include "v3/value.hh"
#include "v3/alloc.hh"  // #700/2a: v3-native Bindings allocation
#include "v3/barrier.hh"  // Phase D write-barrier helpers
// PARSER_PROJECT_PLAN §5.3 site 4: native parse+lower of call-flake.nix.
#include "v3-parse-api.hh"   // nix::v3::parser::parseString
#include "lower_v3.hh"       // canLowerV3 + lowerV3Ast
#include "v3-to-nixexpr.hh"  // toNixExpr (bridge fallback) + twBaseEnvGlobals
#include "nix/util/users.hh" // getHome()

#include "nix/expr/eval.hh"
#include "nix/expr/value/context.hh"      // NixStringContextElem::Opaque (for v3EmitTreeAttrs)
#include "nix/fetchers/fetchers.hh"       // fetchers::Input getters
#include "nix/fetchers/attrs.hh"          // maybeGetStrAttr / maybeGetBoolAttr
#include "nix/flake/flake.hh"
#include "nix/flake/lockfile.hh"   // for flake::LockedNode (dynamic_pointer_cast target)
#include "nix/store/store-api.hh"  // for Store::toStorePath
#include "nix/util/canon-path.hh"  // for CanonPath::rel
#include "nix/util/hash.hh"        // Hash, HashAlgorithm, HashFormat (#701 v3EmitTreeAttrs)

#include <algorithm>
#include <cstring>
#include <ctime>                   // std::gmtime / std::strftime (#701 lastModifiedDate)
#include <sys/resource.h>
#if defined(__APPLE__)
# include <mach/mach.h>
# include <mach/task.h>
#endif
#include <chrono>
#include <deque>
#include <mutex>
#include <optional>

namespace nix::v3 {

namespace {

/// #698 Phase 3: thread-local pointer to libcmd's nix::flakeSettings.
/// Wired by CLI startup via setFlakeSettings.  Lifetime is the
/// process — flakeSettings is a global in common-eval-args.cc.
thread_local const nix::flake::Settings * tlFlakeSettings = nullptr;

} // (close inner anon ns; re-open below)

void setFlakeSettings(const nix::flake::Settings * s) { tlFlakeSettings = s; }
const nix::flake::Settings * getFlakeSettings() { return tlFlakeSettings; }

namespace {

/// The canonical call-flake.nix source, generated from
/// src/libflake/call-flake.nix at build time (see meson.build).
constexpr const char * callFlakeSource =
#include "call-flake.nix.gen.hh"
    ;

/// Lazily-initialised v3-compiled call-flake.nix closure.
///
/// First-call cost: parse + lower + compile (~tens of ms for this
/// 105-line file).  Subsequent calls: O(1) — return the cached
/// Closure Value.
///
/// CU lifetime (per design doc §2.1):
/// `std::deque<CompilationUnit>` — push_back never invalidates
/// prior elements, so closures embedding `c->cu = &back()` stay
/// valid for the program's lifetime.  Same pattern as primImport's
/// importCache.cus.
///
/// We use a LOCAL static deque (not the shared importCache.cus)
/// because: (a) ownership is clearer — clearImportCache shouldn't
/// drop the call-flake CU mid-eval; (b) the deque is reset only on
/// process exit; (c) keeps the symbol table reference dependency
/// localised — the design doc warns against std::unique_ptr that
/// outlives the EvalState, but a static deque has the same lifetime
/// hazard.  TODO Phase 3: reconsider lifetime — pass the cache slot
/// through EvalState if we hit symbol-table-staleness in practice.
///
/// Thread safety: `std::call_once` guarantees atomic initialisation.
/// After init, the Closure Value is read-only.
struct CachedCallFlake {
    std::once_flag flag;
    std::deque<CompilationUnit> cus;  // stable addresses (deque doesn't reallocate)
    Value closureValue;

    /// Build (or fetch cached) the v3-compiled call-flake.nix
    /// closure.  Throws via `std::runtime_error` if any step fails;
    /// callers should propagate to surface v3 language-support gaps
    /// rather than masking with a TW fallback (per design doc §0
    /// Rule 0 falsification criterion).
    Value get(nix::EvalState & ns) {
        std::call_once(flag, [&] {
            // (1) Parse call-flake.nix as a Nix expression via the
            // TW parser.  Per V3-NATIVE, parsing IS TW's
            // responsibility (the .nix-grammar parser lives in TW).
            // v3 owns the post-parse pipeline (lower → bytecode →
            // run).
            //
            // The basePath is synthetic: call-flake.nix is loaded
            // from an in-memory string, so we use the rootPath
            // marker so any relative-path operations inside the
            // expression (there shouldn't be any — call-flake.nix
            // does its own outPath plumbing) point at a clearly-
            // synthetic location.
            // PARSER_PROJECT_PLAN §5.3 site 4: native-parse+lower
            // call-flake.nix when gated (canLowerV3 fallback to the bridge
            // / TW path).  call-flake.nix uses no relative/home paths, so
            // basePath is left empty; positions resolve against a stable
            // heap source for the Pos::String origin.  Cached gate reads.
            static const bool s_nativeParser = std::getenv("NIX_V3_NATIVE_PARSER") != nullptr;
            static const bool s_nativeLower  = std::getenv("NIX_V3_NATIVE_LOWER") != nullptr;
            static const std::string s_homePath = nix::getHome().string();

            nix::Expr * e = nullptr;
            bool useNativeLower = false;
            nix::v3::ast::ParserState v3st;
            std::optional<nix::PosTable::Origin> nativeOrigin;
            if (s_nativeParser) {
                v3st.homePath = s_homePath;
                nix::v3::parser::parseString(v3st, std::string(callFlakeSource));
                auto src = nix::make_ref<std::string>(callFlakeSource);
                auto origin = ns.positions.addOrigin(
                    nix::Pos::String{.source = src}, src->size());
                if (s_nativeLower && nix::v3::canLowerV3(v3st.result)) {
                    useNativeLower = true;
                    nativeOrigin.emplace(origin);
                } else {
                    e = nix::v3::toNixExpr(ns, v3st.result, origin);
                }
            } else {
                e = ns.parseExprFromString(
                    callFlakeSource, ns.rootPath("/«v3-call-flake»"));
            }
            if (e) e->bindVars(ns, ns.staticBaseEnv);

            // (2) Lower into v3 IR.  Same pipeline as primImport.
            auto module = useNativeLower
                ? nix::v3::lowerV3Ast(ns.symbols, v3st.result, &ns.positions,
                                      *nativeOrigin, &nix::v3::twBaseEnvGlobals(ns))
                : lowerNixExpr(e, ns.symbols, ns.positions);
            ir::optimise(module);
            ir::computeFreeVars(module);

            // (3) Compile to bytecode + hold the CU alive.
            cus.push_back(compile(module));

            // (4) Run the top-level expression.  call-flake.nix's
            // top-level form is a 3-arg lambda
            // (`lockFileStr: overrides: fetchTreeFinal: <body>`),
            // so the resulting Value must be a Tag::Closure.
            closureValue = run(cus.back());

            if (closureValue.tag() != Tag::Closure) {
                throw std::runtime_error(
                    "v3::CachedCallFlake::get: call-flake.nix did "
                    "not compile to a closure (tag="
                    + std::to_string(static_cast<int>(closureValue.tag()))
                    + ")");
            }
        });
        return closureValue;
    }
};

/// Process-wide cache.  Single instance per process — the .nix
/// source is fixed at build time, so the compiled closure is
/// reusable across all `builtins.getFlake` calls.
CachedCallFlake g_cachedCallFlake;

} // namespace

// #705 (2026-05-21): expose the cached call-flake closure as a
// scavenger root.  The closureValue may carry a nursery Closure
// payload (if call-flake.nix's bytecode was run while the nursery
// was enabled).  Without walking, a getFlake call after scavenge
// uses a stale closure pointer.
void walkCallFlakeRoot(const std::function<void(Value &)> & visit)
{
    // Only walk if the once-flag has fired.  We can't safely call
    // std::call_once's predicate here, so check the Value's tag
    // — call_once initializes to Tag::Uninitialized (=0) by default
    // and the post-init success path sets Tag::Closure.
    if (g_cachedCallFlake.closureValue.tag() != Tag::Uninitialized)
        visit(g_cachedCallFlake.closureValue);
}

// Forward declaration: defined in primops.cc.  Bridges a TW Value
// (forced to WHNF inside) to a v3 Value (shallow per #662).
extern Value treeWalkerToV3Public(nix::EvalState & nixState, nix::Value & nv);

namespace {

/// #701 Phase 4b: v3-native port of nix::emitTreeAttrs
/// (libexpr/primops/fetchTree.cc:22).  Builds the per-flake-node
/// `sourceInfo` attrset (outPath, narHash, rev/shortRev/revCount,
/// dirtyRev/dirtyShortRev, lastModified/lastModifiedDate, etc.)
/// directly as a v3 Bindings — eliminating the per-node TW bridge
/// the original callFlakeV3 paid via emitTreeAttrs → TW Bindings
/// → treeWalkerToV3Public shallow bridge.
///
/// Why ported (not just bridged): in the per-node loop in
/// callFlakeV3 this runs once per locked flake node.  cardano-node
/// has ~50 nodes; each bridged sourceInfo became 9-13 Bridge thunks
/// that each crossed back into TW the first time their value was
/// forced.  Native construction is ~2× faster per node AND lets
/// downstream v3 code force `sourceInfo.outPath` in v3 dispatch
/// without a bridge round-trip.
///
/// The byte-by-byte parity with TW's emitTreeAttrs is required —
/// these attrs appear in the flake's outputs and any divergence
/// would change downstream drvPaths.
Value v3EmitTreeAttrs(
    nix::EvalState & ns,
    const nix::StorePath & storePath,
    const nix::fetchers::Input & input,
    bool emptyRevFallback,
    bool forceDirty)
{
    // Collect entries in (SymbolId, Value) pairs.  Final Bindings is
    // sorted by SymbolId at the end (binary-search invariant).  The
    // maximum possible entry count is 10
    //   (outPath, narHash, submodules, rev, shortRev, revCount,
    //    dirtyRev, dirtyShortRev, lastModified, lastModifiedDate)
    // so reserve up-front to avoid any rebucket / realloc.
    std::vector<std::pair<SymbolId, Value>> entries;
    entries.reserve(10);

    auto allocStr = [](const std::string & s) -> Value {
        Value v;
        const size_t n = s.size();
        char * buf = Alloc::allocChars(n + 1);
        std::memcpy(buf, s.data(), n);
        buf[n] = '\0';
        v.mkString(buf);
        return v;
    };

    // 1. outPath — store path with Opaque context.  This is the
    // attribute downstream callers depend on for drvPath stability;
    // a missing or wrong-context outPath cascades into wrong drv
    // hashes for everything that imports this flake's sourceInfo.
    {
        const std::string p = ns.store->printStorePath(storePath);
        Value v = allocStr(p);
        nix::NixStringContextElem elem = nix::NixStringContextElem::Opaque{ .path = storePath };
        std::vector<std::string> ctx;
        ctx.push_back(elem.to_string());
        setStringContextEntries(v.payload.str, std::move(ctx));
        entries.emplace_back(ir::globalInternSymbol("outPath"), v);
    }

    // 2. narHash (optional) — SRI-formatted with algo prefix, matches TW.
    if (auto narHash = input.getNarHash()) {
        Value v = allocStr(narHash->to_string(nix::HashFormat::SRI, /*includeAlgo=*/true));
        entries.emplace_back(ir::globalInternSymbol("narHash"), v);
    }

    // 3. submodules — bool, git-only.  TW emits it for type=="git"
    // regardless of the underlying attrs value (defaults to false
    // when the attr is absent).
    if (input.getType() == "git") {
        const bool sub = nix::fetchers::maybeGetBoolAttr(input.attrs, "submodules").value_or(false);
        Value v = sub ? Value::vTrue : Value::vFalse;
        entries.emplace_back(ir::globalInternSymbol("submodules"), v);
    }

    // 4. rev / shortRev / revCount (unless forceDirty).
    if (!forceDirty) {
        if (auto rev = input.getRev()) {
            entries.emplace_back(ir::globalInternSymbol("rev"),      allocStr(rev->gitRev()));
            entries.emplace_back(ir::globalInternSymbol("shortRev"), allocStr(rev->gitShortRev()));
        } else if (emptyRevFallback) {
            // Backwards compat for `builtins.fetchGit`: dirty repos
            // return an empty sha1 as rev.  Matches TW emitTreeAttrs.
            const auto emptyHash = nix::Hash(nix::HashAlgorithm::SHA1);
            entries.emplace_back(ir::globalInternSymbol("rev"),      allocStr(emptyHash.gitRev()));
            entries.emplace_back(ir::globalInternSymbol("shortRev"), allocStr(emptyHash.gitShortRev()));
        }
        Value vRevCount;
        if (auto revCount = input.getRevCount()) {
            vRevCount.mkInt(static_cast<int64_t>(*revCount));
            entries.emplace_back(ir::globalInternSymbol("revCount"), vRevCount);
        } else if (emptyRevFallback) {
            vRevCount.mkInt(0);
            entries.emplace_back(ir::globalInternSymbol("revCount"), vRevCount);
        }
    }

    // 5. dirtyRev / dirtyShortRev (paired; emitted iff dirtyRev is
    // present in input.attrs).  Pre-checked before reading
    // dirtyShortRev because TW does the same — and absence there
    // would be a malformed input.
    if (auto dirtyRev = nix::fetchers::maybeGetStrAttr(input.attrs, "dirtyRev")) {
        entries.emplace_back(ir::globalInternSymbol("dirtyRev"), allocStr(*dirtyRev));
        if (auto dirtyShortRev = nix::fetchers::maybeGetStrAttr(input.attrs, "dirtyShortRev"))
            entries.emplace_back(ir::globalInternSymbol("dirtyShortRev"), allocStr(*dirtyShortRev));
    }

    // 6. lastModified (int seconds-since-epoch) + lastModifiedDate
    // ("%Y%m%d%H%M%S" UTC).  std::gmtime returns a pointer to a
    // statically-allocated tm (thread-unsafe in general, but v3
    // EvalState is currently single-threaded; if/when that changes,
    // switch to gmtime_r).
    if (auto lastModified = input.getLastModified()) {
        Value vLm; vLm.mkInt(static_cast<int64_t>(*lastModified));
        entries.emplace_back(ir::globalInternSymbol("lastModified"), vLm);
        const std::time_t t = static_cast<std::time_t>(*lastModified);
        char dateBuf[24];
        std::strftime(dateBuf, sizeof(dateBuf), "%Y%m%d%H%M%S", std::gmtime(&t));
        entries.emplace_back(ir::globalInternSymbol("lastModifiedDate"), allocStr(std::string(dateBuf)));
    }

    // Sort by SymbolId — Bindings rely on binary-search lookup.
    std::sort(entries.begin(), entries.end(),
        [](const auto & a, const auto & b){ return a.first < b.first; });

    // Allocate + fill the Bindings.  bindingsSetEntry goes through
    // the Phase-D barrier so cell writes into a nursery payload are
    // tracked by the scavenger.
    const uint32_t nEntries = static_cast<uint32_t>(entries.size());
    Bindings * b = Alloc::allocBindings(nEntries);
    V3_STATS_INC(attrsetsAllocated);
    for (uint32_t i = 0; i < nEntries; ++i)
        bindingsSetEntry(b, i, { entries[i].first, /*pos=*/0, entries[i].second });

    Value out;
    out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
    out.payload.bindings = b;
    return out;
}

}  // namespace

/// Public entry point — invoked from `primGetFlake` when the
/// v3-native path is enabled.  Returns the flake's outputs attrset
/// as a v3 Value.
///
/// Mirrors `nix::flake::callFlake` (libflake/flake.cc:928-973) but
/// runs call-flake.nix on v3's VM via the cached closure instead of
/// `state.callFunction(vCallFlake, args, vRes)`.
Value callFlakeV3(EvalState & state, const nix::flake::LockedFlake & lockedFlake)
{
    if (!state.nixEvalState)
        throw std::runtime_error("v3::callFlakeV3: no TW EvalState wired");
    auto & ns = *state.nixEvalState;

    // V3_DBG_CALLFLAKE_TIMING — phase split inside callFlakeV3.
    // Prints ms elapsed per phase to stderr.  Retire when v3-native
    // matches TW on cardano-node (the opt-in gate retirement
    // criterion).
    static const bool s_dbgTiming =
        std::getenv("V3_DBG_CALLFLAKE_TIMING") != nullptr;
    auto t0 = std::chrono::steady_clock::now();
    auto tick = [&](const char * label) {
        if (!s_dbgTiming) return;
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::fprintf(stderr, "v3 callFlakeV3 [%6.1f ms cumulative] %s\n", ms, label);
    };

    // (1) Compile call-flake.nix in v3 (cached after first call).
    Value vCallFlake = g_cachedCallFlake.get(ns);
    tick("cache.get done");

    // (2) Build args V3-NATIVE per the steady-state V3-NATIVE
    //     constraint: pure-data values are v3-allocated, TW touches
    //     only the FFI leaves (emitTreeAttrs for the per-node
    //     sourceInfo, fetchFinalTree primop invocation).
    //
    //     Pre-#700 this was 3 × `treeWalkerToV3Public` on full
    //     TW-built Values, which forced every subsequent
    //     `overrides.${key}.sourceInfo.outPath` etc. select to pay a
    //     v3↔TW round-trip.  Now:
    //       - vLocks: v3 String (Alloc::allocChars).
    //       - vOverrides: outer Bindings is v3-native; per-node
    //         `sourceInfo` is bridged ONCE per node (still pays
    //         emitTreeAttrs's TW construction cost — it's the FFI
    //         leaf for path/hash/timestamp formatting), but
    //         subsequent selects on the OUTER attrset are v3-native.
    //       - vFetchTreeFinal: v3 PrimOp Value wrapping
    //         `__fetchFinalTree` (registered v3-side; its body
    //         bridges into TW's internal primop on invocation —
    //         which is rare with overrides supplied for all nodes).
    auto [lockFileStr, keyMap] = lockedFlake.lockFile.to_string();
    tick("lockFile.to_string done");

    // --- vLocks (v3 String) ---
    Value v3Locks;
    {
        size_t n = lockFileStr.size();
        char * buf = Alloc::allocChars(n + 1);
        std::memcpy(buf, lockFileStr.data(), n);
        buf[n] = '\0';
        v3Locks.mkString(buf);
    }
    tick("vLocks built (v3 String)");

    // --- vOverrides (v3 outer Bindings, sourceInfo bridged once) ---
    Value v3Overrides;
    {
        size_t N = lockedFlake.nodePaths.size();
        Bindings * outer = Alloc::allocBindings(static_cast<uint32_t>(N));
        V3_STATS_INC(attrsetsAllocated);
        // Pre-intern the inner attr keys (used N times each).
        SymbolId sidSourceInfo = ir::globalInternSymbol("sourceInfo");
        SymbolId sidDir        = ir::globalInternSymbol("dir");

        size_t i = 0;
        for (auto & [node, sourcePath] : lockedFlake.nodePaths) {
            auto lockedNode = node.dynamic_pointer_cast<const nix::flake::LockedNode>();
            auto [storePath, subdir] = ns.store->toStorePath(sourcePath.path.abs());

            // #701 Phase 4b LANDED: v3-native sourceInfo construction.
            // Pre-#701 this was `nix::emitTreeAttrs` + `treeWalkerToV3Public`
            // — one bridge per locked flake node (cardano-node has
            // ~50 nodes, so ~50 bridges per callFlakeV3 invocation,
            // each producing 9-13 Bridge thunks that paid a TW
            // round-trip the first time they were forced).
            //
            // The replacement `v3EmitTreeAttrs` builds the same
            // Bindings byte-for-byte (validated by the #759 sweep:
            // 63/64 nixpkgs drvPaths still byte-identical, including
            // cardano-node M5) but allocates v3-native Bindings +
            // v3-native string values — no Bridge thunks, no TW
            // round-trip on per-attr force.
            const auto & inputForNode =
                lockedNode ? lockedNode->lockedRef.input
                           : lockedFlake.flake.lockedRef.input;
            const bool forceDirty = !lockedNode && lockedFlake.flake.forceDirty;
            Value v3SourceInfo = v3EmitTreeAttrs(
                ns, storePath, inputForNode,
                /*emptyRevFallback=*/false, forceDirty);

            // v3 String for `dir`.  CanonPath::rel returns string_view; copy.
            std::string dirRel(nix::CanonPath(subdir).rel());
            Value v3Dir;
            {
                size_t dn = dirRel.size();
                char * dbuf = Alloc::allocChars(dn + 1);
                std::memcpy(dbuf, dirRel.data(), dn);
                dbuf[dn] = '\0';
                v3Dir.mkString(dbuf);
            }

            // Inner Bindings { sourceInfo; dir; } — sorted by SymbolId.
            Bindings * inner = Alloc::allocBindings(2);
            V3_STATS_INC(attrsetsAllocated);
            if (sidSourceInfo < sidDir) {
                bindingsSetEntry(inner, 0, {sidSourceInfo, 0, v3SourceInfo});  // Phase D
                bindingsSetEntry(inner, 1, {sidDir, 0, v3Dir});
            } else {
                bindingsSetEntry(inner, 0, {sidDir, 0, v3Dir});
                bindingsSetEntry(inner, 1, {sidSourceInfo, 0, v3SourceInfo});
            }
            Value v3Inner;
            v3Inner.tag_payload = static_cast<uint64_t>(Tag::Attrs);
            v3Inner.payload.bindings = inner;

            // Outer key — the node-key string from keyMap.
            auto key = keyMap.find(node);
            if (key == keyMap.end())
                throw std::runtime_error(
                    "v3::callFlakeV3: node missing from lockfile keyMap");
            SymbolId sidKey = ir::globalInternSymbol(key->second);

            bindingsSetEntry(outer, i++, {sidKey, 0, v3Inner});  // Phase D
        }
        // Bindings expects entries sorted by SymbolId (binary search).
        std::sort(&outer->entries[0], &outer->entries[outer->size],
            [](const auto & a, const auto & b){ return a.name < b.name; });
        v3Overrides.tag_payload = static_cast<uint64_t>(Tag::Attrs);
        v3Overrides.payload.bindings = outer;
    }
    tick("vOverrides built (v3 outer + bridged sourceInfo per node)");

    // --- vFetchTreeFinal (v3 PrimOp Value) ---
    //
    // Looks up `__fetchFinalTree` in v3's PrimOp registry — Phase 3's
    // accompanying primops.cc change registers this primop (body
    // bridges into TW's internalPrimOps["fetchFinalTree"] when
    // actually invoked).  Stays a v3 Value, doesn't pay a bridge
    // round-trip at lookup time.
    Value v3FetchFinal;
    {
        const PrimOp * po = findPrimOp("__fetchFinalTree");
        if (!po)
            throw std::runtime_error(
                "v3::callFlakeV3: v3 primop `__fetchFinalTree` not "
                "registered — primops.cc registerBuiltinPrimOps "
                "should include it");
        v3FetchFinal.tag_payload = static_cast<uint64_t>(Tag::PrimOp);
        v3FetchFinal.payload.primop = po;
    }
    tick("vFetchTreeFinal built (v3 PrimOp Value)");

    // (4) Apply args via callClosure on the active VMState.  Per
    //     STG-10, reuse the caller's VM — fresh VMState spawning at
    //     TW→v3 boundaries created cross-VM Black-mark issues.
    //
    // Source of vm: the v3 EvalState struct carries a `vm` pointer
    // set by the dispatcher when calling a primop (vm.cc:8858-8859).
    // If that's missing (unusual — primGetFlake should always be
    // invoked from a v3 dispatch loop), fall back to activeV3VM
    // for completeness; if both are null, abort.
    VMState * vm = state.vm ? state.vm : activeV3VM();
    if (!vm)
        throw std::runtime_error(
            "v3::callFlakeV3: no active VMState (state.vm and "
            "activeV3VM both null — must be called inside a v3 "
            "dispatch loop)");

    // #755 instrumentation: log RSS at each callClosure boundary
    // to localize which apply blows up.
    static const bool s_dbgRss =
        std::getenv("V3_DBG_GETFLAKE_RSS") != nullptr;
    auto rssMB = []() -> uint64_t {
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
    if (s_dbgRss)
        std::fprintf(stderr,
            "v3 callFlakeV3: RSS=%llu MB before callClosure(vCallFlake, vLocks)\n",
            (unsigned long long)rssMB());
    Value r1 = callClosure(*vm, vCallFlake, v3Locks);
    if (s_dbgRss)
        std::fprintf(stderr,
            "v3 callFlakeV3: RSS=%llu MB after callClosure #1 (lockFileStr applied)\n",
            (unsigned long long)rssMB());
    Value r2 = callClosure(*vm, r1, v3Overrides);
    if (s_dbgRss)
        std::fprintf(stderr,
            "v3 callFlakeV3: RSS=%llu MB after callClosure #2 (overrides applied)\n",
            (unsigned long long)rssMB());
    Value r3 = callClosure(*vm, r2, v3FetchFinal);
    if (s_dbgRss)
        std::fprintf(stderr,
            "v3 callFlakeV3: RSS=%llu MB after callClosure #3 (fetchTreeFinal applied)\n",
            (unsigned long long)rssMB());
    return r3;
}

/// Phase 2 verification primop — accessible as `builtins.__v3CompileCallFlake null`.
///
/// Triggers the v3-side compile of call-flake.nix and returns:
///   - `"compiled-ok-closure-tag-<N>"` on success (proves the
///     entire parse → lower → optimise → compile → run pipeline
///     produces a closure).
///   - Throws with a descriptive error if compilation fails (v3
///     language-support gap — surface, don't mask).
///
/// This is a TEMPORARY diagnostic primop.  Phase 3 will remove it
/// once `callFlakeV3` is wired into `primGetFlake` and the
/// regression suite gives end-to-end verification.
void primV3CompileCallFlake(EvalState & state, Value * /*args*/, Value & out)
{
    if (!state.nixEvalState)
        throw std::runtime_error(
            "v3 __v3CompileCallFlake: no TW EvalState wired");
    auto & ns = *state.nixEvalState;

    Value closure = g_cachedCallFlake.get(ns);

    // Build a result string describing the closure tag — opaque
    // string, just an "I ran successfully" indicator.
    std::string msg = "compiled-ok-closure-tag-"
                    + std::to_string(static_cast<int>(closure.tag()));

    // Return as a v3 string Value.
    out.tag_payload = static_cast<uint64_t>(Tag::String);
    // We need a stable string — allocate via the v3 string interner
    // or just copy into a static.  For a diagnostic primop, a static
    // is fine: the message is fixed-content.
    static std::string s_msg;
    s_msg = msg;
    out.payload.str = s_msg.c_str();
}

} // namespace nix::v3
