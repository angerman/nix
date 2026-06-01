#pragma once
/// @file
/// v3-native AST — Stage 1.1 of the parser project.
///
/// PARSER_PROJECT_PLAN_2026-06-01.md §2 (Stage 1).  The v3 parser
/// (reused grammar, rewritten actions) emits THIS tree instead of
/// `nix::Expr`, so the parse product is v3-owned and does NOT live in
/// TW's `mem.exprs` arena (the retention eliminated per
/// T4_1_TW_RETENTION_AUDIT_2026-06-01).
///
/// Design contract:
///   * Node hierarchy mirrors `nix::Expr`'s 27 Kind discriminants
///     (nixexpr.hh:109-138) so the action rewrite (Stage 1.4) is a
///     mechanical `state->exprs.add<ExprX>` → `pool.add<X>` swap.
///   * `show()` reproduces `nix::Expr::show()` BYTE-FOR-BYTE
///     (nixexpr.cc:26-262 + the MakeBinOp macro nixexpr.hh:804).
///     This is the validation contract checked by the parser-TI
///     batteries (test/parser-ti/fixtures) + ast-show-test.cc.
///   * Positions are FILE-LOCAL `uint32_t` offsets (the determinism
///     win — NATIVE_PARSER_FEASIBILITY §4.1), NOT TW PosIdx.
///   * Identifier names are stored inline as `std::string` for this
///     first cut; Stage 1.2 (ParserState) interns them into v3's
///     globalSymbolTable.  show() prints the inline string.
///
/// Coverage: ALL 27 nix::Expr Kinds except InheritFrom (a TW-internal
/// pseudo-var that never appears in show() output — the inherit-from
/// SOURCE expr is stored directly in Attrs::inheritFromExprs) and
/// BlackHole (a runtime sentinel, never parsed).  ConcatLists is
/// folded into BinOp("++").
///
/// Header-only + inline show() so the Stage 1.1 unit test compiles
/// standalone — no libnixexprv3 link, no symbol-table dependency.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

namespace nix::v3::ast {

/// Mirrors `nix::Expr::Kind` (nixexpr.hh:109-138).
enum class Kind : uint8_t {
    Unknown = 0,
    Int, Float, String, Path,
    Var, InheritFrom, Select, OpHasAttr,
    Attrs, List, Lambda, Call, Let, With, If, Assert,
    OpNot, OpUpdate, ConcatStrings, Pos, BlackHole,
    OpEq, OpNEq, OpAnd, OpOr, OpImpl, OpConcatLists,
};

/// File-local source position.  0 = unknown.  Content-addressable
/// replacement for TW PosIdx (NATIVE_PARSER_FEASIBILITY §4.1).
using Pos = uint32_t;
constexpr Pos noPos = 0;

struct Node {
    Kind kind = Kind::Unknown;
    Pos  pos  = noPos;
    explicit Node(Kind k, Pos p = noPos) : kind(k), pos(p) {}
    virtual ~Node() = default;
    /// Reproduce nix::Expr::show() byte-for-byte.
    virtual void show(std::ostream & str) const = 0;
};

/// Port of `printLiteralString` (print.cc:32-63): quote + escape a
/// string value exactly as TW does for `--parse` output.
inline void printLiteralString(std::ostream & str, std::string_view s) {
    str << '"';
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '"' || c == '\\') str << '\\' << c;
        else if (c == '\n')        str << "\\n";
        else if (c == '\r')        str << "\\r";
        else if (c == '\t')        str << "\\t";
        else if (c == '$' && i + 1 < s.size() && s[i + 1] == '{') str << "\\$";
        else                       str << c;
    }
    str << '"';
}

/// One component of a select / has-attr path.  Static symbol XOR a
/// dynamic `${expr}` key (mirrors nix::AttrName).
struct AttrName {
    std::string symbol;   // non-empty => static key
    Node * expr = nullptr; // non-null => dynamic ${expr} key
    AttrName(std::string s) : symbol(std::move(s)) {}
    AttrName(Node * e) : expr(e) {}
};

/// Port of `showAttrSelectionPath` (nixexpr.cc:264): dotted path with
/// dynamic keys rendered as `"${e}"`.
inline void showAttrPath(std::ostream & str, const std::vector<AttrName> & path) {
    bool first = true;
    for (auto & a : path) {
        if (!first) str << '.';
        first = false;
        if (a.expr) { str << "\"${"; a.expr->show(str); str << "}\""; }
        else        str << a.symbol;
    }
}

// --- literals -------------------------------------------------------

struct Int : Node {
    int64_t n;
    explicit Int(int64_t n, Pos p = noPos) : Node(Kind::Int, p), n(n) {}
    void show(std::ostream & str) const override { str << n; }
};

struct Float : Node {
    double f;
    explicit Float(double f, Pos p = noPos) : Node(Kind::Float, p), f(f) {}
    // TW: `str << v.fpoint()` — default ostream double formatting
    // (2.0 -> "2", 1e10 -> "1e+10", 1.5 -> "1.5").  C++ default matches.
    void show(std::ostream & str) const override { str << f; }
};

struct String : Node {
    std::string s;
    explicit String(std::string s, Pos p = noPos)
        : Node(Kind::String, p), s(std::move(s)) {}
    void show(std::ostream & str) const override { printLiteralString(str, s); }
};

struct Path : Node {
    std::string p;   // resolved path string view
    explicit Path(std::string p, Pos pp = noPos)
        : Node(Kind::Path, pp), p(std::move(p)) {}
    void show(std::ostream & str) const override { str << p; }
};

// --- variable -------------------------------------------------------

struct Var : Node {
    std::string name;
    explicit Var(std::string name, Pos p = noPos)
        : Node(Kind::Var, p), name(std::move(name)) {}
    void show(std::ostream & str) const override { str << name; }
};

// --- application (ExprCall) -- '(' fun ' ' arg... ')'  nixexpr.cc:190
struct Call : Node {
    Node * fun;
    std::vector<Node *> args;
    Call(Node * fun, std::vector<Node *> args, Pos p = noPos)
        : Node(Kind::Call, p), fun(fun), args(std::move(args)) {}
    void show(std::ostream & str) const override {
        str << '(';
        fun->show(str);
        for (auto * a : args) { str << ' '; a->show(str); }
        str << ')';
    }
};

// --- select (a.b.c or default) -- '(' e ').' path [' or (' def ')']
//   nixexpr.cc:56
struct Select : Node {
    Node * e;
    std::vector<AttrName> path;
    Node * def = nullptr;
    Select(Node * e, std::vector<AttrName> path, Node * def = nullptr,
           Pos p = noPos)
        : Node(Kind::Select, p), e(e), path(std::move(path)), def(def) {}
    // convenience: all-static path
    Select(Node * e, std::vector<std::string> staticPath, Node * def = nullptr,
           Pos p = noPos)
        : Node(Kind::Select, p), e(e), def(def) {
        for (auto & s : staticPath) path.emplace_back(s);
    }
    void show(std::ostream & str) const override {
        str << "(";
        e->show(str);
        str << ").";
        showAttrPath(str, path);
        if (def) { str << " or ("; def->show(str); str << ")"; }
    }
};

// --- has-attr -- '((' e ') ? ' path ')'  nixexpr.cc:68
struct OpHasAttr : Node {
    Node * e;
    std::vector<AttrName> path;
    OpHasAttr(Node * e, std::vector<AttrName> path, Pos p = noPos)
        : Node(Kind::OpHasAttr, p), e(e), path(std::move(path)) {}
    OpHasAttr(Node * e, std::vector<std::string> staticPath, Pos p = noPos)
        : Node(Kind::OpHasAttr, p), e(e) {
        for (auto & s : staticPath) path.emplace_back(s);
    }
    void show(std::ostream & str) const override {
        str << "((";
        e->show(str);
        str << ") ? ";
        showAttrPath(str, path);
        str << ")";
    }
};

// --- lambda --------------------------------------------------------
//   simple:  '(' arg ': ' body ')'                       nixexpr.cc:154
//   formals: '({ a, b ? d, ... }' [' @ ' arg] ': ' body ')'
//   Formals printed in LEXICOGRAPHIC order (nixexpr.cc:163).
struct Formal {
    std::string name;
    Node * def = nullptr;   // `? default`, or null
};
struct Lambda : Node {
    std::string arg;            // simple-arg or @-binding name; "" if none
    bool hasFormals = false;
    std::vector<Formal> formals;
    bool ellipsis = false;
    Node * body;
    Lambda(std::string arg, Node * body, Pos p = noPos)
        : Node(Kind::Lambda, p), arg(std::move(arg)), body(body) {}
    Lambda(std::vector<Formal> formals, bool ellipsis, std::string atArg,
           Node * body, Pos p = noPos)
        : Node(Kind::Lambda, p), arg(std::move(atArg)), hasFormals(true),
          formals(std::move(formals)), ellipsis(ellipsis), body(body) {}
    void show(std::ostream & str) const override {
        str << "(";
        if (hasFormals) {
            str << "{ ";
            std::vector<const Formal *> sorted;
            for (auto & f : formals) sorted.push_back(&f);
            std::sort(sorted.begin(), sorted.end(),
                [](const Formal * a, const Formal * b) { return a->name < b->name; });
            bool first = true;
            for (auto * f : sorted) {
                if (first) first = false; else str << ", ";
                str << f->name;
                if (f->def) { str << " ? "; f->def->show(str); }
            }
            if (ellipsis) { if (!first) str << ", "; str << "..."; }
            str << " }";
            if (!arg.empty()) str << " @ ";
        }
        if (!arg.empty()) str << arg;
        str << ": ";
        body->show(str);
        str << ")";
    }
};

// --- list -- '[ ' '(' e ') ' ... ']'   nixexpr.cc:143
struct List : Node {
    std::vector<Node *> elems;
    explicit List(std::vector<Node *> elems, Pos p = noPos)
        : Node(Kind::List, p), elems(std::move(elems)) {}
    void show(std::ostream & str) const override {
        str << "[ ";
        for (auto * e : elems) { str << "("; e->show(str); str << ") "; }
        str << "]";
    }
};

// --- attribute set (+ showBindings) -- nixexpr.cc:75-141
//   '[rec ]{ ' <inherit group> <inheritFrom groups> <plain> <dynamic> '}'
struct Attrs : Node {
    enum class AttrKind { Plain, Inherited, InheritedFrom };
    struct AttrDef {
        AttrKind kind;
        std::string name;
        Node * value = nullptr;   // Plain only
        int fromIdx = -1;         // InheritedFrom: index into inheritFromExprs
        AttrDef(std::string n, Node * v)
            : kind(AttrKind::Plain), name(std::move(n)), value(v) {}
        AttrDef(std::string n)
            : kind(AttrKind::Inherited), name(std::move(n)) {}
        AttrDef(std::string n, int idx)
            : kind(AttrKind::InheritedFrom), name(std::move(n)), fromIdx(idx) {}
    };
    struct DynamicAttrDef { Node * nameExpr; Node * valueExpr; };

    bool recursive = false;
    std::vector<AttrDef> attrs;
    std::vector<Node *> inheritFromExprs;
    std::vector<DynamicAttrDef> dynamicAttrs;

    Attrs(bool recursive = false, Pos p = noPos)
        : Node(Kind::Attrs, p), recursive(recursive) {}

    void showBindings(std::ostream & str) const {
        // Sort attrs by name (nixexpr.cc:81) — proxy for parse order.
        std::vector<const AttrDef *> sorted;
        for (auto & a : attrs) sorted.push_back(&a);
        std::sort(sorted.begin(), sorted.end(),
            [](const AttrDef * a, const AttrDef * b) { return a->name < b->name; });

        // 1. plain `inherit a b;`
        std::vector<std::string> inherits;
        // 2. `inherit (e) x y;` grouped by source index (map => idx order)
        std::map<int, std::vector<std::string>> inheritsFrom;
        for (auto * a : sorted) {
            switch (a->kind) {
            case AttrKind::Plain: break;
            case AttrKind::Inherited: inherits.push_back(a->name); break;
            case AttrKind::InheritedFrom: inheritsFrom[a->fromIdx].push_back(a->name); break;
            }
        }
        if (!inherits.empty()) {
            str << "inherit";
            for (auto & s : inherits) str << " " << s;
            str << "; ";
        }
        for (auto & [idx, syms] : inheritsFrom) {
            str << "inherit (";
            inheritFromExprs[idx]->show(str);
            str << ")";
            for (auto & s : syms) str << " " << s;
            str << "; ";
        }
        // 3. plain `k = v;` (sorted)
        for (auto * a : sorted) {
            if (a->kind == AttrKind::Plain) {
                str << a->name << " = ";
                a->value->show(str);
                str << "; ";
            }
        }
        // 4. dynamic `"${e}" = v;`
        for (auto & d : dynamicAttrs) {
            str << "\"${";
            d.nameExpr->show(str);
            str << "}\" = ";
            d.valueExpr->show(str);
            str << "; ";
        }
    }
    void show(std::ostream & str) const override {
        if (recursive) str << "rec ";
        str << "{ ";
        showBindings(str);
        str << "}";
    }
};

// --- let -- '(let ' <bindings> 'in ' body ')'   nixexpr.cc:201
struct Let : Node {
    Attrs * attrs;
    Node * body;
    Let(Attrs * attrs, Node * body, Pos p = noPos)
        : Node(Kind::Let, p), attrs(attrs), body(body) {}
    void show(std::ostream & str) const override {
        str << "(let ";
        attrs->showBindings(str);
        str << "in ";
        body->show(str);
        str << ")";
    }
};

// --- with -- '(with ' attrs '; ' body ')'   nixexpr.cc:210
struct With : Node {
    Node * attrs;
    Node * body;
    With(Node * attrs, Node * body, Pos p = noPos)
        : Node(Kind::With, p), attrs(attrs), body(body) {}
    void show(std::ostream & str) const override {
        str << "(with ";
        attrs->show(str);
        str << "; ";
        body->show(str);
        str << ")";
    }
};

// --- if -- '(if ' c ' then ' t ' else ' e ')'   nixexpr.cc:219
struct If : Node {
    Node * cond; Node * then_; Node * else_;
    If(Node * cond, Node * then_, Node * else_, Pos p = noPos)
        : Node(Kind::If, p), cond(cond), then_(then_), else_(else_) {}
    void show(std::ostream & str) const override {
        str << "(if ";
        cond->show(str);
        str << " then ";
        then_->show(str);
        str << " else ";
        else_->show(str);
        str << ")";
    }
};

// --- assert -- 'assert ' c '; ' body   (NO outer parens)  nixexpr.cc:230
struct Assert : Node {
    Node * cond; Node * body;
    Assert(Node * cond, Node * body, Pos p = noPos)
        : Node(Kind::Assert, p), cond(cond), body(body) {}
    void show(std::ostream & str) const override {
        str << "assert ";
        cond->show(str);
        str << "; ";
        body->show(str);
    }
};

// --- prefix ! -- '(! ' e ')'   nixexpr.cc:238
struct OpNot : Node {
    Node * e;
    explicit OpNot(Node * e, Pos p = noPos) : Node(Kind::OpNot, p), e(e) {}
    void show(std::ostream & str) const override {
        str << "(! "; e->show(str); str << ")";
    }
};

// --- __curPos -- 'curPos'   nixexpr.cc:259
struct PosExpr : Node {
    explicit PosExpr(Pos p = noPos) : Node(Kind::Pos, p) {}
    void show(std::ostream & str) const override { str << "__curPos"; }
};

// --- ConcatStrings (`+`) -- '(' e1 ' + ' e2 ' + ' ... ')'  nixexpr.cc:245
struct ConcatStrings : Node {
    std::vector<Node *> es;
    explicit ConcatStrings(std::vector<Node *> es, Pos p = noPos)
        : Node(Kind::ConcatStrings, p), es(std::move(es)) {}
    void show(std::ostream & str) const override {
        str << "(";
        bool first = true;
        for (auto * e : es) { if (first) first = false; else str << " + "; e->show(str); }
        str << ")";
    }
};

// --- binary operators (MakeBinOp, nixexpr.hh:804) ------------------
//   '(' e1 ' ' OP ' ' e2 ')'  for ==, !=, &&, ||, ->, //, ++
struct BinOp : Node {
    const char * op;
    Node * lhs; Node * rhs;
    BinOp(Kind k, const char * op, Node * lhs, Node * rhs, Pos p = noPos)
        : Node(k, p), op(op), lhs(lhs), rhs(rhs) {}
    void show(std::ostream & str) const override {
        str << "(";
        lhs->show(str);
        str << " " << op << " ";
        rhs->show(str);
        str << ")";
    }
};

/// Owns all AST nodes for one parse; freed wholesale (the v3-owned
/// arena that replaces TW's mem.exprs).  `std::deque` gives stable
/// node addresses.  Stage 1.4 wires the parser actions to `add<>`.
struct Pool {
    std::deque<std::unique_ptr<Node>> nodes;
    template <typename T, typename... Args>
    T * add(Args &&... args) {
        auto p = std::make_unique<T>(std::forward<Args>(args)...);
        T * raw = p.get();
        nodes.push_back(std::move(p));
        return raw;
    }
};

/// Render a node to a string (the `e->show()` half of
/// `nix-instantiate --parse`; caller adds the trailing newline).
inline std::string showToString(const Node * n) {
    std::ostringstream oss;
    n->show(oss);
    return oss.str();
}

} // namespace nix::v3::ast
