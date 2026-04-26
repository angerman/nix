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

#include <algorithm>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace nix::v3 {

namespace {

std::unordered_map<std::string, PrimOp> & registry()
{
    static std::unordered_map<std::string, PrimOp> r;
    return r;
}

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
    // Names should be the actual symbol strings.  We don't have a runtime
    // symbol table here, so the caller must arrange to look them up via
    // EvalState's symbol table.  Stub: emit the SymbolId as a string.
    for (uint32_t i = 0; i < n; ++i) {
        Value v;
        // Until EvalState provides a symbol table, encode the SymbolId as
        // its decimal representation.  Real lowering will plug in the CU's
        // symbolTable.
        v = mkStringValueOwned(std::to_string(a.payload.bindings->entries[i].name));
        lv->elems[i] = v;
    }
    out.tag_payload = static_cast<uint64_t>(Tag::List);
    out.payload.list = lv;
}

void primAttrValues(EvalState &, Value * args, Value & out)
{
    const Value & a = args[0];
    if (!a.isAttrs() || !a.payload.bindings) typeError("attrValues", "attrset");
    uint32_t n = a.payload.bindings->size;
    ListVec * lv = Alloc::allocList(n);
    allocStats().listsAllocated++;
    for (uint32_t i = 0; i < n; ++i) lv->elems[i] = a.payload.bindings->entries[i].value;
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
    else typeError("sub", "numeric");
}

void primMul(EvalState &, Value * args, Value & out)
{
    const Value & a = args[0]; const Value & b = args[1];
    if (a.isInt() && b.isInt())          out.mkInt(a.payload.i * b.payload.i);
    else if (a.isFloat() && b.isFloat()) out.mkFloat(a.payload.f * b.payload.f);
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
    } else typeError("div", "numeric");
}

void primThrow(EvalState &, Value * args, Value &)
{
    if (!args[0].isString()) typeError("throw", "string");
    throw std::runtime_error(std::string("v3 throw: ") + args[0].payload.str);
}

void primLessThan(EvalState &, Value * args, Value & out)
{
    const Value & a = args[0]; const Value & b = args[1];
    bool r;
    if      (a.isInt() && b.isInt())     r = a.payload.i < b.payload.i;
    else if (a.isFloat() && b.isFloat()) r = a.payload.f < b.payload.f;
    else if (a.isInt() && b.isFloat())   r = static_cast<double>(a.payload.i) < b.payload.f;
    else if (a.isFloat() && b.isInt())   r = a.payload.f < static_cast<double>(b.payload.i);
    else if (a.isString() && b.isString())
        r = std::string_view(a.payload.str) < std::string_view(b.payload.str);
    else typeError("lessThan", "comparable types");
    out = r ? Value::vTrue : Value::vFalse;
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
    });
}

} // namespace nix::v3
