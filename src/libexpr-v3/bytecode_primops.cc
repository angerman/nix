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

    // #676: post-#676 the CU is held as unique_ptr<CompilationUnit>
    // inside RootResult — its heap address is stable from the moment
    // runRootExpr's make_unique returns, regardless of how many times
    // RootResult itself is moved.  So this branch no longer needs the
    // historical cu-pointer fix-up (the closure's `cu` pointer already
    // points at the stable heap CU).  The holder still owns the CU
    // for lifetime (installedPrimops() keeps it alive for the process).
    auto holder = std::make_unique<InstalledPrimop>();
    holder->name = primopName;
    holder->rr = std::move(rr);
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
    //
    // 2026-05-18: try/catch — some primops are registered only in v3
    // (e.g. __foldlMap from IR Phase C).  getBuiltin throws on
    // missing names.  Falling through to path 2 + path 3 still
    // installs the v3-side replacement, which is all we need for
    // v3-direct evaluation.
    try {
        nix::Value & target = state.getBuiltin(primopName);
        target = *bridged;
    } catch (const std::exception & e) {
        if (dbgEnabled())
            std::fprintf(stderr,
                "v3 bytecode-primop install: '%s' not in TW builtins "
                "(%s) — skipping path 1, continuing with v3-side install\n",
                primopName.c_str(), e.what());
    }

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

        // 2026-05-18: IR Phase C stream-fusion target.  __foldlMap
        // implements `foldl' op nul (map f xs)` in a single iterative
        // pass — no intermediate list allocation, no per-element
        // C-recursion via callClosure.  Body mirrors the foldl'
        // bytecode above but inlines the `f` application per element.
        // The opt_stream_fusion pass rewrites detected foldl'+map
        // patterns to PrimOpCall(__foldlMap, [op, nul, f, xs]).
        if (!std::getenv("NIX_V3_NO_BC_FOLDLMAP"))
            installBytecodePrimop(state, "__foldlMap",
                "op: nul: f: list: "
                "  let n = builtins.length list; "
                "      go = i: acc: "
                "        if i >= n then acc "
                "        else "
                "          let fx = f (builtins.elemAt list i); "
                "              next = op acc fx; "
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

        // T13-T17 (catAttrs, concatLists, listToAttrs, removeAttrs,
        // intersectAttrs) — REVERTED 2026-05-17.  These primops don't
        // take user lambdas as args; they don't C-recurse on callbacks.
        // Their C versions are O(N) (or O(N log N) with sorted-merge);
        // the bytecode equivalents I wrote use repeated `++` / `//`
        // which is O(N²) (each step copies the accumulator).  Measured
        // regression on attrset-build-1k (+60.4%); keeping them as C
        // primops is the right choice for A12b (which targets CALLBACK
        // C-recursion, not arbitrary primop replacement).
        //
        // 2026-05-17b A/B re-test: tried adding ONLY concatLists back
        // (since it shows up in the hello.name C-stack profile) — turns
        // out it makes things WORSE.  vm.frames depth at SIGBUS goes
        // 3215 → 1201 (-63%): the bytecode foldl'+`++` chain consumes
        // more vm.frames per call than the C primConcatLists does, and
        // since the dispatchLoop frame is what blows C-stack, more
        // vm.frames per "primDerivation level" means we hit the C-stack
        // ceiling at fewer levels.  Leave concatLists as C primop.

        // T10 — groupBy: group list elements by key-fn result.
        //   { ${fn x}: [matching xs] for each x in list }
        // Built on bytecode foldl'.  Uses `acc.${key} or []` to
        // accumulate per-key lists.
        if (!std::getenv("NIX_V3_NO_BC_GROUPBY"))
            installBytecodePrimop(state, "groupBy",
                "fn: list: "
                "  builtins.foldl' "
                "    (acc: x: "
                "       let key = fn x; "
                "           prev = acc.${key} or []; "
                "       in acc // { ${key} = prev ++ [x]; }) "
                "    {} "
                "    list");

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
        // 2026-05-17 — primDerivation* hybrid wrapper (Option 4 in the
        // strategic note).  Replaces the user-facing `derivation` /
        // `derivationStrict` primops with a bytecode wrapper that
        // pre-forces top-level attrs (+ list elements) at bytecode
        // level (iterative via the new seq fast-path in lower.cc),
        // then calls the C leaf primop (`__derivationRaw` /
        // `__derivationStrictRaw`) which finds attrs WHNF and so its
        // internal forceValue calls become trivial chases — no
        // C-recursion.
        //
        // The user-requested architectural shape: outer driver in
        // bytecode (attr-walking, iteration), inner FFI leaf for the
        // libnixstore work.  We DON'T replicate primDerivation's full
        // logic in Nix — the leaf primops are the existing C bodies
        // wholesale; the wrapper just hoists the forceValue calls
        // from C to bytecode.  This avoids the regression risk of a
        // ~700-line C-to-Nix port while still breaking the C-stack
        // recursion that hits hello.name today.
        //
        // For inner derivation invocations triggered during pre-force
        // (e.g. `args.buildInputs` containing other derivation thunks):
        // forcing each element via bytecode OP_FORCE pushes a thunk
        // frame, runs the thunk body via the SAME dispatchLoop — when
        // that body invokes `builtins.derivation { ... }`, it hits MY
        // wrapper (intercepted by the install).  All derivation calls
        // ride the same bytecode wrapper, so recursion through the
        // derivation graph runs as vm.frames pushes rather than C
        // stack frames.
        //
        // The wrapper's pre-force does two passes:
        //   (a) shallow: force each top-level attr value (so the
        //       primop's internal `forceValue(attrV)` becomes a no-op
        //       chase).
        //   (b) list-element: for list-typed attrs (args / outputs /
        //       buildInputs / nativeBuildInputs / ...), force each
        //       element so the primop's element-iteration forces
        //       (lines 5206, 5221, 5255, 5277) also become no-ops.
        //
        // The `builtins.isList v` check in pass (b) calls a C primop
        // (primIsList) whose OP_CALL_PRIMOP arg-prep would normally
        // C-recurse on v.  Pass (a) ran first → v is already WHNF
        // → arg-prep's forceValue is a trivial chase.
        if (!std::getenv("NIX_V3_NO_BC_DERIVATION_HYBRID")) {
            // gate: NIX_V3_NO_BC_DERIVATION_HYBRID — opt-out for A/B
            // measurement vs the all-C path.  Retire when bench shows
            // hybrid is unambiguously better (or worse, in which case
            // the wrapper is the revert candidate).
            // Wrapper body: pre-force each top-level attr value, then
            // call the C leaf primop.  TARGETED pre-force — only the
            // attrs that primDerivationStrict's C-body iterates AND
            // would otherwise C-recurse for: the "concrete" string-
            // typed attrs (name, builder, system) + the list-typed
            // attrs (args, outputs, allowedReferences, ...) where
            // primConcatLists / list-iteration is the recursion source.
            //
            // EXCLUDES recursive/extensible attrs like `passthru`,
            // `meta`, `__overrides`, `__functionArgs`, `override*` —
            // these are typically structured by the fix-point pattern
            // and forcing them eagerly trips the
            // `self.passthru // {...}` Blackhole that TW navigates by
            // its on-demand attr-by-attr forcing in primDerivation's
            // iteration order (specifically: when TW iterates and
            // forces passthru, only at THAT moment is self.passthru
            // looked up, and the chain is set up so the inner thunk
            // is Evaluated by then — bytecode-side pre-force ahead of
            // primDerivation's iteration breaks this ordering).
            //
            // Implemented as a hand-rolled filter rather than a full
            // attr-by-attr force: foldl' iterates a HARDCODED list of
            // "safe-to-pre-force" attr names and skips any not present
            // in args (via `args ? k` then `args.${k}`).
            // 2026-05-17 Option 4 full wrapper.  Replaces the prior
            // "pre-force then call C primop" approach.  The wrapper now
            // does phases 1-3 (validation, attr iteration, coerce-to-
            // string) entirely in Nix-source-compiled-to-bytecode, then
            // calls the C FFI leaf `__derivationFromPreprocessed` which
            // runs phases 4-7 (context → inputs, output config,
            // writeDerivation, result attrset) via the shared
            // `buildAndWriteDrvNative` helper in primops.cc.
            //
            // Why "Option 4 full" instead of "pre-force then call C":
            // breaking the C-stack recursion requires every level of
            // recursion through the derivation graph to ride bytecode
            // (vm.frames pushes) rather than C-stack frames.  The
            // pre-force-only approach left the C-body's iteration as
            // a C-recursion vector — primDerivationStrictNative's
            // `forceValue(attrV)` (vm.cc-equiv line 5183) is the
            // call into deeper derivation chains.  Doing the iteration
            // in bytecode replaces every per-level C frame with a
            // dispatchLoop-internal vm.frames push.
            //
            // Falls back to `__derivationStrictRaw` (the C primop) for
            // __structuredAttrs=true derivations — the wrapper doesn't
            // yet handle JSON encoding (TODO: port `valueToJsonWithContext`
            // to bytecode for the full Option 4 closure).
            //
            // (Old wrapper kept below as commented reference.)
#if 0
            // The wrapper has TWO pre-force passes:
            //
            //   (1) safeKeys: shallow-force the concrete-typed attrs
            //       (name, builder, system, args, outputs, outputHash*).
            //       These are the attrs primDerivationStrict reads
            //       directly + the list-of-strings attrs.  Pre-forcing
            //       at bytecode level avoids the C-recursive
            //       forceValue in primDerivationStrictNative.
            //
            //   (2) inputListKeys: deep-force the build-input lists.
            //       buildInputs / nativeBuildInputs / etc. are LISTS
            //       OF DERIVATIONS.  primDerivationStrictNative's
            //       generic attr-loop calls coerceToString on each,
            //       which forces each element — these forces are the
            //       MAIN C-recursion source on hello.name (each
            //       element's derivation thunk triggers another
            //       primDerivation chain).  By pre-forcing each
            //       element via bytecode OP_FORCE (iterative through
            //       op_force_slow + frame push), the inner derivation
            //       chain runs as vm.frames pushes rather than C
            //       stack frames.
            //
            // Why selective rather than "force every attr": forcing
            // recursive fix-point attrs like `passthru` (which often
            // reads `self.passthru` to extend it) ahead of the C
            // primop's own iteration trips Blackhole cycles that TW
            // navigates by on-demand attr-by-attr forcing.  The
            // hardcoded list of safe + input-list keys is the
            // intersection of "primDerivationStrict will force it
            // anyway" and "no fix-point loop hazard".
            const char * wrapper_body =
                "args: "
                "  let "
                "    safeKeys = [ "
                "      \"name\" \"builder\" \"system\" \"args\" "
                "      \"outputs\" \"outputHash\" \"outputHashAlgo\" "
                "      \"outputHashMode\" "
                "    ]; "
                "    forceSafe = "
                "      builtins.foldl' "
                "        (acc: k: "
                "           if args ? ${k} "
                "           then builtins.seq (args.${k}) acc "
                "           else acc) "
                "        null "
                "        safeKeys; "
                "    inputListKeys = [ "
                "      \"buildInputs\" \"nativeBuildInputs\" "
                "      \"propagatedBuildInputs\" \"propagatedNativeBuildInputs\" "
                "      \"depsBuildBuild\" \"depsBuildBuildPropagated\" "
                "      \"depsBuildHost\" \"depsBuildHostPropagated\" "
                "      \"depsBuildTarget\" \"depsBuildTargetPropagated\" "
                "      \"depsHostHost\" \"depsHostHostPropagated\" "
                "      \"depsHostTarget\" \"depsHostTargetPropagated\" "
                "      \"depsTargetTarget\" \"depsTargetTargetPropagated\" "
                "      \"checkInputs\" \"nativeCheckInputs\" "
                "      \"installCheckInputs\" \"nativeInstallCheckInputs\" "
                "    ]; "
                "    forceInputList = k: "
                "      if args ? ${k} "
                "      then "
                "        let lst = args.${k}; in "
                "        if builtins.isList lst "
                "        then "
                "          builtins.foldl' "
                "            (acc: e: builtins.seq e acc) "
                "            null "
                "            lst "
                "        else null "
                "      else null; "
                "    forceInputs = "
                "      builtins.foldl' "
                "        (acc: k: builtins.seq (forceInputList k) acc) "
                "        null "
                "        inputListKeys; "
                "  in "
                "    builtins.seq forceSafe "
                "      (builtins.seq forceInputs ";

            const char * wrapper_tail = ")";

            installBytecodePrimop(state, "derivationStrict",
                std::string(wrapper_body)
                + " (builtins.__derivationStrictRaw args)"
                + wrapper_tail);

            installBytecodePrimop(state, "derivation",
                std::string(wrapper_body)
                + " (builtins.__derivationRaw args)"
                + wrapper_tail);
#endif  // legacy pre-force wrapper

            // Full Option 4 wrapper.  Iterates args's attrs at bytecode
            // level, coerces each non-flag-non-special attr to string
            // via `builtins.toString`, builds the env attrset + special
            // fields, then calls `__derivationFromPreprocessed`.
            //
            // For structured-attrs derivations, falls back to the C
            // primop (the wrapper doesn't yet do JSON encoding).
            //
            // The coerce uses `builtins.toString` (C primToString).
            // toString is C-recursive for nested values (list-of-
            // attrset-with-outPath), but each top-level invocation
            // adds only a SMALL C-frame chain.  The KEY: the OUTER
            // iteration (one entry per attr) runs at bytecode level —
            // no per-attr C-frame stack consumption.
            // Hoisted-structured-flag form (2026-05-18).  The previous
            // shape kept `preprocessed` and its sub-bindings (envEntries
            // / baseEnv / envWithSpecials / ...) in the outer let, then
            // gated only the FINAL select with `if structuredFlag`.
            // v3's emission was forcing those preprocessing thunks even
            // for structured-attrs derivations (where the else branch
            // never runs), tripping "OP_ATTRS_SELECT: not an attrset"
            // when an env attr like cc-wrapper's `isGNU` selector sat
            // on a string (the structured-attrs JSON shape allows env
            // values that aren't string-coercible).  Hoist the check
            // to the OUTER if so `preprocessed` enters scope only on
            // the non-structured path; structured derivations go
            // straight to `__derivationStrictRaw` with no surrounding
            // let-bindings to force eagerly.
            const char * full_wrapper =
                "args: "
                "  if args.__structuredAttrs or false "
                "  then builtins.__derivationStrictRaw args "
                "  else "
                "    let "
                "      keys = builtins.attrNames args; "
                // 2026-05-18 bash bootstrap bisection: TW's
                // primDerivationStrict EMITS `__structuredAttrs` into
                // drv.env (coerced to "" when false) — verified by
                // diffing mirrors-list.drv between v3 and TW.  Pre-fix
                // v3 listed `__structuredAttrs` in flagKeys and
                // EXCLUDED it from env, causing every non-structured
                // nixpkgs derivation that explicitly sets
                // __structuredAttrs=false to diverge from TW (drv hash
                // depends on env attr list).  The cascade tainted
                // bashNonInteractive → stdenv.shell → every derivation
                // on aarch64-darwin nixpkgs.
                //
                // The other "flags" (`__ignoreNulls`, `__contentAddressed`,
                // `impure`) are NOT in TW's emitted env even when set,
                // so they remain in flagKeys.  __structuredAttrs is
                // special: it controls JSON vs flat-env shape, but the
                // false case still flows through to env.
                "      flagKeys = [ "
                "        \"__ignoreNulls\" \"__contentAddressed\" "
                "        \"impure\" "
                "      ]; "
                // `args` is the ONLY attr that skips drv.env (TW
                // populates drv.args from it instead).  `outputs` /
                // `outputHash*` / `builder` / `system` ALL emplace
                // into drv.env in TW's primDerivationStrictNative
                // (lines 5301-5316 in primops.cc), even though they
                // also feed drv.builder / drv.platform / outputHash /
                // declaredOutputs.  Match that here — otherwise the
                // drv hash diverges.
                "      specialEnvKeys = [ \"args\" \"outputs\" ]; "
                "      isFlag = k: builtins.elem k flagKeys; "
                "      isSpecialEnv = k: builtins.elem k specialEnvKeys; "
                // Defensive bool coercion: nixpkgs may pass non-bool
                // values for these flag attrs (e.g. null), and an
                // `if (non-bool)` opcode in subsequent logic would
                // throw "v3: expected bool".  Use `== true` to force
                // a clean bool result for any non-true value.
                "      asBool = v: v == true; "
                "      ignoreNullsFlag = asBool (args.__ignoreNulls or false); "
                "      contentAddressedFlag = asBool (args.__contentAddressed or false); "
                "      impureFlag = asBool (args.impure or false); "
                // 2026-05-19 #665: use `__derivCoerce` (path-copying
                // coerce) for derivation fields that TW handles via
                // `coerceToString(copyToStore=true)`.  `builtins.toString`
                // is now non-copying (TW-compatible user-facing
                // toString), so paths-as-attr-values would leak as
                // raw source-tree paths without the explicit copy.
                // outputs/outputHash*/system are forceStringNoCtx in
                // TW (no copying ever applies — they reject paths) so
                // they can stay on `builtins.toString`.
                "      drvName = args.name; "
                "      builderStr = builtins.__derivCoerce args.builder; "
                "      systemStr = builtins.toString args.system; "
                "      outputsList = "
                "        if args ? outputs "
                "        then builtins.map builtins.toString args.outputs "
                "        else [ \"out\" ]; "
                "      outputsEnvEntry = builtins.concatStringsSep \" \" outputsList; "
                "      argsList = "
                "        if args ? args "
                "        then builtins.map builtins.__derivCoerce args.args "
                "        else [ ]; "
                "      outputHashStr = "
                "        if args ? outputHash then builtins.toString args.outputHash "
                "        else null; "
                "      outputHashAlgoStr = "
                "        if args ? outputHashAlgo then builtins.toString args.outputHashAlgo "
                "        else null; "
                "      outputHashModeStr = "
                "        if args ? outputHashMode then builtins.toString args.outputHashMode "
                "        else null; "
                "      envKeyValue = k: "
                "        if isFlag k then null "
                "        else if isSpecialEnv k then null "
                "        else if ignoreNullsFlag && (args.${k}) == null then null "
                "        else { name = k; value = builtins.__derivCoerce args.${k}; }; "
                "      envEntries = "
                "        builtins.filter (e: e != null) "
                "          (builtins.map envKeyValue keys); "
                "      baseEnv = builtins.listToAttrs envEntries; "
                // Only synthesize an `outputs` env entry when the user
                // ACTUALLY provided `outputs` in args.  TW's
                // primDerivationStrict adds it only in the explicit-
                // outputs branch (lines 5269-5294 in primops.cc).  Adding
                // it when missing creates a divergent drvPath hash.
                "      envWithSpecialsBase = "
                "        baseEnv // { "
                "          builder = builderStr; "
                "          system = systemStr; "
                "          name = drvName; "
                "        }; "
                "      envWithSpecials = "
                "        if args ? outputs "
                "        then envWithSpecialsBase // { outputs = outputsEnvEntry; } "
                "        else envWithSpecialsBase; "
                "      preprocessed = { "
                "        name = drvName; "
                "        builder = builderStr; "
                "        system = systemStr; "
                "        args = argsList; "
                "        outputs = outputsList; "
                "        env = envWithSpecials; "
                "        __ignoreNulls = ignoreNullsFlag; "
                "        __contentAddressed = contentAddressedFlag; "
                "        __impure = impureFlag; "
                "        __structuredAttrs = false; "
                "        outputHash = outputHashStr; "
                "        outputHashAlgo = outputHashAlgoStr; "
                "        outputHashMode = outputHashModeStr; "
                "      }; "
                "    in "
                "      builtins.__derivationFromPreprocessed preprocessed";

            if (!std::getenv("NIX_V3_NO_BC_DERIV_STRICT"))
                installBytecodePrimop(state, "derivationStrict", full_wrapper);

            // Also wrap `derivation` so user-facing `derivation { ... }`
            // routes through MY bytecode wrapper.  Without this, the C
            // primDerivation would call primDerivationStrict (the C
            // function pointer) directly — bypassing my wrapper.
            //
            // The body mirrors primDerivation's logic: call
            // builtins.derivationStrict (intercepted by the wrapper
            // above), then build the output attrset (args // strict //
            // {outPath; drvPath; type; outputName; drvAttrs; all;} +
            // per-output sub-attrsets).
            const char * derivation_wrapper =
                "args: "
                "  let "
                "    strict = builtins.derivationStrict args; "
                "    outputsList = "
                "      if args ? outputs "
                "      then builtins.map builtins.toString args.outputs "
                "      else [ \"out\" ]; "
                "    firstOut = builtins.head outputsList; "
                "    drvPath = strict.drvPath; "
                "    firstOutPath = strict.${firstOut}; "
                "    perOutput = o: { "
                "      inherit drvPath; "
                "      outPath = strict.${o}; "
                "      type = \"derivation\"; "
                "      outputName = o; "
                "    }; "
                "    perOutputAttrs = "
                "      builtins.listToAttrs "
                "        (builtins.map "
                "          (o: { name = o; value = perOutput o; }) "
                "          outputsList); "
                "  in "
                "    args // { "
                "      drvPath = drvPath; "
                "      outPath = firstOutPath; "
                "      type = \"derivation\"; "
                "      outputName = firstOut; "
                "      drvAttrs = args; "
                "      all = builtins.map perOutput outputsList; "
                "    } // perOutputAttrs";

            if (!std::getenv("NIX_V3_NO_BC_DERIV_TOPLEVEL"))
                installBytecodePrimop(state, "derivation", derivation_wrapper);
        }
    } catch (...) {
        // Reset `done` so a future call retries — otherwise a
        // transient error here would permanently disable bytecode
        // primops for the process.
        done = false;
        throw;
    }
}

} // namespace nix::v3
