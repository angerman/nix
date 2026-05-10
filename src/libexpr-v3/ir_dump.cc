/// @file
/// IR text dumper implementation.  See `include/v3/ir_dump.hh` for the
/// format description.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/ir_dump.hh"
#include "v3/primop.hh"

#include <cstdio>
#include <sstream>
#include <string>
#include <variant>

namespace nix::v3::ir {

namespace {

// ---------------------------------------------------------------------------
// Output sink — std::ostringstream wrapped to avoid sprintf+std::string
// bouncing in hot dump paths (matters for V3_DUMP_AT_START on large CUs).
// ---------------------------------------------------------------------------

struct W {
    std::ostringstream out;

    void put(std::string_view s) { out << s; }
    void putln() { out << '\n'; }

    void var(VarId v) {
        if (v == kInvalid) out << "<inv>";
        else out << 'v' << v;
    }
    void block(BlockId b) {
        if (b == kInvalidBlock) out << "<inv-block>";
        else out << 'B' << b;
    }
    void func(FuncId f) { out << 'f' << f; }
    void sym(SymbolId id) {
        if (id == kInvalidSymbol) { out << "<inv-sym>"; return; }
        const auto & g = globalSymbolTable();
        if (id < g.size()) out << '"' << g[id] << '"';
        else out << "<sym=" << id << '>';
    }

    /// Print a comma-separated list of VarIds.
    void varList(const std::vector<VarId> & vs) {
        out << '[';
        for (size_t i = 0; i < vs.size(); ++i) {
            if (i) out << ',';
            var(vs[i]);
        }
        out << ']';
    }

    std::string str() && { return std::move(out).str(); }
    std::string str() const { return out.str(); }
};

void dumpExprInto(W & w, const Module & m, const Expr & e);

// ---------------------------------------------------------------------------
// Per-Expr printers (one std::visit branch per IR variant).  These match
// the ordering in include/v3/ir.hh's `Expr` variant declaration so a
// future variant addition is statically forced to land here too via the
// `static_assert(false)` in the catch-all.
// ---------------------------------------------------------------------------

void dumpLitInt(W & w, const LitInt & e)    { w.put("LitInt "); w.out << e.value; }
void dumpLitFloat(W & w, const LitFloat & e){ w.put("LitFloat "); w.out << e.value; }
void dumpLitBool(W & w, const LitBool & e)  { w.put("LitBool "); w.out << (e.value ? "true" : "false"); }
void dumpLitNull(W & w, const LitNull &)    { w.put("LitNull"); }
void dumpLitString(W & w, const LitString & e) {
    w.put("LitString \"");
    for (char c : e.value) {
        if (c == '"' || c == '\\') w.out << '\\';
        if (c == '\n') { w.out << "\\n"; continue; }
        w.out << c;
    }
    w.put("\"");
}
void dumpLitPath(W & w, const LitPath & e)  { w.put("LitPath \""); w.put(e.path); w.put("\""); }

void dumpVarRef(W & w, const VarRef & e)    { w.put("VarRef "); w.var(e.var); }
void dumpWithLookup(W & w, const WithLookup & e) {
    w.put("WithLookup "); w.sym(e.name);
}

void dumpLambda(W & w, const Lambda & e) {
    w.put("Lambda "); w.func(e.funcIdx);
    w.put(" freeVars="); w.varList(e.freeVars);
    if (!e.lexicalWiths.empty()) {
        w.put(" lexicalWiths="); w.varList(e.lexicalWiths);
    }
}
void dumpApp(W & w, const App & e) {
    w.put("App "); w.var(e.fun); w.put(" "); w.var(e.arg);
}
void dumpForce(W & w, const Force & e) {
    w.put("Force "); w.var(e.thunk);
}
void dumpMkThunk(W & w, const MkThunk & e) {
    w.put("MkThunk "); w.func(e.funcIdx);
    w.put(" freeVars="); w.varList(e.freeVars);
    if (!e.lexicalWiths.empty()) {
        w.put(" lexicalWiths="); w.varList(e.lexicalWiths);
    }
}

void dumpAttrSelect(W & w, const AttrSelect & e) {
    w.put("AttrSelect "); w.var(e.attrs); w.put(" "); w.sym(e.name);
}
void dumpAttrSelectDyn(W & w, const AttrSelectDyn & e) {
    w.put("AttrSelectDyn "); w.var(e.attrs); w.put(" "); w.var(e.nameVar);
}
void dumpHasAttr(W & w, const HasAttr & e) {
    w.put("HasAttr "); w.var(e.attrs); w.put(" "); w.sym(e.name);
}
void dumpHasAttrDyn(W & w, const HasAttrDyn & e) {
    w.put("HasAttrDyn "); w.var(e.attrs); w.put(" "); w.var(e.nameVar);
}
void dumpAttrSet(W & w, const AttrSet & e) {
    w.put("AttrSet {");
    for (size_t i = 0; i < e.entries.size(); ++i) {
        if (i) w.put(",");
        w.sym(e.entries[i].name);
        if (e.entries[i].isInheritFrom) {
            // #558: IF entry — value backfilled by trailing
            // AttrSetSetInheritFrom binding.  Mark with `=IF` to make
            // the dump unambiguous (a kInvalid placeholder would print
            // as `var=0` otherwise).
            w.put("=IF");
        } else {
            w.put("="); w.var(e.entries[i].value);
        }
    }
    w.put("}");
}
void dumpAttrSetSetInheritFrom(W & w, const AttrSetSetInheritFrom & e) {
    w.put("AttrSetSetInheritFrom attrs="); w.var(e.attrSetVar);
    w.put(" {");
    for (size_t i = 0; i < e.entries.size(); ++i) {
        if (i) w.put(",");
        w.put("slot="); w.put(std::to_string(e.entries[i].sortedSlot));
        w.put(":"); w.var(e.entries[i].valueVar);
    }
    w.put("}");
}
void dumpAttrSetDyn(W & w, const AttrSetDyn & e) {
    w.put("AttrSetDyn statics={");
    for (size_t i = 0; i < e.statics.size(); ++i) {
        if (i) w.put(",");
        w.sym(e.statics[i].name); w.put("="); w.var(e.statics[i].value);
    }
    w.put("} dynamics={");
    for (size_t i = 0; i < e.dynamics.size(); ++i) {
        if (i) w.put(",");
        w.var(e.dynamics[i].nameVar); w.put("="); w.var(e.dynamics[i].value);
    }
    w.put("}");
}
void dumpRecBindingSlotRef(W & w, const RecBindingSlotRef & e) {
    w.put("RecBindingSlotRef "); w.var(e.attrs); w.put(" "); w.sym(e.name);
}

void dumpListExpr(W & w, const ListExpr & e) {
    w.put("ListExpr "); w.varList(e.elems);
}
void dumpConcatLists(W & w, const ConcatLists & e) {
    w.put("ConcatLists "); w.var(e.lhs); w.put(" "); w.var(e.rhs);
}

void dumpIf(W & w, const If & e) {
    w.put("If cond="); w.var(e.cond);
    w.put(" then="); w.block(e.thenBlock);
    w.put(" else="); w.block(e.elseBlock);
}
void dumpWith(W & w, const With & e) {
    w.put("With attrs="); w.var(e.attrs);
    w.put(" body="); w.block(e.bodyBlock);
    if (e.recAttrsVar != kInvalid) {
        w.put(" recAttrsVar="); w.var(e.recAttrsVar);
        w.put(" recAttrsName="); w.sym(e.recAttrsName);
    }
}
void dumpAssert(W & w, const Assert & e) {
    w.put("Assert cond="); w.var(e.cond);
    w.put(" body="); w.block(e.bodyBlock);
}

void dumpConcatStrings(W & w, const ConcatStrings & e) {
    w.put("ConcatStrings");
    if (e.forceString) w.put(" forceString=true");
    w.put(" "); w.varList(e.parts);
}

void dumpNot(W & w, const Not & e) {
    w.put("Not "); w.var(e.operand);
}

#define DUMP_BIN(name) \
    void dump##name(W & w, const name & e) { \
        w.put(#name " "); w.var(e.lhs); w.put(" "); w.var(e.rhs); \
    }
DUMP_BIN(Add)
DUMP_BIN(Sub)
DUMP_BIN(Mul)
DUMP_BIN(Div)
DUMP_BIN(Eq)
DUMP_BIN(NEq)
DUMP_BIN(Less)
DUMP_BIN(Update)
#undef DUMP_BIN

void dumpAnd(W & w, const And & e)   { w.put("And ");  w.var(e.lhs); w.put(" rhs="); w.block(e.rhsBlock); }
void dumpOr(W & w, const Or & e)     { w.put("Or ");   w.var(e.lhs); w.put(" rhs="); w.block(e.rhsBlock); }
void dumpImpl(W & w, const Impl & e) { w.put("Impl "); w.var(e.lhs); w.put(" rhs="); w.block(e.rhsBlock); }

void dumpPrimOpCall(W & w, const PrimOpCall & e) {
    w.put("PrimOpCall ");
    if (e.primop && !e.primop->name.empty())
        w.put(std::string("\"") + std::string(e.primop->name) + "\"");
    else
        w.put("\"<?>\"");
    w.put(" "); w.varList(e.args);
}
void dumpLitPrimOp(W & w, const LitPrimOp & e) {
    w.put("LitPrimOp ");
    if (e.primop && !e.primop->name.empty())
        w.put(std::string("\"") + std::string(e.primop->name) + "\"");
    else
        w.put("\"<?>\"");
}
void dumpLitBuiltins(W & w, const LitBuiltins &) { w.put("LitBuiltins"); }

void dumpLetRec(W & w, const LetRec & e) {
    w.put("LetRec recVar="); w.var(e.recVar);
    w.put(e.hasBody ? " kind=let-in-body" : " kind=rec-attrs");
    w.put(" entries={");
    for (size_t i = 0; i < e.entries.size(); ++i) {
        if (i) w.put(",");
        w.sym(e.entries[i].name);
        w.put("=fid="); w.func(e.entries[i].thunkBody);
        if (!e.entries[i].outerUpvalues.empty()) {
            w.put(",outerUp="); w.varList(e.entries[i].outerUpvalues);
        }
        if (!e.entries[i].lexicalWiths.empty()) {
            w.put(",lexW="); w.varList(e.entries[i].lexicalWiths);
        }
    }
    w.put("}");
    if (!e.hiddenEntries.empty()) {
        w.put(" hidden={");
        for (size_t i = 0; i < e.hiddenEntries.size(); ++i) {
            if (i) w.put(",");
            w.var(e.hiddenEntries[i].hiddenVar);
            w.put("=fid="); w.func(e.hiddenEntries[i].thunkBody);
        }
        w.put("}");
    }
}

void dumpExprInto(W & w, const Module & m, const Expr & e)
{
    (void)m;
    std::visit([&w](const auto & x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, LitInt>)            dumpLitInt(w, x);
        else if constexpr (std::is_same_v<T, LitFloat>)     dumpLitFloat(w, x);
        else if constexpr (std::is_same_v<T, LitBool>)      dumpLitBool(w, x);
        else if constexpr (std::is_same_v<T, LitNull>)      dumpLitNull(w, x);
        else if constexpr (std::is_same_v<T, LitString>)    dumpLitString(w, x);
        else if constexpr (std::is_same_v<T, LitPath>)      dumpLitPath(w, x);
        else if constexpr (std::is_same_v<T, VarRef>)       dumpVarRef(w, x);
        else if constexpr (std::is_same_v<T, WithLookup>)   dumpWithLookup(w, x);
        else if constexpr (std::is_same_v<T, Lambda>)       dumpLambda(w, x);
        else if constexpr (std::is_same_v<T, App>)          dumpApp(w, x);
        else if constexpr (std::is_same_v<T, Force>)        dumpForce(w, x);
        else if constexpr (std::is_same_v<T, MkThunk>)      dumpMkThunk(w, x);
        else if constexpr (std::is_same_v<T, AttrSelect>)   dumpAttrSelect(w, x);
        else if constexpr (std::is_same_v<T, AttrSelectDyn>)dumpAttrSelectDyn(w, x);
        else if constexpr (std::is_same_v<T, HasAttr>)      dumpHasAttr(w, x);
        else if constexpr (std::is_same_v<T, HasAttrDyn>)   dumpHasAttrDyn(w, x);
        else if constexpr (std::is_same_v<T, AttrSet>)      dumpAttrSet(w, x);
        else if constexpr (std::is_same_v<T, AttrSetSetInheritFrom>) dumpAttrSetSetInheritFrom(w, x);
        else if constexpr (std::is_same_v<T, AttrSetDyn>)   dumpAttrSetDyn(w, x);
        else if constexpr (std::is_same_v<T, RecBindingSlotRef>) dumpRecBindingSlotRef(w, x);
        else if constexpr (std::is_same_v<T, ListExpr>)     dumpListExpr(w, x);
        else if constexpr (std::is_same_v<T, ConcatLists>)  dumpConcatLists(w, x);
        else if constexpr (std::is_same_v<T, If>)           dumpIf(w, x);
        else if constexpr (std::is_same_v<T, With>)         dumpWith(w, x);
        else if constexpr (std::is_same_v<T, Assert>)       dumpAssert(w, x);
        else if constexpr (std::is_same_v<T, ConcatStrings>)dumpConcatStrings(w, x);
        else if constexpr (std::is_same_v<T, Not>)          dumpNot(w, x);
        else if constexpr (std::is_same_v<T, Add>)          dumpAdd(w, x);
        else if constexpr (std::is_same_v<T, Sub>)          dumpSub(w, x);
        else if constexpr (std::is_same_v<T, Mul>)          dumpMul(w, x);
        else if constexpr (std::is_same_v<T, Div>)          dumpDiv(w, x);
        else if constexpr (std::is_same_v<T, Eq>)           dumpEq(w, x);
        else if constexpr (std::is_same_v<T, NEq>)          dumpNEq(w, x);
        else if constexpr (std::is_same_v<T, Less>)         dumpLess(w, x);
        else if constexpr (std::is_same_v<T, And>)          dumpAnd(w, x);
        else if constexpr (std::is_same_v<T, Or>)           dumpOr(w, x);
        else if constexpr (std::is_same_v<T, Impl>)         dumpImpl(w, x);
        else if constexpr (std::is_same_v<T, Update>)       dumpUpdate(w, x);
        else if constexpr (std::is_same_v<T, PrimOpCall>)   dumpPrimOpCall(w, x);
        else if constexpr (std::is_same_v<T, LitPrimOp>)    dumpLitPrimOp(w, x);
        else if constexpr (std::is_same_v<T, LitBuiltins>)  dumpLitBuiltins(w, x);
        else if constexpr (std::is_same_v<T, LetRec>)       dumpLetRec(w, x);
        else
            // Compile-time guard against new IR variants leaking past this
            // dispatcher.  Adding a new alternative to ir::Expr forces a
            // build error here that points at this file.
            static_assert(sizeof(T) == 0, "ir_dump: missing IR variant");
    }, e);
}

void dumpBlockInto(W & w, const Module & m, BlockId bid, const char * indent)
{
    if (bid == kInvalidBlock || bid >= m.blocks.size()) return;
    const Block & b = m.blocks[bid];
    w.put(indent); w.block(bid); w.put(":"); w.putln();
    for (const auto & bd : b.bindings) {
        w.put(indent); w.put("  ");
        w.var(bd.var); w.put(" = ");
        dumpExprInto(w, m, bd.expr);
        w.putln();
    }
    if (auto * ret = std::get_if<TermReturn>(&b.terminal)) {
        w.put(indent); w.put("  return ");
        if (ret->value == kInvalid) w.put("<none>");
        else w.var(ret->value);
        w.putln();
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::string dumpModule(const Module & m)
{
    W w;
    w.out << "; module n_funcs=" << m.functions.size()
          << " n_blocks=" << m.blocks.size()
          << " nextVar=" << m.nextVar << "\n";
    for (FuncId fid = 0; fid < (FuncId)m.functions.size(); ++fid) {
        const Function & f = m.functions[fid];
        w.out << "; func "; w.func(fid);
        w.out << " entry=";
        if (f.entryBlock == kInvalidBlock) w.put("<none>");
        else w.block(f.entryBlock);
        w.out << " nUp=" << f.freeVars.size();
        if (f.paramVar != kInvalid) {
            w.out << " param="; w.var(f.paramVar);
        }
        if (!f.freeVars.empty()) {
            w.out << " freeVars="; w.varList(f.freeVars);
        }
        if (f.nWithTargets > 0) {
            w.out << " nWiths=" << f.nWithTargets;
        }
        if (!f.name.empty()) {
            w.out << " name=\"" << f.name << "\"";
        }
        w.out << "\n";
        if (f.entryBlock != kInvalidBlock)
            dumpBlockInto(w, m, f.entryBlock, "");
    }
    return std::move(w).str();
}

std::string dumpBlock(const Module & m, BlockId bid)
{
    W w;
    dumpBlockInto(w, m, bid, "");
    return std::move(w).str();
}

std::string dumpExpr(const Module & m, const Expr & e)
{
    W w;
    dumpExprInto(w, m, e);
    return std::move(w).str();
}

// ---------------------------------------------------------------------------
// FileCheck-style checker
// ---------------------------------------------------------------------------

namespace {

std::vector<std::string> splitLines(std::string_view s)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') { out.push_back(std::move(cur)); cur.clear(); }
        else cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

/// Strip leading/trailing whitespace.
std::string trim(std::string_view s)
{
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) ++a;
    size_t b = s.size();
    while (b > a && (s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\r')) --b;
    return std::string(s.substr(a, b - a));
}

struct CheckDirective {
    enum class Kind { Match, NotMatch };
    Kind kind;
    std::string pattern;
    size_t directiveLine = 0;  // for diagnostics
};

/// Parse `; CHECK: pat` and `; CHECK-NOT: pat` directives from the
/// expected text.  Lines that don't start with `; CHECK` are ignored.
std::vector<CheckDirective> parseChecks(std::string_view expected)
{
    std::vector<CheckDirective> out;
    auto lines = splitLines(expected);
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string l = trim(lines[i]);
        if (l.size() < 2 || l[0] != ';') continue;
        // Skip leading "; ".
        size_t p = 1;
        while (p < l.size() && (l[p] == ' ' || l[p] == '\t')) ++p;
        std::string_view rest(l.data() + p, l.size() - p);
        // CHECK-NOT
        constexpr std::string_view tagNot = "CHECK-NOT:";
        constexpr std::string_view tagPos = "CHECK:";
        if (rest.substr(0, tagNot.size()) == tagNot) {
            std::string_view pat = rest.substr(tagNot.size());
            out.push_back({CheckDirective::Kind::NotMatch, trim(pat), i + 1});
        } else if (rest.substr(0, tagPos.size()) == tagPos) {
            std::string_view pat = rest.substr(tagPos.size());
            out.push_back({CheckDirective::Kind::Match, trim(pat), i + 1});
        }
        // Other "; ..." lines are ignored — lets users write commentary.
    }
    return out;
}

} // namespace

std::string checkIr(std::string_view actual, std::string_view expected)
{
    auto actualLines = splitLines(actual);
    auto checks = parseChecks(expected);
    if (checks.empty())
        return "; checkIr: no `; CHECK:` directives found in expected text";

    size_t cursor = 0;  // current line in actualLines we've matched up to (exclusive).
    for (size_t i = 0; i < checks.size(); ++i) {
        const auto & c = checks[i];
        if (c.kind == CheckDirective::Kind::Match) {
            // Search forward from cursor.
            size_t hit = std::string::npos;
            for (size_t a = cursor; a < actualLines.size(); ++a) {
                if (actualLines[a].find(c.pattern) != std::string::npos) {
                    hit = a; break;
                }
            }
            if (hit == std::string::npos) {
                std::ostringstream e;
                e << "checkIr: CHECK directive #" << (i + 1)
                  << " (expected line " << c.directiveLine << ") failed:\n"
                  << "  expected to find: '" << c.pattern << "'\n"
                  << "  in actual lines [" << cursor << ".."
                  << actualLines.size() << "):\n";
                size_t ctxFrom = cursor;
                size_t ctxTo = std::min(actualLines.size(), cursor + 16);
                for (size_t a = ctxFrom; a < ctxTo; ++a)
                    e << "    [" << a << "] " << actualLines[a] << "\n";
                return e.str();
            }
            cursor = hit + 1;
        } else {
            // CHECK-NOT: forbid the pattern from appearing between the
            // current cursor and the NEXT positive CHECK's match position
            // (or end of actual if no further positive check).
            size_t scanEnd = actualLines.size();
            for (size_t j = i + 1; j < checks.size(); ++j) {
                if (checks[j].kind == CheckDirective::Kind::Match) {
                    for (size_t a = cursor; a < actualLines.size(); ++a)
                        if (actualLines[a].find(checks[j].pattern) != std::string::npos) {
                            scanEnd = a; break;
                        }
                    break;
                }
            }
            for (size_t a = cursor; a < scanEnd; ++a) {
                if (actualLines[a].find(c.pattern) != std::string::npos) {
                    std::ostringstream e;
                    e << "checkIr: CHECK-NOT directive #" << (i + 1)
                      << " (expected line " << c.directiveLine << ") failed:\n"
                      << "  forbidden pattern: '" << c.pattern << "'\n"
                      << "  found at actual line " << a << ": "
                      << actualLines[a] << "\n";
                    return e.str();
                }
            }
        }
    }
    return {};  // success
}

} // namespace nix::v3::ir
