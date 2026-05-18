/// @file
/// v3-eval — CLI for the v3 evaluator.
///
/// Usage:
///   v3-eval [--file PATH | --expr EXPR] [--json] [--strict]
///   v3-eval EXPR                        # default: --expr
///
/// Reads a Nix expression (from --expr argument, stdin/--file), runs it
/// through v3 (parse → bindVars → lower → compile → run), and prints
/// the resulting value.
///
///   --file PATH      read the expression from PATH ('-' = stdin)
///   --expr EXPR      use EXPR (mutually exclusive with --file)
///   --json           print the result as JSON (forces evaluation of
///                    nested thunks)
///   --strict         force-evaluate the result deeply before printing
///                    (default: just WHNF)
///
/// IR dump modes (for LLVM-FileCheck-style testing of optimizer passes):
///   --emit-ir        dump POST-optimisation IR to stdout and exit
///                    (suppresses normal evaluation + value print)
///   --emit-ir-raw    dump PRE-optimisation (lowered, no opt passes) IR
///   --no-opt         alias for emit/eval without running optimise()
///
/// See lode/IR_CHECK_INFRASTRUCTURE_PLAN_2026-05-18.md for the
/// IR-CHECK design that consumes these flags.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/lower.hh"
#include "v3/vm.hh"
#include "v3/primop.hh"
#include "v3/alloc.hh"
#include "v3/disasm.hh"
#include "v3/ir.hh"
#include "v3/ir_dump.hh"
#include "v3/limits.hh"
#include "v3/print.hh"

#include "nix/expr/eval.hh"
#include "nix/expr/eval-gc.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/store/store-open.hh"
#include "nix/store/globals.hh"
#include "nix/main/shared.hh"
#include "nix/util/canon-path.hh"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using nix::v3::Value;
using nix::v3::Tag;

// Inversion phase 1 step 1: forceDeep / printNixValue / toJsonValue
// moved to `v3/print.hh` so the integrated `nix` CLI can reuse the
// same surface form under v3-direct.  Using-declarations bring them
// into the local namespace so the rest of v3-eval.cc reads unchanged.
using nix::v3::forceDeep;
using nix::v3::printNixValue;
using nix::v3::toJsonValue;

static int printValue(const Value & r, bool jsonOut,
                      const std::vector<std::string> & symTab)
{
    if (jsonOut) {
        std::cout << toJsonValue(r, symTab).dump() << "\n";
        return 0;
    }
    printNixValue(std::cout, r, symTab);
    std::cout << "\n";
    return 0;
}

static std::string slurp(const std::string & path)
{
    if (path == "-") {
        std::stringstream ss;
        ss << std::cin.rdbuf();
        return ss.str();
    }
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open '" + path + "'");
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void usage(const char * argv0)
{
    std::fprintf(stderr,
        "usage: %s [--file PATH | --expr EXPR] [--json] [--strict]\n"
        "       %s EXPR\n",
        argv0, argv0);
}

// IR dump mode: which point in the pipeline to dump from.
enum class IrDumpMode {
    None,       // normal eval, no dump
    PostOpt,    // --emit-ir: dump after lower + optimise + computeFreeVars
    PreOpt,     // --emit-ir-raw: dump after lower only (before optimise)
};

int main(int argc, char ** argv)
{
    std::string path, expr;
    bool jsonOut = false, strict = false;
    // IR-CHECK MVP (2026-05-18): IR dump mode + opt control.
    IrDumpMode irDumpMode = IrDumpMode::None;
    bool noOpt = false;  // --no-opt: skip optimise() entirely
    // Extra search-path entries (each is either "PATH" or "NAME=PATH").
    // Mirrors `nix-instantiate -I` so the lang test runner's per-test
    // .flags files (which reference `-I lang/dir1` etc.) work.
    std::vector<std::string> extraSearchPath;
    // Auto-args: `--arg NAME EXPR` / `--argstr NAME STR`.  Used by the
    // autoargs lang test.
    std::vector<std::pair<std::string, std::string>> autoArgs;
    std::vector<std::pair<std::string, std::string>> autoArgsStr;
    // -A path.path.path: select an attrset member from the result.
    std::string attrPath;
    // Experimental-feature flags collected from CLI; applied AFTER
    // initNix() so the Config-system setter has a chance to take.
    std::vector<std::string> extraExperimentalFeatures;
    std::string              experimentalFeaturesOverride;

    for (int i = 1; i < argc; ++i) {
        std::string_view a(argv[i]);
        if      (a == "--file" && i + 1 < argc) path = argv[++i];
        else if (a == "--expr" && i + 1 < argc) expr = argv[++i];
        else if (a == "--json")     jsonOut = true;
        else if (a == "--strict")   strict = true;
        else if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        else if (a == "-I" && i + 1 < argc)
            extraSearchPath.emplace_back(argv[++i]);
        else if (a == "-A" && i + 1 < argc)
            attrPath = argv[++i];
        else if (a == "--arg" && i + 2 < argc) {
            std::string n = argv[++i]; std::string v = argv[++i];
            autoArgs.emplace_back(std::move(n), std::move(v));
        }
        else if (a == "--argstr" && i + 2 < argc) {
            std::string n = argv[++i]; std::string v = argv[++i];
            autoArgsStr.emplace_back(std::move(n), std::move(v));
        }
        // Silently accept (and ignore) flags that the upstream
        // lang.flags files pass through but that don't affect the
        // result we compare against — warnings, lint passes, etc.
        else if (a == "--lint-absolute-path-literals" ||
                 a == "--lint-short-path-literals") {
            // takes one argument (warn|fatal|off) — skip it
            if (i + 1 < argc) ++i;
        }
        else if (a == "--abort-on-warn" || a == "--show-trace" ||
                 a == "--no-show-trace") {
            // ignore
        }
        else if (a == "--extra-experimental-features" && i + 1 < argc) {
            // Apply *after* initNix() — see below.
            extraExperimentalFeatures.emplace_back(argv[++i]);
        }
        else if (a == "--experimental-features" && i + 1 < argc) {
            experimentalFeaturesOverride = argv[++i];
        }
        // IR-CHECK MVP (2026-05-18).
        else if (a == "--emit-ir")        irDumpMode = IrDumpMode::PostOpt;
        else if (a == "--emit-ir-raw")    irDumpMode = IrDumpMode::PreOpt;
        else if (a == "--no-opt")         noOpt = true;
        else if (!a.empty() && a[0] == '-') {
            // Unknown flag — quietly ignore so test runners can pass
            // nix-instantiate flags without v3-eval refusing them.
        }
        else { expr = argv[i]; }
    }

    if (expr.empty() && path.empty()) { usage(argv[0]); return 2; }

    try {
        nix::initNix();
        nix::initGC();

        // Apply experimental-feature flags collected from the CLI.
        // Has to happen *after* initNix so the Config setter takes
        // effect against the loaded nix.conf state.
        if (!experimentalFeaturesOverride.empty())
            nix::experimentalFeatureSettings.set(
                "experimental-features", experimentalFeaturesOverride);
        for (auto & v : extraExperimentalFeatures)
            nix::experimentalFeatureSettings.set(
                "extra-experimental-features", v);

        // Read-only mode: makes derivationStrict + builtins.path
        // compute store paths *locally* (via the Nix derivation hash
        // protocol) instead of writing to a real store.  We need this
        // for tests that expect concrete /nix/store/<hash>-name paths
        // from purely-evaluated derivations.
        nix::settings.readOnlyMode = true;
        auto store = nix::openStore("dummy://");
        nix::fetchers::Settings fetchSettings{};
        bool readOnlyMode = true;
        nix::EvalSettings evalSettings{readOnlyMode};
        evalSettings.nixPath = {};

        // Build the LookupPath from -I flags (highest priority) and the
        // NIX_PATH env var (lower).  Both feed `<foo>` lookup and
        // `builtins.findFile` / `__nixPath`.
        nix::Strings rawSearchPath;
        for (auto & e : extraSearchPath) rawSearchPath.emplace_back(e);
        if (const char * np = std::getenv("NIX_PATH"); np && *np) {
            std::string s(np);
            // NIX_PATH is colon-separated.
            size_t start = 0;
            while (start <= s.size()) {
                size_t end = s.find(':', start);
                if (end == std::string::npos) end = s.size();
                if (end > start) rawSearchPath.emplace_back(s.substr(start, end - start));
                start = end + 1;
            }
        }
        auto lookupPath = nix::LookupPath::parse(rawSearchPath);

        nix::EvalState state(lookupPath, store, fetchSettings, evalSettings, nullptr);

        nix::Expr * e;
        if (!path.empty() && path != "-") {
            // Use parseExprFromFile so relative imports inside the file
            // resolve against the file's own directory, matching
            // tree-walker behaviour.
            std::filesystem::path abs = std::filesystem::absolute(path);
            e = state.parseExprFromFile(nix::SourcePath(state.rootFS, nix::CanonPath(abs.string())));
        } else {
            if (path == "-") {
                try { expr = slurp(path); }
                catch (const std::exception & ex) {
                    std::fprintf(stderr, "v3-eval: %s\n", ex.what());
                    return 1;
                }
            }
            // Anchor relative paths inside the expression to the current
            // working directory (matching `nix-instantiate --eval --expr`
            // behaviour).  Without this, `./foo` lowers to `/foo`.
            std::string cwd = std::filesystem::current_path().string();
            e = state.parseExprFromString(expr, state.rootPath(nix::CanonPath(cwd)));
        }
        e->bindVars(state, state.staticBaseEnv);

        nix::v3::registerBuiltinPrimOps();
        nix::v3::setNixEvalState(&state);
        // Phase 1.6: read NIX_V3_MAX_HEAP / NIX_V3_MAX_CPU_TIME /
        // NIX_V3_MAX_WALL_TIME and arm the dispatch-loop poll.
        nix::v3::initLimits();

        auto m = nix::v3::lowerNixExpr(e, state.symbols, state.positions);

        // IR-CHECK MVP path: when --emit-ir / --emit-ir-raw is set,
        // dump the IR at the requested phase and exit BEFORE compile.
        //
        // Note: normal v3-eval eval (no --emit-ir flag) intentionally
        // skips ir::optimise() — that's a pre-existing v3-eval design
        // (it calls compile() directly, not runRootExpr() which is
        // the runtime path that runs optimise).  We don't change that
        // default here; --emit-ir gives explicit control via --no-opt.
        if (irDumpMode != IrDumpMode::None) {
            if (irDumpMode == IrDumpMode::PostOpt && !noOpt)
                nix::v3::ir::optimise(m);
            // computeFreeVars BEFORE the dump so freeVars=[...] fields
            // are populated (otherwise every Lambda shows nUp=0 and
            // RAW-mode fixtures can't tell capture-bearing lambdas
            // apart from capture-free ones).
            nix::v3::ir::computeFreeVars(m);
            std::cout << nix::v3::ir::dumpModule(m);
            return 0;
        }

        nix::v3::ir::computeFreeVars(m);
        auto cu = nix::v3::compile(m);
        Value r = nix::v3::run(cu);

        // If --arg/--argstr were given, the file's top-level expression
        // is expected to be a function (typically with formal args).
        // Compile each arg expression in its own CU, run it, then call
        // the top-level function with a single attrset containing all
        // the autoargs.  This mirrors `nix-instantiate --arg`.
        if (!autoArgs.empty() || !autoArgsStr.empty()) {
            nix::v3::VMState vmA;
            vmA.frames.push_back(nix::v3::CallFrame{
                .cu = &cu, .closure = nullptr, .thunk = nullptr,
                .ip = cu.entryOffset, .stackBaseOffset = 0,
                .withStackBase = 0, .flags = 0,
            });
            // Build the args attrset.
            std::vector<std::pair<nix::v3::SymbolId, Value>> argEntries;
            auto cwdSrc = state.rootPath(nix::CanonPath(std::filesystem::current_path().string()));
            for (auto & [n, v] : autoArgs) {
                nix::Expr * ae = state.parseExprFromString(v, cwdSrc);
                ae->bindVars(state, state.staticBaseEnv);
                auto am = nix::v3::lowerNixExpr(ae, state.symbols, state.positions);
                nix::v3::ir::computeFreeVars(am);
                auto * acu = new nix::v3::CompilationUnit(nix::v3::compile(am));
                Value av = nix::v3::run(*acu);
                argEntries.emplace_back(nix::v3::ir::globalInternSymbol(n), av);
            }
            for (auto & [n, v] : autoArgsStr) {
                Value sv;
                char * buf = static_cast<char *>(std::malloc(v.size() + 1));
                std::memcpy(buf, v.data(), v.size()); buf[v.size()] = '\0';
                sv.mkString(buf);
                argEntries.emplace_back(nix::v3::ir::globalInternSymbol(n), sv);
            }
            std::sort(argEntries.begin(), argEntries.end(),
                      [](auto & a, auto & b) { return a.first < b.first; });
            auto * argsB = nix::v3::Alloc::allocBindings(static_cast<uint32_t>(argEntries.size()));
            for (size_t i = 0; i < argEntries.size(); ++i) {
                argsB->entries[i].name  = argEntries[i].first;
                argsB->entries[i].value = argEntries[i].second;
            }
            Value argsVal;
            argsVal.tag_payload = static_cast<uint64_t>(nix::v3::Tag::Attrs);
            argsVal.payload.bindings = argsB;
            r = nix::v3::forceValue(vmA, r);
            r = nix::v3::callClosure(vmA, r, argsVal);
        }

        // -A path.path: descend into the result attrset.
        if (!attrPath.empty()) {
            nix::v3::VMState vmS;
            vmS.frames.push_back(nix::v3::CallFrame{
                .cu = &cu, .closure = nullptr, .thunk = nullptr,
                .ip = cu.entryOffset, .stackBaseOffset = 0,
                .withStackBase = 0, .flags = 0,
            });
            r = nix::v3::forceValue(vmS, r);
            std::string segment;
            for (size_t i = 0; i <= attrPath.size(); ++i) {
                if (i == attrPath.size() || attrPath[i] == '.') {
                    if (!segment.empty()) {
                        if (!r.isAttrs() || !r.payload.bindings)
                            throw std::runtime_error("v3-eval -A: not an attrset");
                        nix::v3::SymbolId sid = nix::v3::ir::globalInternSymbol(segment);
                        const Value * found = r.payload.bindings->lookup(sid);
                        if (!found)
                            throw std::runtime_error("v3-eval -A: attribute '" + segment + "' not found");
                        r = nix::v3::forceValue(vmS, *found);
                        segment.clear();
                    }
                } else {
                    segment.push_back(attrPath[i]);
                }
            }
        }

        // For --strict and --json modes we need to force any thunks
        // remaining inside lists/attrsets so the output is concrete.
        if (jsonOut || strict) {
            // Use a fresh VMState wrapping the same CU (the result was
            // produced by run() which created its own VMState — that's
            // gone by now).  We re-enter dispatchLoop via callClosure /
            // forceValue, which need a live frame on the VMState.  So
            // run again with a sentinel: call a no-op closure to give
            // forceDeep a frame to operate on.
            //
            // Simpler approach: construct a fresh VMState and a fake
            // top-level frame with the CU, then forceDeep.
            nix::v3::VMState vm;
            vm.frames.push_back(nix::v3::CallFrame{
                .cu = &cu, .closure = nullptr, .thunk = nullptr,
                .ip = cu.entryOffset, .stackBaseOffset = 0,
                .withStackBase = 0, .flags = 0,
            });
            r = forceDeep(vm, r);
        }

        // Use the global symbol table for printing — it's append-only
        // and a superset of every per-CU table, so it always covers
        // attribute names from imported CUs that the top-level CU's
        // (frozen-at-compile-time) snapshot wouldn't see.
        int rc = printValue(r, jsonOut, nix::v3::ir::globalSymbolTable());
        if (std::getenv("NIX_VM_STATS")) {
            nix::v3::dumpPrimOpStats(stderr);
            auto & a = nix::v3::allocStats();
            std::fprintf(stderr,
                "v3 alloc stats: closures=%llu thunks=%llu lists=%llu attrsets=%llu envs=%llu arena=%llu MB\n",
                (unsigned long long)a.closuresAllocated,
                (unsigned long long)a.thunksAllocated,
                (unsigned long long)a.listsAllocated,
                (unsigned long long)a.attrsetsAllocated,
                (unsigned long long)a.envsAllocated,
                (unsigned long long)(nix::v3::threadArena().bytesAllocated() >> 20));
            // IR Phase E (2026-05-18): selector-lambda fast-path
            // counter.  Confirms the emit-time peephole + runtime
            // dispatch are actually firing on the workload.  Zero
            // here = no selectors recognised (silent regression);
            // healthy nixpkgs evals should see this in the millions.
            std::fprintf(stderr,
                "v3 fastpath stats: selectorLambda=%llu intrinsicFix=%llu intrinsicExtends=%llu intrinsicCompose=%llu\n",
                (unsigned long long)a.selectorLambdaCalls,
                (unsigned long long)a.intrinsicFixCalls,
                (unsigned long long)a.intrinsicExtendsCalls,
                (unsigned long long)a.intrinsicComposeCalls);
            // Phase 13: thunk-force counters.  ratio = forced/allocated.
            // A healthy lazy evaluator has ratio ≤ 1 (most thunks are
            // forced once or never).  ratio > 1 means we're allocating
            // duplicate thunks for the same logical binding (memo bug)
            // or a thunk gets re-forced after Suspended → Blackhole →
            // Suspended (state regression) — both are slowness root
            // causes.
            double forceRatio = a.thunksAllocated
                ? double(a.thunksForced) / double(a.thunksAllocated) : 0.0;
            std::fprintf(stderr,
                "v3 force stats: thunksForced=%llu bridgeForced=%llu "
                "ratio_forced_per_alloc=%.3f\n",
                (unsigned long long)a.thunksForced,
                (unsigned long long)a.bridgeThunksForced,
                forceRatio);
            // Top-N hot LambdaDescriptors across the entry CU + every
            // imported CU.  Override count via V3_DBG_FORCES_TOPN
            // (default 20).
            if (std::getenv("V3_DBG_FORCES")) {
                size_t topN = 20;
                if (const char * e = std::getenv("V3_DBG_FORCES_TOPN"))
                    topN = static_cast<size_t>(std::strtoul(e, nullptr, 10));
                nix::v3::dumpHotDescriptors(stderr, topN, &cu);
            }
            // Bindings size histogram — informs VM-2 polymorphic
            // Bindings sizing.  Buckets:
            //  0=empty, 1, 2, 3-4, 5-8, 9-16, 17-32, 33-64, 65-128, 129+.
            static const char * const labels[10] = {
                "0", "1", "2", "3-4", "5-8", "9-16",
                "17-32", "33-64", "65-128", "129+"
            };
            uint64_t total = 0;
            for (auto v : a.attrsetSizeBuckets) total += v;
            std::fprintf(stderr, "v3 attrset size histogram (total=%llu):\n",
                (unsigned long long)total);
            for (size_t i = 0; i < 10; ++i) {
                if (a.attrsetSizeBuckets[i] == 0) continue;
                double pct = total ? 100.0 * a.attrsetSizeBuckets[i] / total
                                   : 0.0;
                std::fprintf(stderr, "  size %-7s %10llu (%5.1f%%)\n",
                    labels[i],
                    (unsigned long long)a.attrsetSizeBuckets[i], pct);
            }

            // 2026-05-18 per-opcode dispatch profile (NIX_VM_OPCOUNTS=1
            // must be set for any per-op data to have been gathered;
            // the dispatch loop only bumps under that gate).  Reports
            // the top 20 hot opcodes sorted by count, with each row's
            // percentage of total dispatches.  Drives VM-level
            // optimisation focus: e.g. if OP_GET_LOCAL_FORCE is 40%
            // of dispatches, fusing the GET+FORCE peephole is the
            // first win to grab; if OP_CALL dominates, looking at
            // the call fast-paths (selectorLambda, identityLambda)
            // pays.
            if (std::getenv("NIX_VM_OPCOUNTS")) {
                uint64_t opTotal = 0;
                for (size_t i = 0; i < 256; ++i) opTotal += a.opcodeCounts[i];
                if (opTotal > 0) {
                    // Collect (count, op) pairs for non-zero entries
                    // then sort descending.
                    std::vector<std::pair<uint64_t, uint8_t>> rows;
                    rows.reserve(64);
                    for (size_t i = 0; i < 256; ++i) {
                        if (a.opcodeCounts[i] > 0)
                            rows.emplace_back(a.opcodeCounts[i],
                                static_cast<uint8_t>(i));
                    }
                    std::sort(rows.begin(), rows.end(),
                        [](const auto & a, const auto & b) {
                            return a.first > b.first;
                        });
                    std::fprintf(stderr,
                        "v3 opcode profile (total=%llu, top 20 of %zu "
                        "distinct):\n",
                        (unsigned long long)opTotal, rows.size());
                    size_t shown = std::min<size_t>(rows.size(), 20);
                    for (size_t i = 0; i < shown; ++i) {
                        double pct = 100.0 * double(rows[i].first) / double(opTotal);
                        std::fprintf(stderr, "  %-26s %12llu (%5.2f%%)\n",
                            nix::v3::opName(
                                static_cast<nix::v3::Op>(rows[i].second)),
                            (unsigned long long)rows[i].first, pct);
                    }
                }
            }
        }
        return rc;
    } catch (const std::exception & ex) {
        // IR-CHECK robustness (plan §1.4 R1.4): when --emit-ir is
        // active and lowering / optimisation throws, emit a marker
        // line on stdout so a downstream `v3-check %s` invocation
        // can detect the failure cleanly (in addition to the
        // non-zero exit code that pipefail-aware runners already
        // catch).  Without this marker, a partial stdout buffer
        // could be silently consumed by v3-check and produce a
        // misleading "no CHECK matched" diagnostic.
        if (irDumpMode != IrDumpMode::None) {
            std::cout << "; LOWERING ERROR: " << ex.what() << "\n";
        }
        std::fprintf(stderr, "v3-eval error: %s\n", ex.what());
        return 1;
    }
}
