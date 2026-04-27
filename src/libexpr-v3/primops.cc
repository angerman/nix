/// @file
/// v3 starter primop set — implementations against v3::Value.
///
/// Implements a curated subset that covers the most common Nix patterns
/// without requiring store / fetcher / parser plumbing:
///
///   length, head, tail, elemAt
///   attrNames, attrValues, hasAttr, getAttr
///   isAttrs, isList, isFunction, isString, isInt, isBool, isNull,
///   isFloat, isPath
///   toString, typeOf
///   add, sub, mul, div  (numeric — same as the inline arith ops; useful
///                        when invoked indirectly via builtins.<op>)
///   stringLength
///   throw (terminates)
///
/// Bigger primops (import, derivationStrict, fetch*, exec) interface with
/// substantial C++ infrastructure and are deferred to AST integration.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/primop.hh"
#include "v3/alloc.hh"
#include "v3/lower.hh"
#include "v3/vm.hh"

#include "nix/expr/eval.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/util/canon-path.hh"
#include "nix/util/hash.hh"

#include <nlohmann/json.hpp>
#include <toml.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace nix::v3 {

namespace {

std::unordered_map<std::string, PrimOp> & registry()
{
    static std::unordered_map<std::string, PrimOp> r;
    return r;
}

thread_local nix::EvalState * tlNixEvalState = nullptr;

std::mutex & registryMutex()
{
    static std::mutex m;
    return m;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

[[noreturn]] inline void typeError(std::string_view op, std::string_view expected)
{
    throw std::runtime_error("v3 primop " + std::string(op) + ": expected " + std::string(expected));
}

inline bool valueEqual(VMState & vm, Value a, Value b)
{
    a = forceValue(vm, a);
    b = forceValue(vm, b);
    if (a.tag() != b.tag()) {
        if (a.isInt() && b.isFloat()) return static_cast<double>(a.payload.i) == b.payload.f;
        if (a.isFloat() && b.isInt()) return a.payload.f == static_cast<double>(b.payload.i);
        return false;
    }
    switch (a.tag()) {
    case Tag::Int:    return a.payload.i == b.payload.i;
    case Tag::Float:  return a.payload.f == b.payload.f;
    case Tag::Bool:   return a.payload.i == b.payload.i;
    case Tag::Null:   return true;
    case Tag::String: return std::string_view(a.payload.str) == std::string_view(b.payload.str);
    case Tag::Path:   return std::string_view(a.payload.path) == std::string_view(b.payload.path);
    case Tag::List: {
        auto * la = a.payload.list; auto * lb = b.payload.list;
        if (la == lb) return true;
        uint32_t na = la ? la->size : 0; uint32_t nb = lb ? lb->size : 0;
        if (na != nb) return false;
        for (uint32_t i = 0; i < na; ++i)
            if (!valueEqual(vm, la->elems[i], lb->elems[i])) return false;
        return true;
    }
    case Tag::Attrs: {
        auto * aa = a.payload.bindings; auto * bb = b.payload.bindings;
        if (aa == bb) return true;
        uint32_t na = aa ? aa->size : 0; uint32_t nb = bb ? bb->size : 0;
        if (na != nb) return false;
        for (uint32_t i = 0; i < na; ++i) {
            if (aa->entries[i].name != bb->entries[i].name) return false;
            if (!valueEqual(vm, aa->entries[i].value, bb->entries[i].value)) return false;
        }
        return true;
    }
    case Tag::Uninitialized:
    case Tag::Closure:
    case Tag::Thunk:
    case Tag::PrimOp:
    case Tag::PrimOpApp:
    case Tag::App:
    case Tag::Blackhole:
    case Tag::External:
    default:          return a.payload.raw == b.payload.raw;
    }
}

inline std::string toStr(const Value & v)
{
    switch (v.tag()) {
    case Tag::String: return std::string(v.payload.str);
    case Tag::Path:   return std::string(v.payload.path);
    case Tag::Int:    return std::to_string(v.payload.i);
    case Tag::Float:  return std::to_string(v.payload.f);
    case Tag::Bool:   return v.payload.i == 1 ? "1" : "";
    case Tag::Null:   return "";
    case Tag::Uninitialized:
    case Tag::Attrs:
    case Tag::List:
    case Tag::Closure:
    case Tag::Thunk:
    case Tag::PrimOp:
    case Tag::PrimOpApp:
    case Tag::App:
    case Tag::Blackhole:
    case Tag::External:
    default:          throw std::runtime_error("v3 toString: cannot stringify this type");
    }
}

inline Value mkStringValueOwned(std::string s)
{
    char * buf = static_cast<char *>(std::malloc(s.size() + 1));
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
    Value v;
    v.mkString(buf);
    return v;
}

// ---------------------------------------------------------------------------
// Primop bodies
// ---------------------------------------------------------------------------

void primLength(EvalState &, Value * args, Value & out)
{
    const Value & v = args[0];
    int64_t n = 0;
    if (v.isList())   n = v.payload.list ? v.payload.list->size : 0;
    else if (v.isString()) n = static_cast<int64_t>(std::strlen(v.payload.str));
    else                   typeError("length", "list or string");
    out.mkInt(n);
}

void primHead(EvalState &, Value * args, Value & out)
{
    const Value & v = args[0];
    if (!v.isList() || !v.payload.list || v.payload.list->size == 0)
        throw std::runtime_error("v3 primop head: empty list or wrong type");
    out = v.payload.list->elems[0];
}

void primTail(EvalState &, Value * args, Value & out)
{
    const Value & v = args[0];
    if (!v.isList() || !v.payload.list || v.payload.list->size == 0)
        throw std::runtime_error("v3 primop tail: empty list or wrong type");
    uint32_t n = v.payload.list->size;
    ListVec * out_l = Alloc::allocList(n - 1);
    allocStats().listsAllocated++;
    for (uint32_t i = 1; i < n; ++i)
        out_l->elems[i - 1] = v.payload.list->elems[i];
    out.tag_payload = static_cast<uint64_t>(Tag::List);
    out.payload.list = out_l;
}

void primElemAt(EvalState &, Value * args, Value & out)
{
    const Value & lst = args[0];
    const Value & idx = args[1];
    if (!lst.isList() || !idx.isInt()) typeError("elemAt", "list and int");
    uint32_t n = lst.payload.list ? lst.payload.list->size : 0;
    if (idx.payload.i < 0 || static_cast<uint64_t>(idx.payload.i) >= n)
        throw std::runtime_error("v3 primop elemAt: index out of range");
    out = lst.payload.list->elems[idx.payload.i];
}

void primAttrNames(EvalState &, Value * args, Value & out)
{
    const Value & a = args[0];
    if (!a.isAttrs() || !a.payload.bindings) typeError("attrNames", "attrset");
    uint32_t n = a.payload.bindings->size;
    ListVec * lv = Alloc::allocList(n);
    allocStats().listsAllocated++;
    auto & symTab = ir::globalSymbolTable();
    for (uint32_t i = 0; i < n; ++i) {
        SymbolId sid = a.payload.bindings->entries[i].name;
        Value v = mkStringValueOwned(sid < symTab.size() ? symTab[sid] : std::to_string(sid));
        lv->elems[i] = v;
    }
    // Sort lexicographically by name — matches tree-walker semantics
    // and decouples output order from the global symbol-table
    // insertion order.
    std::sort(lv->elems, lv->elems + n,
        [](const Value & x, const Value & y) {
            return std::string_view(x.payload.str) < std::string_view(y.payload.str);
        });
    out.tag_payload = static_cast<uint64_t>(Tag::List);
    out.payload.list = lv;
}

void primAttrValues(EvalState &, Value * args, Value & out)
{
    const Value & a = args[0];
    if (!a.isAttrs() || !a.payload.bindings) typeError("attrValues", "attrset");
    uint32_t n = a.payload.bindings->size;
    // Build (name, value) pairs, sort by name, then drop the name.
    auto & symTab = ir::globalSymbolTable();
    std::vector<std::pair<std::string_view, Value>> pairs;
    pairs.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        SymbolId sid = a.payload.bindings->entries[i].name;
        std::string_view nm = sid < symTab.size() ? std::string_view(symTab[sid]) : std::string_view("");
        pairs.emplace_back(nm, a.payload.bindings->entries[i].value);
    }
    std::sort(pairs.begin(), pairs.end(),
        [](const auto & x, const auto & y) { return x.first < y.first; });
    ListVec * lv = Alloc::allocList(n);
    allocStats().listsAllocated++;
    for (uint32_t i = 0; i < n; ++i) lv->elems[i] = pairs[i].second;
    out.tag_payload = static_cast<uint64_t>(Tag::List);
    out.payload.list = lv;
}

void primIsAttrs   (EvalState &, Value * args, Value & out) { out = args[0].isAttrs()    ? Value::vTrue : Value::vFalse; }
void primIsList    (EvalState &, Value * args, Value & out) { out = args[0].isList()     ? Value::vTrue : Value::vFalse; }
void primIsFunction(EvalState &, Value * args, Value & out) { out = (args[0].isClosure() || args[0].isPrimOp() || args[0].tag() == Tag::PrimOpApp) ? Value::vTrue : Value::vFalse; }
void primIsString  (EvalState &, Value * args, Value & out) { out = args[0].isString()   ? Value::vTrue : Value::vFalse; }
void primIsInt     (EvalState &, Value * args, Value & out) { out = args[0].isInt()      ? Value::vTrue : Value::vFalse; }
void primIsBool    (EvalState &, Value * args, Value & out) { out = args[0].isBool()     ? Value::vTrue : Value::vFalse; }
void primIsNull    (EvalState &, Value * args, Value & out) { out = args[0].isNull()     ? Value::vTrue : Value::vFalse; }
void primIsFloat   (EvalState &, Value * args, Value & out) { out = args[0].isFloat()    ? Value::vTrue : Value::vFalse; }
void primIsPath    (EvalState &, Value * args, Value & out) { out = args[0].isPath()     ? Value::vTrue : Value::vFalse; }

void primToString(EvalState &, Value * args, Value & out)
{
    out = mkStringValueOwned(toStr(args[0]));
}

void primTypeOf(EvalState &, Value * args, Value & out)
{
    const Value & v = args[0];
    const char * t = "unknown";
    switch (v.tag()) {
    case Tag::Int:    t = "int";    break;
    case Tag::Float:  t = "float";  break;
    case Tag::Bool:   t = "bool";   break;
    case Tag::Null:   t = "null";   break;
    case Tag::String: t = "string"; break;
    case Tag::Path:   t = "path";   break;
    case Tag::Attrs:  t = "set";    break;
    case Tag::List:   t = "list";   break;
    case Tag::Closure:
    case Tag::PrimOp:
    case Tag::PrimOpApp: t = "lambda"; break;
    case Tag::Thunk:  t = "thunk"; break;
    case Tag::Uninitialized:
    case Tag::App:
    case Tag::Blackhole:
    case Tag::External:
    default:          t = "unknown";
    }
    out = mkStringValueOwned(t);
}

void primStringLength(EvalState &, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("stringLength", "string");
    out.mkInt(static_cast<int64_t>(std::strlen(args[0].payload.str)));
}

void primAdd(EvalState &, Value * args, Value & out)
{
    const Value & a = args[0]; const Value & b = args[1];
    if (a.isInt() && b.isInt())          out.mkInt(a.payload.i + b.payload.i);
    else if (a.isFloat() && b.isFloat()) out.mkFloat(a.payload.f + b.payload.f);
    else if (a.isInt() && b.isFloat())   out.mkFloat(static_cast<double>(a.payload.i) + b.payload.f);
    else if (a.isFloat() && b.isInt())   out.mkFloat(a.payload.f + static_cast<double>(b.payload.i));
    else typeError("add", "numeric");
}

void primSub(EvalState &, Value * args, Value & out)
{
    const Value & a = args[0]; const Value & b = args[1];
    if (a.isInt() && b.isInt())          out.mkInt(a.payload.i - b.payload.i);
    else if (a.isFloat() && b.isFloat()) out.mkFloat(a.payload.f - b.payload.f);
    else if (a.isInt() && b.isFloat())   out.mkFloat(static_cast<double>(a.payload.i) - b.payload.f);
    else if (a.isFloat() && b.isInt())   out.mkFloat(a.payload.f - static_cast<double>(b.payload.i));
    else typeError("sub", "numeric");
}

void primMul(EvalState &, Value * args, Value & out)
{
    const Value & a = args[0]; const Value & b = args[1];
    if (a.isInt() && b.isInt())          out.mkInt(a.payload.i * b.payload.i);
    else if (a.isFloat() && b.isFloat()) out.mkFloat(a.payload.f * b.payload.f);
    else if (a.isInt() && b.isFloat())   out.mkFloat(static_cast<double>(a.payload.i) * b.payload.f);
    else if (a.isFloat() && b.isInt())   out.mkFloat(a.payload.f * static_cast<double>(b.payload.i));
    else typeError("mul", "numeric");
}

void primDiv(EvalState &, Value * args, Value & out)
{
    const Value & a = args[0]; const Value & b = args[1];
    if (a.isInt() && b.isInt()) {
        if (b.payload.i == 0) throw std::runtime_error("v3 primop div: division by zero");
        out.mkInt(a.payload.i / b.payload.i);
    } else if (a.isFloat() && b.isFloat()) {
        out.mkFloat(a.payload.f / b.payload.f);
    } else if (a.isInt() && b.isFloat()) {
        out.mkFloat(static_cast<double>(a.payload.i) / b.payload.f);
    } else if (a.isFloat() && b.isInt()) {
        out.mkFloat(a.payload.f / static_cast<double>(b.payload.i));
    } else typeError("div", "numeric");
}

void primThrow(EvalState &, Value * args, Value &)
{
    if (!args[0].isString()) typeError("throw", "string");
    throw std::runtime_error(std::string("v3 throw: ") + args[0].payload.str);
}

void primConcatLists(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isList()) typeError("concatLists", "list of lists");
    uint32_t total = 0;
    auto & outer = args[0];
    // Force each outer element (each should be a list); they're lazy
    // by default now.
    for (uint32_t i = 0; i < outer.payload.list->size; ++i) {
        outer.payload.list->elems[i] = forceValue(*state.vm, outer.payload.list->elems[i]);
        const Value & el = outer.payload.list->elems[i];
        if (!el.isList()) typeError("concatLists", "list of lists");
        total += el.payload.list ? el.payload.list->size : 0;
    }
    ListVec * result = Alloc::allocList(total);
    allocStats().listsAllocated++;
    uint32_t k = 0;
    for (uint32_t i = 0; i < outer.payload.list->size; ++i) {
        const Value & el = outer.payload.list->elems[i];
        if (!el.payload.list) continue;
        for (uint32_t j = 0; j < el.payload.list->size; ++j)
            result->elems[k++] = el.payload.list->elems[j];
    }
    out.tag_payload = static_cast<uint64_t>(Tag::List);
    out.payload.list = result;
}

void primConcatStringsSep(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("concatStringsSep", "separator string");
    if (!args[1].isList())   typeError("concatStringsSep", "list of strings");
    std::string sep(args[0].payload.str);
    std::string result;
    auto * list = args[1].payload.list;
    for (uint32_t i = 0; list && i < list->size; ++i) {
        if (i > 0) result += sep;
        Value el = forceValue(*state.vm, list->elems[i]);
        if (!el.isString()) typeError("concatStringsSep", "list of strings");
        result += el.payload.str;
    }
    out = mkStringValueOwned(result);
}

void primSubstring(EvalState &, Value * args, Value & out)
{
    if (!args[0].isInt() || !args[1].isInt() || !args[2].isString())
        typeError("substring", "(int, int, string)");
    int64_t start = args[0].payload.i;
    int64_t len = args[1].payload.i;
    std::string_view src(args[2].payload.str);
    if (start < 0) start = 0;
    if (static_cast<size_t>(start) >= src.size()) {
        out = mkStringValueOwned("");
        return;
    }
    size_t available = src.size() - start;
    size_t actualLen = (len < 0) ? available : std::min(static_cast<size_t>(len), available);
    out = mkStringValueOwned(std::string(src.substr(start, actualLen)));
}

void primMap(EvalState & state, Value * args, Value & out)
{
    if (!args[1].isList()) typeError("map", "list");
    auto * src = args[1].payload.list;
    if (!src || src->size == 0) {
        out.tag_payload = static_cast<uint64_t>(Tag::List);
        out.payload.list = Alloc::allocList(0);
        allocStats().listsAllocated++;
        return;
    }
    Value fun = args[0];
    ListVec * result = Alloc::allocList(src->size);
    allocStats().listsAllocated++;
    for (uint32_t i = 0; i < src->size; ++i) {
        result->elems[i] = callClosure(*state.vm, fun, src->elems[i]);
    }
    out.tag_payload = static_cast<uint64_t>(Tag::List);
    out.payload.list = result;
}

void primFilter(EvalState & state, Value * args, Value & out)
{
    if (!args[1].isList()) typeError("filter", "list");
    auto * src = args[1].payload.list;
    if (!src || src->size == 0) {
        out.tag_payload = static_cast<uint64_t>(Tag::List);
        out.payload.list = Alloc::allocList(0);
        allocStats().listsAllocated++;
        return;
    }
    Value pred = args[0];
    std::vector<Value> kept;
    kept.reserve(src->size);
    for (uint32_t i = 0; i < src->size; ++i) {
        Value r = callClosure(*state.vm, pred, src->elems[i]);
        if (!r.isBool()) typeError("filter", "predicate returning bool");
        if (r.payload.i == 1) kept.push_back(src->elems[i]);
    }
    ListVec * result = Alloc::allocList(static_cast<uint32_t>(kept.size()));
    allocStats().listsAllocated++;
    for (size_t i = 0; i < kept.size(); ++i) result->elems[i] = kept[i];
    out.tag_payload = static_cast<uint64_t>(Tag::List);
    out.payload.list = result;
}

void primFoldl(EvalState & state, Value * args, Value & out)
{
    // foldl' op nul list  —  strict left fold
    if (!args[2].isList()) typeError("foldl'", "list");
    Value op = args[0];
    Value acc = args[1];
    auto * src = args[2].payload.list;
    if (src) {
        for (uint32_t i = 0; i < src->size; ++i) {
            // Curried: op acc elem
            Value step1 = callClosure(*state.vm, op, acc);
            acc = callClosure(*state.vm, step1, src->elems[i]);
        }
    }
    out = acc;
}

void primGenList(EvalState & state, Value * args, Value & out)
{
    if (!args[1].isInt()) typeError("genList", "int length");
    int64_t n = args[1].payload.i;
    if (n < 0) throw std::runtime_error("v3 primop genList: negative length");
    Value gen = args[0];
    ListVec * result = Alloc::allocList(static_cast<uint32_t>(n));
    allocStats().listsAllocated++;
    for (int64_t i = 0; i < n; ++i) {
        Value idx; idx.mkInt(i);
        result->elems[i] = callClosure(*state.vm, gen, idx);
    }
    out.tag_payload = static_cast<uint64_t>(Tag::List);
    out.payload.list = result;
}

void primAll(EvalState & state, Value * args, Value & out)
{
    if (!args[1].isList()) typeError("all", "list");
    auto * src = args[1].payload.list;
    Value pred = args[0];
    bool all = true;
    if (src) {
        for (uint32_t i = 0; i < src->size; ++i) {
            Value r = callClosure(*state.vm, pred, src->elems[i]);
            if (!r.isBool()) typeError("all", "bool from predicate");
            if (r.payload.i == 0) { all = false; break; }
        }
    }
    out = all ? Value::vTrue : Value::vFalse;
}

void primAny(EvalState & state, Value * args, Value & out)
{
    if (!args[1].isList()) typeError("any", "list");
    auto * src = args[1].payload.list;
    Value pred = args[0];
    bool any = false;
    if (src) {
        for (uint32_t i = 0; i < src->size; ++i) {
            Value r = callClosure(*state.vm, pred, src->elems[i]);
            if (!r.isBool()) typeError("any", "bool from predicate");
            if (r.payload.i == 1) { any = true; break; }
        }
    }
    out = any ? Value::vTrue : Value::vFalse;
}

void primGetEnv(EvalState &, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("getEnv", "string");
    const char * e = std::getenv(args[0].payload.str);
    out = mkStringValueOwned(e ? e : "");
}

void primCompareVersions(EvalState &, Value * args, Value & out)
{
    // Simple string compare for now (nix has more sophisticated semver
    // logic; revisit if any test needs real semver behavior).
    if (!args[0].isString() || !args[1].isString())
        typeError("compareVersions", "two strings");
    int cmp = std::strcmp(args[0].payload.str, args[1].payload.str);
    out.mkInt(cmp < 0 ? -1 : (cmp > 0 ? 1 : 0));
}

void primConcatMap(EvalState & state, Value * args, Value & out)
{
    if (!args[1].isList()) typeError("concatMap", "list");
    auto * src = args[1].payload.list;
    Value fn = args[0];
    std::vector<Value> all;
    if (src) {
        for (uint32_t i = 0; i < src->size; ++i) {
            Value r = callClosure(*state.vm, fn, src->elems[i]);
            if (!r.isList()) typeError("concatMap", "function returning list");
            if (r.payload.list)
                for (uint32_t j = 0; j < r.payload.list->size; ++j)
                    all.push_back(r.payload.list->elems[j]);
        }
    }
    ListVec * result = Alloc::allocList(static_cast<uint32_t>(all.size()));
    allocStats().listsAllocated++;
    for (size_t i = 0; i < all.size(); ++i) result->elems[i] = all[i];
    out.tag_payload = static_cast<uint64_t>(Tag::List);
    out.payload.list = result;
}

void primPartition(EvalState & state, Value * args, Value & out)
{
    if (!args[1].isList()) typeError("partition", "list");
    auto * src = args[1].payload.list;
    Value pred = args[0];
    std::vector<Value> right_, wrong_;
    if (src) {
        for (uint32_t i = 0; i < src->size; ++i) {
            Value r = callClosure(*state.vm, pred, src->elems[i]);
            if (!r.isBool()) typeError("partition", "predicate returning bool");
            if (r.payload.i == 1) right_.push_back(src->elems[i]);
            else                  wrong_.push_back(src->elems[i]);
        }
    }
    auto mkList = [](std::vector<Value> & v) {
        ListVec * l = Alloc::allocList(static_cast<uint32_t>(v.size()));
        allocStats().listsAllocated++;
        for (size_t i = 0; i < v.size(); ++i) l->elems[i] = v[i];
        Value out;
        out.tag_payload = static_cast<uint64_t>(Tag::List);
        out.payload.list = l;
        return out;
    };
    Value rightV = mkList(right_);
    Value wrongV = mkList(wrong_);

    // Intern via the global table so the resulting attrset's SymbolIds
    // match what other CUs and the JSON printer use.
    SymbolId sRight = ir::globalInternSymbol("right");
    SymbolId sWrong = ir::globalInternSymbol("wrong");

    Bindings * b = Alloc::allocBindings(2);
    allocStats().attrsetsAllocated++;
    if (sRight < sWrong) {
        b->entries[0] = {sRight, rightV};
        b->entries[1] = {sWrong, wrongV};
    } else {
        b->entries[0] = {sWrong, wrongV};
        b->entries[1] = {sRight, rightV};
    }
    out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
    out.payload.bindings = b;
}

/// Helper: intern a string into the global symbol table so the
/// resulting SymbolId is usable across CUs (matches what lower.cc
/// emits in OP_ATTRS_INIT and what attrset bindings store).
inline SymbolId vmIntern(EvalState & /*state*/, std::string_view s)
{
    return ir::globalInternSymbol(s);
}

inline std::string_view vmSymName(EvalState & /*state*/, SymbolId id)
{
    auto & st = ir::globalSymbolTable();
    return id < st.size() ? std::string_view(st[id]) : std::string_view("");
}

/// listToAttrs: takes a list of `{ name = "..."; value = ...; }` and
/// builds an attrset.
void primListToAttrs(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isList()) typeError("listToAttrs", "list");
    auto * src = args[0].payload.list;
    if (!src || src->size == 0) {
        Bindings * b = Alloc::allocBindings(0);
        allocStats().attrsetsAllocated++;
        out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
        out.payload.bindings = b;
        return;
    }
    SymbolId nameSym  = vmIntern(state, "name");
    SymbolId valueSym = vmIntern(state, "value");
    std::vector<std::pair<SymbolId, Value>> entries;
    entries.reserve(src->size);
    for (uint32_t i = 0; i < src->size; ++i) {
        Value el = forceValue(*state.vm, src->elems[i]);
        if (!el.isAttrs() || !el.payload.bindings)
            typeError("listToAttrs", "list of attrsets");
        const Value * nvRaw = el.payload.bindings->lookup(nameSym);
        const Value * vvRaw = el.payload.bindings->lookup(valueSym);
        if (!nvRaw || !vvRaw)
            typeError("listToAttrs", "{ name = string; value = ...; }");
        Value nv = forceValue(*state.vm, *nvRaw);
        if (!nv.isString())
            typeError("listToAttrs", "{ name = string; value = ...; }");
        SymbolId k = vmIntern(state, nv.payload.str);
        // value stays lazy on purpose
        entries.emplace_back(k, *vvRaw);
    }
    std::sort(entries.begin(), entries.end(),
        [](auto & a, auto & b) { return a.first < b.first; });
    // De-duplicate (last-write-wins for nix listToAttrs).
    std::vector<std::pair<SymbolId, Value>> dedup;
    dedup.reserve(entries.size());
    for (auto & p : entries) {
        if (!dedup.empty() && dedup.back().first == p.first)
            dedup.back() = p;
        else
            dedup.push_back(p);
    }
    Bindings * b = Alloc::allocBindings(static_cast<uint32_t>(dedup.size()));
    allocStats().attrsetsAllocated++;
    for (size_t i = 0; i < dedup.size(); ++i) {
        b->entries[i].name  = dedup[i].first;
        b->entries[i].value = dedup[i].second;
    }
    out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
    out.payload.bindings = b;
}

void primRemoveAttrs(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isAttrs()) typeError("removeAttrs", "attrset");
    if (!args[1].isList())  typeError("removeAttrs", "list of strings");
    auto * src = args[0].payload.bindings;
    auto * names = args[1].payload.list;
    if (!src || !names || names->size == 0) { out = args[0]; return; }
    std::unordered_set<SymbolId> toRemove;
    for (uint32_t i = 0; i < names->size; ++i) {
        const Value & el = names->elems[i];
        if (!el.isString()) typeError("removeAttrs", "list of strings");
        toRemove.insert(vmIntern(state, el.payload.str));
    }
    Bindings * result = Alloc::allocBindings(src->size);
    allocStats().attrsetsAllocated++;
    uint32_t k = 0;
    for (uint32_t i = 0; i < src->size; ++i) {
        if (toRemove.count(src->entries[i].name) == 0) {
            result->entries[k++] = src->entries[i];
        }
    }
    result->size = k;
    out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
    out.payload.bindings = result;
}

void primIntersectAttrs(EvalState &, Value * args, Value & out)
{
    if (!args[0].isAttrs() || !args[1].isAttrs())
        typeError("intersectAttrs", "two attrsets");
    auto * keep = args[0].payload.bindings;
    auto * src  = args[1].payload.bindings;
    if (!keep || !src) {
        Bindings * b = Alloc::allocBindings(0);
        allocStats().attrsetsAllocated++;
        out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
        out.payload.bindings = b;
        return;
    }
    Bindings * result = Alloc::allocBindings(src->size);
    allocStats().attrsetsAllocated++;
    uint32_t k = 0;
    for (uint32_t i = 0; i < src->size; ++i) {
        if (keep->lookup(src->entries[i].name))
            result->entries[k++] = src->entries[i];
    }
    result->size = k;
    out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
    out.payload.bindings = result;
}

void primMapAttrs(EvalState & state, Value * args, Value & out)
{
    Value fn = args[0];
    if (!args[1].isAttrs()) typeError("mapAttrs", "attrset");
    auto * src = args[1].payload.bindings;
    if (!src) { out = args[1]; return; }
    Bindings * result = Alloc::allocBindings(src->size);
    allocStats().attrsetsAllocated++;
    for (uint32_t i = 0; i < src->size; ++i) {
        SymbolId sym = src->entries[i].name;
        Value nameStr = mkStringValueOwned(std::string(vmSymName(state, sym)));
        // Build a Tag::App chain that, when forced, applies
        // `fn name value`.  This keeps mapAttrs lazy: `mapAttrs throw
        // attrs` only fires the throw on the entries actually demanded
        // by callers, matching tree-walker.
        ValuePair * pp1 = static_cast<ValuePair *>(std::malloc(sizeof(ValuePair)));
        pp1->left  = fn;
        pp1->right = nameStr;
        Value step1; step1.tag_payload = static_cast<uint64_t>(Tag::App); step1.payload.pair = pp1;
        ValuePair * pp2 = static_cast<ValuePair *>(std::malloc(sizeof(ValuePair)));
        pp2->left  = step1;
        pp2->right = src->entries[i].value;
        Value step2; step2.tag_payload = static_cast<uint64_t>(Tag::App); step2.payload.pair = pp2;
        result->entries[i].name  = sym;
        result->entries[i].value = step2;
    }
    out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
    out.payload.bindings = result;
}

void primElem(EvalState & state, Value * args, Value & out)
{
    if (!args[1].isList()) typeError("elem", "list");
    auto * src = args[1].payload.list;
    Value x = args[0];
    bool found = false;
    if (src) {
        for (uint32_t i = 0; i < src->size; ++i) {
            if (valueEqual(*state.vm, x, src->elems[i])) { found = true; break; }
        }
    }
    out = found ? Value::vTrue : Value::vFalse;
}

void primGetAttr(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("getAttr", "string");
    if (!args[1].isAttrs())  typeError("getAttr", "attrset");
    SymbolId k = vmIntern(state, args[0].payload.str);
    auto * b = args[1].payload.bindings;
    if (!b) throw std::runtime_error("v3 primop getAttr: attribute not found");
    const Value * v = b->lookup(k);
    if (!v) throw std::runtime_error("v3 primop getAttr: attribute not found");
    out = *v;
}

void primHasAttr(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("hasAttr", "string");
    if (!args[1].isAttrs())  typeError("hasAttr", "attrset");
    SymbolId k = vmIntern(state, args[0].payload.str);
    auto * b = args[1].payload.bindings;
    out = (b && b->has(k)) ? Value::vTrue : Value::vFalse;
}

void primCatAttrs(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("catAttrs", "string");
    if (!args[1].isList())   typeError("catAttrs", "list");
    SymbolId k = vmIntern(state, args[0].payload.str);
    auto * lst = args[1].payload.list;
    std::vector<Value> kept;
    if (lst) {
        for (uint32_t i = 0; i < lst->size; ++i) {
            Value el = forceValue(*state.vm, lst->elems[i]);
            if (!el.isAttrs() || !el.payload.bindings) continue;
            const Value * v = el.payload.bindings->lookup(k);
            if (v) kept.push_back(*v);
        }
    }
    ListVec * result = Alloc::allocList(static_cast<uint32_t>(kept.size()));
    allocStats().listsAllocated++;
    for (size_t i = 0; i < kept.size(); ++i) result->elems[i] = kept[i];
    out.tag_payload = static_cast<uint64_t>(Tag::List);
    out.payload.list = result;
}

void primReplaceStrings(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isList() || !args[1].isList() || !args[2].isString())
        typeError("replaceStrings", "(list, list, string)");
    auto * froms = args[0].payload.list;
    auto * tos   = args[1].payload.list;
    if (!froms || !tos || froms->size != tos->size)
        throw std::runtime_error("v3 primop replaceStrings: lists must have equal length");
    // Force `from` elements upfront — every iteration of the outer
    // loop reads them, and they're lazy by default.  `to` elements
    // are forced lazily inside the match branch (matches tree-walker;
    // unused replacements never fire).
    for (uint32_t j = 0; j < froms->size; ++j) {
        froms->elems[j] = forceValue(*state.vm, froms->elems[j]);
        if (!froms->elems[j].isString())
            typeError("replaceStrings", "list of strings");
    }
    std::string s(args[2].payload.str);
    std::string result;
    size_t i = 0;
    while (i < s.size()) {
        bool matched = false;
        for (uint32_t j = 0; j < froms->size; ++j) {
            const Value & f = froms->elems[j];
            std::string_view fv(f.payload.str);
            if (fv.empty()) continue;
            if (s.compare(i, fv.size(), fv) == 0) {
                Value t = forceValue(*state.vm, tos->elems[j]);
                if (!t.isString())
                    typeError("replaceStrings", "list of strings");
                result.append(t.payload.str);
                i += fv.size();
                matched = true;
                break;
            }
        }
        if (!matched) { result.push_back(s[i]); ++i; }
    }
    out = mkStringValueOwned(result);
}

void primAbort(EvalState &, Value * args, Value &)
{
    if (!args[0].isString()) typeError("abort", "string");
    throw std::runtime_error(std::string("v3 abort: ") + args[0].payload.str);
}

void primSeq(EvalState &, Value * args, Value & out)
{
    // seq: forces first arg, returns second.  Force already happened in
    // the caller's strict context (Force inserted by lowerExpr); we
    // just return args[1] here.  (For lazy semantics this would matter
    // more, but our v3 currently uses strict eval almost everywhere.)
    (void)args;
    out = args[1];
}

/// Recursively force every thunk reachable from `v`, propagating any
/// error.  Lists/attrsets are traversed; functions are not entered.
/// Tracks visited containers to break cycles like `let as = {y = as;}; in as`.
static Value forceDeepRec(VMState & vm, Value v, std::unordered_set<const void *> & seen)
{
    v = forceValue(vm, v);
    if (v.isList() && v.payload.list) {
        if (!seen.insert(v.payload.list).second) return v;
        for (uint32_t i = 0; i < v.payload.list->size; ++i)
            v.payload.list->elems[i] = forceDeepRec(vm, v.payload.list->elems[i], seen);
    } else if (v.isAttrs() && v.payload.bindings) {
        if (!seen.insert(v.payload.bindings).second) return v;
        for (uint32_t i = 0; i < v.payload.bindings->size; ++i)
            v.payload.bindings->entries[i].value =
                forceDeepRec(vm, v.payload.bindings->entries[i].value, seen);
    }
    return v;
}

static Value forceDeepRec(VMState & vm, Value v)
{
    std::unordered_set<const void *> seen;
    return forceDeepRec(vm, v, seen);
}

void primDeepSeq(EvalState & state, Value * args, Value & out)
{
    // Force `args[0]` deeply, throwing on any contained error, then
    // return `args[1]`.  Matches tree-walker semantics — used to
    // ensure errors in lazy structure are surfaced before returning.
    forceDeepRec(*state.vm, args[0]);
    out = args[1];
}

/// builtins.unsafeGetAttrPos: tree-walker tracks AST positions; v3
/// doesn't yet, so return null (mirrors a missing pos).
void primUnsafeGetAttrPos(EvalState &, Value *, Value & out) { out.mkNull(); }

/// builtins.toPath path-or-string -> path; bring-up uses identity.
/// Also accepts attrsets with `__toString` or `outPath` (the standard
/// Nix coercion path).
void primToPath(EvalState & state, Value * args, Value & out)
{
    auto fromString = [&](const char * s) {
        char * buf = static_cast<char *>(std::malloc(std::strlen(s) + 1));
        std::strcpy(buf, s);
        out.tag_payload = static_cast<uint64_t>(Tag::Path);
        out.payload.path = buf;
    };
    Value v = forceValue(*state.vm, args[0]);
    if (v.isPath())   { out = v; return; }
    if (v.isString()) { fromString(v.payload.str); return; }
    if (v.isAttrs() && v.payload.bindings) {
        static const SymbolId tsId  = ir::globalInternSymbol("__toString");
        static const SymbolId outId = ir::globalInternSymbol("outPath");
        if (auto * fn = v.payload.bindings->lookup(tsId)) {
            Value forced = forceValue(*state.vm, *fn);
            Value s = callClosure(*state.vm, forced, v);
            s = forceValue(*state.vm, s);
            if (s.isString()) { fromString(s.payload.str); return; }
        }
        if (auto * op = v.payload.bindings->lookup(outId)) {
            Value forced = forceValue(*state.vm, *op);
            if (forced.isString()) { fromString(forced.payload.str); return; }
            if (forced.isPath())   { out = forced; return; }
        }
    }
    typeError("toPath", "string or path");
}

/// builtins.splitVersion "1.2.3-alpha" -> ["1" "2" "3" "alpha"].
/// Splits on '.' and '-'; consecutive separators produce empty strings
/// (matching tree-walker behaviour).
void primSplitVersion(EvalState &, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("splitVersion", "string");
    std::string_view s(args[0].payload.str);
    std::vector<std::string> parts;
    std::string cur;
    auto emit = [&]() {
        if (!cur.empty()) parts.push_back(std::move(cur));
        cur.clear();
    };
    for (char c : s) {
        if (c == '.' || c == '-') emit();
        else cur.push_back(c);
    }
    emit();
    ListVec * lv = Alloc::allocList(static_cast<uint32_t>(parts.size()));
    allocStats().listsAllocated++;
    for (size_t i = 0; i < parts.size(); ++i) lv->elems[i] = mkStringValueOwned(parts[i]);
    out.tag_payload = static_cast<uint64_t>(Tag::List);
    out.payload.list = lv;
}

/// String-context primops.  v3 doesn't track string contexts yet, so
/// these are stubs that match the no-context case behaviour:
///   unsafeDiscardStringContext s  ->  s
///   hasContext s                  ->  false
///   getContext s                  ->  {} (empty attrset)
void primUnsafeDiscardStringContext(EvalState &, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("unsafeDiscardStringContext", "string");
    out = args[0];
}
void primHasContext(EvalState &, Value *, Value & out) { out = Value::vFalse; }
void primGetContext(EvalState &, Value *, Value & out)
{
    Bindings * b = Alloc::allocBindings(0);
    out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
    out.payload.bindings = b;
}

/// builtins.unsafeDiscardOutputDependency: same identity treatment.
void primUnsafeDiscardOutputDependency(EvalState &, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("unsafeDiscardOutputDependency", "string");
    out = args[0];
}

/// builtins.__nixPath : list of `{prefix, path}` attrsets.  Reads the
/// host EvalState's LookupPath.  Used implicitly by `<x>` syntax.
void primNixPath(EvalState & state, Value *, Value & out)
{
    if (!state.nixEvalState) {
        ListVec * empty = Alloc::allocList(0);
        out.tag_payload = static_cast<uint64_t>(Tag::List);
        out.payload.list = empty;
        return;
    }
    auto lookupPath = state.nixEvalState->getLookupPath();
    auto & lp = lookupPath.elements;
    ListVec * lv = Alloc::allocList(static_cast<uint32_t>(lp.size()));
    allocStats().listsAllocated++;
    SymbolId sPath   = vmIntern(state, "path");
    SymbolId sPrefix = vmIntern(state, "prefix");
    size_t i = 0;
    for (auto & el : lp) {
        Bindings * b = Alloc::allocBindings(2);
        SymbolId nA = sPath, nB = sPrefix;
        Value vA = mkStringValueOwned(el.path.s);
        Value vB = mkStringValueOwned(el.prefix.s);
        if (nA < nB) {
            b->entries[0] = {nA, vA};
            b->entries[1] = {nB, vB};
        } else {
            b->entries[0] = {nB, vB};
            b->entries[1] = {nA, vA};
        }
        Value v;
        v.tag_payload = static_cast<uint64_t>(Tag::Attrs);
        v.payload.bindings = b;
        lv->elems[i++] = v;
    }
    out.tag_payload = static_cast<uint64_t>(Tag::List);
    out.payload.list = lv;
}

/// builtins.__findFile : list-of-{prefix, path} → name → resolved path.
/// Looks up `name` in the search path entries and returns the matching
/// SourcePath.  Throws if no entry matches.
void primFindFile(EvalState & state, Value * args, Value & out)
{
    if (!state.nixEvalState)
        throw std::runtime_error("v3 primop findFile: no nix EvalState wired");
    if (!args[0].isList()) typeError("findFile", "list of {path, prefix}");
    if (!args[1].isString()) typeError("findFile", "string");

    // Build a LookupPath from the v3 list.  Each element is an attrset
    // with `path` (string-or-path) and `prefix` (string).
    nix::LookupPath lp;
    auto * lst = args[0].payload.list;
    if (lst) {
        SymbolId sPath   = vmIntern(state, "path");
        SymbolId sPrefix = vmIntern(state, "prefix");
        for (uint32_t i = 0; i < lst->size; ++i) {
            Value el = forceValue(*state.vm, lst->elems[i]);
            if (!el.isAttrs() || !el.payload.bindings) continue;
            const Value * pV = el.payload.bindings->lookup(sPath);
            const Value * prV = el.payload.bindings->lookup(sPrefix);
            std::string p, prefix;
            if (pV) {
                Value f = forceValue(*state.vm, *pV);
                if (f.isString()) p = f.payload.str;
                else if (f.isPath()) p = f.payload.path;
            }
            if (prV) {
                Value f = forceValue(*state.vm, *prV);
                if (f.isString()) prefix = f.payload.str;
            }
            if (p.empty()) continue;
            lp.elements.push_back({nix::LookupPath::Prefix{prefix},
                                   nix::LookupPath::Path{p}});
        }
    }
    auto sp = state.nixEvalState->findFile(lp, args[1].payload.str);
    char * buf = static_cast<char *>(std::malloc(sp.path.abs().size() + 1));
    std::strcpy(buf, sp.path.abs().c_str());
    out.tag_payload = static_cast<uint64_t>(Tag::Path);
    out.payload.path = buf;
}

/// builtins.zipAttrsWith fn list-of-attrsets:
///   merge a list of attrsets, applying `fn name [values]` to combine
///   per-name lists.  Order in the value list mirrors source order.
void primZipAttrsWith(EvalState & state, Value * args, Value & out)
{
    Value fn = args[0];
    if (!args[1].isList()) typeError("zipAttrsWith", "list of attrsets");
    auto * lst = args[1].payload.list;
    // Group by symbol id, preserving the value order seen in `lst`.
    std::unordered_map<SymbolId, std::vector<Value>> byName;
    if (lst) {
        for (uint32_t i = 0; i < lst->size; ++i) {
            Value attrs = forceValue(*state.vm, lst->elems[i]);
            if (!attrs.isAttrs() || !attrs.payload.bindings) continue;
            for (uint32_t j = 0; j < attrs.payload.bindings->size; ++j) {
                auto & en = attrs.payload.bindings->entries[j];
                byName[en.name].push_back(en.value);
            }
        }
    }
    std::vector<std::pair<SymbolId, Value>> entries;
    entries.reserve(byName.size());
    for (auto & [sid, vs] : byName) {
        // Build the value list, then call fn name list.
        ListVec * vl = Alloc::allocList(static_cast<uint32_t>(vs.size()));
        allocStats().listsAllocated++;
        for (size_t i = 0; i < vs.size(); ++i) vl->elems[i] = vs[i];
        Value lv;
        lv.tag_payload = static_cast<uint64_t>(Tag::List);
        lv.payload.list = vl;
        // Call fn with name (string) then values (list).
        auto & symTab = ir::globalSymbolTable();
        std::string nm = sid < symTab.size() ? symTab[sid] : std::to_string(sid);
        Value nameV = mkStringValueOwned(nm);
        Value step1 = callClosure(*state.vm, fn, nameV);
        Value combined = callClosure(*state.vm, step1, lv);
        entries.emplace_back(sid, combined);
    }
    std::sort(entries.begin(), entries.end(),
        [](const auto & a, const auto & b) { return a.first < b.first; });
    Bindings * b = Alloc::allocBindings(static_cast<uint32_t>(entries.size()));
    allocStats().attrsetsAllocated++;
    for (size_t i = 0; i < entries.size(); ++i) {
        b->entries[i].name = entries[i].first;
        b->entries[i].value = entries[i].second;
    }
    out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
    out.payload.bindings = b;
}

/// builtins.trace msg val: print msg to stderr, return val unchanged.
// Forward decl — defined later in this TU.
nlohmann::json valueToJson(EvalState & state, const Value & v);

void primTrace(EvalState & state, Value * args, Value & out)
{
    // tree-walker stringifies via printValueAsJSON-ish; we approximate:
    // strings print as-is, ints/bools/null/path render via toStr,
    // and complex values render as a short JSON dump.
    Value v = args[0];
    if (v.isString())   std::fprintf(stderr, "trace: %s\n", v.payload.str);
    else if (v.isInt()) std::fprintf(stderr, "trace: %lld\n", (long long)v.payload.i);
    else if (v.isFloat()) std::fprintf(stderr, "trace: %g\n", v.payload.f);
    else if (v.isBool()) std::fprintf(stderr, "trace: %s\n", v.payload.i == 1 ? "true" : "false");
    else if (v.isNull()) std::fprintf(stderr, "trace: null\n");
    else if (v.isPath()) std::fprintf(stderr, "trace: %s\n", v.payload.path);
    else {
        try {
            auto j = valueToJson(state, v);
            std::fprintf(stderr, "trace: %s\n", j.dump().c_str());
        } catch (...) {
            std::fprintf(stderr, "trace: <complex value>\n");
        }
    }
    out = args[1];
}

/// builtins.traceVerbose: same as trace but only when --trace-verbose;
/// for v3 we treat it as plain trace (no flag plumbing yet).
void primTraceVerbose(EvalState & state, Value * args, Value & out)
{
    primTrace(state, args, out);
}

void primBaseNameOf(EvalState &, Value * args, Value & out)
{
    // Mirrors tree-walker's legacyBaseNameOf: at most ONE trailing
    // slash is stripped, so `baseNameOf "a/"` is "a" but
    // `baseNameOf "a//"` is "" (a 10-year-old quirk that
    // `eval-okay-baseNameOf.nix` pins down).
    std::string_view s;
    if (args[0].isString()) s = args[0].payload.str;
    else if (args[0].isPath()) s = args[0].payload.path;
    else typeError("baseNameOf", "string or path");
    if (s.empty()) { out = mkStringValueOwned(""); return; }
    size_t last = s.size() - 1;
    if (s[last] == '/' && last > 0) last -= 1;
    size_t pos = s.rfind('/', last);
    if (pos == std::string_view::npos) pos = 0;
    else pos += 1;
    out = mkStringValueOwned(std::string(s.substr(pos, last - pos + 1)));
}

void primDirOf(EvalState &, Value * args, Value & out)
{
    std::string s;
    bool isPathV = false;
    if (args[0].isString()) s = args[0].payload.str;
    else if (args[0].isPath()) { s = args[0].payload.path; isPathV = true; }
    else typeError("dirOf", "string or path");
    auto pos = s.find_last_of('/');
    std::string dir = (pos == std::string::npos) ? "." :
                      (pos == 0) ? "/" : s.substr(0, pos);
    if (isPathV) {
        Value v;
        // Allocate a long-lived string for the path payload.
        char * buf = static_cast<char *>(std::malloc(dir.size() + 1));
        std::memcpy(buf, dir.data(), dir.size()); buf[dir.size()] = '\0';
        v.tag_payload = static_cast<uint64_t>(Tag::Path);
        v.payload.path = buf;
        out = v;
    } else {
        out = mkStringValueOwned(dir);
    }
}

void primPathExists(EvalState &, Value * args, Value & out)
{
    std::string s;
    if (args[0].isString()) s = args[0].payload.str;
    else if (args[0].isPath()) s = args[0].payload.path;
    else typeError("pathExists", "string or path");
    out = std::filesystem::exists(s) ? Value::vTrue : Value::vFalse;
}

void primSplitString(EvalState &, Value * args, Value & out)
{
    if (!args[0].isString() || !args[1].isString())
        typeError("splitString", "(separator, string)");
    std::string_view sep(args[0].payload.str);
    std::string_view s(args[1].payload.str);
    std::vector<Value> parts;
    if (sep.empty()) {
        // Empty separator: split into per-character strings.
        for (char c : s) {
            std::string single(1, c);
            parts.push_back(mkStringValueOwned(single));
        }
    } else {
        size_t pos = 0;
        while (pos <= s.size()) {
            size_t next = s.find(sep, pos);
            if (next == std::string_view::npos) {
                parts.push_back(mkStringValueOwned(std::string(s.substr(pos))));
                break;
            }
            parts.push_back(mkStringValueOwned(std::string(s.substr(pos, next - pos))));
            pos = next + sep.size();
        }
    }
    ListVec * lv = Alloc::allocList(static_cast<uint32_t>(parts.size()));
    allocStats().listsAllocated++;
    for (size_t i = 0; i < parts.size(); ++i) lv->elems[i] = parts[i];
    out.tag_payload = static_cast<uint64_t>(Tag::List);
    out.payload.list = lv;
}

/// builtins.genericClosure { startSet, operator } -- BFS closure of
/// startSet under operator.  Items are deduplicated by their "key" attr.
void primGenericClosure(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isAttrs() || !args[0].payload.bindings)
        typeError("genericClosure", "attrset");
    SymbolId sStart = vmIntern(state, "startSet");
    SymbolId sOp    = vmIntern(state, "operator");
    SymbolId sKey   = vmIntern(state, "key");
    const Value * startVRaw = args[0].payload.bindings->lookup(sStart);
    const Value * opVRaw    = args[0].payload.bindings->lookup(sOp);
    if (!startVRaw || !opVRaw)
        typeError("genericClosure", "{ startSet, operator }");
    // Attrset entries are lazy thunks; force before structural use.
    Value startV = forceValue(*state.vm, *startVRaw);
    Value opV    = forceValue(*state.vm, *opVRaw);
    if (!startV.isList()) typeError("genericClosure", "startSet must be a list");

    // Process queue: BFS.  workQueue is the items to process; result is
    // accumulated as we go.  seen[key.toString()] = true.
    std::vector<Value> result;
    std::vector<Value> work;
    if (startV.payload.list) {
        for (uint32_t i = 0; i < startV.payload.list->size; ++i)
            work.push_back(startV.payload.list->elems[i]);
    }
    std::unordered_set<std::string> seen;

    auto keyOf = [&](Value & it) -> std::string {
        it = forceValue(*state.vm, it);
        if (!it.isAttrs() || !it.payload.bindings)
            throw std::runtime_error("v3 primop genericClosure: items must be attrsets with a 'key' attr");
        const Value * kRaw = it.payload.bindings->lookup(sKey);
        if (!kRaw) throw std::runtime_error("v3 primop genericClosure: item missing 'key' attr");
        Value k = forceValue(*state.vm, *kRaw);
        if (k.isString()) return std::string(k.payload.str);
        if (k.isInt())    return std::to_string(k.payload.i);
        throw std::runtime_error("v3 primop genericClosure: 'key' must be string or int");
    };

    while (!work.empty()) {
        Value it = work.back(); work.pop_back();
        std::string key = keyOf(it);
        if (!seen.insert(key).second) continue;
        result.push_back(it);
        Value next = callClosure(*state.vm, opV, it);
        if (!next.isList())
            throw std::runtime_error("v3 primop genericClosure: operator must return a list");
        if (next.payload.list) {
            for (uint32_t i = 0; i < next.payload.list->size; ++i)
                work.push_back(next.payload.list->elems[i]);
        }
    }

    ListVec * lv = Alloc::allocList(static_cast<uint32_t>(result.size()));
    allocStats().listsAllocated++;
    for (size_t i = 0; i < result.size(); ++i) lv->elems[i] = result[i];
    out.tag_payload = static_cast<uint64_t>(Tag::List);
    out.payload.list = lv;
}

/// builtins.match regex string -> list of captures or null on no-match.
/// Supports the standard regex syntax via std::regex (POSIX-ish).
void primMatch(EvalState &, Value * args, Value & out)
{
    if (!args[0].isString() || !args[1].isString())
        typeError("match", "(regex, string)");
    try {
        std::regex re(args[0].payload.str);
        std::cmatch m;
        if (!std::regex_match(args[1].payload.str, m, re)) {
            out = Value::vNull;
            return;
        }
        // m[0] is the entire match; captures are m[1..m.size()-1].
        size_t nGroups = m.size() > 0 ? m.size() - 1 : 0;
        ListVec * lv = Alloc::allocList(static_cast<uint32_t>(nGroups));
        allocStats().listsAllocated++;
        for (size_t i = 0; i < nGroups; ++i) {
            if (m[i + 1].matched)
                lv->elems[i] = mkStringValueOwned(m[i + 1].str());
            else
                lv->elems[i] = Value::vNull;
        }
        out.tag_payload = static_cast<uint64_t>(Tag::List);
        out.payload.list = lv;
    } catch (const std::regex_error & e) {
        throw std::runtime_error(std::string("v3 primop match: invalid regex: ") + e.what());
    }
}

/// builtins.split regex string -> list alternating strings and captures.
/// E.g. split "[ ]+" "hello  world" -> ["hello" [] "world"].
void primSplit(EvalState &, Value * args, Value & out)
{
    if (!args[0].isString() || !args[1].isString())
        typeError("split", "(regex, string)");
    try {
        std::regex re(args[0].payload.str);
        std::string_view s(args[1].payload.str);
        std::vector<Value> parts;
        std::cregex_iterator it(s.data(), s.data() + s.size(), re);
        std::cregex_iterator end;
        size_t pos = 0;
        for (; it != end; ++it) {
            auto match = *it;
            // Add the literal piece between previous and this match.
            parts.push_back(mkStringValueOwned(std::string(s.substr(pos, match.position(0) - pos))));
            // Add the captured groups as a list.
            size_t nGroups = match.size() > 0 ? match.size() - 1 : 0;
            ListVec * caps = Alloc::allocList(static_cast<uint32_t>(nGroups));
            allocStats().listsAllocated++;
            for (size_t i = 0; i < nGroups; ++i) {
                if (match[i + 1].matched)
                    caps->elems[i] = mkStringValueOwned(match[i + 1].str());
                else
                    caps->elems[i] = Value::vNull;
            }
            Value capsV;
            capsV.tag_payload = static_cast<uint64_t>(Tag::List);
            capsV.payload.list = caps;
            parts.push_back(capsV);
            pos = match.position(0) + match.length(0);
        }
        // Trailing piece.
        parts.push_back(mkStringValueOwned(std::string(s.substr(pos))));

        ListVec * lv = Alloc::allocList(static_cast<uint32_t>(parts.size()));
        allocStats().listsAllocated++;
        for (size_t i = 0; i < parts.size(); ++i) lv->elems[i] = parts[i];
        out.tag_payload = static_cast<uint64_t>(Tag::List);
        out.payload.list = lv;
    } catch (const std::regex_error & e) {
        throw std::runtime_error(std::string("v3 primop split: invalid regex: ") + e.what());
    }
}

/// Map nix algo string -> HashAlgorithm enum.
inline nix::HashAlgorithm parseHashAlgo(std::string_view a)
{
    if (a == "md5")    return nix::HashAlgorithm::MD5;
    if (a == "sha1")   return nix::HashAlgorithm::SHA1;
    if (a == "sha256") return nix::HashAlgorithm::SHA256;
    if (a == "sha512") return nix::HashAlgorithm::SHA512;
    if (a == "blake3") return nix::HashAlgorithm::BLAKE3;
    throw std::runtime_error("v3: unknown hash algorithm '" + std::string(a) + "'");
}

/// builtins.hashString algo s -> hex string of the digest.  Backed by
/// nix::hashString (libutil) which uses libcrypto.
void primHashString(EvalState &, Value * args, Value & out)
{
    if (!args[0].isString() || !args[1].isString())
        typeError("hashString", "(algo, string)");
    auto algo = parseHashAlgo(args[0].payload.str);
    auto h = nix::hashString(algo, args[1].payload.str);
    out = mkStringValueOwned(h.to_string(nix::HashFormat::Base16, false));
}

void primHashFile(EvalState &, Value * args, Value & out)
{
    if (!args[0].isString())
        typeError("hashFile", "(algo, path)");
    std::string path;
    if (args[1].isString())     path = args[1].payload.str;
    else if (args[1].isPath())  path = args[1].payload.path;
    else typeError("hashFile", "(algo, path)");
    auto algo = parseHashAlgo(args[0].payload.str);
    auto h = nix::hashFile(algo, path);
    out = mkStringValueOwned(h.to_string(nix::HashFormat::Base16, false));
}

void primConvertHash(EvalState & state, Value * args, Value & out)
{
    // builtins.convertHash { hash; hashAlgo?; toHashFormat; } -> string
    // hashAlgo is optional when hash is in `algo:body` or SRI form.
    if (!args[0].isAttrs() || !args[0].payload.bindings)
        typeError("convertHash", "attrset");
    SymbolId sHash = vmIntern(state, "hash");
    SymbolId sAlgo = vmIntern(state, "hashAlgo");
    SymbolId sFmt  = vmIntern(state, "toHashFormat");
    const Value * vhRaw = args[0].payload.bindings->lookup(sHash);
    const Value * vaRaw = args[0].payload.bindings->lookup(sAlgo);
    const Value * vfRaw = args[0].payload.bindings->lookup(sFmt);
    if (!vhRaw || !vfRaw)
        typeError("convertHash", "{ hash; hashAlgo?; toHashFormat; }");
    Value vh = forceValue(*state.vm, *vhRaw);
    Value vf = forceValue(*state.vm, *vfRaw);
    if (!vh.isString() || !vf.isString())
        typeError("convertHash", "{ hash; hashAlgo?; toHashFormat; }");

    nix::HashFormat fmt;
    std::string_view fs(vf.payload.str);
    if (fs == "base16")        fmt = nix::HashFormat::Base16;
    else if (fs == "nix32")    fmt = nix::HashFormat::Nix32;
    else if (fs == "base32")   fmt = nix::HashFormat::Nix32;  // alias
    else if (fs == "base64")   fmt = nix::HashFormat::Base64;
    else if (fs == "sri")      fmt = nix::HashFormat::SRI;
    else throw std::runtime_error("v3 convertHash: unknown format '" + std::string(fs) + "'");

    nix::Hash parsed{nix::HashAlgorithm::SHA256}; // dummy default
    if (vaRaw) {
        Value va = forceValue(*state.vm, *vaRaw);
        if (!va.isString())
            typeError("convertHash", "{ hash; hashAlgo?; toHashFormat; }");
        parsed = nix::Hash::parseAny(vh.payload.str, parseHashAlgo(va.payload.str));
    } else {
        // No hashAlgo — infer from `algo:body` or SRI prefix.
        parsed = nix::Hash::parseAny(vh.payload.str, std::nullopt);
    }
    out = mkStringValueOwned(parsed.to_string(fmt, false));
}

/// builtins.currentSystem and similar: just return host triple.
void primCurrentSystem(EvalState &, Value *, Value & out)
{
    // Use a sensible default; nix tests usually mock this.
#if defined(__APPLE__) && defined(__aarch64__)
    out = mkStringValueOwned("aarch64-darwin");
#elif defined(__APPLE__) && defined(__x86_64__)
    out = mkStringValueOwned("x86_64-darwin");
#elif defined(__linux__) && defined(__aarch64__)
    out = mkStringValueOwned("aarch64-linux");
#elif defined(__linux__) && defined(__x86_64__)
    out = mkStringValueOwned("x86_64-linux");
#else
    out = mkStringValueOwned("unknown-unknown");
#endif
}

void primCurrentTime(EvalState &, Value *, Value & out)
{
    out.mkInt(static_cast<int64_t>(std::time(nullptr)));
}

void primNixVersion(EvalState &, Value *, Value & out)
{
    out = mkStringValueOwned("v3-0.1");
}

/// builtins.readFile path -> string contents.
void primReadFile(EvalState &, Value * args, Value & out)
{
    std::string path;
    if (args[0].isString()) path = args[0].payload.str;
    else if (args[0].isPath()) path = args[0].payload.path;
    else typeError("readFile", "string or path");
    std::ifstream f(path);
    if (!f) throw std::runtime_error("v3 primop readFile: cannot open " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    out = mkStringValueOwned(ss.str());
}

/// builtins.readDir path -> attrset of name -> "regular"|"directory"|"symlink"|"unknown".
void primReadDir(EvalState & state, Value * args, Value & out)
{
    std::string path;
    if (args[0].isString()) path = args[0].payload.str;
    else if (args[0].isPath()) path = args[0].payload.path;
    else typeError("readDir", "string or path");
    std::vector<std::pair<SymbolId, Value>> entries;
    for (auto & ent : std::filesystem::directory_iterator(path)) {
        std::string name = ent.path().filename().string();
        const char * type =
            ent.is_directory()  ? "directory" :
            ent.is_symlink()    ? "symlink" :
            ent.is_regular_file() ? "regular" :
                                  "unknown";
        SymbolId k = vmIntern(state, name);
        entries.emplace_back(k, mkStringValueOwned(type));
    }
    std::sort(entries.begin(), entries.end(),
        [](auto & a, auto & b) { return a.first < b.first; });
    Bindings * b = Alloc::allocBindings(static_cast<uint32_t>(entries.size()));
    allocStats().attrsetsAllocated++;
    for (size_t i = 0; i < entries.size(); ++i) {
        b->entries[i].name  = entries[i].first;
        b->entries[i].value = entries[i].second;
    }
    out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
    out.payload.bindings = b;
}

/// builtins.parseDrvName "name-1.2.3" -> { name = "name"; version = "1.2.3"; }
void primParseDrvName(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("parseDrvName", "string");
    std::string s(args[0].payload.str);
    // Split at the first '-' followed by a digit.
    size_t cut = std::string::npos;
    for (size_t i = 0; i + 1 < s.size(); ++i) {
        if (s[i] == '-' && std::isdigit(static_cast<unsigned char>(s[i + 1]))) {
            cut = i; break;
        }
    }
    std::string name, version;
    if (cut == std::string::npos) { name = s; version = ""; }
    else { name = s.substr(0, cut); version = s.substr(cut + 1); }

    SymbolId sName    = vmIntern(state, "name");
    SymbolId sVersion = vmIntern(state, "version");
    Bindings * b = Alloc::allocBindings(2);
    allocStats().attrsetsAllocated++;
    Value vn = mkStringValueOwned(name);
    Value vv = mkStringValueOwned(version);
    if (sName < sVersion) { b->entries[0] = {sName, vn}; b->entries[1] = {sVersion, vv}; }
    else                  { b->entries[0] = {sVersion, vv}; b->entries[1] = {sName, vn}; }
    out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
    out.payload.bindings = b;
}

/// builtins.groupBy keyFn list -> { key = [items with that key]; }
void primGroupBy(EvalState & state, Value * args, Value & out)
{
    if (!args[1].isList()) typeError("groupBy", "list");
    auto * src = args[1].payload.list;
    Value keyFn = args[0];
    std::unordered_map<std::string, std::vector<Value>> groups;
    if (src) {
        for (uint32_t i = 0; i < src->size; ++i) {
            Value k = callClosure(*state.vm, keyFn, src->elems[i]);
            if (!k.isString()) typeError("groupBy", "key fn returning string");
            groups[std::string(k.payload.str)].push_back(src->elems[i]);
        }
    }
    std::vector<std::pair<SymbolId, Value>> entries;
    entries.reserve(groups.size());
    for (auto & [name, items] : groups) {
        ListVec * lv = Alloc::allocList(static_cast<uint32_t>(items.size()));
        allocStats().listsAllocated++;
        for (size_t i = 0; i < items.size(); ++i) lv->elems[i] = items[i];
        Value lstV;
        lstV.tag_payload = static_cast<uint64_t>(Tag::List);
        lstV.payload.list = lv;
        entries.emplace_back(vmIntern(state, name), lstV);
    }
    std::sort(entries.begin(), entries.end(),
        [](auto & a, auto & b) { return a.first < b.first; });
    Bindings * b = Alloc::allocBindings(static_cast<uint32_t>(entries.size()));
    allocStats().attrsetsAllocated++;
    for (size_t i = 0; i < entries.size(); ++i) {
        b->entries[i].name  = entries[i].first;
        b->entries[i].value = entries[i].second;
    }
    out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
    out.payload.bindings = b;
}

void primReadFileType(EvalState &, Value * args, Value & out)
{
    std::string path;
    if (args[0].isString()) path = args[0].payload.str;
    else if (args[0].isPath()) path = args[0].payload.path;
    else typeError("readFileType", "string or path");
    std::error_code ec;
    auto status = std::filesystem::symlink_status(path, ec);
    if (ec) throw std::runtime_error("v3 readFileType: " + ec.message());
    const char * t;
    if      (std::filesystem::is_symlink(status))   t = "symlink";
    else if (std::filesystem::is_directory(status)) t = "directory";
    else if (std::filesystem::is_regular_file(status)) t = "regular";
    else                                            t = "unknown";
    out = mkStringValueOwned(t);
}

void primAddErrorContext(EvalState &, Value * args, Value & out)
{
    // Stub: nix's tree-walker pre-pends a context message to error
    // messages from the second arg.  v3 doesn't track context yet —
    // just return the second argument (the wrapped value).  The first
    // arg (the prefix string) is currently ignored.
    (void)args;
    out = args[1];
}

/// Construct a v3-side derivation result that mirrors what tree-walker
/// produces from corepkgs/derivation.nix.  The outer wrapper:
///   1. calls derivationStrict to synthesize the per-output paths
///   2. picks `outputs[0]` (default "out")
///   3. returns that output's attrset, populated with `commonAttrs //
///      { outPath; drvPath; type = "derivation"; outputName; }`.
/// `commonAttrs` = drvAttrs // listToAttrs(outputs) // { all; drvAttrs; }.
void primDerivation(EvalState & state, Value * args, Value & out);

/// Construct a "fake" derivation attrset.  Real `derivation` interfaces
/// with the store; we accept the input attrset and tag it with a
/// synthetic `outPath` so code that just reads outPath works.  Useful
/// for testing nix expressions that build up drv attrsets without
/// actually realizing them.
void primDerivationStrict(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isAttrs() || !args[0].payload.bindings)
        typeError("derivationStrict", "attrset");
    SymbolId sName    = vmIntern(state, "name");
    SymbolId sOutputs = vmIntern(state, "outputs");
    SymbolId sDrvPath = vmIntern(state, "drvPath");

    auto * src = args[0].payload.bindings;
    const Value * nameVRaw = src->lookup(sName);
    if (!nameVRaw)
        typeError("derivationStrict", "attrset with `name` string");
    Value nameV = forceValue(*state.vm, *nameVRaw);
    if (!nameV.isString())
        typeError("derivationStrict", "attrset with `name` string");
    std::string name(nameV.payload.str);

    // Read outputs (default ["out"]).  derivationStrict's result is
    // an attrset { drvPath; <output1>; <output2>; ... } with one
    // path per declared output.
    std::vector<std::string> outputs;
    if (auto * outV = src->lookup(sOutputs)) {
        Value f = forceValue(*state.vm, *outV);
        if (f.isList() && f.payload.list) {
            for (uint32_t i = 0; i < f.payload.list->size; ++i) {
                Value el = forceValue(*state.vm, f.payload.list->elems[i]);
                if (el.isString()) outputs.push_back(el.payload.str);
            }
        }
    }
    if (outputs.empty()) outputs.push_back("out");

    // Synthesize fake store paths.  Real nix interacts with the store;
    // v3 just produces stable identifiers good enough for tests that
    // string-interpolate / string-compare drvPath / outPath values.
    std::vector<std::pair<SymbolId, Value>> entries;
    entries.reserve(outputs.size() + 1);
    entries.emplace_back(sDrvPath, mkStringValueOwned("/v3-fake-store/" + name + ".drv"));
    for (auto & o : outputs) {
        SymbolId sO = vmIntern(state, o);
        std::string p = "/v3-fake-store/" + name + (o == "out" ? "" : "-" + o);
        entries.emplace_back(sO, mkStringValueOwned(p));
    }
    std::sort(entries.begin(), entries.end(),
        [](auto & a, auto & b) { return a.first < b.first; });
    Bindings * b = Alloc::allocBindings(static_cast<uint32_t>(entries.size()));
    allocStats().attrsetsAllocated++;
    for (size_t i = 0; i < entries.size(); ++i) {
        b->entries[i].name  = entries[i].first;
        b->entries[i].value = entries[i].second;
    }
    out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
    out.payload.bindings = b;
}

void primDerivation(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isAttrs() || !args[0].payload.bindings)
        typeError("derivation", "attrset");
    auto * src = args[0].payload.bindings;

    // 1. Run derivationStrict to get per-output paths + drvPath.
    Value strict;
    primDerivationStrict(state, args, strict);
    if (!strict.isAttrs() || !strict.payload.bindings)
        throw std::runtime_error("v3 derivation: derivationStrict didn't return an attrset");
    auto * strictB = strict.payload.bindings;

    // 2. Read `outputs` (default ["out"]).
    std::vector<std::string> outputs;
    SymbolId sOutputs = vmIntern(state, "outputs");
    if (auto * outV = src->lookup(sOutputs)) {
        Value f = forceValue(*state.vm, *outV);
        if (f.isList() && f.payload.list) {
            for (uint32_t i = 0; i < f.payload.list->size; ++i) {
                Value el = forceValue(*state.vm, f.payload.list->elems[i]);
                if (el.isString()) outputs.push_back(el.payload.str);
            }
        }
    }
    if (outputs.empty()) outputs.push_back("out");

    SymbolId sOutPath  = vmIntern(state, "outPath");
    SymbolId sDrvPath  = vmIntern(state, "drvPath");
    SymbolId sType     = vmIntern(state, "type");
    SymbolId sOutName  = vmIntern(state, "outputName");
    SymbolId sAll      = vmIntern(state, "all");
    SymbolId sDrvAttrs = vmIntern(state, "drvAttrs");
    const Value * drvPathV = strictB->lookup(sDrvPath);

    // Build commonAttrs = drvAttrs // listToAttrs outputs-list // { all; drvAttrs; }.
    // For v3 we just produce the first output's value (which is what
    // the wrapper's `(builtins.head outputsList).value` returns); the
    // `all` field is omitted as it isn't structurally required by the
    // tests and would re-introduce the same cycle.
    const std::string & firstOut = outputs[0];
    SymbolId sFirstOut = vmIntern(state, firstOut);
    const Value * outPath = strictB->lookup(sFirstOut);

    // Result: drvAttrs // { outPath; drvPath; type = "derivation"; outputName; drvAttrs = drvAttrs; }
    std::vector<std::pair<SymbolId, Value>> entries;
    entries.reserve(src->size + 5);
    for (uint32_t i = 0; i < src->size; ++i)
        entries.emplace_back(src->entries[i].name, src->entries[i].value);
    if (outPath)  entries.emplace_back(sOutPath, *outPath);
    if (drvPathV) entries.emplace_back(sDrvPath, *drvPathV);
    entries.emplace_back(sType,    mkStringValueOwned("derivation"));
    entries.emplace_back(sOutName, mkStringValueOwned(firstOut));
    entries.emplace_back(sDrvAttrs, args[0]);
    std::sort(entries.begin(), entries.end(),
        [](auto & a, auto & b) { return a.first < b.first; });
    std::vector<std::pair<SymbolId, Value>> dedup;
    dedup.reserve(entries.size());
    for (auto & p : entries) {
        if (!dedup.empty() && dedup.back().first == p.first) dedup.back() = p;
        else dedup.push_back(p);
    }
    Bindings * b = Alloc::allocBindings(static_cast<uint32_t>(dedup.size()));
    allocStats().attrsetsAllocated++;
    for (size_t i = 0; i < dedup.size(); ++i) {
        b->entries[i].name  = dedup[i].first;
        b->entries[i].value = dedup[i].second;
    }
    out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
    out.payload.bindings = b;
}

/// Cache of compiled-and-evaluated imported files.  Closures returned
/// by `import` reference their CompilationUnit's bytecode and constant
/// pools by raw pointer — those must outlive the closure, so we keep
/// the CUs (and the eval result) here for the lifetime of the process.
/// Keyed by absolute path so repeated imports are idempotent.
struct ImportCache
{
    std::deque<CompilationUnit> cus;       // stable addresses (deque doesn't reallocate)
    std::unordered_map<std::string, Value> results;
};
inline ImportCache & importCache()
{
    static ImportCache c;
    return c;
}

/// builtins.import path -- read the file at `path`, parse, lower, run.
/// Returns the resulting v3 Value.  Requires state.nixEvalState to be
/// set (the host EvalState providing parser + symbol table).
void primImport(EvalState & state, Value * args, Value & out)
{
    if (!state.nixEvalState)
        throw std::runtime_error("v3 primop import: no nix EvalState wired (run via v3-eval)");
    std::string path;
    if (args[0].isString()) path = args[0].payload.str;
    else if (args[0].isPath()) path = args[0].payload.path;
    else typeError("import", "string or path");

    auto & cache = importCache();
    if (auto it = cache.results.find(path); it != cache.results.end()) {
        out = it->second;
        return;
    }

    auto & ns = *state.nixEvalState;
    nix::Expr * e = ns.parseExprFromFile(nix::SourcePath(ns.rootFS, nix::CanonPath(path)));
    e->bindVars(ns, ns.staticBaseEnv);

    auto module = lowerNixExpr(e, ns.symbols, ns.positions);
    nix::v3::ir::computeFreeVars(module);
    cache.cus.push_back(compile(module));
    // Each imported file is its own CompilationUnit; we re-enter the
    // VM to run it with its own top-level frame.  Keep the CU alive
    // (it's borrowed by closures returned from the eval).
    out = run(cache.cus.back());
    cache.results.emplace(path, out);
}

/// XML escape: `<>&"` and unprintable chars become entities.
static std::string xmlEscape(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '&': out += "&amp;"; break;
        case '"': out += "&quot;"; break;
        default:  out.push_back(c); break;
        }
    }
    return out;
}

/// Recursive XML serializer mirroring tree-walker's printValueAsXML
/// (no source-location tracking — v3 doesn't carry that yet anyway).
static void valueToXml(EvalState & state, std::string & out, Value v, int indent)
{
    auto pad = [&](int n) { for (int i = 0; i < n; ++i) out += "  "; };
    v = forceValue(*state.vm, v);
    pad(indent);
    switch (v.tag()) {
    case Tag::String:
        out += "<string value=\""; out += xmlEscape(v.payload.str); out += "\" />\n";
        return;
    case Tag::Int:
        out += "<int value=\""; out += std::to_string(v.payload.i); out += "\" />\n";
        return;
    case Tag::Float:
        out += "<float value=\""; out += std::to_string(v.payload.f); out += "\" />\n";
        return;
    case Tag::Bool:
        out += "<bool value=\""; out += v.payload.i == 1 ? "true" : "false"; out += "\" />\n";
        return;
    case Tag::Null:
        out += "<null />\n";
        return;
    case Tag::Path:
        out += "<path value=\""; out += xmlEscape(v.payload.path); out += "\" />\n";
        return;
    case Tag::List:
        out += "<list>\n";
        if (v.payload.list)
            for (uint32_t i = 0; i < v.payload.list->size; ++i)
                valueToXml(state, out, v.payload.list->elems[i], indent + 1);
        pad(indent);
        out += "</list>\n";
        return;
    case Tag::Attrs: {
        out += "<attrs>\n";
        if (v.payload.bindings) {
            // Sort by name for stable output.
            auto & symTab = ir::globalSymbolTable();
            std::vector<std::pair<std::string_view, Value>> entries;
            entries.reserve(v.payload.bindings->size);
            for (uint32_t i = 0; i < v.payload.bindings->size; ++i) {
                SymbolId sid = v.payload.bindings->entries[i].name;
                std::string_view nm = sid < symTab.size() ? std::string_view(symTab[sid]) : std::string_view("");
                entries.emplace_back(nm, v.payload.bindings->entries[i].value);
            }
            std::sort(entries.begin(), entries.end(),
                [](auto & a, auto & b) { return a.first < b.first; });
            for (auto & [nm, val] : entries) {
                pad(indent + 1);
                out += "<attr name=\""; out += xmlEscape(nm); out += "\">\n";
                valueToXml(state, out, val, indent + 2);
                pad(indent + 1);
                out += "</attr>\n";
            }
        }
        pad(indent);
        out += "</attrs>\n";
        return;
    }
    case Tag::Closure:
    case Tag::PrimOp:
    case Tag::PrimOpApp:
        out += "<function />\n";
        return;
    case Tag::Uninitialized:
    case Tag::Thunk:
    case Tag::App:
    case Tag::Blackhole:
    case Tag::External:
    default:
        out += "<unevaluated />\n";
        return;
    }
}

void primToXML(EvalState & state, Value * args, Value & out)
{
    std::string s = "<?xml version='1.0' encoding='utf-8'?>\n<expr>\n";
    valueToXml(state, s, args[0], 1);
    s += "</expr>\n";
    out = mkStringValueOwned(s);
}

/// builtins.parseFlakeRef "github:NixOS/nixpkgs/23.05?dir=lib"
/// → { type = "github"; owner = "NixOS"; repo = "nixpkgs"; ref = "23.05"; dir = "lib"; }
/// Minimal hand-rolled parser covering the github / git / path /
/// url-with-query forms exercised by the lang tests.  The full
/// flake-ref grammar is much richer; this stub plus tree-walker's
/// fetchers would be the cutover point for end-to-end flakes.
static void splitOnce(std::string_view s, char sep, std::string_view & lhs, std::string_view & rhs)
{
    auto p = s.find(sep);
    if (p == std::string_view::npos) { lhs = s; rhs = {}; return; }
    lhs = s.substr(0, p);
    rhs = s.substr(p + 1);
}

void primParseFlakeRef(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("parseFlakeRef", "string");
    std::string_view s(args[0].payload.str);
    // Split off any `?key=value&...` query string.
    std::string_view base = s, query;
    splitOnce(s, '?', base, query);
    std::vector<std::pair<std::string, std::string>> attrs;

    // type:rest
    std::string_view typ, rest;
    splitOnce(base, ':', typ, rest);
    if (rest.empty()) {
        // Bare path like "/foo/bar".
        attrs.emplace_back("type", "path");
        attrs.emplace_back("path", std::string(base));
    } else if (typ == "github" || typ == "gitlab" || typ == "sourcehut") {
        attrs.emplace_back("type", std::string(typ));
        // owner/repo[/ref]
        std::string_view owner, after;
        splitOnce(rest, '/', owner, after);
        std::string_view repo, ref;
        splitOnce(after, '/', repo, ref);
        attrs.emplace_back("owner", std::string(owner));
        attrs.emplace_back("repo",  std::string(repo));
        if (!ref.empty())
            attrs.emplace_back("ref", std::string(ref));
    } else if (typ == "git" || typ == "hg" || typ == "tarball" || typ == "file") {
        attrs.emplace_back("type", std::string(typ));
        attrs.emplace_back("url",  std::string(rest));
    } else if (typ == "path") {
        attrs.emplace_back("type", "path");
        attrs.emplace_back("path", std::string(rest));
    } else {
        // Fallback: type with raw url body.
        attrs.emplace_back("type", std::string(typ));
        attrs.emplace_back("url",  std::string(rest));
    }
    // Apply query-string overrides (key=value, &-separated).
    while (!query.empty()) {
        std::string_view part, qrest;
        splitOnce(query, '&', part, qrest);
        std::string_view k, v;
        splitOnce(part, '=', k, v);
        attrs.emplace_back(std::string(k), std::string(v));
        query = qrest;
    }
    // Build sorted Bindings.
    std::vector<std::pair<SymbolId, Value>> entries;
    entries.reserve(attrs.size());
    for (auto & p : attrs)
        entries.emplace_back(vmIntern(state, p.first), mkStringValueOwned(p.second));
    std::sort(entries.begin(), entries.end(),
        [](auto & a, auto & b) { return a.first < b.first; });
    Bindings * b = Alloc::allocBindings(static_cast<uint32_t>(entries.size()));
    allocStats().attrsetsAllocated++;
    for (size_t i = 0; i < entries.size(); ++i) b->entries[i] = {entries[i].first, entries[i].second};
    out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
    out.payload.bindings = b;
}

void primFlakeRefToString(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isAttrs() || !args[0].payload.bindings)
        typeError("flakeRefToString", "attrset");
    auto * src = args[0].payload.bindings;
    auto getStr = [&](const char * name) -> std::string {
        SymbolId id = vmIntern(state, name);
        auto * v = src->lookup(id);
        if (!v) return {};
        Value f = forceValue(*state.vm, *v);
        if (f.isString()) return std::string(f.payload.str);
        return {};
    };
    std::string typ = getStr("type");
    std::string out_s;
    if (typ == "github" || typ == "gitlab" || typ == "sourcehut") {
        out_s = typ + ":" + getStr("owner") + "/" + getStr("repo");
        std::string ref = getStr("ref");
        if (!ref.empty()) out_s += "/" + ref;
    } else if (typ == "path") {
        out_s = "path:" + getStr("path");
    } else {
        // Generic url-bearing type.
        out_s = typ + ":" + getStr("url");
    }
    // Query-string for known extra attrs.
    std::string q;
    for (const char * key : {"dir", "rev", "ref", "narHash"}) {
        if (std::string(key) == "ref") continue;  // already in path for github-style
        std::string val = getStr(key);
        if (val.empty()) continue;
        if (!q.empty()) q += "&";
        q += std::string(key) + "=" + val;
    }
    if (!q.empty()) out_s += "?" + q;
    out = mkStringValueOwned(out_s);
}

/// Convert a toml::value (toml11) into a v3 Value recursively.
static Value tomlToValue(EvalState & state, const toml::value & t)
{
    Value v;
    switch (t.type()) {
    case toml::value_t::table: {
        auto & tab = t.as_table();
        std::vector<std::pair<SymbolId, Value>> entries;
        entries.reserve(tab.size());
        for (auto & elem : tab)
            entries.emplace_back(vmIntern(state, elem.first), tomlToValue(state, elem.second));
        std::sort(entries.begin(), entries.end(),
            [](auto & a, auto & b) { return a.first < b.first; });
        Bindings * b = Alloc::allocBindings(static_cast<uint32_t>(entries.size()));
        allocStats().attrsetsAllocated++;
        for (size_t i = 0; i < entries.size(); ++i) {
            b->entries[i].name  = entries[i].first;
            b->entries[i].value = entries[i].second;
        }
        v.tag_payload = static_cast<uint64_t>(Tag::Attrs);
        v.payload.bindings = b;
        return v;
    }
    case toml::value_t::array: {
        auto & arr = t.as_array();
        ListVec * lv = Alloc::allocList(static_cast<uint32_t>(arr.size()));
        allocStats().listsAllocated++;
        for (size_t i = 0; i < arr.size(); ++i)
            lv->elems[i] = tomlToValue(state, arr[i]);
        v.tag_payload = static_cast<uint64_t>(Tag::List);
        v.payload.list = lv;
        return v;
    }
    case toml::value_t::boolean: v = t.as_boolean() ? Value::vTrue : Value::vFalse; return v;
    case toml::value_t::integer:  v.mkInt(t.as_integer()); return v;
    case toml::value_t::floating: v.mkFloat(t.as_floating()); return v;
    case toml::value_t::string:   v = mkStringValueOwned(t.as_string()); return v;
    case toml::value_t::local_datetime:
    case toml::value_t::offset_datetime:
    case toml::value_t::local_date:
    case toml::value_t::local_time: {
        // Render the datetime via toml11's stream operator and tag it.
        std::ostringstream s;
        s << t;
        std::string str = s.str();
        SymbolId sType = vmIntern(state, "_type");
        SymbolId sVal  = vmIntern(state, "value");
        Bindings * b = Alloc::allocBindings(2);
        allocStats().attrsetsAllocated++;
        Value typeV = mkStringValueOwned("timestamp");
        Value valV  = mkStringValueOwned(str);
        if (sType < sVal) { b->entries[0]={sType,typeV}; b->entries[1]={sVal,valV}; }
        else              { b->entries[0]={sVal,valV};  b->entries[1]={sType,typeV}; }
        v.tag_payload = static_cast<uint64_t>(Tag::Attrs);
        v.payload.bindings = b;
        return v;
    }
    case toml::value_t::empty: v.mkNull(); return v;
    }
    v.mkNull();
    return v;
}

/// builtins.fromTOML s -- parse a TOML document into a v3 attrset.
void primFromTOML(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("fromTOML", "string");
    std::istringstream stream{std::string(args[0].payload.str)};
    try {
        out = tomlToValue(state, toml::parse(stream, "fromTOML"));
    } catch (std::exception & e) {
        throw std::runtime_error(std::string("v3 primop fromTOML: ") + e.what());
    }
}

/// builtins.path { path; name?; filter?; recursive?; sha256?; }:
/// add a path to the (fake) v3 store.  Real Nix would copy the path
/// (with `filter` applied) and verify against `sha256`; v3 returns
/// a synthetic store path string built from `name` (defaulting to
/// the basename).  Filter/recursive/sha256 are accepted but unused.
void primPath(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isAttrs() || !args[0].payload.bindings)
        typeError("path", "attrset");
    SymbolId sPath = vmIntern(state, "path");
    SymbolId sName = vmIntern(state, "name");
    auto * src = args[0].payload.bindings;
    const Value * pathV = src->lookup(sPath);
    if (!pathV)
        typeError("path", "attrset with `path`");
    Value forcedPath = forceValue(*state.vm, *pathV);
    std::string p;
    if (forcedPath.isString())     p = forcedPath.payload.str;
    else if (forcedPath.isPath())  p = forcedPath.payload.path;
    else typeError("path", "{ path = string-or-path; ... }");
    std::string name;
    if (auto * nameV = src->lookup(sName)) {
        Value f = forceValue(*state.vm, *nameV);
        if (f.isString()) name = f.payload.str;
    }
    if (name.empty()) {
        auto pos = p.find_last_of('/');
        name = pos == std::string::npos ? p : p.substr(pos + 1);
        if (name.empty()) name = "source";
    }
    // Synthesize a path like the real store would.  Tests that just
    // read the result string don't usually care about the exact form.
    std::string outPath = "/v3-fake-store/" + name;
    char * buf = static_cast<char *>(std::malloc(outPath.size() + 1));
    std::strcpy(buf, outPath.c_str());
    out.tag_payload = static_cast<uint64_t>(Tag::Path);
    out.payload.path = buf;
}

/// builtins.scopedImport scope path -- like import, but extends the
/// base env with `scope`'s entries while evaluating the file.  v3
/// doesn't have the same kind of overridable base env tree-walker
/// uses, so for simple cases we just route through primImport (the
/// scope is ignored for now).  This is enough for tests that use
/// scopedImport for library wrappers without overriding builtins.
void primScopedImport(EvalState & state, Value * args, Value & out)
{
    // args[0] is the scope (an attrset), args[1] is the path.
    // For now we ignore scope and route through import.
    Value importArg = args[1];
    primImport(state, &importArg, out);
}

/// builtins.functionArgs lam → { name = false; ... } where the bool
/// indicates whether the formal has a default value.  For simple
/// lambdas (no formals) returns an empty attrset.
void primFunctionArgs(EvalState &, Value * args, Value & out)
{
    Value v = args[0];
    if (v.tag() == Tag::Closure && v.payload.closure && v.payload.closure->desc) {
        const LambdaDescriptor * desc = v.payload.closure->desc;
        if (!desc->hasFormals) {
            Bindings * b = Alloc::allocBindings(0);
            allocStats().attrsetsAllocated++;
            out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
            out.payload.bindings = b;
            return;
        }
        // Build sorted entries.
        std::vector<std::pair<SymbolId, Value>> entries;
        entries.reserve(desc->formals.size());
        for (auto & f : desc->formals) {
            Value bv = f.second ? Value::vTrue : Value::vFalse;
            entries.emplace_back(f.first, bv);
        }
        std::sort(entries.begin(), entries.end(),
            [](auto & a, auto & b) { return a.first < b.first; });
        Bindings * b = Alloc::allocBindings(static_cast<uint32_t>(entries.size()));
        allocStats().attrsetsAllocated++;
        for (size_t i = 0; i < entries.size(); ++i) {
            b->entries[i].name  = entries[i].first;
            b->entries[i].value = entries[i].second;
        }
        out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
        out.payload.bindings = b;
        return;
    }
    if (v.tag() == Tag::PrimOp || v.tag() == Tag::PrimOpApp) {
        // PrimOps don't have introspectable formals; return empty.
        Bindings * b = Alloc::allocBindings(0);
        allocStats().attrsetsAllocated++;
        out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
        out.payload.bindings = b;
        return;
    }
    typeError("functionArgs", "lambda");
}

/// nlohmann::json -> v3 Value (recursive).
Value jsonToValue(EvalState & state, const nlohmann::json & j)
{
    Value out;
    if (j.is_null())     { out.mkNull(); return out; }
    if (j.is_boolean())  { out = j.get<bool>() ? Value::vTrue : Value::vFalse; return out; }
    if (j.is_number_integer()) {
        out.mkInt(j.get<int64_t>()); return out;
    }
    if (j.is_number_float()) {
        out.mkFloat(j.get<double>()); return out;
    }
    if (j.is_string()) {
        out = mkStringValueOwned(j.get<std::string>()); return out;
    }
    if (j.is_array()) {
        ListVec * lv = Alloc::allocList(static_cast<uint32_t>(j.size()));
        allocStats().listsAllocated++;
        for (size_t i = 0; i < j.size(); ++i) lv->elems[i] = jsonToValue(state, j[i]);
        out.tag_payload = static_cast<uint64_t>(Tag::List);
        out.payload.list = lv;
        return out;
    }
    if (j.is_object()) {
        std::vector<std::pair<SymbolId, Value>> entries;
        entries.reserve(j.size());
        for (auto it = j.begin(); it != j.end(); ++it) {
            SymbolId k = vmIntern(state, it.key());
            entries.emplace_back(k, jsonToValue(state, it.value()));
        }
        std::sort(entries.begin(), entries.end(),
            [](auto & a, auto & b) { return a.first < b.first; });
        Bindings * b = Alloc::allocBindings(static_cast<uint32_t>(entries.size()));
        allocStats().attrsetsAllocated++;
        for (size_t i = 0; i < entries.size(); ++i) {
            b->entries[i].name = entries[i].first;
            b->entries[i].value = entries[i].second;
        }
        out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
        out.payload.bindings = b;
        return out;
    }
    throw std::runtime_error("v3 jsonToValue: unsupported JSON type");
}

/// v3 Value -> nlohmann::json.
nlohmann::json valueToJson(EvalState & state, const Value & vRaw)
{
    using json = nlohmann::json;
    // Force first — App / Thunk reach this primop unchanged when
    // wrapped in attrsets/lists.
    Value v = forceValue(*state.vm, vRaw);
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
                arr.push_back(valueToJson(state, v.payload.list->elems[i]));
        return arr;
    }
    case Tag::Attrs: {
        // `__toString self` overrides JSON serialization — call it
        // and use the resulting string.  Standard Nix coercion.
        if (v.payload.bindings) {
            static const SymbolId tsId = ir::globalInternSymbol("__toString");
            if (auto * fn = v.payload.bindings->lookup(tsId)) {
                Value forced = forceValue(*state.vm, *fn);
                Value s = callClosure(*state.vm, forced, v);
                s = forceValue(*state.vm, s);
                if (s.isString()) return json(std::string(s.payload.str));
            }
            // `outPath` (a derivation-like value) — serialize as the
            // path string.
            static const SymbolId outId = ir::globalInternSymbol("outPath");
            if (auto * op = v.payload.bindings->lookup(outId)) {
                Value forced = forceValue(*state.vm, *op);
                if (forced.isString()) return json(std::string(forced.payload.str));
                if (forced.isPath())   return json(std::string(forced.payload.path));
            }
        }
        json obj = json::object();
        if (v.payload.bindings) {
            for (uint32_t i = 0; i < v.payload.bindings->size; ++i) {
                auto & en = v.payload.bindings->entries[i];
                std::string_view k = vmSymName(state, en.name);
                obj[std::string(k)] = valueToJson(state, en.value);
            }
        }
        return obj;
    }
    case Tag::Uninitialized:
    case Tag::Closure:
    case Tag::PrimOp:
    case Tag::PrimOpApp:
    case Tag::Thunk:
    case Tag::App:
    case Tag::Blackhole:
    case Tag::External:
    default:
        throw std::runtime_error("v3 toJSON: unsupported value type");
    }
}

void primFromJSON(EvalState & state, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("fromJSON", "string");
    auto j = nlohmann::json::parse(std::string(args[0].payload.str), nullptr, /*allow_exceptions=*/true);
    out = jsonToValue(state, j);
}

void primToJSON(EvalState & state, Value * args, Value & out)
{
    auto j = valueToJson(state, args[0]);
    out = mkStringValueOwned(j.dump());
}

/// builtins.sort: sort a list using a comparator.  cmp(a, b) is true if
/// a should come before b.
void primSort(EvalState & state, Value * args, Value & out)
{
    if (!args[1].isList()) typeError("sort", "list");
    auto * src = args[1].payload.list;
    Value cmp = args[0];
    if (!src || src->size <= 1) { out = args[1]; return; }
    ListVec * result = Alloc::allocList(src->size);
    allocStats().listsAllocated++;
    for (uint32_t i = 0; i < src->size; ++i) result->elems[i] = src->elems[i];
    std::sort(result->elems, result->elems + src->size,
        [&](const Value & a, const Value & b) {
            Value step1 = callClosure(*state.vm, cmp, a);
            Value r = callClosure(*state.vm, step1, b);
            if (!r.isBool()) typeError("sort", "comparator returning bool");
            return r.payload.i == 1;
        });
    out.tag_payload = static_cast<uint64_t>(Tag::List);
    out.payload.list = result;
}

/// builtins.bitAnd / bitOr / bitXor on int.
void primBitAnd(EvalState &, Value * args, Value & out)
{
    if (!args[0].isInt() || !args[1].isInt()) typeError("bitAnd", "two ints");
    out.mkInt(args[0].payload.i & args[1].payload.i);
}
void primBitOr(EvalState &, Value * args, Value & out)
{
    if (!args[0].isInt() || !args[1].isInt()) typeError("bitOr", "two ints");
    out.mkInt(args[0].payload.i | args[1].payload.i);
}
void primBitXor(EvalState &, Value * args, Value & out)
{
    if (!args[0].isInt() || !args[1].isInt()) typeError("bitXor", "two ints");
    out.mkInt(args[0].payload.i ^ args[1].payload.i);
}

/// floor / ceil for floats.
void primFloor(EvalState &, Value * args, Value & out)
{
    if (args[0].isInt())   { out = args[0]; return; }
    if (!args[0].isFloat()) typeError("floor", "float or int");
    out.mkInt(static_cast<int64_t>(std::floor(args[0].payload.f)));
}
void primCeil(EvalState &, Value * args, Value & out)
{
    if (args[0].isInt())   { out = args[0]; return; }
    if (!args[0].isFloat()) typeError("ceil", "float or int");
    out.mkInt(static_cast<int64_t>(std::ceil(args[0].payload.f)));
}

/// stringLength has a 1-arg version; stringToInt would be nice but
/// nix has only specific primops.  Add fromString-ish helpers:
void primParseInt(EvalState &, Value * args, Value & out)
{
    if (!args[0].isString()) typeError("parseInt", "string");
    try {
        int64_t v = std::stoll(args[0].payload.str);
        out.mkInt(v);
    } catch (...) {
        throw std::runtime_error("v3 parseInt: invalid integer");
    }
}

/// tryEval: forces the argument; returns
///   { success = true;  value = result;       } on success,
///   { success = false; value = false;        } on caught exception.
void primTryEval(EvalState & state, Value * args, Value & out)
{
    SymbolId sSuccess = vmIntern(state, "success");
    SymbolId sValue   = vmIntern(state, "value");
    Value successV, valueV;
    try {
        valueV = forceValue(*state.vm, args[0]);
        successV = Value::vTrue;
    } catch (const std::exception &) {
        successV = Value::vFalse;
        valueV = Value::vFalse;
    }
    Bindings * b = Alloc::allocBindings(2);
    allocStats().attrsetsAllocated++;
    if (sSuccess < sValue) {
        b->entries[0] = {sSuccess, successV};
        b->entries[1] = {sValue, valueV};
    } else {
        b->entries[0] = {sValue, valueV};
        b->entries[1] = {sSuccess, successV};
    }
    out.tag_payload = static_cast<uint64_t>(Tag::Attrs);
    out.payload.bindings = b;
}

/// Forward-declared so primLessThan can recurse through list elements.
static bool valueLessHelper(VMState & vm, const Value & a, const Value & b);

void primLessThan(EvalState & state, Value * args, Value & out)
{
    bool r = valueLessHelper(*state.vm, args[0], args[1]);
    out = r ? Value::vTrue : Value::vFalse;
}

static bool valueLessHelper(VMState & vm, const Value & a, const Value & b)
{
    if      (a.isInt() && b.isInt())     return a.payload.i < b.payload.i;
    else if (a.isFloat() && b.isFloat()) return a.payload.f < b.payload.f;
    else if (a.isInt() && b.isFloat())   return static_cast<double>(a.payload.i) < b.payload.f;
    else if (a.isFloat() && b.isInt())   return a.payload.f < static_cast<double>(b.payload.i);
    else if (a.isString() && b.isString())
        return std::string_view(a.payload.str) < std::string_view(b.payload.str);
    else if (a.isList() && b.isList()) {
        // Lexicographic compare; force lazy elements as we go.
        uint32_t na = a.payload.list ? a.payload.list->size : 0;
        uint32_t nb = b.payload.list ? b.payload.list->size : 0;
        uint32_t n = std::min(na, nb);
        for (uint32_t i = 0; i < n; ++i) {
            Value ai = forceValue(vm, a.payload.list->elems[i]);
            Value bi = forceValue(vm, b.payload.list->elems[i]);
            if (valueLessHelper(vm, ai, bi)) return true;
            if (valueLessHelper(vm, bi, ai)) return false;
        }
        return na < nb;
    }
    typeError("lessThan", "comparable types");
}

} // namespace

// ---------------------------------------------------------------------------
// Public registry API
// ---------------------------------------------------------------------------

const PrimOp * findPrimOp(std::string_view name)
{
    std::lock_guard<std::mutex> g(registryMutex());
    auto it = registry().find(std::string(name));
    return it == registry().end() ? nullptr : &it->second;
}

const std::unordered_map<std::string, PrimOp> & allRegisteredPrimOps()
{
    return registry();
}

void setNixEvalState(nix::EvalState * st) { tlNixEvalState = st; }
nix::EvalState * getNixEvalState() { return tlNixEvalState; }

void registerPrimOp(const PrimOp & op)
{
    std::lock_guard<std::mutex> g(registryMutex());
    registry()[std::string(op.name)] = op;
}

void registerBuiltinPrimOps()
{
    static std::once_flag flag;
    std::call_once(flag, []() {
        registerPrimOp({"length",       1, primLength});
        registerPrimOp({"head",         1, primHead});
        registerPrimOp({"tail",         1, primTail});
        registerPrimOp({"elemAt",       2, primElemAt});
        registerPrimOp({"attrNames",    1, primAttrNames});
        registerPrimOp({"attrValues",   1, primAttrValues});
        registerPrimOp({"isAttrs",      1, primIsAttrs});
        registerPrimOp({"isList",       1, primIsList});
        registerPrimOp({"isFunction",   1, primIsFunction});
        registerPrimOp({"isString",     1, primIsString});
        registerPrimOp({"isInt",        1, primIsInt});
        registerPrimOp({"isBool",       1, primIsBool});
        registerPrimOp({"isNull",       1, primIsNull});
        registerPrimOp({"isFloat",      1, primIsFloat});
        registerPrimOp({"isPath",       1, primIsPath});
        registerPrimOp({"toString",     1, primToString});
        registerPrimOp({"typeOf",       1, primTypeOf});
        registerPrimOp({"stringLength", 1, primStringLength});
        registerPrimOp({"add",          2, primAdd});
        registerPrimOp({"sub",          2, primSub});
        registerPrimOp({"mul",          2, primMul});
        registerPrimOp({"div",          2, primDiv});
        registerPrimOp({"throw",        1, primThrow});
        registerPrimOp({"lessThan",     2, primLessThan});

        // Internal aliases used by the parser: `a * b` lowers to a Call of
        // `__mul`; same for __sub / __add / __div / __lessThan.
        registerPrimOp({"__add",        2, primAdd});
        registerPrimOp({"__sub",        2, primSub});
        registerPrimOp({"__mul",        2, primMul});
        registerPrimOp({"__div",        2, primDiv});
        registerPrimOp({"__lessThan",   2, primLessThan});
        registerPrimOp({"concatLists",        1, primConcatLists});
        registerPrimOp({"concatStringsSep",   2, primConcatStringsSep});
        registerPrimOp({"substring",          3, primSubstring});
        // Higher-order callback primops (re-enter the VM via callClosure).
        registerPrimOp({"map",                2, primMap});
        registerPrimOp({"filter",             2, primFilter});
        registerPrimOp({"foldl'",             3, primFoldl});
        registerPrimOp({"genList",            2, primGenList});
        registerPrimOp({"all",                2, primAll});
        registerPrimOp({"any",                2, primAny});
        registerPrimOp({"concatMap",          2, primConcatMap});
        registerPrimOp({"partition",          2, primPartition});
        registerPrimOp({"getEnv",             1, primGetEnv});
        registerPrimOp({"compareVersions",    2, primCompareVersions});
        registerPrimOp({"listToAttrs",        1, primListToAttrs});
        registerPrimOp({"removeAttrs",        2, primRemoveAttrs});
        registerPrimOp({"intersectAttrs",     2, primIntersectAttrs});
        registerPrimOp({"mapAttrs",           2, primMapAttrs});
        registerPrimOp({"elem",               2, primElem});
        registerPrimOp({"getAttr",            2, primGetAttr});
        registerPrimOp({"hasAttr",            2, primHasAttr});
        registerPrimOp({"catAttrs",           2, primCatAttrs});
        registerPrimOp({"replaceStrings",     3, primReplaceStrings});
        registerPrimOp({"abort",              1, primAbort});
        registerPrimOp({"seq",                2, primSeq});
        registerPrimOp({"deepSeq",            2, primDeepSeq});
        registerPrimOp({"trace",              2, primTrace});
        registerPrimOp({"traceVerbose",       2, primTraceVerbose});
        registerPrimOp({"zipAttrsWith",       2, primZipAttrsWith});
        registerPrimOp({"unsafeGetAttrPos",   2, primUnsafeGetAttrPos});
        registerPrimOp({"toPath",             1, primToPath});
        registerPrimOp({"splitVersion",       1, primSplitVersion});
        registerPrimOp({"unsafeDiscardStringContext",      1, primUnsafeDiscardStringContext});
        registerPrimOp({"hasContext",         1, primHasContext});
        registerPrimOp({"getContext",         1, primGetContext});
        registerPrimOp({"unsafeDiscardOutputDependency",   1, primUnsafeDiscardOutputDependency});
        registerPrimOp({"tryEval",            1, primTryEval});
        registerPrimOp({"baseNameOf",         1, primBaseNameOf});
        registerPrimOp({"dirOf",              1, primDirOf});
        registerPrimOp({"pathExists",         1, primPathExists});
        registerPrimOp({"splitString",        2, primSplitString});
        registerPrimOp({"sort",               2, primSort});
        registerPrimOp({"bitAnd",             2, primBitAnd});
        registerPrimOp({"bitOr",              2, primBitOr});
        registerPrimOp({"bitXor",             2, primBitXor});
        registerPrimOp({"floor",              1, primFloor});
        registerPrimOp({"ceil",               1, primCeil});
        registerPrimOp({"parseInt",           1, primParseInt});
        registerPrimOp({"fromJSON",           1, primFromJSON});
        registerPrimOp({"toJSON",             1, primToJSON});
        registerPrimOp({"functionArgs",       1, primFunctionArgs});
        registerPrimOp({"import",             1, primImport});
        registerPrimOp({"readFile",           1, primReadFile});
        registerPrimOp({"readDir",            1, primReadDir});
        registerPrimOp({"parseDrvName",       1, primParseDrvName});
        registerPrimOp({"groupBy",            2, primGroupBy});
        registerPrimOp({"match",              2, primMatch});
        registerPrimOp({"split",              2, primSplit});
        registerPrimOp({"hashString",         2, primHashString});
        registerPrimOp({"currentSystem",      0, primCurrentSystem});
        registerPrimOp({"currentTime",        0, primCurrentTime});
        registerPrimOp({"nixVersion",         0, primNixVersion});
        registerPrimOp({"genericClosure",     1, primGenericClosure});
        registerPrimOp({"hashFile",           2, primHashFile});
        registerPrimOp({"convertHash",        1, primConvertHash});
        registerPrimOp({"readFileType",       1, primReadFileType});
        registerPrimOp({"addErrorContext",    2, primAddErrorContext});
        registerPrimOp({"derivationStrict",   1, primDerivationStrict});
        // C++ port of corepkgs/derivation.nix — derivationStrict
        // synthesizes paths, primDerivation wraps them up with
        // commonAttrs and outputName for tree-walker parity.
        registerPrimOp({"derivation",         1, primDerivation});
        registerPrimOp({"findFile",           2, primFindFile});
        registerPrimOp({"__findFile",         2, primFindFile});
        registerPrimOp({"nixPath",            0, primNixPath});
        registerPrimOp({"__nixPath",          0, primNixPath});
        registerPrimOp({"scopedImport",       2, primScopedImport});
        registerPrimOp({"path",               1, primPath});
        registerPrimOp({"fromTOML",           1, primFromTOML});
        registerPrimOp({"parseFlakeRef",      1, primParseFlakeRef});
        registerPrimOp({"flakeRefToString",   1, primFlakeRefToString});
        registerPrimOp({"toXML",              1, primToXML});
    });
}

} // namespace nix::v3
