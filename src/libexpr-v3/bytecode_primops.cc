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
#include "v3/primop.hh"
#include "v3/ir.hh"
#include "v3/alloc.hh"

#include "nix/expr/eval.hh"
#include "nix/expr/nixexpr.hh"
#include "nix/util/source-path.hh"

// Forward declaration: defined in vm.cc.
namespace nix::v3 {
Value getBuiltinsValue() noexcept;
}

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
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
/// The Closure Value's `payload.closure->cu` field references the
/// CompilationUnit by pointer.  The pointer is set during `run(cu)`
/// inside `runRootExpr`, so it points to wherever the cu lived at
/// that moment.  Any subsequent MOVE of the cu invalidates the
/// pointer.  Fix: store the RootResult AS-IS (cu + value together)
/// in a heap-allocated holder, and patch `closure->cu` to point at
/// the holder's final cu location.
struct InstalledPrimop {
    std::string name;        // primop name (e.g. "foldl'")
    RootResult  rr;          // owns cu + the compiled lambda Value
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

/// Side-table: maps a v3 PrimOp pointer to its bytecode-Closure
/// replacement Value.  Populated by `installBytecodePrimop`.
/// Read by:
///   - `vm.cc` OP_LIT_PRIMOP to push the replacement instead of a
///     Tag::PrimOp Value (so `let f = builtins.foldl'; in f a b c`
///     and similar dynamic dispatch see the closure).
///   - `lower.cc` `lowerCall` to skip the static PrimOpCall path
///     for replaced primops (so saturated `builtins.foldl' a b c`
///     calls also see the closure via the App-chain → OP_CALL
///     emit path).
std::unordered_map<const PrimOp *, Value> & primopReplacementMap()
{
    static std::unordered_map<const PrimOp *, Value> m;
    return m;
}

} // anonymous namespace

const Value * lookupPrimopReplacement(const PrimOp * po) noexcept
{
    if (!po) return nullptr;
    auto & m = primopReplacementMap();
    auto it = m.find(po);
    return it == m.end() ? nullptr : &it->second;
}

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

    // Stash the holder FIRST so the CU + Value live at stable heap
    // addresses.  Then patch the closure's cu pointer to point at the
    // heap-stable location.  The closure was built by `run()` inside
    // runRootExpr with `desc->cu = &local_cu`; after we move the cu
    // to the holder, that pointer is stale unless we re-point it.
    // Bridge and side-table install must use the PATCHED value, not
    // the pre-move one.
    auto holder = std::make_unique<InstalledPrimop>();
    holder->name = primopName;
    holder->rr = std::move(rr);
    if (holder->rr.value.tag() == Tag::Closure
        && holder->rr.value.payload.closure)
    {
        // Cast-away-const intentional: the Closure was built with
        // `desc->cu = &cu` where cu was at the old address.  We
        // re-point at the heap-stable address now.
        Closure * c = const_cast<Closure *>(holder->rr.value.payload.closure);
        c->cu = &holder->rr.cu;
    }
    InstalledPrimop * installedPtr = holder.get();
    installedPrimops().push_back(std::move(holder));
    auto & installed = *installedPtr;

    // Bridge the (PATCHED) v3 Closure to a TW Value
    // (`mkPrimOpApp(__v3_call_bridge_1, handle)`) so that:
    //   - TW dispatch (`callFunction` from primops or top-level CLI)
    //     routes back into v3 via primV3CallBridge1.
    //   - v3 dispatch (OP_CALL on the bridged value) unwraps via
    //     `tryUnwrapBridge1Closure` (vm.cc:2920) and dispatches the
    //     underlying closure on the same VM — no fresh dispatchLoop.
    nix::Value * bridged = v3ToTreeWalkerPublic(state, installed.rr.value);
    if (!bridged) {
        throw std::runtime_error(
            "installBytecodePrimop: v3ToTreeWalkerPublic returned null "
            "for '" + primopName + "'");
    }

    // Install path 1: mutate the Value in TW's builtins attrset so
    // TW-side dispatch (and any code reading TW's baseEnv) sees the
    // bytecode closure.  Both `state.getBuiltins().attrs()->get(sym)
    // ->value` and `state.baseEnv.values[displ]` point to the same
    // Value* (per `addPrimOp` in libexpr/eval.cc:580-589), so this
    // single mutation propagates to all TW lookup paths.
    nix::Value & target = state.getBuiltin(primopName);
    target = *bridged;

    // Install path 2: register in v3's side-table keyed by v3 PrimOp
    // pointer.  This is what makes v3's OP_LIT_PRIMOP / OP_CALL_PRIMOP
    // dispatch see the replacement — v3 has its own builtins attrset
    // (vm.cc:8298 getBuiltinsValue) built from the v3 PrimOp registry,
    // bypassing TW's builtins entirely.  The v3 lookup in vm.cc and
    // the v3 lowerCall skip-check in lower.cc both consult
    // `lookupPrimopReplacement(po)`.
    const PrimOp * po = findPrimOp(primopName);
    if (!po) {
        // Should not happen: getBuiltin succeeded above, so the primop
        // is in TW's registry — but the v3 registry is independent.
        // Most primops are dual-registered (in both); if not, the
        // OP_LIT_PRIMOP / OP_CALL_PRIMOP redirect won't fire and the
        // installed closure is only visible to dynamic TW lookups.
        if (dbgEnabled())
            std::fprintf(stderr,
                "v3 bytecode-primop install: '%s' has no v3 PrimOp "
                "registration; closure visible only to TW dispatch\n",
                primopName.c_str());
        return;
    }
    primopReplacementMap()[po] = installed.rr.value;

    // Install path 3: patch v3's static `vBuiltins` attrset in place
    // so dynamic dispatch (`builtins.foldl'`, `let f = builtins.foldl';
    // in f`) sees the closure.  vBuiltins is built lazily on the
    // first OP_LIT_BUILTINS access; if the install happens AFTER that
    // (e.g. because compiling a previous bytecode-primop source
    // triggered the first access), patching in place is required.
    // If vBuiltins hasn't been built yet, we still need to patch:
    // calling getBuiltinsValue() materialises it now with the
    // replacement applied at the OP_LIT_BUILTINS-rebuild check (which
    // we don't have — so the materialise-then-patch is the cleanest).
    {
        Value vBuiltins = getBuiltinsValue();
        if (vBuiltins.isAttrs() && vBuiltins.payload.bindings) {
            SymbolId sid = ir::globalInternSymbol(primopName);
            Bindings * b = vBuiltins.payload.bindings;
            for (uint32_t i = 0; i < b->size; ++i) {
                if (b->entries[i].name == sid) {
                    b->entries[i].value = installed.rr.value;
                    break;
                }
            }
        }
    }
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
        // T0b status (2026-05-17): dispatch hook live.
        //   - vm.cc OP_LIT_PRIMOP checks lookupPrimopReplacement and
        //     pushes the closure Value if found.
        //   - lower.cc lowerCall skips the static PrimOpCall emission
        //     for replaced primops (forcing the call through the
        //     generic App-chain → OP_CALL path that goes through the
        //     OP_LIT_PRIMOP redirect above).
        //   - installBytecodePrimop also patches v3's static vBuiltins
        //     in place so dynamic lookups of `builtins.foo` see the
        //     replacement.
        //
        // T0b self-test gate: install `floor` as `x: x + 1` so that
        // `builtins.floor 41 == 42` under v3.  TW remains unchanged
        // (this is opt-in for verification only).
        if (std::getenv("NIX_V3_BYTECODE_PRIMOP_SELFTEST"))
            installBytecodePrimop(state, "floor", "x: x + 1");

        // Phase 1: bytecode-emit callback-heavy primops.  Each
        // conversion replaces the C primop's callClosure-per-iteration
        // (C-recursive) with a Nix-source loop where the inner
        // `op acc elem` call dispatches via OP_CALL (iterative, since
        // commit 7f5a392f4) and the outer recursive `go i acc` is
        // rewritten to OP_TAIL_CALL by emit.cc's tail-call peephole.
        // Result: O(1) C-stack regardless of list size.
        //
        // Disable per-primop via NIX_V3_NO_BC_<NAME>=1, or globally
        // via NIX_V3_NO_BYTECODE_PRIMOPS=1 (gated above in run.cc).
        // Hot primops first; each one runs the property suite + lang
        // tests + bench as part of its landing commit.

        // ORDER MATTERS: bytecode primops are visible to the lowerer
        // only AFTER they're installed.  If primop B's source uses
        // primop A, install A first so B's lowering sees A as
        // replaced and emits App-chain → OP_CALL on the closure
        // (rather than PrimOpCall on A's C function, which would
        // C-recurse on every callback).  Foundation primops
        // (foldl', map) install first; primops built on top of them
        // (filter, all, any) install after.

        // T1 — foldl': strict left fold.  Foundation: many downstream
        // primops (filter, partition, listToAttrs, ...) compose on
        // top of it.  Strict via `builtins.seq` so the accumulator is
        // WHNF on every tail call (matches TW primFoldl semantics).
        // The recursive `go` is rewritten to OP_TAIL_CALL by emit.cc's
        // peephole — O(1) vm.frames regardless of list size.
        if (!std::getenv("NIX_V3_NO_BC_FOLDL"))
            installBytecodePrimop(state, "foldl'",
                "op: nul: list: "
                "  let n = builtins.length list; "
                "      go = i: acc: "
                "        if i >= n then acc "
                "        else "
                "          let next = op acc (builtins.elemAt list i); "
                "          in builtins.seq next (go (i + 1) next); "
                "  in go 0 nul");

        // T2 — map: lazy list mapping.  Preserves TW's primMap
        // laziness (each result entry is forced on demand) by
        // expressing map in terms of genList — which itself is a
        // C primop that builds Tag::App entries lazily.
        if (!std::getenv("NIX_V3_NO_BC_MAP"))
            installBytecodePrimop(state, "map",
                "fn: list: "
                "  builtins.genList "
                "    (i: fn (builtins.elemAt list i)) "
                "    (builtins.length list)");

        // T4 — all: short-circuit fold for "every elem satisfies pred".
        // Direct tail-recursive go with early exit on false.  Pure
        // bytecode iteration (no foldl' dependency — needs early
        // exit which foldl' doesn't provide).
        if (!std::getenv("NIX_V3_NO_BC_ALL"))
            installBytecodePrimop(state, "all",
                "pred: list: "
                "  let n = builtins.length list; "
                "      go = i: "
                "        if i >= n then true "
                "        else if pred (builtins.elemAt list i) "
                "             then go (i + 1) "
                "             else false; "
                "  in go 0");

        // T6 — concatMap: apply fn to each elem (fn returns a list),
        // concat the results.  Built on bytecode foldl' (T1) with
        // `++` between accumulator and each sublist.  Elements
        // inside the sublists are passed through unchanged (lazy
        // values remain lazy).  Matches TW primConcatMap semantics
        // (strict on the spine, lazy on the elements).
        if (!std::getenv("NIX_V3_NO_BC_CONCATMAP"))
            installBytecodePrimop(state, "concatMap",
                "fn: list: "
                "  builtins.foldl' "
                "    (acc: x: acc ++ (fn x)) "
                "    [] "
                "    list");

        // T5 — any: short-circuit fold for "some elem satisfies pred".
        // Mirror of all (early exit on true instead of false).
        if (!std::getenv("NIX_V3_NO_BC_ANY"))
            installBytecodePrimop(state, "any",
                "pred: list: "
                "  let n = builtins.length list; "
                "      go = i: "
                "        if i >= n then false "
                "        else if pred (builtins.elemAt list i) "
                "             then true "
                "             else go (i + 1); "
                "  in go 0");

        // T13 — catAttrs: collect attr `name` from each attrset in the
        // list that has it.  Built on bytecode foldl'.  Used by
        // nixpkgs lib heavily (e.g. `catAttrs "buildInputs" deps`).
        if (!std::getenv("NIX_V3_NO_BC_CATATTRS"))
            installBytecodePrimop(state, "catAttrs",
                "name: list: "
                "  builtins.foldl' "
                "    (acc: x: "
                "       if builtins.hasAttr name x "
                "       then acc ++ [(builtins.getAttr name x)] "
                "       else acc) "
                "    [] "
                "    list");

        // T14 — concatLists: flatten a list of lists.  Trivial via
        // foldl' (T1) — the same shape as concatMap with the identity
        // function.
        if (!std::getenv("NIX_V3_NO_BC_CONCATLISTS"))
            installBytecodePrimop(state, "concatLists",
                "lists: "
                "  builtins.foldl' "
                "    (acc: xs: acc ++ xs) "
                "    [] "
                "    lists");

        // T15 — listToAttrs: convert a list of {name, value} records
        // to an attrset.  Order-sensitive: later entries override
        // earlier ones via // (matches TW's primListToAttrs).  Built
        // on bytecode foldl' + attrset merge.
        if (!std::getenv("NIX_V3_NO_BC_LISTTOATTRS"))
            installBytecodePrimop(state, "listToAttrs",
                "list: "
                "  builtins.foldl' "
                "    (acc: x: acc // { ${x.name} = x.value; }) "
                "    {} "
                "    list");

        // T16 — removeAttrs: build a new attrset that excludes `names`.
        // Composes bytecode filter (T3) + listToAttrs (T15) on the
        // attrNames of the input.  Each kept name's value is read
        // from the original via `getAttr`.
        if (!std::getenv("NIX_V3_NO_BC_REMOVEATTRS"))
            installBytecodePrimop(state, "removeAttrs",
                "attrs: names: "
                "  let keep = builtins.filter "
                "               (n: ! (builtins.elem n names)) "
                "               (builtins.attrNames attrs); "
                "  in builtins.listToAttrs "
                "       (builtins.map "
                "          (n: { name = n; value = builtins.getAttr n attrs; }) "
                "          keep)");

        // T17 — intersectAttrs: keep entries in `b` whose names also
        // appear in `a`.  TW's primIntersectAttrs is non-trivial in C
        // (sorted-merge); the bytecode version is simpler: filter
        // attrNames of b by membership in a, then rebuild via
        // listToAttrs.  O(|b|×log|a|) via hasAttr lookups.
        if (!std::getenv("NIX_V3_NO_BC_INTERSECTATTRS"))
            installBytecodePrimop(state, "intersectAttrs",
                "a: b: "
                "  let keep = builtins.filter "
                "               (n: builtins.hasAttr n a) "
                "               (builtins.attrNames b); "
                "  in builtins.listToAttrs "
                "       (builtins.map "
                "          (n: { name = n; value = builtins.getAttr n b; }) "
                "          keep)");

        // T9 — partition: { right, wrong } split by predicate.
        // Built on bytecode foldl' (T1).  Two accumulators carried
        // in an attrset; iteration is iterative via foldl''s
        // OP_TAIL_CALL.
        if (!std::getenv("NIX_V3_NO_BC_PARTITION"))
            installBytecodePrimop(state, "partition",
                "pred: list: "
                "  builtins.foldl' "
                "    (acc: x: "
                "       if pred x "
                "       then { right = acc.right ++ [x]; wrong = acc.wrong; } "
                "       else { right = acc.right; wrong = acc.wrong ++ [x]; }) "
                "    { right = []; wrong = []; } "
                "    list");

        // T3 — filter: iterate, keep elements where pred returns true.
        // Built on bytecode foldl' (T1) — the iteration runs via
        // OP_TAIL_CALL inside foldl' so no per-element C-recursion.
        // Each step does either `acc ++ [x]` (kept) or skip; result
        // elements are passed through unchanged (lazy values remain
        // lazy).  Worst-case O(N²) due to repeated ++, matching TW
        // primFilter's append-per-match semantics.
        if (!std::getenv("NIX_V3_NO_BC_FILTER"))
            installBytecodePrimop(state, "filter",
                "pred: list: "
                "  builtins.foldl' "
                "    (acc: x: if pred x then acc ++ [x] else acc) "
                "    [] "
                "    list");
    } catch (...) {
        // Reset `done` so a future call retries — otherwise a
        // transient error here would permanently disable bytecode
        // primops for the process.
        done = false;
        throw;
    }
}

} // namespace nix::v3
