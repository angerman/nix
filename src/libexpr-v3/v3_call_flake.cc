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

#include "nix/expr/eval.hh"
#include "nix/flake/flake.hh"
#include "nix/flake/lockfile.hh"   // for flake::LockedNode (dynamic_pointer_cast target)
#include "nix/store/store-api.hh"  // for Store::toStorePath
#include "nix/util/canon-path.hh"  // for CanonPath::rel

#include <algorithm>
#include <cstring>
#include <chrono>
#include <deque>
#include <mutex>

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
            nix::Expr * e = ns.parseExprFromString(
                callFlakeSource,
                ns.rootPath("/«v3-call-flake»"));
            e->bindVars(ns, ns.staticBaseEnv);

            // (2) Lower into v3 IR.  Same pipeline as primImport
            // (primops.cc:7180-7183).
            auto module = lowerNixExpr(e, ns.symbols, ns.positions);
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

// Forward declaration: defined in primops.cc.  Bridges a TW Value
// (forced to WHNF inside) to a v3 Value (shallow per #662).
extern Value treeWalkerToV3Public(nix::EvalState & nixState, nix::Value & nv);

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
        allocStats().attrsetsAllocated++;
        // Pre-intern the inner attr keys (used N times each).
        SymbolId sidSourceInfo = ir::globalInternSymbol("sourceInfo");
        SymbolId sidDir        = ir::globalInternSymbol("dir");

        size_t i = 0;
        for (auto & [node, sourcePath] : lockedFlake.nodePaths) {
            auto lockedNode = node.dynamic_pointer_cast<const nix::flake::LockedNode>();
            auto [storePath, subdir] = ns.store->toStorePath(sourcePath.path.abs());

            // Build TW sourceInfo via emitTreeAttrs (FFI leaf — knows
            // how to format outPath context, narHash, lastModified
            // etc. from a fetchers::Input).
            //
            // TODO #701 (Phase 4b — deferred): port emitTreeAttrs to v3.
            // Currently this is the ONE residual bridge per node in
            // callFlakeV3; eliminating it requires ~150-200 LoC reading
            // fetchers::Input + StorePath fields and constructing a v3
            // Bindings with proper NixStringContext.  Deferred until a
            // workload appears where the per-node sourceInfo
            // construction dominates.  See
            // lode/V3_NATIVE_CALL_FLAKE_DESIGN_2026-05-20.md §9 and
            // memory `project_701_deferred_emitTreeAttrs_v3.md`.
            nix::Value * twSourceInfo = ns.allocValue();
            nix::emitTreeAttrs(
                ns,
                storePath,
                lockedNode ? lockedNode->lockedRef.input
                           : lockedFlake.flake.lockedRef.input,
                *twSourceInfo,
                false,
                !lockedNode && lockedFlake.flake.forceDirty);
            // Bridge ONCE per node — subsequent reads are v3-native
            // until the per-attr value is forced.
            Value v3SourceInfo = treeWalkerToV3Public(ns, *twSourceInfo);

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
            allocStats().attrsetsAllocated++;
            if (sidSourceInfo < sidDir) {
                inner->entries[0] = {sidSourceInfo, v3SourceInfo};
                inner->entries[1] = {sidDir,        v3Dir};
            } else {
                inner->entries[0] = {sidDir,        v3Dir};
                inner->entries[1] = {sidSourceInfo, v3SourceInfo};
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

            outer->entries[i++] = {sidKey, v3Inner};
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

    Value r1 = callClosure(*vm, vCallFlake, v3Locks);
    Value r2 = callClosure(*vm, r1, v3Overrides);
    Value r3 = callClosure(*vm, r2, v3FetchFinal);
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
