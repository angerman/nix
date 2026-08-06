/// @file
/// Bytecode-primop infrastructure — T0 of the A12b architectural
/// refactor.  See `include/v3/bytecode_primops.hh` for rationale.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
/// Input Output Group.  SPDX-License-Identifier: Apache-2.0

#include "v3/bytecode_primops.hh"
#include "v3/run.hh"
#include "v3/value.hh"
#include "v3/barrier.hh"  // Phase D write-barrier helpers
#include "v3/bytecode.hh"
#include "v3/primop.hh"
#include "v3/ir.hh"
#include "v3/alloc.hh"
// PARSER_PROJECT_PLAN §5.3 site 5: the wrapper sources are parsed+lowered+
// run via runRootExprFromString (run.hh), and the TW-builtin install goes
// through ffi::setTreeWalkerBuiltin — so no direct TW / parser / position
// headers here.  ffi.hh provides the ffi shims + the nix::Value /
// nix::EvalState forward-decls (for the v3ToTreeWalkerPublic decl + param).
#include "v3/ffi.hh"

// Forward declaration: defined in vm.cc.
namespace nix::v3 {
Value getBuiltinsValue() noexcept;
Value * peekBuiltinsValue() noexcept;  // #705
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


namespace nix::v3 {

namespace {

/// Process-global storage that keeps installed bytecode primops alive.
///
/// The Closure Value's `asClosure()->cu` field references the
/// CompilationUnit by pointer.  The pointer is set during `run(cu)`
/// inside `runRootExpr`, so it points to wherever the cu lived at
/// that moment.  Any subsequent MOVE of the cu invalidates the
/// pointer.  Fix: store the RootResult AS-IS (cu + value together)
/// in a heap-allocated holder, and patch `closure->cu` to point at
/// the holder's final cu location.
struct InstalledPrimop {
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
///   - `vm.cc` OP_LIT_PRIMOP (only) to push the replacement instead of
///     a Tag::PrimOp Value (so `let f = builtins.foldl'; in f a b c`
///     and similar dynamic dispatch see the closure).  OP_CALL_PRIMOP
///     does NOT consult it.
///   - `opt_primop_fuse.cc` to SKIP fusing a replaced primop into a
///     saturated PrimOpCall (→ OP_CALL_PRIMOP), so `builtins.foldl' a b c`
///     routes through the App-chain → OP_LIT_PRIMOP redirect → OP_CALL
///     instead of the static fast path.
std::unordered_map<const PrimOp *, Value> & primopReplacementMap()
{
    static std::unordered_map<const PrimOp *, Value> m;
    return m;
}

} // anonymous namespace

// #705 (2026-05-20): expose the primopReplacementMap as a scavenger
// root.  Each entry's `Value` payload can be a nursery Closure*
// (the bytecode-primop install path compiles a Nix source via
// `runRootExpr` and the resulting closure may be allocated through
// the nursery).  Without this walk, the map holds a stale closure
// pointer after scavenge → the next OP_LIT_PRIMOP hands the dispatch
// a Value that derefs into freed nursery memory.
//
// Discovered 2026-05-21 while hunting the hello.drvPath scavenge
// SIGSEGV.  The crash signature (`desc = nullptr` in forceValue
// after chase through Tag::Thunk steps) traces back to dispatch
// reading a stale closure handed out via `lookupPrimopReplacement`.
void walkBytecodePrimopRoots(const std::function<void(Value &)> & visit)
{
    for (auto & [po, v] : primopReplacementMap()) {
        (void)po;
        visit(v);
    }
    // GC_AUDIT_ROUND_2 N12: each `InstalledPrimop::rr.value` carries the
    // same closure also held in `primopReplacementMap`.  Today nothing
    // re-reads `rr.value` (the map is the only consumer), so failing to
    // walk it is latent — but it IS the holder's record of the live
    // closure; if any future code reads `installed.rr.value` after a
    // scavenge it would see a stale nursery pointer.  Walking both
    // copies costs O(nPrimops) and keeps the redundant slot consistent
    // with primopReplacementMap.
    for (auto & up : installedPrimops()) {
        if (up) visit(up->rr.value);
    }
}

void walkBuiltinsRoot(const std::function<void(Value &)> & visit)
{
    // Only walk if the singleton has been materialised.  Otherwise
    // there's nothing reachable through it yet (and triggering
    // materialisation during scavenge would re-enter the allocator
    // path we're about to reset).
    if (Value * b = peekBuiltinsValue()) visit(*b);
}

const Value * lookupPrimopReplacement(const PrimOp * po) noexcept
{
    if (!po) return nullptr;
    auto & m = primopReplacementMap();
    auto it = m.find(po);
    return it == m.end() ? nullptr : &it->second;
}

// T-8 (CODEBASE_REVIEW_2026-06-11): query whether a primop has a bytecode
// override (installBytecodePrimop records it in installedNames()).  Used by
// opt_strictness::producesWHNF to distrust its always-WHNF whitelist for an
// overridden primop whose bytecode impl's WHNF-ness is unknown.
bool isBytecodePrimopInstalled(std::string_view name)
{
    auto & s = installedNames();
    return s.find(std::string(name)) != s.end();
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

    // §5.3 site 5: native parse+lower+run the wrapper source (no nix::Expr).
    // These are fixed internal expressions (lambdas + `builtins.X`, no path
    // literals) — routed through the single synthetic-source entry
    // `runRootExprFromString`, which owns the parse + canLowerV3 + lower
    // plumbing, so this file needs no TW / parser / position headers.
    // The inner `installAllBytecodePrimops` call would re-enter here, so we
    // guard with `tl_installInProgress`.
    bool wasInProgress = tl_installInProgress;
    tl_installInProgress = true;
    RootResult rr = runRootExprFromString(state, nixSource);
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
    holder->rr = std::move(rr);
    InstalledPrimop * installedPtr = holder.get();
    installedPrimops().push_back(std::move(holder));
    auto & installed = *installedPtr;

    // TW_VALUE_ERADICATION F4 (2026-06-02): install "path 1" — mutating the
    // bytecode closure into TW's builtins via a v3ToTreeWalkerPublic bridge,
    // gated NIX_V3_KEEP_TW_BUILTINS_MUTATION — is DELETED with the bridge
    // apparatus.  It was default-off since #697 (it caused a 16× cardano-node
    // slowdown).  v3-direct dispatch never needed it: path 2
    // (primopReplacementMap, read by OP_LIT_PRIMOP / lower.cc) + path 3
    // (vBuiltins patch) carry the bytecode replacement entirely.

    // Install path 2: register in v3's side-table keyed by v3 PrimOp
    // pointer.  This is what makes v3's OP_LIT_PRIMOP dispatch see the
    // replacement — v3 has its own builtins attrset (getBuiltinsValue in
    // vm.cc) built from the v3 PrimOp registry, bypassing TW's builtins
    // entirely.  OP_LIT_PRIMOP (vm.cc) consults `lookupPrimopReplacement(po)`
    // and pushes the closure in place of the Tag::PrimOp; opt_primop_fuse.cc
    // consults it too, to keep replaced primops OFF the saturated
    // OP_CALL_PRIMOP fast path.  OP_CALL_PRIMOP itself does NOT consult it.
    const PrimOp * po = findPrimOp(primopName);
    if (!po) {
        // Should not happen: getBuiltin succeeded above, so the primop
        // is in TW's registry — but the v3 registry is independent.
        // Most primops are dual-registered (in both); if not, the
        // OP_LIT_PRIMOP redirect won't fire and the installed closure
        // is only visible to dynamic TW lookups.
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
        if (vBuiltins.isAttrs() && vBuiltins.asAttrs()) {
            SymbolId sid = ir::globalInternSymbol(primopName);
            Bindings * b = vBuiltins.asAttrs();
            for (uint32_t i = 0; i < b->size; ++i) {
                if (b->entries[i].name == sid) {
                    bindingsSetValue(b, i, installed.rr.value);  // Phase D barrier
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
        //   - opt_primop_fuse.cc skips fusing a replaced primop into a
        //     static OP_CALL_PRIMOP, forcing the call through the generic
        //     App-chain → OP_LIT_PRIMOP redirect → OP_CALL path above.
        //   - installBytecodePrimop also patches v3's static vBuiltins
        //     in place so dynamic lookups of `builtins.foo` see the
        //     replacement.
        //
        // ────────────────────────────────────────────────────────────────
        // Bytecode list/data primops — RETIRED 2026-08 (cleanup batch 2).
        //
        // A family of callback-heavy list/data primops (foldl', map, all,
        // any, concatMap, partition, filter, sort, genericClosure,
        // zipAttrsWith) was reimplemented in Nix source and installed via
        // the hook above to move per-element iteration off the C stack.
        //
        // VERDICT — bench/bc-vs-cpp.sh (3 regimes: per-element / per-call /
        // fusable-chain; memory project_bytecode_primop_regressions_
        // 2026-06-07): every bytecode reimpl LOSES to the native C primop in
        // every regime and config (C++ +395…4630 ns/call; even best-case
        // stream fusion ~2x behind), and several were also O(n^2) via `++` /
        // `//` accumulation (Nix has no O(1) append).  All the opt-in
        // NIX_V3_BC_<NAME> handles + the NIX_V3_BYTECODE_PRIMOP_SELFTEST
        // probe are removed — the native C primops are the sole path.
        //
        // ONE EXCEPTION stays bytecode-DEFAULT — groupBy — for LAZINESS
        // correctness, not perf: C primGroupBy's deepForceList force-
        // evaluates list ELEMENTS before applying keyFn, so it throws on a
        // `throw` element a lazy keyFn would skip (TW = lazy, C++ THROWS).
        // The bytecode form is lazy-correct + measured linear.  (filter and
        // partition were once exceptions too; both C primops were since made
        // lazy-correct — primFilter's deepForceList was removed — so they
        // flipped to the C default, which also wins on perf.)  Opt OUT of the
        // bytecode groupBy via NIX_V3_NO_BC_GROUPBY=1 (reverts to the C
        // primop, which is a laziness divergence — diagnostic only).
        // ────────────────────────────────────────────────────────────────
        // T10 — groupBy: group list elements by key-fn result.
        //   { ${fn x}: [matching xs] for each x in list }
        // Built on `builtins.foldl'` (now the native C primop).  Uses
        // `acc.${key} or []` to accumulate per-key lists.
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
