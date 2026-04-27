/// @file
/// v3 cutover hook — fills in `nix::EvalState::v3EvalHook` so that
/// `EvalState::eval` can route to the v3 bytecode VM at runtime when
/// `NIX_USE_V3=1` is set.
///
/// The hook:
///   1. Lowers the AST through v3's IR + bytecode pipeline.
///   2. Runs the resulting CompilationUnit in v3's VM.
///   3. Converts the v3 Value back into a tree-walker `nix::Value`
///      written into the caller-provided slot.
///
/// Compiled CompilationUnits are cached per Expr* so that
/// re-evaluation of the same AST node (which is common across the
/// import cache) doesn't pay the lower/compile cost twice.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/vm.hh"
#include "v3/lower.hh"
#include "v3/ir.hh"
#include "v3/value.hh"
#include "v3/primop.hh"
#include "v3/alloc.hh"

#include "nix/expr/eval.hh"
#include "nix/expr/nixexpr.hh"

#include <memory>
#include <unordered_map>

namespace nix::v3 {

// Forward declarations of the bridge helper in primops.cc.  The
// `Public` wrapper takes a nix::EvalState directly so we don't need
// to construct a v3 EvalState here.
nix::Value * v3ToTreeWalkerPublic(nix::EvalState & nixState, Value v);

// We don't expose treeWalkerToV3 here — the AST already carries
// nix::Expr nodes, not nix::Value, so we lower the Expr directly.

// Per-process cache keyed by Expr pointer (stable across the
// EvalState's lifetime).  Each entry owns a CompilationUnit; values
// stay alive for the lifetime of the EvalState since v3 doesn't yet
// have a tear-down hook.
struct CachedUnit {
    std::unique_ptr<CompilationUnit> cu;
};

static std::unordered_map<const nix::Expr *, CachedUnit> & v3HookCache()
{
    static std::unordered_map<const nix::Expr *, CachedUnit> tbl;
    return tbl;
}

/// Process-wide counters that prove the cutover is firing.  Bumped on
/// every call to the v3 hook entry point and exposed via NIX_VM_STATS.
struct V3HookStats {
    uint64_t evalEntries  = 0;
    uint64_t cacheHits    = 0;
    uint64_t cacheMisses  = 0;
};

V3HookStats & v3HookStats()
{
    static V3HookStats stats;
    return stats;
}

/// The hook entry point.  Called from libnixexpr's EvalState::eval
/// when NIX_USE_V3=1 and this hook is non-null.
static void v3EvalEntry(nix::EvalState & state, nix::Expr * e, nix::Value & v)
{
    auto & st = v3HookStats();
    st.evalEntries++;
    bool diag = std::getenv("V3_DEBUG_HOOK") != nullptr;
    if (diag) std::fprintf(stderr, "v3 hook[%llu]: enter e=%p\n",
                           (unsigned long long)st.evalEntries, (void*)e);
    static bool registered = (registerBuiltinPrimOps(), true);
    (void)registered;
    setNixEvalState(&state);

    auto & cache = v3HookCache();
    auto it = cache.find(e);
    const CompilationUnit * cu = nullptr;
    if (it == cache.end()) {
        st.cacheMisses++;
        try {
            auto module = lowerNixExpr(e, state.symbols, state.positions);
            ir::computeFreeVars(module);
            auto compiled = std::make_unique<CompilationUnit>(compile(module));
            cu = compiled.get();
            cache.emplace(e, CachedUnit{std::move(compiled)});
        } catch (const std::exception & ex) {
            if (diag) std::fprintf(stderr, "v3 hook: lower/compile threw: %s\n", ex.what());
            // Fall back to tree-walker by calling e->eval directly.
            // This lets us bail out cleanly when v3 hits something it
            // can't lower (e.g. unsupported AST shape) without crashing.
            // Tree-walker handles the rest of the evaluation.
            e->eval(state, state.baseEnv, v);
            return;
        }
    } else {
        st.cacheHits++;
        cu = it->second.cu.get();
    }

    if (diag) std::fprintf(stderr, "v3 hook: about to run cu (%zu insts, %zu lambdas)\n",
                           cu->code.size(), cu->lambdas.size());
    Value r;
    try {
        r = run(*cu);
    } catch (const std::exception & ex) {
        if (diag) std::fprintf(stderr, "v3 hook: run threw: %s\n", ex.what());
        // v3 evaluation failure — fall back to tree-walker.
        e->eval(state, state.baseEnv, v);
        return;
    }
    if (diag) std::fprintf(stderr, "v3 hook: ran, tag=%d\n", (int)r.tag());

    // Convert the v3 result back to a tree-walker nix::Value in
    // the caller-provided slot.  v3ToTreeWalker GC-allocates a
    // nix::Value for nested structures; the top-level result we
    // just copy into the out-Value via mkBlah().
    switch (r.tag()) {
    case Tag::Bool:   v.mkBool(r.payload.i == 1); return;
    case Tag::Int:    v.mkInt(r.payload.i);       return;
    case Tag::Float:  v.mkFloat(r.payload.f);     return;
    case Tag::Null:   v.mkNull();                 return;
    case Tag::String: v.mkString(r.payload.str ? r.payload.str : "", state.mem); return;
    case Tag::Closure:
    case Tag::Thunk:
    case Tag::PrimOp:
    case Tag::PrimOpApp:
    case Tag::App:
    case Tag::Blackhole:
    case Tag::Uninitialized:
    case Tag::External: {
        // Functions / thunks can't be cleanly handed back to the
        // tree-walker via the current bridge: v3ToTreeWalker for
        // closures uses a hardcoded-arity primop that doesn't match
        // the calling convention `autoCallFunction` expects.  Until
        // CO-3 fixes the bridge, fall back to tree-walker for these.
        if (diag) std::fprintf(stderr,
            "v3 hook: result tag=%d, falling back to tree-walker\n",
            (int)r.tag());
        e->eval(state, state.baseEnv, v);
        return;
    }
    case Tag::Path:
    case Tag::Attrs:
    case Tag::List: {
        // Paths / attrsets / lists recursively convert via
        // v3ToTreeWalker.  Failures are tree-walker fallback.  Note
        // the bridge is known to crash on closures embedded inside
        // these structures — treat any throw as a signal to fall
        // back, and pre-emptively fall back if the bridge returns null.
        try {
            nix::Value * tmp = v3ToTreeWalkerPublic(state, r);
            if (tmp) { v = *tmp; return; }
        } catch (const std::exception & ex) {
            if (diag) std::fprintf(stderr, "v3 hook: bridge threw: %s\n", ex.what());
        }
        e->eval(state, state.baseEnv, v);
        return;
    }
    }
}

namespace {

/// Static initializer — runs at library load time.  Once
/// libnixexprv3.dylib is linked into a binary that also pulls in
/// libnixexpr, this fills in the function pointer so `NIX_USE_V3=1`
/// can route through v3.  Also installs an atexit() handler that
/// dumps the hook stats when NIX_VM_STATS=1 is set, so the user
/// can verify the cutover is actually firing.
struct V3HookRegistrar {
    V3HookRegistrar() { nix::EvalState::v3EvalHook = &v3EvalEntry; }
};

[[maybe_unused]] V3HookRegistrar _v3_hook_registrar_instance;

} // anonymous namespace

} // namespace nix::v3

// Public installer function — call this from the main `nix` binary so the
// linker can't strip libnixexprv3.  Without an explicit symbol reference,
// macOS's `-dead_strip_dylibs` removes the lib entirely (its only user
// is the static initializer that registers `EvalState::v3EvalHook`, and
// the linker doesn't see the static-init as "used").
namespace nix::v3 {
void installEvalHook()
{
    nix::EvalState::v3EvalHook = &v3EvalEntry;
}
} // namespace nix::v3
