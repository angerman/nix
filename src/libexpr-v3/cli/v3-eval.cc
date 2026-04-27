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

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

using nix::v3::Value;
using nix::v3::Tag;

/// Recursively force a v3 Value to its full normal form.
static Value forceDeep(nix::v3::VMState & vm, Value v)
{
    v = nix::v3::forceValue(vm, v);
    if (v.isList() && v.payload.list)
        for (uint32_t i = 0; i < v.payload.list->size; ++i)
            v.payload.list->elems[i] = forceDeep(vm, v.payload.list->elems[i]);
    else if (v.isAttrs() && v.payload.bindings)
        for (uint32_t i = 0; i < v.payload.bindings->size; ++i)
            v.payload.bindings->entries[i].value =
                forceDeep(vm, v.payload.bindings->entries[i].value);
    return v;
}

/// v3 Value → JSON.  Forces thunks; rejects functions/external.
static nlohmann::json toJsonValue(const Value & v,
                                   const std::vector<std::string> & symTab)
{
    using json = nlohmann::json;
    switch (v.tag()) {
    case Tag::Null:   return json(nullptr);
    case Tag::Bool:   return json(v.payload.i == 1);
    case Tag::Int:    return json(v.payload.i);
    case Tag::Float:  return json(v.payload.f);
    case Tag::String: return json(std::string(v.payload.str));
    case Tag::Path:   return json(std::string(v.payload.path));
    case Tag::List: {
        json arr = json::array();
        if (v.payload.list)
            for (uint32_t i = 0; i < v.payload.list->size; ++i)
                arr.push_back(toJsonValue(v.payload.list->elems[i], symTab));
        return arr;
    }
    case Tag::Attrs: {
        json obj = json::object();
        if (v.payload.bindings)
            for (uint32_t i = 0; i < v.payload.bindings->size; ++i) {
                auto & en = v.payload.bindings->entries[i];
                std::string key = (en.name < symTab.size()) ? symTab[en.name] :
                    std::to_string(en.name);
                obj[key] = toJsonValue(en.value, symTab);
            }
        return obj;
    }
    case Tag::Closure:
    case Tag::PrimOp:
    case Tag::PrimOpApp:
        return json("<function>");
    case Tag::Thunk:
        return json("<thunk>");
    case Tag::Uninitialized:
    case Tag::App:
    case Tag::Blackhole:
    case Tag::External:
    default:
        return json(nullptr);
    }
}

static int printValue(const Value & r, bool jsonOut,
                      const std::vector<std::string> & symTab)
{
    if (jsonOut) {
        std::cout << toJsonValue(r, symTab).dump() << "\n";
        return 0;
    }
    switch (r.tag()) {
    case Tag::Int:    std::printf("%lld\n", (long long)r.payload.i); return 0;
    case Tag::Float:  std::printf("%g\n", r.payload.f); return 0;
    case Tag::Bool:   std::printf("%s\n", r.payload.i == 1 ? "true" : "false"); return 0;
    case Tag::Null:   std::printf("null\n"); return 0;
    case Tag::String: std::printf("\"%s\"\n", r.payload.str); return 0;
    case Tag::Path:   std::printf("path \"%s\"\n", r.payload.path); return 0;
    case Tag::List:   std::printf("<list of %u>\n",
                          r.payload.list ? r.payload.list->size : 0); return 0;
    case Tag::Attrs:  std::printf("<attrs of %u>\n",
                          r.payload.bindings ? r.payload.bindings->size : 0); return 0;
    case Tag::Closure:std::printf("<closure>\n"); return 0;
    case Tag::Thunk:  std::printf("<thunk>\n"); return 0;
    case Tag::Uninitialized:
    case Tag::PrimOp:
    case Tag::PrimOpApp:
    case Tag::App:
    case Tag::Blackhole:
    case Tag::External:
    default:          std::printf("<value tag=%d>\n", (int)r.tag()); return 0;
    }
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

int main(int argc, char ** argv)
{
    std::string path, expr;
    bool jsonOut = false, strict = false;

    for (int i = 1; i < argc; ++i) {
        std::string_view a(argv[i]);
        if (a == "--file" && i + 1 < argc) path = argv[++i];
        else if (a == "--expr" && i + 1 < argc) expr = argv[++i];
        else if (a == "--json")     jsonOut = true;
        else if (a == "--strict")   strict = true;
        else if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        else if (!a.empty() && a[0] == '-') { usage(argv[0]); return 2; }
        else { expr = argv[i]; }
    }

    if (expr.empty() && path.empty()) { usage(argv[0]); return 2; }
    if (!path.empty()) {
        try { expr = slurp(path); }
        catch (const std::exception & ex) {
            std::fprintf(stderr, "v3-eval: %s\n", ex.what());
            return 1;
        }
    }

    try {
        nix::initNix();
        nix::initGC();

        auto store = nix::openStore("dummy://");
        nix::fetchers::Settings fetchSettings{};
        bool readOnlyMode = true;
        nix::EvalSettings evalSettings{readOnlyMode};
        evalSettings.nixPath = {};

        nix::EvalState state(nix::LookupPath{}, store, fetchSettings, evalSettings, nullptr);

        nix::Expr * e = state.parseExprFromString(expr, state.rootPath(nix::CanonPath::root));
        e->bindVars(state, state.staticBaseEnv);

        nix::v3::registerBuiltinPrimOps();
        nix::v3::setNixEvalState(&state);

        auto m = nix::v3::lowerNixExpr(e, state.symbols);
        nix::v3::ir::computeFreeVars(m);
        auto cu = nix::v3::compile(m);
        Value r = nix::v3::run(cu);

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
                .cu = &cu, .ip = cu.entryOffset, .resultSlot = 0,
                .flags = 0, ._pad0 = 0, .stackBaseOffset = 0,
                .closure = nullptr, .resultPtr = nullptr, .thunk = nullptr,
            });
            r = forceDeep(vm, r);
        }

        return printValue(r, jsonOut, cu.symbolTable);
    } catch (const std::exception & ex) {
        std::fprintf(stderr, "v3-eval error: %s\n", ex.what());
        return 1;
    }
}
