/// @file
/// v3 Value pretty-printing, JSON rendering, and deep-forcing.
///
/// Implementation lifted verbatim from `cli/v3-eval.cc` so the
/// integrated `nix` CLI can reuse the same surface form under
/// v3-direct (inversion phase 1; see `lode/INVERSION_PLAN_2026-05-08.md`).
/// The lang-test golden suite checks byte-exactness — do NOT modify
/// the output format here without also updating
/// `tests/functional/lang/eval-okay-*.exp`.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/print.hh"
#include "v3/alloc.hh"
#include "v3/primop.hh"  // forceValue

#include <nlohmann/json.hpp>

#include <ostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include <stdexcept>
#include <utility>
#include <algorithm>

namespace nix::v3 {

Value forceDeep(VMState & vm, Value v, std::set<const void *> & seen)
{
    v = forceValue(vm, v);
    if (v.isList() && v.payload.list && v.payload.list->size > 0) {
        if (seen.insert(v.payload.list).second) {
            for (uint32_t i = 0; i < v.payload.list->size; ++i)
                v.payload.list->elems[i] = forceDeep(vm, v.payload.list->elems[i], seen);
        }
    } else if (v.isAttrs() && v.payload.bindings) {
        if (seen.insert(v.payload.bindings).second) {
            for (uint32_t i = 0; i < v.payload.bindings->size; ++i)
                v.payload.bindings->entries[i].value =
                    forceDeep(vm, v.payload.bindings->entries[i].value, seen);
        }
    }
    return v;
}

Value forceDeep(VMState & vm, Value v)
{
    std::set<const void *> seen;
    return forceDeep(vm, v, seen);
}

nlohmann::json toJsonValue(const Value & v,
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
        // Match tree-walker semantics — refuse to serialize a function
        // instead of silently producing a sentinel.
        throw std::runtime_error("cannot convert a function to JSON");
    case Tag::Thunk:
        return json("<thunk>");
    case Tag::Uninitialized:
    case Tag::App:
    case Tag::Blackhole:
    case Tag::External:
    case Tag::Slot:
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

void printNixValue(std::ostream & out, const Value & v,
                   const std::vector<std::string> & symTab,
                   std::set<const void *> & seen)
{
    switch (v.tag()) {
    case Tag::Int:    out << (long long)v.payload.i; return;
    case Tag::Float:  out << v.payload.f; return;
    case Tag::Bool:   out << (v.payload.i == 1 ? "true" : "false"); return;
    case Tag::Null:   out << "null"; return;
    case Tag::String: printLiteralString(out, v.payload.str ? std::string_view(v.payload.str) : std::string_view()); return;
    case Tag::Path:   out << (v.payload.path ? v.payload.path : ""); return;
    case Tag::List: {
        // Match tree-walker exactly: lists track by the address of the
        // *Value wrapper* (`&v`), so two slots that share a ListVec but
        // sit in distinct Value cells print independently.  Attrsets
        // track by Bindings* (`v.attrs()`), so two attrset values that
        // share the same Bindings (e.g. one from `__overrides` and one
        // from the rec body) collapse to «repeated» on the second
        // visit.  Empty lists are never tracked.
        if (v.payload.list && v.payload.list->size > 0 &&
            !seen.insert(&v).second) {
            out << "«repeated»"; return;
        }
        out << "[ ";
        if (v.payload.list)
            for (uint32_t i = 0; i < v.payload.list->size; ++i) {
                printNixValue(out, v.payload.list->elems[i], symTab, seen);
                out << ' ';
            }
        out << "]";
        return;
    }
    case Tag::Attrs: {
        // Empty attrsets share the global singleton — tracking them in
        // `seen` would (incorrectly) print `«repeated»` for every
        // sibling empty attrset.  Only deduplicate non-empty attrsets,
        // which is where shared-Bindings cycles actually matter.
        if (v.payload.bindings && v.payload.bindings->size > 0 &&
            !seen.insert(v.payload.bindings).second) {
            out << "«repeated»"; return;
        }
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
                printNixValue(out, *val, symTab, seen);
                out << "; ";
            }
        }
        out << "}";
        return;
    }
    // The simplified `<LAMBDA>` / `<PRIMOP>` / `<PRIMOP-APP>` /
    // `<thunk>` form matches TW's `printAmbiguous` (libexpr/print-
    // ambiguous.cc), which is what `nix-instantiate --eval --strict`
    // (the lang-test baseline) emits.  TW's OTHER printer — the
    // user-facing `ValuePrinter` used by `nix eval` (libexpr/print.cc:
    // printFunction) — emits the richer `«lambda <name>? @ <pos>»`
    // form.  v3 currently uses the simplified form for BOTH v3-eval
    // and the runV3DirectEval path, which means `nix eval --impure`
    // output diverges cosmetically from TW's `nix eval` (drvPath
    // values still match — see #665/#666/#667).  Matching TW's
    // `nix eval` printer requires (a) context-sensitive printer
    // selection (eval vs eval-via-instantiate) and (b) refactoring
    // lower.cc to fill `desc->name` from TW's contextual-name
    // heuristic rather than the arg name.  Tracked separately
    // (#669); the simplified form is preserved here to keep the
    // lang-test goldens passing.
    case Tag::Closure: out << "<LAMBDA>"; return;
    case Tag::PrimOp:  out << "<PRIMOP>"; return;
    case Tag::PrimOpApp:out << "<PRIMOP-APP>"; return;
    case Tag::Thunk:    out << "<thunk>"; return;
    case Tag::App:      out << "<APP>"; return;
    case Tag::Blackhole:out << "<BLACKHOLE>"; return;
    case Tag::External: out << "<EXTERNAL>"; return;
    case Tag::Slot:     out << "<SLOT>"; return;
    case Tag::Uninitialized:
    default:            out << "<value tag=" << (int)v.tag() << ">"; return;
    }
}

void printNixValue(std::ostream & out, const Value & v,
                   const std::vector<std::string> & symTab)
{
    std::set<const void *> seen;
    printNixValue(out, v, symTab, seen);
}

} // namespace nix::v3
