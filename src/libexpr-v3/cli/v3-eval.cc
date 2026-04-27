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
#include "v3/ir.hh"

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
        // Match tree-walker semantics — refuse to serialize a
        // function instead of silently producing a sentinel.
        throw std::runtime_error("cannot convert a function to JSON");
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

/// Print a string literal in Nix's source-code form: backslash-escape
/// `"`, `\`, control whitespace, and `${` (which would otherwise start
/// an interpolation).
static void printLiteralString(std::ostream & out, std::string_view s)
{
    out << '"';
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '"' || c == '\\') { out << '\\' << c; }
        else if (c == '\n')        { out << "\\n"; }
        else if (c == '\r')        { out << "\\r"; }
        else if (c == '\t')        { out << "\\t"; }
        else if (c == '$' && i + 1 < s.size() && s[i + 1] == '{') { out << "\\$"; }
        else                       { out << c; }
    }
    out << '"';
}

/// Identifier rules used by Nix's pretty-printer for attrset keys: bare
/// identifiers stay bare, anything that would parse oddly gets quoted.
static const std::set<std::string> kNixReservedKeywords = {
    "if", "then", "else", "assert", "with", "let", "in", "rec", "inherit",
};

static void printAttrName(std::ostream & out, std::string_view s)
{
    if (s.empty() || kNixReservedKeywords.count(std::string(s))) {
        printLiteralString(out, s);
        return;
    }
    char c = s[0];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    for (size_t i = 1; ok && i < s.size(); ++i) {
        char d = s[i];
        ok = (d >= 'a' && d <= 'z') || (d >= 'A' && d <= 'Z') ||
             (d >= '0' && d <= '9') || d == '_' || d == '\'' || d == '-';
    }
    if (ok) out << s;
    else    printLiteralString(out, s);
}

/// Pretty-print a v3 Value in the same surface form as nix-instantiate
/// --eval --strict.  Used by the lang regression suite — output must
/// match the existing .exp golden files byte-for-byte.
static void printNixValue(std::ostream & out, const Value & v,
                          const std::vector<std::string> & symTab)
{
    switch (v.tag()) {
    case Tag::Int:    out << (long long)v.payload.i; return;
    case Tag::Float:  out << v.payload.f; return;
    case Tag::Bool:   out << (v.payload.i == 1 ? "true" : "false"); return;
    case Tag::Null:   out << "null"; return;
    case Tag::String: printLiteralString(out, v.payload.str ? std::string_view(v.payload.str) : std::string_view()); return;
    case Tag::Path:   out << (v.payload.path ? v.payload.path : ""); return;
    case Tag::List: {
        out << "[ ";
        if (v.payload.list)
            for (uint32_t i = 0; i < v.payload.list->size; ++i) {
                printNixValue(out, v.payload.list->elems[i], symTab);
                out << ' ';
            }
        out << "]";
        return;
    }
    case Tag::Attrs: {
        out << "{ ";
        if (v.payload.bindings) {
            // Sort by symbol name for deterministic order matching tw output.
            std::vector<std::pair<std::string, const Value *>> items;
            items.reserve(v.payload.bindings->size);
            for (uint32_t i = 0; i < v.payload.bindings->size; ++i) {
                auto & en = v.payload.bindings->entries[i];
                std::string key = (en.name < symTab.size())
                    ? symTab[en.name] : std::to_string(en.name);
                items.emplace_back(std::move(key), &en.value);
            }
            std::sort(items.begin(), items.end(),
                      [](auto & a, auto & b) { return a.first < b.first; });
            for (auto & [name, val] : items) {
                printAttrName(out, name);
                out << " = ";
                printNixValue(out, *val, symTab);
                out << "; ";
            }
        }
        out << "}";
        return;
    }
    case Tag::Closure: out << "<LAMBDA>"; return;
    case Tag::PrimOp:  out << "<PRIMOP>"; return;
    case Tag::PrimOpApp:out << "<PRIMOP-APP>"; return;
    case Tag::Thunk:    out << "<thunk>"; return;
    case Tag::App:      out << "<APP>"; return;
    case Tag::Blackhole:out << "<BLACKHOLE>"; return;
    case Tag::External: out << "<EXTERNAL>"; return;
    case Tag::Uninitialized:
    default:            out << "<value tag=" << (int)v.tag() << ">"; return;
    }
}

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

int main(int argc, char ** argv)
{
    std::string path, expr;
    bool jsonOut = false, strict = false;
    // Extra search-path entries (each is either "PATH" or "NAME=PATH").
    // Mirrors `nix-instantiate -I` so the lang test runner's per-test
    // .flags files (which reference `-I lang/dir1` etc.) work.
    std::vector<std::string> extraSearchPath;
    // Auto-args: `--arg NAME EXPR` / `--argstr NAME STR`.  Used by the
    // autoargs lang test.
    std::vector<std::pair<std::string, std::string>> autoArgs;
    std::vector<std::pair<std::string, std::string>> autoArgsStr;

    for (int i = 1; i < argc; ++i) {
        std::string_view a(argv[i]);
        if      (a == "--file" && i + 1 < argc) path = argv[++i];
        else if (a == "--expr" && i + 1 < argc) expr = argv[++i];
        else if (a == "--json")     jsonOut = true;
        else if (a == "--strict")   strict = true;
        else if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        else if (a == "-I" && i + 1 < argc)
            extraSearchPath.emplace_back(argv[++i]);
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

        auto m = nix::v3::lowerNixExpr(e, state.symbols, state.positions);
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
        return printValue(r, jsonOut, nix::v3::ir::globalSymbolTable());
    } catch (const std::exception & ex) {
        std::fprintf(stderr, "v3-eval error: %s\n", ex.what());
        return 1;
    }
}
