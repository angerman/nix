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

// #705 (2026-05-20): expose the primopReplacementMap as a scavenger
// root.  Each entry's `Value` payload can be a nursery Closure*
// (the bytecode-primop install path compiles a Nix source via
// `runRootExpr` and the resulting closure may be allocated through
// the nursery).  Without this walk, the map holds a stale closure
// pointer after scavenge → next OP_LIT_PRIMOP / OP_CALL_PRIMOP
// hands the dispatch a Value that derefs into freed nursery memory.
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
    holder->name = primopName;
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

        // NOTE: a __mapMap (map∘map) fusion target was prototyped here and
        // FALSIFIED 2026-06-05 — measured NEUTRAL (insns 54000117 vs
        // 54000115; peak RSS 791 vs 791 MB on a 1M chain).  map∘map
        // eliminates only the transient intermediate spine; lazy elements
        // are forced exactly once either way and the intermediate ListVec is
        // GC-reclaimed as forcing proceeds, so there is no win at peak.  The
        // winning fusion shape eliminates the OUTPUT list too (foldl'∘map →
        // __foldlMap above).  Candidate registry + verdicts live in the
        // kRules table comment in opt_stream_fusion.cc.

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
        // O(n) via the native O(total) `concatLists` builder over a lazy
        // `map` — NOT `foldl' (acc: x: acc ++ fn x)`, whose per-element
        // `acc ++ …` copies the growing accumulator → O(n²) (Nix `++` is
        // always a full copy).  `map` keeps the spine lazy / elements
        // lazy; `concatLists` does ONE count+alloc+copy pass.
        if (!std::getenv("NIX_V3_NO_BC_CONCATMAP"))
            installBytecodePrimop(state, "concatMap",
                "fn: list: builtins.concatLists (builtins.map fn list)");

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
        //
        // O(n): tag each element with its predicate result ONCE (lazy
        // `map`), then build each side with the native O(total)
        // `concatLists` builder.  The previous `foldl' (… acc.right ++
        // [x] …)` was **O(n²)** — `acc.right ++ [x]` copies the growing
        // accumulator every match (Nix `++` is a full copy); same bug
        // class as the old filter/concatMap.  Tagging keeps `pred` to one
        // evaluation per element (matches TW primPartition's single pass
        // + left-to-right throw order); elements stay lazy.
        if (!std::getenv("NIX_V3_NO_BC_PARTITION"))
            installBytecodePrimop(state, "partition",
                "pred: list: "
                "  let tagged = builtins.map (x: { v = x; k = pred x; }) list; "
                "  in { "
                "    right = builtins.concatLists "
                "              (builtins.map (e: if e.k then [ e.v ] else []) tagged); "
                "    wrong = builtins.concatLists "
                "              (builtins.map (e: if e.k then [] else [ e.v ]) tagged); "
                "  }");

        // T3 — filter: keep elements where pred returns true.
        //
        // O(n): emit a 1- or 0-element list per element via a lazy `map`,
        // then flatten once with the native O(total) `concatLists`
        // builder.  The predicate still runs through the VM (map's
        // OP_CALL), so this stays V3-native with no per-element
        // C-recursion; elements are passed through unchanged (lazy values
        // stay lazy) — same observable semantics as TW primFilter.
        //
        // The previous definition `foldl' (acc: x: if pred x then acc ++
        // [x] else acc) [] list` was **O(n²)**: each kept element's
        // `acc ++ [x]` copies the entire growing accumulator (Nix `++` is
        // always a full copy), so n appends = 1+2+…+n element-copies.  At
        // 100k it allocated ~6 GB and OOM'd (LISTTOATTRS_QUADRATIC sibling;
        // the old comment's "matches TW append-per-match" was wrong — TW's
        // primFilter uses an amortised list builder and is O(n)).  Reverts
        // to the C-side O(n) primFilter via NIX_V3_NO_BC_FILTER=1.
        if (!std::getenv("NIX_V3_NO_BC_FILTER"))
            installBytecodePrimop(state, "filter",
                "pred: list: "
                "  builtins.concatLists "
                "    (builtins.map (x: if pred x then [ x ] else []) list)");

        // T18 — sort (Tier 2a; mergesort 2026-06-07).  Stable bottom-up
        // top-down mergesort.  The per-element CMP callback runs under
        // OP_CALL (iterative VM dispatch), keeping this V3-native.
        //
        // O(n log² n).  Pure Nix has NO O(1) append (`++` always copies),
        // so a recursive element-by-element merge (`[x] ++ rest`) would be
        // O(n²) total — no better than the insertion sort it replaces
        // (which measured O(n³): a 200k sort ran > 25 min).  Instead the
        // MERGE builds its output in ONE `genList` pass where each output
        // position k is resolved by a binary-search partition over the two
        // sorted runs (the "k-th element of two sorted arrays" trick):
        // O(log) cmp per element, no incremental list growth.  Split is
        // index-based (`genList`+`elemAt`, O(1) access).  Net O(n log² n)
        // comparisons; verified byte-identical + STABLE vs TW
        // `builtins.sort` (ints, records-by-key, reverse/dup/empty) and
        // sub-quadratic (16×n → ~1.8× time).
        //
        // Stability (TW: equal elements keep input order, left run wins):
        // the merge takes from the right run b only when `cmp b[j] a[i]`
        // is STRICT; ties resolve to a (the left/earlier run).  The
        // partition's leftBad/rightBad mirror that strictness.  Reverts to
        // the O(n log n) C primSort via NIX_V3_NO_BC_SORT=1.
        if (!std::getenv("NIX_V3_NO_BC_SORT"))
            installBytecodePrimop(state, "sort",
                "cmp: list: "
                "  let ea = builtins.elemAt; "
                "      merge = a: b: "
                "        let na = builtins.length a; nb = builtins.length b; "
                "            at = k: "
                "              let t = k + 1; lo0 = t - nb; lo = if lo0 > 0 then lo0 else 0; "
                "                  hi = if t < na then t else na; "
                "                  findAi = l: h: "
                "                    if l >= h then l "
                "                    else let ai = (l + h) / 2; bi = t - ai; "
                "                             leftBad = ai > 0 && bi < nb && cmp (ea b bi) (ea a (ai - 1)); "
                "                         in if leftBad then findAi l ai "
                "                            else let rightBad = bi > 0 && ai < na && ! (cmp (ea b (bi - 1)) (ea a ai)); "
                "                                 in if rightBad then findAi (ai + 1) h else ai; "
                "                  ai = findAi lo hi; bi = t - ai; "
                "              in if ai == 0 then ea b (bi - 1) "
                "                 else if bi == 0 then ea a (ai - 1) "
                "                 else if cmp (ea b (bi - 1)) (ea a (ai - 1)) then ea a (ai - 1) "
                "                 else ea b (bi - 1); "
                "        in builtins.genList at (na + nb); "
                "      go = lst: "
                "        let m = builtins.length lst; "
                "        in if m <= 1 then lst "
                "           else let half = m / 2; "
                "                    left = builtins.genList (i: ea lst i) half; "
                "                    right = builtins.genList (i: ea lst (half + i)) (m - half); "
                "                in merge (go left) (go right); "
                "  in go list");

        // T19 — genericClosure (Tier 2b, 2026-05-29).  BFS closure
        // computation with key-based dedup.  C primGenericClosure
        // uses `std::deque` + `unordered_set<std::string>` and calls
        // `callClosure` per item; bytecode uses list-based work-queue
        // + attrset-based seen-set + native OP_CALL for `operator it`.
        //
        // Key dedup: per-type stringification (raw for strings, toString
        // for int/float/path, "true"/"false" for bool).  Cross-type
        // collision (e.g. int 1 vs string "1") is impossible because
        // the firstType check throws on mixed types — matches the C
        // version's `firstKeyTag` rejection (and the
        // eval-fail-genericClosure-keys-incompatible-types contract).
        // NaN float keys are rejected via `k != k` (IEEE).
        //
        // 2026-05-29: initial version prefixed strings with "S"/"I"/...
        // for extra safety.  That propagated `k`'s string context into
        // the prefixed result (Nix string `+` unions context); under
        // SHADOW cache's deep-force pass on derivation inputs the
        // prefixed string surfaced as a context-bearing value whose
        // body started with "S" — libstore then rejected it as not
        // matching any valid store-path.  Dropping the prefix matches
        // the C version exactly (raw key string) and eliminates the
        // context-propagation hazard.
        //
        // Asymptotic: O(M²) where M = final result size (per-step
        // tail+head+concat are O(M)).  C is O(M) via deque/set.  For
        // typical nixpkgs uses (M ≤ 100) this is fine.  Reverts via
        // NIX_V3_NO_BC_GENERIC_CLOSURE=1.
        if (!std::getenv("NIX_V3_NO_BC_GENERIC_CLOSURE"))
            installBytecodePrimop(state, "genericClosure",
                "arg: "
                "  let "
                "    startSet = arg.startSet; "
                "    operator = arg.operator; "
                "    keyToStr = k: "
                "      let t = builtins.typeOf k; in "
                "      if t == \"string\" then k "
                "      else if t == \"int\" then builtins.toString k "
                "      else if t == \"float\" then "
                "        (if k != k then throw \"NaN key is not orderable\" "
                "         else builtins.toString k) "
                "      else if t == \"path\" then toString k "
                "      else if t == \"bool\" then (if k then \"true\" else \"false\") "
                "      else throw \"'key' must be string / int / float / path / bool\"; "
                "    go = work: result: seen: firstType: "
                "      if work == [] then result "
                "      else "
                "        let "
                "          it      = builtins.head work; "
                "          rest    = builtins.tail work; "
                "          k       = it.key; "
                "          curType = builtins.typeOf k; "
                "          newType = "
                "            if firstType == null then curType "
                "            else if firstType == curType then firstType "
                "            else throw \"cannot compare keys of incompatible types\"; "
                // unsafeDiscardStringContext: when k is a string with
                // store-path context (common in nixpkgs derivation
                // attrs), Nix's dynamic-attr-key opcodes preserve
                // context on the intermediate `ks` value.  Under
                // SHADOW-cache forceDeep, that context can leak into
                // downstream derivation env entries with a string body
                // that doesn't match the context's store path —
                // libstore then rejects "string not allowed to refer
                // to a store path".  Stripping context here matches
                // the C primGenericClosure exactly (it copies
                // k.payload.str into a std::string, dropping context).
                "          ks      = builtins.unsafeDiscardStringContext (keyToStr k); "
                "        in "
                "          builtins.seq newType ( "
                "            if seen ? ${ks} "
                "            then go rest result seen newType "
                "            else "
                "              let next = operator it; in "
                "              go (rest ++ next) (result ++ [it]) (seen // { ${ks} = null; }) newType "
                "          ); "
                "  in go startSet [] {} null");

        // T20 — zipAttrsWith.  DEFAULT = the native C primZipAttrsWith
        // (O(N × K_total) via unordered_map, lazy Tag::App entries, fn
        // applied via callClosure — flat loop, no per-key C-recursion).
        //
        // The bytecode version below is OPT-IN ONLY (NIX_V3_BC_ZIP_ATTRS_WITH=1)
        // because it is **O(n²)** (measured 2026-06-07: 16k sets = 0.96 s/499 MB
        // vs the C primop's 0.13 s/41 MB; 32k = 1875 MB).  Two quadratic costs:
        // the name-union `foldl' (a: b: a // b) {} sets` (++/// accumulation —
        // same antipattern as the old filter/sort), AND `catAttrs name sets`
        // re-walked once PER name = O(N × S).  The C version is lazy too (Tag::
        // App entries), so the bytecode form has no compensating advantage —
        // it was a blanket Tier-2c V3-native install that regressed.  Verified
        // byte-identical (value-list order, dup keys, fn application) + --core
        // 20/20 on the C path.
        if (std::getenv("NIX_V3_BC_ZIP_ATTRS_WITH"))
            installBytecodePrimop(state, "zipAttrsWith",
                "fn: sets: "
                "  let "
                "    allNames = "
                "      builtins.attrNames "
                "        (builtins.foldl' (a: b: a // b) {} sets); "
                "  in builtins.listToAttrs "
                "       (builtins.map "
                "         (name: { inherit name; "
                "                  value = fn name (builtins.catAttrs name sets); }) "
                "         allNames)");

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
