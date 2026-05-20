/// @file
/// v3 root-expression entry point implementation.
///
/// Mirrors the pipeline open-coded in `cli/v3-eval.cc:430-438`.  Lifted
/// to a shared helper so the integrated `nix` CLI can use the same
/// path under v3-direct (inversion phase 1).
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/run.hh"
#include "v3/lower.hh"
#include "v3/vm.hh"
#include "v3/primop.hh"
#include "v3/ir.hh"
#include "v3/alloc.hh"
#include "v3/bytecode_primops.hh"
#include "v3/limits.hh"

#include "nix/expr/eval.hh"

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace nix::v3 {

namespace {

/// Phase-timing helper: present iff `V3_TIMING` env var is set.  We
/// cache the env-var lookup once per process via a static-const-bool;
/// mirrors the pattern used throughout vm.cc / primops.cc for
/// hot-path env-var checks (see #538 follow-ups).
struct PhaseTimer {
    using Clock = std::chrono::steady_clock;
    using TP = Clock::time_point;
    bool active;
    TP start;
    double lower_ms = 0, compile_ms = 0, optimise_ms = 0, run_ms = 0;
    explicit PhaseTimer() : active(s_active())
    {
        if (active) start = Clock::now();
    }
    void mark(double & accum)
    {
        if (!active) return;
        TP now = Clock::now();
        accum += std::chrono::duration<double, std::milli>(now - start).count();
        start = now;
    }
    ~PhaseTimer()
    {
        if (!active) return;
        // Sum bridge time (TW-side) from the per-kind telemetry so
        // we can report the v3-VM / TW split.  Returns 0 unless
        // NIX_V3_BRIDGE_TIMING=1 was set; in that case the bridge
        // callbacks accumulate wall-time at each call site.
        double bridge_ms = bridgeTotalNs() / 1e6;
        // VM-time is run_ms minus bridge_ms (the time spent inside
        // TW callbacks reached from v3, otherwise accounted under
        // run).  Clamp negative (clock granularity) to 0.
        double vm_ms = run_ms - bridge_ms;
        if (vm_ms < 0) vm_ms = 0;
        std::fprintf(stderr,
            "v3-direct timing (ms): lower=%.3f optimise=%.3f compile=%.3f "
            "run=%.3f vm=%.3f bridge=%.3f\n",
            lower_ms, optimise_ms, compile_ms, run_ms, vm_ms, bridge_ms);
        // If bridge timing is enabled, also dump the per-kind
        // breakdown so we can see WHERE the TW time goes.
        if (bridgeTimingEnabled())
            dumpBridgeTelemetry(stderr);
    }
private:
    static bool s_active()
    {
        static const bool v = std::getenv("V3_TIMING") != nullptr;
        return v;
    }
};

} // anonymous namespace

/// Force-link the v3 library.  Called once from `mainWrapped` so the
/// linker's `-dead_strip_dylibs` pass keeps libnixexprv3 in the
/// binary.  No side effects.
bool keepLibAlive()
{
    return true;
}

RootResult runRootExpr(nix::EvalState & state, nix::Expr * e)
{
    // Idempotent: register the builtin primop table on first call.
    // Safe to call per-invocation — the underlying registry is global
    // and de-duplicates by name.  The registration cost is constant
    // (one-time map fill) so per-call overhead is negligible.
    registerBuiltinPrimOps();

    // Wire the global tlNixEvalState pointer so v3 primops that need
    // to reach back into TW (e.g. `import`, derivation strict-merge,
    // store-side path operations) can find it.  Caller is responsible
    // for keeping `state` alive for the lifetime of any returned Bridge
    // thunks; see `primops.cc treeWalkerToV3` nFunction case for the
    // address-stability contract.
    setNixEvalState(&state);

    // Phase 1.6 — initialise resource limits.  Idempotent across
    // subsequent runRootExpr calls.  Reads NIX_V3_MAX_HEAP /
    // NIX_V3_MAX_CPU_TIME / NIX_V3_MAX_WALL_TIME and installs the
    // Boehm OOM handler if heap cap is set.  Cheap (one mutex + a
    // boolean check) when re-entered.
    initLimits();

    // A12b T0: install bytecode replacements for callback primops
    // (foldl' / map / filter / etc.) once per process.  The install
    // itself runs runRootExpr recursively to compile each primop's
    // Nix source; the function has its own thread-local guard that
    // short-circuits on recursive entry so we don't loop.  Disable
    // with NIX_V3_NO_BYTECODE_PRIMOPS=1 for A/B comparison.
    static const bool s_noBytecodePrimops =
        std::getenv("NIX_V3_NO_BYTECODE_PRIMOPS") != nullptr;
    if (!s_noBytecodePrimops)
        installAllBytecodePrimops(state);

    // V3_TIMING phase split — capture lower / compile / run / bridge
    // phase durations so bench harnesses can attribute time.  A no-op
    // (zero overhead) when V3_TIMING is unset.
    PhaseTimer pt;

    // Lower the AST → IR → bytecode.  `lowerNixExpr` requires `e` to
    // have had `bindVars` applied; the caller's contract.
    auto module = lowerNixExpr(e, state.symbols, state.positions);
    pt.mark(pt.lower_ms);

    // #538: run the IR optimization pipeline (constant fold, CSE,
    // strictness, alias inline, primop fuse, DCE).  Without this the
    // v3-direct path emits massive amounts of redundant SET_LOCAL /
    // GET_LOCAL through trivial bindings, plus per-LitInt force
    // overhead — the lowerer's A-normal-form-style binding-per-
    // subexpression pattern bloats the bytecode unless the optimizer
    // collapses VarRef chains and elides redundant Forces.  The
    // import-primop path (`primops.cc primImport`) already does this;
    // the runRootExpr path silently skipped it before this fix.
    static const bool s_noOptimise =
        std::getenv("NIX_V3_NO_OPTIMISE") != nullptr;
    if (!s_noOptimise) ir::optimise(module);
    pt.mark(pt.optimise_ms);

    // computeFreeVars: populates each `ir::Function::freeVars` from
    // `Function::vars`.  Required before `compile` so the emitter
    // knows which upvalues each closure captures.  Must run AFTER
    // `optimise` so any newly-introduced VarRef aliases are walked.
    ir::computeFreeVars(module);

    // Compile IR to bytecode.  The CompilationUnit owns
    // `stringConstants` referenced by OP_LIT_STR / OP_LIT_PATH; the
    // resulting Value's string/path payloads point into that vector.
    //
    // #676 — heap-allocate the CU via make_unique so its address is
    // stable across any RootResult moves the caller may do (e.g.
    // `std::optional::emplace` in eval.cc --apply).  Closures emitted
    // by `run()` capture `c->cu = &cu` at OP_MAKE_CLOSURE time; if
    // the CU lived inside RootResult by value, a subsequent move
    // would leave every captured cu pointer dangling.  Pre-#676 this
    // manifested as a SIGTRAP on `nix eval --impure --apply '(x: 42)'
    // --expr '1'` — the closure's stale cu pointer made dispatchLoop
    // read garbage bytecode.
    RootResult out{std::make_unique<CompilationUnit>(compile(module)), Value{}};
    pt.mark(pt.compile_ms);

    // Run.  STG-10 (vm.cc:5530) automatically routes through
    // `runOnExistingVm` if we're re-entered from another v3 dispatch
    // loop — so calling `runRootExpr` from inside a primop is safe.
    out.value = run(*out.cu);
    pt.mark(pt.run_ms);

    // NIX_VM_STATS=1: dump alloc counters at completion.  Lets us
    // attribute alloc explosions to thunks vs closures vs Bindings
    // vs lists.
    static const bool s_dumpStats =
        std::getenv("NIX_VM_STATS") != nullptr;
    if (__builtin_expect(s_dumpStats, 0)) {
        const auto & a = allocStats();
        std::fprintf(stderr,
            "v3-direct alloc: values=%llu closures=%llu thunks=%llu "
            "lists=%llu attrsets=%llu pairs=%llu thunksForced=%llu bridge=%llu insns=%llu\n",
            (unsigned long long)a.valuesAllocated,
            (unsigned long long)a.closuresAllocated,
            (unsigned long long)a.thunksAllocated,
            (unsigned long long)a.listsAllocated,
            (unsigned long long)a.attrsetsAllocated,
            (unsigned long long)a.pairsAllocated,
            (unsigned long long)a.thunksForced,
            (unsigned long long)a.bridgeThunksForced,
            (unsigned long long)a.bytecodeInstructions);
        // #702: BYTES per allocation category.  The count counters
        // above are partly bumped at primop call sites and miss
        // Alloc::* invocations from vm.cc dispatch; the byte
        // counters are bumped inside Alloc::* itself so they are
        // authoritative.  Use them to attribute non-Boehm RSS
        // growth (per `hello-drvpath-analysis.md`: the 4 GB on
        // hello.drvPath lives outside Boehm — these byte counters
        // tell us which v3 subsystem owns the growth).
        const uint64_t totalAllocBytes =
              a.bytesValues + a.bytesClosures + a.bytesThunks + a.bytesEnvs
            + a.bytesLists  + a.bytesBindings + a.bytesPairs   + a.bytesChars;
        std::fprintf(stderr,
            "v3-direct bytes (in arena/nursery): values=%.1fMB closures=%.1fMB "
            "thunks=%.1fMB envs=%.1fMB lists=%.1fMB bindings=%.1fMB pairs=%.1fMB "
            "chars=%.1fMB total_alloc=%.1fMB arena_pinned=%.1fMB\n",
            a.bytesValues   / 1e6,
            a.bytesClosures / 1e6,
            a.bytesThunks   / 1e6,
            a.bytesEnvs     / 1e6,
            a.bytesLists    / 1e6,
            a.bytesBindings / 1e6,
            a.bytesPairs    / 1e6,
            a.bytesChars    / 1e6,
            totalAllocBytes / 1e6,
            threadArena().bytesAllocated() / 1e6);
        // #660 verification: dump bridge-primop call counts.  v3-eval
        // already does this via its own NIX_VM_STATS path; mirror here
        // so the integrated `nix` CLI (and any future v3 driver that
        // goes through `runRootExpr`) reports the same data without
        // depending on the CLI specifically.
        dumpPrimOpStats(stderr);
    }
    return out;
}

} // namespace nix::v3
