/// @file
/// Bytecode-primop infrastructure — T0 of the A12b architectural
/// refactor.  See `include/v3/bytecode_primops.hh` for rationale.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
/// Input Output Group.  SPDX-License-Identifier: Apache-2.0

#include "v3/bytecode_primops.hh"
#include "v3/run.hh"
#include "v3/value.hh"
#include "v3/bytecode.hh"

#include "nix/expr/eval.hh"
#include "nix/expr/nixexpr.hh"
#include "nix/util/source-path.hh"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

// Forward declaration: defined in primops.cc.  Bridges a v3 Value into
// a TW Value (wrapping closures as `mkPrimOpApp(__v3_call_bridge_1, h)`
// so TW can dispatch them and v3 can unwrap them via
// tryUnwrapBridge1Closure in OP_CALL).
namespace nix::v3 {
nix::Value * v3ToTreeWalkerPublic(nix::EvalState & nixState, Value v);
}

namespace nix::v3 {

namespace {

/// Process-global storage that keeps installed bytecode primops alive.
///
/// The Value field references string constants / path constants in the
/// CompilationUnit; if `cu` were freed, the Value would dangle.  We
/// keep both in a holder that lives forever (one entry per installed
/// primop, never removed).
struct InstalledPrimop {
    std::string      name;       // primop name (e.g. "foldl'")
    CompilationUnit  cu;         // owns bytecode + string constants
    Value            v3Closure;  // the compiled lambda value
};

std::vector<std::unique_ptr<InstalledPrimop>> & installedPrimops()
{
    static std::vector<std::unique_ptr<InstalledPrimop>> v;
    return v;
}

/// Guard against recursive install: `installBytecodePrimop` calls
/// `runRootExpr`, and `runRootExpr` calls `installAllBytecodePrimops`
/// once-per-process via `std::call_once`.  Without this guard, the
/// recursive call would deadlock the once-flag (or, with eager call,
/// re-enter and pile primops twice).
thread_local bool tl_installInProgress = false;

/// V3_DBG_BYTECODE_PRIMOP=1 prints a one-line trace per install.
inline bool dbgEnabled()
{
    static const bool v = std::getenv("V3_DBG_BYTECODE_PRIMOP") != nullptr;
    return v;
}

/// Names already installed (idempotency).
std::set<std::string> & installedNames()
{
    static std::set<std::string> s;
    return s;
}

} // anonymous namespace

void installBytecodePrimop(
    nix::EvalState & state,
    const std::string & primopName,
    const std::string & nixSource)
{
    // Idempotent: same name → no-op.
    if (installedNames().count(primopName)) return;
    installedNames().insert(primopName);

    if (dbgEnabled())
        std::fprintf(stderr, "v3 bytecode-primop install: %s\n",
                     primopName.c_str());

    // Parse + bind-vars (parseExprFromString applies bindVars against
    // staticBaseEnv automatically, so `builtins.length` etc. in the
    // source resolves correctly to TW's pre-registered primops).
    nix::Expr * expr = state.parseExprFromString(
        nixSource, state.rootPath("."));

    // Run via the v3 pipeline.  The inner `installAllBytecodePrimops`
    // call would re-enter here, so we guard with `tl_installInProgress`.
    bool wasInProgress = tl_installInProgress;
    tl_installInProgress = true;
    RootResult rr = runRootExpr(state, expr);
    tl_installInProgress = wasInProgress;

    // The compiled top-level expression must be a Closure (the lambda
    // body of the primop source).
    if (rr.value.tag() != Tag::Closure) {
        throw std::runtime_error(
            "installBytecodePrimop: source for '" + primopName +
            "' did not compile to a Tag::Closure (got tag=" +
            std::to_string(static_cast<int>(rr.value.tag())) + ")");
    }

    // Bridge the v3 Closure to a TW Value
    // (`mkPrimOpApp(__v3_call_bridge_1, handle)`) so that:
    //   - TW dispatch (`callFunction` from primops or top-level CLI)
    //     routes back into v3 via primV3CallBridge1.
    //   - v3 dispatch (OP_CALL on the bridged value) unwraps via
    //     `tryUnwrapBridge1Closure` (vm.cc:2920) and dispatches the
    //     underlying closure on the same VM — no fresh dispatchLoop.
    nix::Value * bridged = v3ToTreeWalkerPublic(state, rr.value);
    if (!bridged) {
        throw std::runtime_error(
            "installBytecodePrimop: v3ToTreeWalkerPublic returned null "
            "for '" + primopName + "'");
    }

    // Stash the holder so the CU + Value live forever.
    auto holder = std::make_unique<InstalledPrimop>();
    holder->name = primopName;
    holder->cu = std::move(rr.cu);
    holder->v3Closure = rr.value;
    installedPrimops().push_back(std::move(holder));

    // Install: mutate the Value in TW's builtins attrset.  Both
    // `state.getBuiltins().attrs()->get(sym)->value` and
    // `state.baseEnv.values[displ]` point to the same Value* (per
    // `addPrimOp` in libexpr/eval.cc:580-589), so this single
    // mutation propagates to all lookup paths.
    nix::Value & target = state.getBuiltin(primopName);
    target = *bridged;
}


void installAllBytecodePrimops(nix::EvalState & state)
{
    if (tl_installInProgress) return;

    // Static guard: the install runs once per process.  We use
    // call_once-style flagging instead of `std::call_once` because
    // the once-flag would deadlock the recursive runRootExpr call
    // that happens during install.
    static bool done = false;
    if (done) return;
    done = true;  // set BEFORE work so nested calls short-circuit

    try {
        // T0 status (2026-05-17): the install pipeline (parse → lower
        // → compile → bridge → mutate TW.getBuiltin) is verified to
        // work — V3_DBG_BYTECODE_PRIMOP=1 confirms the install fires
        // and produces a valid bridged Closure Value.
        //
        // HOWEVER, v3 bypasses TW's builtins attrset when dispatching
        // `builtins.X`: `getBuiltinsValue()` in vm.cc:8298 builds v3's
        // own builtins attrset from the v3 PrimOp registry directly.
        // Mutating TW.getBuiltin therefore does NOT redirect v3
        // dispatch.  Additionally, `lower.cc` statically resolves
        // saturated `builtins.foo arg1 arg2` calls to OP_CALL_PRIMOP
        // referencing the C PrimOp pointer, bypassing the runtime
        // builtins lookup entirely.
        //
        // To make the install effective, T0b (next sub-task) must
        // ONE OF:
        //   (a) Add a side-table `unordered_map<const PrimOp*, Value>`
        //       of replacements and check it in OP_LIT_PRIMOP +
        //       OP_CALL_PRIMOP in vm.cc.  OP_CALL_PRIMOP redirect is
        //       non-trivial: args are already on the stack and
        //       dispatching a closure curried over N args requires
        //       the iterative OP_CALL chain (partial — only fun-force
        //       converted, primop-arg redirect not yet wired up).
        //
        //   (b) IR-level inlining: at lower-time (in
        //       fusePrimOpApps or a new pass), rewrite saturated calls
        //       to replaced primops as the inlined source IR.  Avoids
        //       runtime dispatch entirely.  Each call site gets
        //       specialised bytecode.  Higher up-front cost
        //       (IR inlining + variable substitution) but cleaner
        //       runtime.
        //
        // This scaffold (T0) lands the install function + bridge wiring
        // + idempotency / recursion guards / dbg trace.  Subsequent
        // commits (T0b → T1-T17) layer on the dispatch hook and the
        // actual primop sources.

        // Phase 1 (T1-T17): no primops installed yet.  Each conversion
        // task appends one `installBytecodePrimop(state, name, src)`
        // call here after T0b's dispatch hook is wired up.
    } catch (...) {
        // Reset `done` so a future call retries — otherwise a
        // transient error here would permanently disable bytecode
        // primops for the process.
        done = false;
        throw;
    }
}

} // namespace nix::v3
