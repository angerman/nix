#include "nix/cmd/command-installable-value.hh"
#include "nix/cmd/installable-attr-path.hh"
#include "nix/main/common-args.hh"
#include "nix/main/shared.hh"
#include "nix/store/store-api.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/value-to-json.hh"
#include "nix/util/eval-trace.hh"

// v3 INVERSION step 3 (#526): when NIX_V3_DIRECT_EVAL=1 + --expr/--file
// installable, bypass `installable->toValue` (which routes through
// state.eval and TW's eval hook) and run the v3 pipeline directly
// from parsed Expr to v3::Value to printer output.  No bridge to TW
// shapes, so the lib.fix formals-closure refusal cycle disappears at
// the source.
#include "v3/run.hh"
#include "v3/print.hh"
#include "v3/primop.hh"
#include "v3/alloc.hh"
#include "v3/ir.hh"
#include "v3/vm.hh"
#include "nix/util/users.hh"            // getHome() for the parser's ~/x
#include "nix/util/file-descriptor.hh"  // drainFD (stdin)

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <set>
#include <sstream>

namespace nix {

/// v3-direct path for `nix eval --expr/--file`.  Bypasses
/// `installable->toValue` (which routes through TW's `state.eval` and
/// the v3 eval hook + bridge) and runs v3 from parsed Expr to result
/// directly.  Output rendered by v3's own printer.
///
/// Returns true if the v3-direct path handled the command; false if
/// the caller should fall through to the existing TW path.
///
/// Currently handles: --expr / --file with -A attrPath, --apply,
/// --raw, --json, default print.  Falls back to TW for: flake
/// installables (no --expr/--file), --write-to (recursive directory
/// emission depends on TW Value shape), autoArgs (--arg/--argstr —
/// not yet routed through v3-direct).
static bool runV3DirectEval(
    EvalState & state,
    SourceExprCommand & cmd,
    Installable & installable,
    bool raw,
    bool json,
    std::optional<std::string> apply,
    std::optional<std::filesystem::path> writeTo)
{
    nix::evalTrace::mark("eval.cc:runV3DirectEval entry");
    // --write-to is recursive directory emission; falls back to TW.
    if (writeTo) {
        nix::evalTrace::mark("eval.cc:runV3DirectEval reject(writeTo)");
        return false;
    }
    // autoArgs not yet supported on the v3-direct path.
    if (cmd.getAutoArgs(state)->size() > 0) {
        nix::evalTrace::mark("eval.cc:runV3DirectEval reject(autoArgs)");
        return false;
    }

    // We need the user's --expr / --file.  Without one of these we
    // can't reconstruct the expression for v3 (the installable's
    // already-evaluated TW Value is a one-shot bridge dead-end —
    // re-using it would defeat the inversion's purpose).
    if (!cmd.expr && !cmd.file) {
        nix::evalTrace::mark("eval.cc:runV3DirectEval reject(no-expr/file)");
        return false;
    }

    // Only handle InstallableAttrPath shapes (the kind constructed
    // from --expr/--file + positional attrPath).  Flake installables
    // need different machinery — Phase 2.
    if (!dynamic_cast<InstallableAttrPath *>(&installable)) {
        nix::evalTrace::mark("eval.cc:runV3DirectEval reject(not InstallableAttrPath)");
        return false;
    }
    // Read the attrPath through the public virtual `Installable::what`
    // (the override in InstallableAttrPath is private but virtual
    // access is resolved against the static type at the call site).
    std::string attrPath = installable.what();  // e.g. "lib.fix" or "" for root.

    // PARSER_PROJECT_PLAN §5.3: acquire the raw `.nix` source, then native
    // parse+lower+run — NO nix::Expr.  Relative (`./x`) / home (`~/x`) path
    // literals resolve against the file's dir / $HOME; the position origin
    // (file vs in-memory string) is built inside runRootExprFromString from
    // `v3file` so this site needs no `nix/...` position type.
    std::string v3src, v3base;
    std::optional<nix::SourcePath> v3file;  // set for a real file; null → string/stdin
    if (cmd.file) {
        if (*cmd.file == "-") {
            v3src = nix::drainFD(STDIN_FILENO);
            v3base = absPath(cmd.getCommandBaseDir()).string();
        } else {
            auto dir = absPath(cmd.getCommandBaseDir());
            nix::SourcePath sp = lookupFileArg(state, cmd.file->string(), &dir);
            v3src = sp.resolveSymlinks().readFile();
            if (auto par = sp.path.parent()) v3base = par->abs();
            v3file.emplace(sp);
        }
    } else {
        v3base = absPath(cmd.getCommandBaseDir()).string();
        v3src = *cmd.expr;
    }

    // Run v3 pipeline.  Returns (cu, value) — keep cu alive for the
    // lifetime of the value (string / path payloads point into
    // cu->stringConstants).  setNixEvalState is wired internally;
    // primops that need TW (import, derivation strict-merge) reach
    // back via the global pointer.
    nix::evalTrace::mark("eval.cc:100 runRootExprFromString(root)");
    auto rootResult = v3::runRootExprFromString(
        state, v3src, v3base, nix::getHome().string(), v3file ? &*v3file : nullptr);
    v3::Value r = rootResult.value;

    // Set up a VMState for further forcing / callClosure work.  STG-10
    // (vm.cc:5530+) ensures a single VMState is shared across re-
    // entries within this thread, but we still need an outer frame
    // for the chase loops to land on.  Push a synthetic frame on
    // rootResult.cu so dispatchLoop's `vm.frames.back().cu` is valid
    // when callClosure / forceValue need to run inner thunks.
    v3::VMState vm;
    vm.frames.push_back(v3::CallFrame{
        .cu = rootResult.cu.get(), .closure = nullptr, .thunk = nullptr,
        .ip = rootResult.cu->entryOffset, .stackBaseOffset = 0,
        .withStackBase = 0, .flags = 0,
    });

    // Force the result to WHNF so attr/list access works.
    nix::evalTrace::mark("eval.cc:117 forceValue(root-WHNF)");
    r = v3::forceValue(vm, r);

    // -A attrPath descent: split on '.' and lookup successive attrs.
    if (!attrPath.empty()) {
        std::string segment;
        for (size_t i = 0; i <= attrPath.size(); ++i) {
            if (i == attrPath.size() || attrPath[i] == '.') {
                if (!segment.empty()) {
                    if (!r.isAttrs() || !r.asAttrs()) {
                        state.error<EvalError>(
                            "v3-direct -A: '%1%' is not an attrset",
                            segment).debugThrow();
                    }
                    auto sid = v3::ir::globalInternSymbol(segment);
                    const v3::Value * found = r.asAttrs()->lookup(sid);
                    if (!found) {
                        state.error<EvalError>(
                            "v3-direct -A: attribute '%1%' not found",
                            segment).debugThrow();
                    }
                    nix::evalTrace::mark("eval.cc:137 forceValue(attrPath-segment)");
                    r = v3::forceValue(vm, *found);
                    segment.clear();
                }
            } else {
                segment.push_back(attrPath[i]);
            }
        }
    }

    // --apply: run the apply expression and call it on the descended
    // value.  Allocates its own cu so the apply expr has a stable IR
    // home; the cu must outlive any string/path payloads produced by
    // the call (same reason rootResult.cu sticks around).
    std::optional<v3::RootResult> applyResult;
    if (apply) {
        auto dir = absPath(cmd.getCommandBaseDir());
        // In-memory expr string → null originPath selects Pos::String.
        nix::evalTrace::mark("eval.cc:156 runRootExprFromString(--apply)");
        applyResult.emplace(v3::runRootExprFromString(
            state, *apply, dir.string(), nix::getHome().string(), nullptr));
        nix::evalTrace::mark("eval.cc:157 forceValue(--apply fn)");
        v3::Value applyV = v3::forceValue(vm, applyResult->value);
        r = v3::callClosure(vm, applyV, r);
        nix::evalTrace::mark("eval.cc:159 forceValue(--apply result)");
        r = v3::forceValue(vm, r);
    }

    // Render.  Same dispatch as the TW path: --raw → string-coerce,
    // --json → toJsonValue, default → printNixValue.
    if (raw) {
        // String-coerce: deep-force, demand a Tag::String.  No context
        // tracking here (we'd need to lift coerceToString into v3 for
        // full parity; that's Phase 2 work).  For now, accept only
        // already-string values.
        nix::evalTrace::mark("eval.cc:169 forceDeep(--raw)");
        r = v3::forceDeep(vm, r);
        if (r.tag() != v3::Tag::String) {
            state.error<EvalError>(
                "v3-direct --raw: result is not a string (tag=%1%)",
                (int)r.tag()).debugThrow();
        }
        std::string_view sv = r.asString() ? r.asString() : "";
        std::cout.write(sv.data(), (std::streamsize)sv.size());
    } else if (json) {
        // #675: toJsonValue now lazy-forces internally + short-circuits
        // on derivations (outPath / __toString).  Skip the upfront
        // forceDeep — it would still work but uselessly traverses the
        // whole graph (TW takes <2s on hello.drvAttrs.src; pre-fix v3
        // took >3 min and produced 0 bytes due to the missing
        // short-circuit).
        nix::evalTrace::mark("eval.cc:178 lazy json");
        std::cout << v3::toJsonValue(vm, r, v3::ir::globalSymbolTable()).dump() << "\n";
    } else {
        // Default print.  TW renders `{ a = 1; b = throw "no"; c = 3; }`
        // as `{ a = 1; b = «error: no»; c = 3; }` — per-attr try/catch
        // catches `throw` deep in the value and emits an inline error
        // token.  v3's vm-aware printer overload mirrors that: forces
        // each value lazily inside a try/catch, recursing without an
        // upfront `forceDeep` (which would propagate the throw to the
        // top level and abort the print).
        //
        // #669 contributed the rich-form tokens (`«derivation /path»`,
        // `«lambda <name>? @ <pos>»`, `«primop <name>»`); the new
        // VMState-aware overload (this commit) adds the lazy + per-
        // error force discipline.
        nix::evalTrace::mark("eval.cc:182 lazy print(default)");
        std::ostringstream os;
        v3::printNixValueRich(os, vm, r, v3::ir::globalSymbolTable());
        logger->cout("%s", os.str());
    }

    // 2026-05-29 evening: production end-of-eval bridge + import-cache
    // clear.  Per `lode/BRIDGES_HOLD_RETENTION_2026-05-29.md`, v3 ↔ TW
    // bridge tables retain 99.8-99.9 % of arena bytes at end-of-eval.
    // Now that rendering is complete and no further TW callbacks are
    // expected (the v3-direct path is single-shot per `nix eval`
    // invocation; `nix repl` does NOT route through this function),
    // drop the global-root retention so the OS can reclaim arena
    // pages on process exit.  Default-on; opt out via
    // NIX_V3_KEEP_GLOBAL_ROOTS=1.
    v3::clearPostEvalGlobalRoots();

    return true;
}

struct CmdEval : MixJSON, InstallableValueCommand, MixReadOnlyOption
{
    bool raw = false;
    std::optional<std::string> apply;
    std::optional<std::filesystem::path> writeTo;

    CmdEval()
        : InstallableValueCommand()
    {
        addFlag({
            .longName = "raw",
            .description = "Print strings without quotes or escaping.",
            .handler = {&raw, true},
        });

        addFlag({
            .longName = "apply",
            .description = "Apply the function *expr* to each argument.",
            .labels = {"expr"},
            .handler = {&apply},
        });

        addFlag({
            .longName = "write-to",
            .description = "Write a string or attrset of strings to *path*.",
            .labels = {"path"},
            .handler = {&writeTo},
        });
    }

    std::string description() override
    {
        return "evaluate a Nix expression";
    }

    std::string doc() override
    {
        return
#include "eval.md"
            ;
    }

    Category category() override
    {
        return catSecondary;
    }

    void run(ref<Store> store, ref<InstallableValue> installable) override
    {
        nix::evalTrace::mark("eval.cc:CmdEval::run ENTRY");
        if (raw && json)
            throw UsageError("--raw and --json are mutually exclusive");

        nix::evalTrace::mark("eval.cc:CmdEval::run before getEvalState");
        auto state = getEvalState();
        nix::evalTrace::mark("eval.cc:CmdEval::run after getEvalState");

        // v3 INVERSION step 3 (#526): when NIX_V3_DIRECT_EVAL=1, try
        // the v3-direct path first.  If it handles the command (--expr/
        // --file shape, no autoArgs, no --write-to), we're done — no
        // bridge cycles, no fallback retries.  Otherwise fall through
        // to the existing TW path which still goes via state.eval +
        // hooks + bridges (and may succeed or hit the lib.fix issue).
        static const bool s_directEval =
            std::getenv("NIX_V3_DIRECT_EVAL") != nullptr;
        if (s_directEval) {
            nix::evalTrace::mark("eval.cc:CmdEval::run before runV3DirectEval");
            if (runV3DirectEval(*state, *this, *installable, raw, json,
                                 apply, writeTo)) {
                nix::evalTrace::mark("eval.cc:CmdEval::run v3-direct returned true");
                return;
            }
            nix::evalTrace::mark("eval.cc:CmdEval::run v3-direct returned false, falling back");
            // T-2 (CODEBASE_REVIEW_2026-06-11): with NIX_V3_DIRECT_EVAL=1 set,
            // the v3-direct path silently ran the tree-walker for shapes it
            // doesn't handle (flake installables like `nixpkgs#hello`,
            // --write-to, --arg/--argstr). That made measurement A/Bs compare
            // TW-vs-TW while believing v3 was engaged. Surface the fallback;
            // NIX_V3_REQUIRE=1 turns it into a hard error — baking the
            // measurement gate's ENGAGED check into the binary.
            static const bool s_v3Require =
                std::getenv("NIX_V3_REQUIRE") != nullptr;
            if (s_v3Require)
                throw Error(
                    "NIX_V3_REQUIRE=1: the v3-direct evaluator does not handle "
                    "this `nix eval` invocation (a flake installable, --write-to, "
                    "or --arg/--argstr) and would silently fall back to the "
                    "tree-walker. Use `nix eval --expr`/`--file` (plain, with no "
                    "--arg) or the standalone v3-eval binary to engage v3.");
            warn("NIX_V3_DIRECT_EVAL=1: this `nix eval` invocation is not handled "
                 "by v3-direct (flake installable / --write-to / --arg); falling "
                 "back to the tree-walker. Set NIX_V3_REQUIRE=1 to fail instead.");
        }

        nix::evalTrace::mark("eval.cc:CmdEval::run TW installable->toValue");
        auto [v, pos] = installable->toValue(*state);
        NixStringContext context;

        if (apply) {
            auto vApply = state->allocValue();
            state->eval(state->parseExprFromString(*apply, state->rootPath(".")), *vApply);
            auto vRes = state->allocValue();
            state->callFunction(*vApply, *v, *vRes, noPos);
            v = vRes;
        }

        if (writeTo) {
            logger->stop();

            if (pathExists(*writeTo))
                throw Error("path '%s' already exists", writeTo->string());

            [&](this const auto & recurse, Value & v, const PosIdx pos, const std::filesystem::path & path) -> void {
                state->forceValue(v, pos);
                if (v.type() == nString)
                    // FIXME: disallow strings with contexts?
                    writeFile(path, v.string_view());
                else if (v.type() == nAttrs) {
                    [[maybe_unused]] bool directoryCreated = std::filesystem::create_directory(path);
                    // Directory should not already exist
                    assert(directoryCreated);
                    for (auto & attr : *v.attrs()) {
                        std::string_view name = state->symbols[attr.name];
                        try {
                            if (name == "." || name == "..")
                                throw Error("invalid file name '%s'", name);
                            recurse(*attr.value, attr.pos, path / name);
                        } catch (Error & e) {
                            e.addTrace(
                                state->positions[attr.pos], HintFmt("while evaluating the attribute '%s'", name));
                            throw;
                        }
                    }
                } else
                    state->error<TypeError>("value at '%s' is not a string or an attribute set", state->positions[pos])
                        .debugThrow();
            }(*v, pos, *writeTo);
        }

        else if (raw) {
            logger->stop();
            writeFull(
                getStandardOutput(),
                *state->coerceToString(noPos, *v, context, "while generating the eval command output"));
        }

        else if (json) {
            printJSON(printValueAsJSON(*state, true, *v, pos, context, false));
        }

        else {
            logger->cout("%s", ValuePrinter(*state, *v, PrintOptions{.force = true, .derivationPaths = true}));
        }
    }
};

static auto rCmdEval = registerCommand<CmdEval>("eval");

} // namespace nix
