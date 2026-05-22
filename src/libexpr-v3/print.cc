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
#include "v3/barrier.hh"  // Phase D write-barrier helpers
#include "v3/closure.hh"  // LambdaDescriptor for printNixValueRich
#include "v3/ir.hh"      // globalInternSymbol for toJsonValue short-circuit
#include "v3/primop.hh"  // forceValue, PrimOp

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

// -------------------------------------------------------------------
// GC_AUDIT_ROUND_2 Round 1 #7 — deep-force root stack
// -------------------------------------------------------------------
//
// Thread-local stack of in-flight Values for the deep-force /
// rich-print / JSON-render recursions.  Each recursion frame pushes
// its `v` (the Value whose List / Attrs container it's about to
// iterate) and accesses the container through `roots[idx]` instead of
// the C-local `v`.  The scavenger walks the stack via
// walkDeepForceRoots (declared in print.hh) so that when the
// underlying ListVec / Bindings is forwarded nursery->tenured during
// the inner forceValue, the slot's payload pointer follows.
//
// Using std::vector + index is robust against vector growth from
// deeper recursion frames (push_back may reallocate; indices stay
// valid because the new buffer is a copy).  We do NOT cache
// references / pointers into the vector across calls that may push.
//
// Lifetime: thread_local; no destructor needed (process-exit
// reclaims the storage).  Currently only invoked from print.cc
// recursion frames where the v3 dispatch is the only producer of
// nursery-allocated containers — no other module pushes onto it.
namespace {
thread_local std::vector<Value> tlDeepForceRoots;

struct DeepForceGuard
{
    size_t idx;
    DeepForceGuard(const Value & v) : idx(tlDeepForceRoots.size())
    {
        tlDeepForceRoots.push_back(v);
    }
    ~DeepForceGuard()
    {
        // Single-threaded LIFO discipline — pop the slot we pushed.
        // Defensive: only pop if back() matches our index (vector
        // growth never reorders; this should always hold).
        if (tlDeepForceRoots.size() > idx)
            tlDeepForceRoots.resize(idx);
    }
    Value & ref()       { return tlDeepForceRoots[idx]; }
    const Value & ref() const { return tlDeepForceRoots[idx]; }
};
} // namespace

void walkDeepForceRoots(const std::function<void(Value &)> & visit)
{
    for (auto & v : tlDeepForceRoots) visit(v);
}

Value forceDeep(VMState & vm, Value v, std::set<const void *> & seen)
{
    // A12b (2026-05-22): iterative tree walk via `tlDeepForceRoots`
    // as the GC-protected work queue.  Pre-fix this function
    // C-recursed at every nested list/attrset level, with one
    // DeepForceGuard per recursion frame.  Deep nesting (e.g.
    // `forceDeep (toJSON (... deeply structured ...))` on
    // package-metadata graphs) could exhaust the C-stack.
    //
    // Iterative design: capture `baseIdx`; enqueue containers
    // (push onto tlDeepForceRoots — already walked by the
    // scavenger via walkDeepForceRoots); cursor through entries
    // from baseIdx upward.  Each container is read via
    // `tlDeepForceRoots[cur].payload.X` so a scavenge inside an
    // inner forceValue that forwards the container updates the
    // pointer we observe on the next access.  At the end we resize
    // the stack back to baseIdx (popping all our enqueued entries
    // at once).
    //
    // Semantics preserved:
    //   * The whole transitive container graph is forced.
    //   * Cycles are detected via the `seen` set (insert-then-
    //     enqueue — cycles never re-enqueue).
    //   * In-place writeback (elems[i] = forced; bindingsSetValue
    //     for attrs) — downstream readers see the forced WHNF.
    //   * Return value is the (possibly scavenge-updated) root.
    Value root = forceValue(vm, v);

    const size_t baseIdx = tlDeepForceRoots.size();
    bool rootEnqueued = false;
    size_t rootEnqIdx = 0;

    auto maybeEnqueue = [&](Value cv) -> bool {
        if (cv.isList() && cv.payload.list && cv.payload.list->size > 0
            && seen.insert(cv.payload.list).second)
        {
            tlDeepForceRoots.push_back(cv);
            return true;
        }
        if (cv.isAttrs() && cv.payload.bindings
            && cv.payload.bindings->size > 0
            && seen.insert(cv.payload.bindings).second)
        {
            tlDeepForceRoots.push_back(cv);
            return true;
        }
        return false;
    };

    if (maybeEnqueue(root)) {
        rootEnqueued = true;
        rootEnqIdx = tlDeepForceRoots.size() - 1;
    }

    size_t cur = baseIdx;
    while (cur < tlDeepForceRoots.size()) {
        // Always read parent's payload through tlDeepForceRoots[cur]
        // — its pointer may have been forwarded by an inner
        // forceValue scavenge.  Indices into the vector are stable
        // across reallocations; pointers into it are not.
        if (tlDeepForceRoots[cur].isList()) {
            const uint32_t size =
                tlDeepForceRoots[cur].payload.list->size;
            for (uint32_t i = 0; i < size; ++i) {
                Value child = forceValue(
                    vm, tlDeepForceRoots[cur].payload.list->elems[i]);
                tlDeepForceRoots[cur].payload.list->elems[i] = child;
                maybeEnqueue(child);
            }
        } else if (tlDeepForceRoots[cur].isAttrs()) {
            const uint32_t size =
                tlDeepForceRoots[cur].payload.bindings->size;
            for (uint32_t i = 0; i < size; ++i) {
                Value child = forceValue(
                    vm, tlDeepForceRoots[cur].payload.bindings->entries[i].value);
                bindingsSetValue(  // Phase D barrier
                    tlDeepForceRoots[cur].payload.bindings, i, child);
                maybeEnqueue(child);
            }
        }
        ++cur;
    }

    // Capture the (possibly-updated) root before popping the stack.
    if (rootEnqueued)
        root = tlDeepForceRoots[rootEnqIdx];
    tlDeepForceRoots.resize(baseIdx);
    return root;
}

Value forceDeep(VMState & vm, Value v)
{
    std::set<const void *> seen;
    return forceDeep(vm, v, seen);
}

nlohmann::json toJsonValue(VMState & vm, Value v,
                            const std::vector<std::string> & symTab)
{
    using json = nlohmann::json;
    // Force lazily as we serialize.  Mirrors TW's printValueAsJSON in
    // libexpr/value-to-json.cc which interleaves force + emit instead
    // of doing a deep-force upfront.  Critical for perf on derivations:
    // a 50-attr derivation that short-circuits via outPath becomes
    // O(1) instead of O(transitive-graph).
    v = forceValue(vm, v);
    switch (v.tag()) {
    case Tag::Null:   return json(nullptr);
    case Tag::Bool:   return json(v.payload.i == 1);
    case Tag::Int:    return json(v.payload.i);
    case Tag::Float:  return json(v.payload.f);
    case Tag::String: return json(std::string(v.payload.str));
    case Tag::Path:   return json(std::string(v.payload.path));
    case Tag::List: {
        json arr = json::array();
        if (v.payload.list) {
            // Round 1 #7: hold `v` on the deep-force root stack across
            // recursive toJsonValue calls — its payload.list may sit
            // in the nursery and get forwarded by a scavenge inside
            // the recursion's forceValue.
            DeepForceGuard g(v);
            for (uint32_t i = 0; i < g.ref().payload.list->size; ++i)
                arr.push_back(toJsonValue(vm, g.ref().payload.list->elems[i], symTab));
        }
        return arr;
    }
    case Tag::Attrs: {
        // TW parity (value-to-json.cc:52-58): attrset short-circuits.
        //
        //   1) `__toString self` → emit its string result.
        //   2) `outPath` → emit just outPath (no full attrset emit).
        //
        // Without this, a derivation (which has outPath but ~50 other
        // attrs) renders as a deeply-nested JSON object instead of a
        // single string.  Pre-fix, `nix eval --impure --json --expr
        // 'hello.drvAttrs.src'` took >3 minutes and produced 0 bytes
        // (forceDeep blew through the full nixpkgs graph reachable
        // from src); TW does it in <2s.  See follow-on memo
        // project_675_tojson_shortcircuit.
        if (v.payload.bindings) {
            static const SymbolId tsId = ir::globalInternSymbol("__toString");
            if (auto * fn = v.payload.bindings->lookup(tsId)) {
                Value forced = forceValue(vm, *fn);
                if (forced.tag() == Tag::Closure
                    || forced.tag() == Tag::PrimOp
                    || forced.tag() == Tag::PrimOpApp)
                {
                    Value s = callClosure(vm, forced, v);
                    s = forceValue(vm, s);
                    if (s.isString())
                        return json(std::string(s.payload.str));
                }
            }
            static const SymbolId outId = ir::globalInternSymbol("outPath");
            if (auto * op = v.payload.bindings->lookup(outId)) {
                return toJsonValue(vm, *op, symTab);
            }
        }
        json obj = json::object();
        if (v.payload.bindings) {
            // Round 1 #7: same root-stack protection.  Bindings are
            // tenured-only today, but reading entries through the
            // stack slot is uniform and zero-cost; it makes the JSON
            // walk match the printer / forceDeep discipline.
            DeepForceGuard g(v);
            for (uint32_t i = 0; i < g.ref().payload.bindings->size; ++i) {
                auto & en = g.ref().payload.bindings->entries[i];
                // #670/#671 follow-on: copy key to owning std::string
                // before recursive toJsonValue — recursion may force
                // values that intern new symbols, invalidating any
                // string_view into the global symbol table.
                std::string key = (en.name < symTab.size())
                    ? std::string(symTab[en.name])
                    : std::to_string(en.name);
                obj[std::move(key)] = toJsonValue(vm, en.value, symTab);
            }
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

// ===========================================================================
// printNixValueRich — TW-style printer for `nix eval --impure` (#669)
// ---------------------------------------------------------------------------
// Differs from `printNixValue` only in the function / derivation / primop
// cases.  Scalars and recursive structure (lists, attrset entries) reuse
// the same formatting so output stays consistent across modes.
// ===========================================================================

/// Returns true iff the attrset looks like a derivation (matches TW's
/// `isDerivation(v)` predicate: has `type = "derivation"` attr).  When
/// true, `outDrvPath` is set to the value's `drvPath` attr (a String).
/// Used by the rich printer to emit `«derivation <drvPath>»` instead of
/// expanding the full attrset.
static bool tryGetDerivationDrvPath(const Value & v,
                                    const std::vector<std::string> & symTab,
                                    std::string_view & outDrvPath)
{
    if (v.tag() != Tag::Attrs || !v.payload.bindings) return false;
    auto * b = v.payload.bindings;
    // Helper: linear-scan lookup of a key by name (Bindings is sorted by
    // SymbolId, not name, so we cannot bsearch on the name directly without
    // resolving every SymbolId first).  Derivations have ~5-10 attrs at this
    // level so the linear cost is negligible.
    auto find = [&](std::string_view want) -> const Value * {
        for (uint32_t i = 0; i < b->size; ++i) {
            auto & en = b->entries[i];
            std::string_view nm = (en.name < symTab.size())
                ? std::string_view(symTab[en.name])
                : std::string_view{};
            if (nm == want) return &en.value;
        }
        return nullptr;
    };
    const Value * typeV = find("type");
    if (!typeV || typeV->tag() != Tag::String) return false;
    if (!typeV->payload.str || std::string_view(typeV->payload.str) != "derivation")
        return false;
    const Value * drvPathV = find("drvPath");
    if (!drvPathV || drvPathV->tag() != Tag::String) return false;
    outDrvPath = drvPathV->payload.str ? drvPathV->payload.str : "";
    return true;
}

/// Emit TW's `«lambda <name>? @ <file>:<line>:<col>»` token.  `nameHint`
/// is used when the closure's `desc->contextualName` is empty (which
/// happens for dynamic-attr names — TW's parser only setName's static
/// attr/let bindings at compile time, while dynamic attrs like
/// `{ ${name} = f: ...; }` get setName at runtime via eval.cc:1566.
/// V3 doesn't propagate that runtime mutation to the LambdaDescriptor,
/// so we accept the printing context's attr-name as a hint.
static void printClosureToken(std::ostream & out, const Closure * c,
                              std::string_view nameHint)
{
    out << "«lambda";
    if (c && c->desc) {
        if (!c->desc->contextualName.empty()) {
            out << ' ' << c->desc->contextualName;
        } else if (!nameHint.empty()) {
            out << ' ' << nameHint;
        }
        if (auto * ps = resolvePosSnapshot(c->desc->posHandle)) {
            out << " @ ";
            if (ps->file.empty()) {
                out << "«string»";
            } else if (ps->file == "<string>") {
                out << "«string»";
            } else if (ps->file == "<stdin>") {
                out << "«stdin»";
            } else if (ps->file == "<unknown>") {
                out << "«none»";
            } else {
                out << ps->file;
            }
            out << ':' << ps->line << ':' << ps->column;
        }
    }
    out << "»";
}

void printNixValueRich(std::ostream & out, const Value & v,
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
        if (v.payload.list && v.payload.list->size > 0 &&
            !seen.insert(&v).second) {
            out << "«repeated»"; return;
        }
        out << "[ ";
        if (v.payload.list)
            for (uint32_t i = 0; i < v.payload.list->size; ++i) {
                printNixValueRich(out, v.payload.list->elems[i], symTab, seen);
                out << ' ';
            }
        out << "]";
        return;
    }
    case Tag::Attrs: {
        // Derivation detection happens BEFORE cycle tracking so the
        // compact form prints even if we'd revisit the bindings — TW
        // does the same (a derivation rendered twice prints
        // `«derivation /path»` both times).
        std::string_view drvPath;
        if (tryGetDerivationDrvPath(v, symTab, drvPath)) {
            out << "«derivation " << drvPath << "»";
            return;
        }
        if (v.payload.bindings && v.payload.bindings->size > 0 &&
            !seen.insert(v.payload.bindings).second) {
            out << "«repeated»"; return;
        }
        out << "{ ";
        if (v.payload.bindings) {
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
                printNixValueRich(out, *val, symTab, seen);
                out << "; ";
            }
        }
        out << "}";
        return;
    }
    case Tag::Closure: {
        // TW format: «lambda <name>? @ <file>:<line>:<col>».  The
        // dispatch is factored into `printClosureToken` so the lazy
        // overload can pass an attr-name hint for dynamic-attr-bound
        // lambdas (where `desc->contextualName` is empty at compile
        // time but TW assigns the name at runtime).
        printClosureToken(out, v.payload.closure, std::string_view{});
        return;
    }
    case Tag::PrimOp: {
        out << "«primop";
        if (v.payload.primop && !v.payload.primop->name.empty())
            out << ' ' << v.payload.primop->name;
        out << "»";
        return;
    }
    case Tag::PrimOpApp: {
        // TW format: «partially applied primop <name>».  Walk the
        // App-spine via ValuePair::left until we reach the base
        // Tag::PrimOp; that's the primop being curried.  Limit to a
        // small depth to avoid pathological loops (real chains are
        // bounded by the primop's arity, at most 8).
        out << "«partially applied primop";
        const Value * cur = &v;
        int hops = 0;
        while (cur && cur->tag() == Tag::PrimOpApp && cur->payload.pair && hops < 16) {
            cur = &cur->payload.pair->left;
            ++hops;
        }
        if (cur && cur->tag() == Tag::PrimOp
            && cur->payload.primop && !cur->payload.primop->name.empty())
            out << ' ' << cur->payload.primop->name;
        out << "»";
        return;
    }
    case Tag::Thunk:    out << "«thunk»"; return;
    case Tag::App:      out << "«thunk»"; return;  // TW prints both Thunk + App as «thunk»
    // TW's printThunk emits the explanatory phrasing for Blackhole — see
    // libexpr/print.cc:489-500.  The phrasing is intentionally hedged
    // ("potential") because a blackhole-in-context might still resolve
    // via builtins.trace etc.; match it byte-for-byte.
    case Tag::Blackhole:out << "«potential infinite recursion»"; return;
    case Tag::External: out << "«external»"; return;
    case Tag::Slot:     out << "«slot»"; return;  // v3-only; no TW analog
    case Tag::Uninitialized:
    default:            out << "«value tag=" << (int)v.tag() << "»"; return;
    }
}

void printNixValueRich(std::ostream & out, const Value & v,
                       const std::vector<std::string> & symTab)
{
    std::set<const void *> seen;
    printNixValueRich(out, v, symTab, seen);
}

// ---------------------------------------------------------------------------
// Lazy + per-error rich printer (TW parity for `nix eval` on lazy values)
// ---------------------------------------------------------------------------
//
// TW's `ValuePrinter::print` (libexpr/print.cc:546-631) forces each value
// INLINE inside a try/catch, so an attrset like `{ a = 1; b = throw "no";
// c = 3; }` renders as `{ a = 1; b = «error: no»; c = 3; }` rather than
// aborting the whole print.
//
// Pre-fix `runV3DirectEval` ran `forceDeep` BEFORE the printer, so any
// `throw` deep in the attrset propagated up and aborted the print
// entirely.  This overload mirrors TW: force the value at the entry, and
// recurse into list/attrset children that each in turn force themselves
// inside their own try/catch.
//
// `vm` is required to call `forceValue`.  All other shape and tag
// handling is identical to the no-vm overload, so we delegate via a
// per-recursion lambda that forces + dispatches.
void printNixValueRich(std::ostream & out, VMState & vm, const Value & v,
                       const std::vector<std::string> & symTab,
                       std::set<const void *> & seen)
{
    // Force the current value before printing.  TW catches errors at
    // every depth (libexpr/print.cc:625) and emits `«error: <msg>»`
    // instead; we mirror that with std::exception catch (v3 errors all
    // derive from BaseError → std::exception).
    //
    // Track-repeated identity uses the caller-supplied `&v` (the
    // attrset-entry's storage address), NOT the stack-local force
    // target — `&forced` is reused across sibling recursion frames and
    // would generate false-positive `«repeated»` for the second of
    // two equally-structured sibling list values.  Matches TW's
    // `seen->insert(&v)` discipline.
    Value forced;
    try {
        forced = forceValue(vm, v);
    } catch (const std::exception & e) {
        out << "«error: " << e.what() << "»";
        return;
    }

    // Only List and Attrs need the lazy-recursing path (their children
    // may be unforced thunks that throw); every other tag is a leaf
    // the no-vm overload handles correctly given an already-forced
    // value.  Use if/else so we don't have to enumerate every Tag for
    // `-Werror=switch-enum`.
    if (forced.tag() == Tag::List) {
        if (forced.payload.list && forced.payload.list->size > 0 &&
            !seen.insert(&v).second) {
            out << "«repeated»"; return;
        }
        out << "[ ";
        if (forced.payload.list) {
            // Round 1 #7: hold `forced` on the deep-force root stack
            // so its `payload.list` survives scavenge during the
            // recursive printNixValueRich call (which re-enters
            // forceValue).
            DeepForceGuard g(forced);
            for (uint32_t i = 0; i < g.ref().payload.list->size; ++i) {
                printNixValueRich(out, vm, g.ref().payload.list->elems[i],
                                  symTab, seen);
                out << ' ';
            }
        }
        out << "]";
        return;
    }
    if (forced.tag() == Tag::Attrs) {
        // Derivation detection must force `type` (TW's
        // `EvalState::isDerivation`; libexpr/eval.cc:2848) AND
        // `drvPath` for the print shortcut.  Without force, the
        // bindings entry is Tag::Thunk, the helper rejects, and the
        // printer falls through to the full attrset path — which
        // forces every attr including `passthru.tests`, triggering
        // nixpkgs's deprecation warning that TW never emits because
        // it short-circuits at the `type` check.
        if (forced.payload.bindings) {
            auto * b = forced.payload.bindings;
            // Inline force-then-check; can't reuse the const helper
            // because we need to mutate-in-place for cache and the
            // helper's signature is `const Value &`.
            auto findEntry = [&](std::string_view want) -> Value * {
                for (uint32_t i = 0; i < b->size; ++i) {
                    std::string_view nm = (b->entries[i].name < symTab.size())
                        ? std::string_view(symTab[b->entries[i].name])
                        : std::string_view{};
                    if (nm == want) return &b->entries[i].value;
                }
                return nullptr;
            };
            Value * typeV = findEntry("type");
            if (typeV) {
                try { *typeV = forceValue(vm, *typeV); }
                catch (const std::exception &) { typeV = nullptr; }
            }
            if (typeV && typeV->tag() == Tag::String
                && typeV->payload.str
                && std::string_view(typeV->payload.str) == "derivation")
            {
                Value * drvPathV = findEntry("drvPath");
                if (drvPathV) {
                    try { *drvPathV = forceValue(vm, *drvPathV); }
                    catch (const std::exception &) { drvPathV = nullptr; }
                }
                if (drvPathV && drvPathV->tag() == Tag::String
                    && drvPathV->payload.str)
                {
                    out << "«derivation " << drvPathV->payload.str << "»";
                    return;
                }
            }
        }
        if (forced.payload.bindings && forced.payload.bindings->size > 0 &&
            !seen.insert(forced.payload.bindings).second) {
            out << "«repeated»"; return;
        }
        out << "{ ";
        if (forced.payload.bindings) {
            std::vector<std::pair<std::string, const Value *>> items;
            items.reserve(forced.payload.bindings->size);
            for (uint32_t i = 0; i < forced.payload.bindings->size; ++i) {
                auto & en = forced.payload.bindings->entries[i];
                std::string key = (en.name < symTab.size())
                    ? symTab[en.name] : std::to_string(en.name);
                items.emplace_back(std::move(key), &en.value);
            }
            std::sort(items.begin(), items.end(),
                      [](auto & a, auto & b) { return a.first < b.first; });
            for (auto & [name, val] : items) {
                printAttrName(out, name);
                out << " = ";
                printNixValueRich(out, vm, *val, symTab, seen);
                out << "; ";
            }
            // KNOWN LIMITATION: dynamic-attr-bound lambdas like `{ ${n}
            // = f: ...; }` print without a name because TW's runtime
            // name assignment (eval.cc:1566 `i.valueExpr->setName`) is
            // a TW AST mutation that doesn't translate cleanly to v3
            // bytecode.  Using the attr-name as a printer hint was
            // tried and rejected — it over-applies for var-bound
            // attrs like `{ __unfix__ = f; }` (fix' in nixpkgs).
            // A correct fix needs per-Closure runtime-name storage,
            // updated by an emit-time-detected dynamic-attr-inline-
            // lambda path.  Tracked as future work.
        }
        out << "}";
        return;
    }
    // Scalars + closures + primops + thunks: delegate to the no-vm
    // overload so leaf-token formatting stays in one place.
    printNixValueRich(out, forced, symTab, seen);
}

void printNixValueRich(std::ostream & out, VMState & vm, const Value & v,
                       const std::vector<std::string> & symTab)
{
    std::set<const void *> seen;
    printNixValueRich(out, vm, v, symTab, seen);
}

} // namespace nix::v3
