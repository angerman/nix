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
///     precedence battery (test/parser-ti/fixtures/precedence).
///   * Positions are FILE-LOCAL `uint32_t` offsets (the determinism
///     win — NATIVE_PARSER_FEASIBILITY §4.1), NOT TW PosIdx.
///   * Identifier names are stored inline as `std::string` for this
///     first cut; Stage 1.2 (ParserState) interns them into v3's
///     globalSymbolTable.  show() prints the inline string.
///
/// THIS FIRST CUT covers the OPERATOR CORE + literals + lambda/call/
/// select/hasattr exercised by the precedence battery.  Remaining
/// kinds (Float/String/Path/Attrs/List/Let/With/If/Assert/Inherit/
/// Pos/ConcatLists) are queued for Stage 1.1 follow-ups; their
/// show() formats are documented in parser/README.md.
///
/// Header-only + inline show() so the Stage 1.1 unit test
/// (test/ast-show-test.cc) compiles standalone — no libnixexprv3
/// link, no symbol-table dependency.  This keeps the architectural
/// core unit-testable in isolation per the falsification rule.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

namespace nix::v3::ast {

/// Mirrors `nix::Expr::Kind` (nixexpr.hh:109-138).  Stage 1.1 first
/// cut implements the subset marked (✓); the rest are declared so the
/// enum is stable across follow-ups.
enum class Kind : uint8_t {
    Unknown = 0,
    Int,            // ✓
    Float,          // (queued)
    String,         // (queued)
    Path,           // (queued)
    Var,            // ✓
    InheritFrom,    // (queued)
    Select,         // ✓
    OpHasAttr,      // ✓
    Attrs,          // (queued)
    List,           // (queued)
    Lambda,         // ✓
    Call,           // ✓
    Let,            // (queued)
    With,           // (queued)
    If,             // (queued)
    Assert,         // (queued)
    OpNot,          // ✓
    OpUpdate,       // ✓ (binary)
    ConcatStrings,  // ✓
    Pos,            // (queued)
    BlackHole,      // (queued)
    OpEq,           // ✓ (binary)
    OpNEq,          // ✓ (binary)
    OpAnd,          // ✓ (binary)
    OpOr,           // ✓ (binary)
    OpImpl,         // ✓ (binary)
    OpConcatLists,  // ✓ (binary)
};

/// File-local source position.  0 = unknown.  This is the
/// content-addressable replacement for TW PosIdx — a byte offset
/// (or line/col) relative to the file start, stable across import
/// orderings (NATIVE_PARSER_FEASIBILITY §4.1).
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

// --- literals -------------------------------------------------------

struct Int : Node {
    int64_t n;
    explicit Int(int64_t n, Pos p = noPos) : Node(Kind::Int, p), n(n) {}
    void show(std::ostream & str) const override { str << n; }
};

// --- variable -------------------------------------------------------

struct Var : Node {
    std::string name;
    explicit Var(std::string name, Pos p = noPos)
        : Node(Kind::Var, p), name(std::move(name)) {}
    void show(std::ostream & str) const override { str << name; }
};

// --- application (ExprCall) ----------------------------------------
//   show: '(' fun ' ' arg1 ' ' arg2 ... ')'   (nixexpr.cc:190)
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

// --- select (a.b.c or default) -------------------------------------
//   show: '(' e ').' path [ ' or (' def ')' ]   (nixexpr.cc:56)
//   First-cut: static keys only (the precedence battery uses no
//   dynamic ${} keys in select paths).
struct Select : Node {
    Node * e;
    std::vector<std::string> path;   // static attr names
    Node * def = nullptr;            // `or` default, or null
    Select(Node * e, std::vector<std::string> path, Node * def = nullptr,
           Pos p = noPos)
        : Node(Kind::Select, p), e(e), path(std::move(path)), def(def) {}
    void show(std::ostream & str) const override {
        str << "(";
        e->show(str);
        str << ").";
        bool first = true;
        for (auto & k : path) { if (!first) str << '.'; first = false; str << k; }
        if (def) { str << " or ("; def->show(str); str << ")"; }
    }
};

// --- has-attr (e ? path) -------------------------------------------
//   show: '((' e ') ? ' path ')'   (nixexpr.cc:68)
struct OpHasAttr : Node {
    Node * e;
    std::vector<std::string> path;
    OpHasAttr(Node * e, std::vector<std::string> path, Pos p = noPos)
        : Node(Kind::OpHasAttr, p), e(e), path(std::move(path)) {}
    void show(std::ostream & str) const override {
        str << "((";
        e->show(str);
        str << ") ? ";
        bool first = true;
        for (auto & k : path) { if (!first) str << '.'; first = false; str << k; }
        str << ")";
    }
};

// --- lambda --------------------------------------------------------
//   simple:  '(' arg ': ' body ')'                       (nixexpr.cc:154)
//   formals: '({ a, b ? d, ... }' [' @ ' arg] ': ' body ')'
//   Formals are printed in LEXICOGRAPHIC order (nixexpr.cc:163).
struct Formal {
    std::string name;
    Node * def = nullptr;   // `? default`, or null
};
struct Lambda : Node {
    std::string arg;            // simple-arg name, or @-binding name; "" if none
    bool hasFormals = false;
    std::vector<Formal> formals;
    bool ellipsis = false;
    Node * body;
    // simple lambda: arg + body
    Lambda(std::string arg, Node * body, Pos p = noPos)
        : Node(Kind::Lambda, p), arg(std::move(arg)), body(body) {}
    // formals lambda
    Lambda(std::vector<Formal> formals, bool ellipsis, std::string atArg,
           Node * body, Pos p = noPos)
        : Node(Kind::Lambda, p), arg(std::move(atArg)), hasFormals(true),
          formals(std::move(formals)), ellipsis(ellipsis), body(body) {}
    void show(std::ostream & str) const override {
        str << "(";
        if (hasFormals) {
            str << "{ ";
            // lexicographic order by name (nixexpr.cc:163).
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

// --- prefix ! (ExprOpNot) ------------------------------------------
//   show: '(! ' e ')'   (nixexpr.cc:238)
struct OpNot : Node {
    Node * e;
    explicit OpNot(Node * e, Pos p = noPos) : Node(Kind::OpNot, p), e(e) {}
    void show(std::ostream & str) const override {
        str << "(! "; e->show(str); str << ")";
    }
};

// --- ConcatStrings (the `+` operator + string interpolation) -------
//   show: '(' e1 ' + ' e2 ' + ' ... ')'   (nixexpr.cc:245)
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
//   show: '(' e1 ' ' OP ' ' e2 ')'
//   Covers ==, !=, &&, ||, ->, //, ++.
struct BinOp : Node {
    const char * op;   // "==", "!=", "&&", "||", "->", "//", "++"
    Node * lhs;
    Node * rhs;
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
/// node addresses without per-node heap churn beyond the deque's
/// block allocation.  Stage 1.4 wires the parser actions to `add<>`.
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

/// Render a node to a string, matching `nix-instantiate --parse`
/// (which prints `e->show()` + trailing newline — the newline is the
/// caller's responsibility, as in v3-eval --parse).
inline std::string showToString(const Node * n) {
    std::ostringstream oss;
    n->show(oss);
    return oss.str();
}

} // namespace nix::v3::ast
