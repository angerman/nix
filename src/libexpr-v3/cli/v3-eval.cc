/// @file
/// v3-eval — minimal CLI: parses a Nix expression, lowers it through v3,
/// runs the bytecode, prints the resulting value.
///
/// Usage: v3-eval '<nix-expression>'
///
/// Limitations during bring-up: the lowerer only supports a subset of Nix
/// (literals, arithmetic, conditional, simple lambda, application).
/// Unsupported AST nodes throw a clear error.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/lower.hh"
#include "v3/vm.hh"
#include "v3/primop.hh"
#include "v3/alloc.hh"

#include "nix/expr/eval.hh"
#include "nix/expr/eval-gc.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/store/store-open.hh"
#include "nix/store/globals.hh"
#include "nix/main/shared.hh"
#include "nix/util/canon-path.hh"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

static int printValue(const nix::v3::Value & r)
{
    using namespace nix::v3;
    switch (r.tag()) {
    case Tag::Int:    std::printf("%lld\n", (long long)r.payload.i); return 0;
    case Tag::Float:  std::printf("%g\n", r.payload.f); return 0;
    case Tag::Bool:   std::printf("%s\n", r.payload.i == 1 ? "true" : "false"); return 0;
    case Tag::Null:   std::printf("null\n"); return 0;
    case Tag::String: std::printf("\"%s\"\n", r.payload.str); return 0;
    case Tag::List:   std::printf("<list of %u>\n", r.payload.list ? r.payload.list->size : 0); return 0;
    case Tag::Attrs:  std::printf("<attrs of %u>\n", r.payload.bindings ? r.payload.bindings->size : 0); return 0;
    case Tag::Closure:std::printf("<closure>\n"); return 0;
    case Tag::Thunk:  std::printf("<thunk>\n"); return 0;
    case Tag::Path:   std::printf("path \"%s\"\n", r.payload.path); return 0;
    case Tag::Uninitialized:
    case Tag::PrimOp:
    case Tag::PrimOpApp:
    case Tag::App:
    case Tag::Blackhole:
    case Tag::External:
    default:          std::printf("<value tag=%d>\n", (int)r.tag()); return 0;
    }
}

int main(int argc, char ** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s '<nix-expression>'\n", argv[0]);
        return 2;
    }

    try {
        // libstore requires this once-per-process bootstrap.
        nix::initNix();
        nix::initGC();

        // Stand up a minimal nix EvalState (parser + symbol table only).
        auto store = nix::openStore("dummy://");
        nix::fetchers::Settings fetchSettings{};
        bool readOnlyMode = true;
        nix::EvalSettings evalSettings{readOnlyMode};
        evalSettings.nixPath = {};

        nix::EvalState state(nix::LookupPath{}, store, fetchSettings, evalSettings, nullptr);

        nix::Expr * e = state.parseExprFromString(argv[1], state.rootPath(nix::CanonPath::root));
        // bindVars resolves variable references (level/displ).
        e->bindVars(state, state.staticBaseEnv);

        nix::v3::registerBuiltinPrimOps();
        auto m = nix::v3::lowerNixExpr(e, state.symbols);
        nix::v3::ir::computeFreeVars(m);
        auto cu = nix::v3::compile(m);
        nix::v3::Value r = nix::v3::run(cu);

        return printValue(r);
    } catch (const std::exception & ex) {
        std::fprintf(stderr, "v3-eval error: %s\n", ex.what());
        return 1;
    }
}
