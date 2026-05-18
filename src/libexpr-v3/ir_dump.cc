/// @file
/// IR text dumper implementation.  See `include/v3/ir_dump.hh` for the
/// format description.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/ir_dump.hh"
#include "v3/primop.hh"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <regex>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

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
    enum class Kind { Match, NotMatch, Label, Next };
    Kind kind;
    std::string pattern;
    /// True if the pattern contains `{{regex}}` placeholders that need
    /// regex semantics.  False = plain substring match (cheap fast path).
    bool hasRegex = false;
    size_t directiveLine = 0;  // for diagnostics
};

/// Convert a `{{regex}}`-flavoured FileCheck pattern into a `std::regex`.
/// Literal text outside the `{{...}}` delimiters is escaped; text inside
/// is taken verbatim as a regex fragment.  Example:
///   "v{{[0-9]+}} = LitInt {{[0-9]+}}"
/// → regex literal: `v[0-9]+ = LitInt [0-9]+`
std::regex compileRegexPattern(std::string_view pat)
{
    std::string out;
    out.reserve(pat.size() + 8);
    size_t i = 0;
    while (i < pat.size()) {
        if (i + 1 < pat.size() && pat[i] == '{' && pat[i+1] == '{') {
            // Find closing `}}`.
            size_t end = pat.find("}}", i + 2);
            if (end == std::string_view::npos) {
                // Unmatched `{{` — treat the rest as literal.
                for (size_t k = i; k < pat.size(); ++k) {
                    char c = pat[k];
                    if (std::strchr("\\^$.|?*+(){}[]", c)) out.push_back('\\');
                    out.push_back(c);
                }
                break;
            }
            // Emit the contents of `{{...}}` as raw regex.
            out.append(pat.data() + i + 2, end - (i + 2));
            i = end + 2;
        } else {
            char c = pat[i];
            // Escape regex metacharacters in literal segments.
            if (std::strchr("\\^$.|?*+(){}[]", c)) out.push_back('\\');
            out.push_back(c);
            ++i;
        }
    }
    return std::regex(out);
}

/// True if `pat` contains a `{{...}}` regex segment.
bool patternHasRegex(std::string_view pat)
{
    for (size_t i = 0; i + 1 < pat.size(); ++i)
        if (pat[i] == '{' && pat[i+1] == '{') return true;
    return false;
}

/// Test whether `actualLine` matches `directive`'s pattern.  Plain
/// patterns use substring search (cheap); regex patterns build a
/// `std::regex` lazily on each call (acceptable — typical fixtures
/// have under 100 directives × under 1000 actual lines).
bool lineMatches(const CheckDirective & d, std::string_view actualLine)
{
    if (!d.hasRegex)
        return actualLine.find(d.pattern) != std::string::npos;
    try {
        std::regex re = compileRegexPattern(d.pattern);
        return std::regex_search(actualLine.begin(), actualLine.end(), re);
    } catch (const std::regex_error &) {
        return false;
    }
}

/// Parse CHECK directives from `expected`.  Supports:
///   - prefix-character `;` OR `#` (LLVM `.ll` style and Nix `.nix` style).
///   - prefix-name PREFIX (default "CHECK"; e.g. "RAW" or "OPT" with
///     LLVM's `--check-prefix=`).
///   - PREFIX:           positive match (forward search).
///   - PREFIX-NOT:       negative — forbid pattern before next positive.
///   - PREFIX-LABEL:     strong anchor — resets cursor after matching.
///   - PREFIX-NEXT:      must be the IMMEDIATELY-NEXT line after prior match.
///   - `{{regex}}`       embedded regex within an otherwise-literal pattern.
///   - RUN:/COM: line-skip: if a line contains "RUN:" or "COM:" anywhere,
///     ignore any PREFIX directive on the same line.  Matches LLVM behavior
///     and lets you write `# RUN: v3-eval ... | v3-check %s` without
///     accidentally matching itself.
std::vector<CheckDirective>
parseChecks(std::string_view expected, std::string_view prefix = "CHECK")
{
    std::vector<CheckDirective> out;
    auto lines = splitLines(expected);

    // Build the four directive tags for this prefix: e.g. "CHECK:",
    // "CHECK-NOT:", "CHECK-LABEL:", "CHECK-NEXT:".
    std::string tagMatch (prefix); tagMatch  += ":";
    std::string tagNot   (prefix); tagNot    += "-NOT:";
    std::string tagLabel (prefix); tagLabel  += "-LABEL:";
    std::string tagNext  (prefix); tagNext   += "-NEXT:";

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string & raw = lines[i];
        std::string l = trim(raw);
        if (l.size() < 2) continue;
        // Comment-prefix character.  LLVM uses `;`; we also accept `#`
        // for Nix-style fixtures, and `//` for C-comment style.
        char c0 = l[0];
        size_t p = 0;
        if      (c0 == ';' || c0 == '#') p = 1;
        else if (c0 == '/' && l.size() >= 2 && l[1] == '/') p = 2;
        else continue;
        while (p < l.size() && (l[p] == ' ' || l[p] == '\t')) ++p;
        if (p >= l.size()) continue;
        std::string_view rest(l.data() + p, l.size() - p);

        // RUN:/COM: line-skip.  If this line is a RUN: shell-command or
        // a COM: comment, don't parse CHECK on it.  Matches LLVM.
        if (rest.find("RUN:") != std::string::npos
            || rest.find("COM:") != std::string::npos)
            continue;

        auto tryTag = [&](std::string_view tag, CheckDirective::Kind k) -> bool {
            if (rest.size() < tag.size()) return false;
            if (rest.substr(0, tag.size()) != tag) return false;
            std::string_view pat = rest.substr(tag.size());
            std::string p2 = trim(pat);
            out.push_back({k, p2, patternHasRegex(p2), i + 1});
            return true;
        };

        // Order matters: -LABEL / -NOT / -NEXT must be probed BEFORE
        // the bare PREFIX: tag (which is a prefix of all of them).
        if      (tryTag(tagLabel, CheckDirective::Kind::Label)) {}
        else if (tryTag(tagNot,   CheckDirective::Kind::NotMatch)) {}
        else if (tryTag(tagNext,  CheckDirective::Kind::Next)) {}
        else if (tryTag(tagMatch, CheckDirective::Kind::Match)) {}
        // Other comment lines are ignored (commentary allowed).
    }
    return out;
}

/// Render a one-line summary of a directive for diagnostics.
std::string directiveDesc(const CheckDirective & c, size_t idx)
{
    const char * kindStr;
    switch (c.kind) {
        case CheckDirective::Kind::Match:    kindStr = "CHECK";       break;
        case CheckDirective::Kind::NotMatch: kindStr = "CHECK-NOT";   break;
        case CheckDirective::Kind::Label:    kindStr = "CHECK-LABEL"; break;
        case CheckDirective::Kind::Next:     kindStr = "CHECK-NEXT";  break;
    }
    std::ostringstream s;
    s << kindStr << " directive #" << (idx + 1)
      << " (expected line " << c.directiveLine << ")";
    return s.str();
}

/// Format a "no match" diagnostic with context lines from `actualLines`.
std::string formatNoMatch(
    const CheckDirective & c, size_t idx,
    const std::vector<std::string> & actualLines,
    size_t cursor)
{
    std::ostringstream e;
    e << "checkIr: " << directiveDesc(c, idx) << " failed:\n"
      << "  expected to find: '" << c.pattern << "'"
      << (c.hasRegex ? "  (regex)" : "") << "\n"
      << "  in actual lines [" << cursor << ".."
      << actualLines.size() << "):\n";
    size_t ctxFrom = cursor;
    size_t ctxTo = std::min(actualLines.size(), cursor + 16);
    for (size_t a = ctxFrom; a < ctxTo; ++a)
        e << "    [" << a << "] " << actualLines[a] << "\n";
    return e.str();
}

/// Find the line index where directive `c`'s pattern would match
/// searching forward from `from`.  Returns npos if no match.
size_t findFirstMatch(
    const CheckDirective & c,
    const std::vector<std::string> & actualLines,
    size_t from)
{
    for (size_t a = from; a < actualLines.size(); ++a)
        if (lineMatches(c, actualLines[a])) return a;
    return std::string::npos;
}

/// Run the actual matching algorithm.  Shared between `checkIr` (default
/// CHECK prefix) and `checkIrEx` (custom prefix).
std::string runChecks(
    const std::vector<std::string> & actualLines,
    const std::vector<CheckDirective> & checks)
{
    if (checks.empty())
        return "checkIr: no `CHECK` directives found in expected text";

    size_t cursor = 0;  // exclusive — past the most recently matched line.
    // After a CHECK-LABEL hit, subsequent CHECK-NOT may not look back
    // past the label.  Track the LABEL floor.
    size_t labelFloor = 0;
    // For CHECK-NEXT, we need the EXACT line index where the prior
    // positive (CHECK/LABEL/NEXT) matched, so we can require the next
    // directive at `priorMatchLine + 1`.
    size_t priorMatchLine = std::string::npos;

    for (size_t i = 0; i < checks.size(); ++i) {
        const auto & c = checks[i];

        switch (c.kind) {
        case CheckDirective::Kind::Match: {
            size_t hit = findFirstMatch(c, actualLines, cursor);
            if (hit == std::string::npos)
                return formatNoMatch(c, i, actualLines, cursor);
            priorMatchLine = hit;
            cursor = hit + 1;
            break;
        }

        case CheckDirective::Kind::Label: {
            // Strong anchor: scan forward from cursor; on match, RESET
            // cursor.  CHECK-NOT directives positioned BEFORE the label
            // in the directive list cannot scan past the label match
            // (handled below in NotMatch case via `labelFloor`).
            size_t hit = findFirstMatch(c, actualLines, cursor);
            if (hit == std::string::npos)
                return formatNoMatch(c, i, actualLines, cursor);
            priorMatchLine = hit;
            cursor = hit + 1;
            labelFloor = hit + 1;
            break;
        }

        case CheckDirective::Kind::Next: {
            // Must match the line IMMEDIATELY after the prior positive
            // match (cursor == priorMatchLine + 1 at this point if the
            // prior directive was a positive Match/Label).  If
            // priorMatchLine is npos (no prior positive), the directive
            // is malformed.
            if (priorMatchLine == std::string::npos) {
                std::ostringstream e;
                e << "checkIr: " << directiveDesc(c, i)
                  << " has no prior CHECK / CHECK-LABEL — "
                     "CHECK-NEXT requires a preceding positive directive\n";
                return e.str();
            }
            size_t expectLine = priorMatchLine + 1;
            if (expectLine >= actualLines.size()
                || !lineMatches(c, actualLines[expectLine])) {
                std::ostringstream e;
                e << "checkIr: " << directiveDesc(c, i) << " failed:\n"
                  << "  expected to find: '" << c.pattern << "'"
                  << (c.hasRegex ? "  (regex)" : "") << "\n"
                  << "  on line " << expectLine << " (immediately after "
                  << "the previous positive match at line "
                  << priorMatchLine << ")\n"
                  << "  actual line " << expectLine << ": "
                  << (expectLine < actualLines.size()
                      ? actualLines[expectLine] : "<eof>") << "\n";
                return e.str();
            }
            priorMatchLine = expectLine;
            cursor = expectLine + 1;
            break;
        }

        case CheckDirective::Kind::NotMatch: {
            // Forbid pattern from appearing between cursor and the
            // NEXT positive directive's match position (or
            // end-of-actual if none).  Honour labelFloor so we don't
            // scan back past a recent CHECK-LABEL anchor.
            size_t scanEnd = actualLines.size();
            for (size_t j = i + 1; j < checks.size(); ++j) {
                if (checks[j].kind == CheckDirective::Kind::Match
                    || checks[j].kind == CheckDirective::Kind::Label
                    || checks[j].kind == CheckDirective::Kind::Next) {
                    size_t lh = findFirstMatch(checks[j], actualLines, cursor);
                    if (lh != std::string::npos) scanEnd = lh;
                    break;
                }
            }
            size_t scanStart = std::max(cursor, labelFloor);
            for (size_t a = scanStart; a < scanEnd; ++a) {
                if (lineMatches(c, actualLines[a])) {
                    std::ostringstream e;
                    e << "checkIr: " << directiveDesc(c, i) << " failed:\n"
                      << "  forbidden pattern: '" << c.pattern << "'"
                      << (c.hasRegex ? "  (regex)" : "") << "\n"
                      << "  found at actual line " << a << ": "
                      << actualLines[a] << "\n";
                    return e.str();
                }
            }
            break;
        }
        }
    }
    return {};  // success
}

} // namespace

std::string checkIr(std::string_view actual, std::string_view expected)
{
    auto actualLines = splitLines(actual);
    auto checks = parseChecks(expected, "CHECK");
    return runChecks(actualLines, checks);
}

std::string checkIrEx(
    std::string_view actual,
    std::string_view expected,
    std::string_view prefix)
{
    auto actualLines = splitLines(actual);
    auto checks = parseChecks(expected, prefix);
    return runChecks(actualLines, checks);
}

} // namespace nix::v3::ir
