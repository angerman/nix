/// @file
/// Stage 1.1 unit test — v3 AST show() byte-equality with TW.
///
/// PARSER_PROJECT_PLAN_2026-06-01.md §2 (Stage 1.1).  Constructs v3
/// AST nodes by hand and asserts `show()` reproduces the goldens from
/// the parser-TI precedence battery
/// (test/parser-ti/fixtures/precedence/*.exp) BYTE-FOR-BYTE.
///
/// This validates the show() contract in ISOLATION — no parser, no
/// lowering, no symbol table.  When the v3-native parser (Stage 1.4)
/// emits these same node shapes, `v3-eval --parse` output is
/// guaranteed to match the battery goldens because show() is proven
/// here.
///
/// Standalone: includes only the header-only AST; links nothing.
/// Build: `c++ -std=c++23 -I include test/ast-show-test.cc`.
///
/// Per [[falsification-rule]]: kills "v3 AST cannot reproduce TW's
/// show() format" — each PASS is a node-shape whose rendering is
/// locked to the committed golden.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/ast/expr.hh"

#include <cstdio>
#include <string>

using namespace nix::v3::ast;

static int failures = 0;
static int checks = 0;

static void check(const char * name, const Node * n, const std::string & golden)
{
    ++checks;
    std::string got = showToString(n);
    if (got == golden) {
        std::printf("  ok   %-22s %s\n", name, got.c_str());
    } else {
        ++failures;
        std::printf("  FAIL %-22s\n    golden: %s\n    got:    %s\n",
                    name, golden.c_str(), got.c_str());
    }
}

// Convenience builders over a single Pool.
int main()
{
    Pool m;
    auto V = [&](const char * s) -> Node * { return m.add<Var>(std::string(s)); };
    auto L = [&](const char * arg, Node * body) -> Node * {
        return m.add<Lambda>(std::string(arg), body);
    };

    std::printf("=== Stage 1.1 v3 AST show() vs TW precedence goldens ===\n");

    // add-vs-mul:  a: b: c: a + b * c
    //   golden: (a: (b: (c: (a + (__mul b c)))))
    check("add-vs-mul",
        L("a", L("b", L("c",
            m.add<ConcatStrings>(std::vector<Node *>{
                V("a"),
                m.add<Call>(V("__mul"), std::vector<Node *>{V("b"), V("c")})
            })))),
        "(a: (b: (c: (a + (__mul b c)))))");

    // app-vs-add:  f: a: b: f a + b
    //   golden: (f: (a: (b: ((f a) + b))))
    check("app-vs-add",
        L("f", L("a", L("b",
            m.add<ConcatStrings>(std::vector<Node *>{
                m.add<Call>(V("f"), std::vector<Node *>{V("a")}),
                V("b")
            })))),
        "(f: (a: (b: ((f a) + b))))");

    // app-left:  f: a: b: f a b
    //   golden: (f: (a: (b: (f a b))))
    check("app-left",
        L("f", L("a", L("b",
            m.add<Call>(V("f"), std::vector<Node *>{V("a"), V("b")})))),
        "(f: (a: (b: (f a b))))");

    // not-vs-add:  a: b: !a + b
    //   golden: (a: (b: (! (a + b))))
    check("not-vs-add",
        L("a", L("b",
            m.add<OpNot>(
                m.add<ConcatStrings>(std::vector<Node *>{V("a"), V("b")})))),
        "(a: (b: (! (a + b))))");

    // rel-lt:  a: b: a < b   (desugars to __lessThan a b)
    //   golden: (a: (b: (__lessThan a b)))
    check("rel-lt",
        L("a", L("b",
            m.add<Call>(V("__lessThan"), std::vector<Node *>{V("a"), V("b")}))),
        "(a: (b: (__lessThan a b)))");

    // rel-gt:  a: b: a > b   (desugars to __lessThan b a — swapped)
    //   golden: (a: (b: (__lessThan b a)))
    check("rel-gt",
        L("a", L("b",
            m.add<Call>(V("__lessThan"), std::vector<Node *>{V("b"), V("a")}))),
        "(a: (b: (__lessThan b a)))");

    // rel-geq:  a: b: a >= b   (desugars to ! (__lessThan a b))
    //   golden: (a: (b: (! (__lessThan a b))))
    check("rel-geq",
        L("a", L("b",
            m.add<OpNot>(
                m.add<Call>(V("__lessThan"), std::vector<Node *>{V("a"), V("b")})))),
        "(a: (b: (! (__lessThan a b))))");

    // negate-vs-add:  a: b: -a + b   (-a desugars to __sub 0 a)
    //   golden: (a: (b: ((__sub 0 a) + b)))
    check("negate-vs-add",
        L("a", L("b",
            m.add<ConcatStrings>(std::vector<Node *>{
                m.add<Call>(V("__sub"), std::vector<Node *>{m.add<Int>(0), V("a")}),
                V("b")
            }))),
        "(a: (b: ((__sub 0 a) + b)))");

    // impl-right:  a: b: c: a -> b -> c
    //   golden: (a: (b: (c: (a -> (b -> c)))))
    check("impl-right",
        L("a", L("b", L("c",
            m.add<BinOp>(Kind::OpImpl, "->", V("a"),
                m.add<BinOp>(Kind::OpImpl, "->", V("b"), V("c")))))),
        "(a: (b: (c: (a -> (b -> c)))))");

    // or-vs-and:  a: b: c: a || b && c
    //   golden: (a: (b: (c: (a || (b && c)))))
    check("or-vs-and",
        L("a", L("b", L("c",
            m.add<BinOp>(Kind::OpOr, "||", V("a"),
                m.add<BinOp>(Kind::OpAnd, "&&", V("b"), V("c")))))),
        "(a: (b: (c: (a || (b && c)))))");

    // update-right:  a: b: c: a // b // c
    //   golden: (a: (b: (c: (a // (b // c)))))
    check("update-right",
        L("a", L("b", L("c",
            m.add<BinOp>(Kind::OpUpdate, "//", V("a"),
                m.add<BinOp>(Kind::OpUpdate, "//", V("b"), V("c")))))),
        "(a: (b: (c: (a // (b // c)))))");

    // concat-right:  a: b: c: a ++ b ++ c
    //   golden: (a: (b: (c: (a ++ (b ++ c)))))
    check("concat-right",
        L("a", L("b", L("c",
            m.add<BinOp>(Kind::OpConcatLists, "++", V("a"),
                m.add<BinOp>(Kind::OpConcatLists, "++", V("b"), V("c")))))),
        "(a: (b: (c: (a ++ (b ++ c)))))");

    // select-or-default:  a: b: a.b or b
    //   golden: (a: (b: (a).b or (b)))
    check("select-or-default",
        L("a", L("b",
            m.add<Select>(V("a"), std::vector<std::string>{"b"}, V("b")))),
        "(a: (b: (a).b or (b)))");

    // select-chain:  a: a.b.c
    //   golden: (a: (a).b.c)
    check("select-chain",
        L("a", m.add<Select>(V("a"), std::vector<std::string>{"b", "c"})),
        "(a: (a).b.c)");

    // hasattr-path:  a: a ? x.y
    //   golden: (a: ((a) ? x.y))
    check("hasattr-path",
        L("a", m.add<OpHasAttr>(V("a"), std::vector<std::string>{"x", "y"})),
        "(a: ((a) ? x.y))");

    // negate-vs-select:  a: -a.b   (select binds tighter than negate)
    //   golden: (a: (__sub 0 (a).b))
    check("negate-vs-select",
        L("a",
            m.add<Call>(V("__sub"), std::vector<Node *>{
                m.add<Int>(0),
                m.add<Select>(V("a"), std::vector<std::string>{"b"})
            })),
        "(a: (__sub 0 (a).b))");

    // ---- Stage 1.1 batch 2: remaining node kinds ----

    // float-1.5:  1.5  ->  1.5
    check("float-1.5", m.add<Float>(1.5), "1.5");
    // float-2.0:  2.0  ->  2   (default double formatting)
    check("float-2.0", m.add<Float>(2.0), "2");

    // string-simple:  "hello"  ->  "hello"
    check("string-simple", m.add<String>(std::string("hello")), "\"hello\"");
    // string-escape:  value a"b\c  ->  "a\"b\\c"
    check("string-escape", m.add<String>(std::string("a\"b\\c")),
        "\"a\\\"b\\\\c\"");

    // list:  a: b: [ a b 1 ]  ->  (a: (b: [ (a) (b) (1) ]))
    check("list",
        L("a", L("b",
            m.add<List>(std::vector<Node *>{V("a"), V("b"), m.add<Int>(1)}))),
        "(a: (b: [ (a) (b) (1) ]))");

    // if:  a: b: c: if a then b else c  ->  (a: (b: (c: (if a then b else c))))
    check("if",
        L("a", L("b", L("c",
            m.add<If>(V("a"), V("b"), V("c"))))),
        "(a: (b: (c: (if a then b else c))))");

    // with:  a: b: with a; b  ->  (a: (b: (with a; b)))
    check("with",
        L("a", L("b", m.add<With>(V("a"), V("b")))),
        "(a: (b: (with a; b)))");

    // assert:  a: b: assert a; b  ->  (a: (b: assert a; b))   [no inner parens]
    check("assert",
        L("a", L("b", m.add<Assert>(V("a"), V("b")))),
        "(a: (b: assert a; b))");

    // let:  let x = 1; y = 2; in x  ->  (let x = 1; y = 2; in x)
    {
        auto * at = m.add<Attrs>(false);
        at->attrs.emplace_back(std::string("x"), m.add<Int>(1));
        at->attrs.emplace_back(std::string("y"), m.add<Int>(2));
        check("let", m.add<Let>(at, V("x")), "(let x = 1; y = 2; in x)");
    }

    // attrs-plain (sorted):  { b = 1; a = 2; }  ->  { a = 2; b = 1; }
    {
        auto * at = m.add<Attrs>(false);
        at->attrs.emplace_back(std::string("b"), m.add<Int>(1));
        at->attrs.emplace_back(std::string("a"), m.add<Int>(2));
        check("attrs-plain", at, "{ a = 2; b = 1; }");
    }

    // attrs-rec:  rec { a = 1; b = a; }
    {
        auto * at = m.add<Attrs>(true);
        at->attrs.emplace_back(std::string("a"), m.add<Int>(1));
        at->attrs.emplace_back(std::string("b"), V("a"));
        check("attrs-rec", at, "rec { a = 1; b = a; }");
    }

    // attrs-empty:  { }
    check("attrs-empty", m.add<Attrs>(false), "{ }");

    // attrs-inherit:  a: { inherit a; }  ->  (a: { inherit a; })
    {
        auto * at = m.add<Attrs>(false);
        at->attrs.emplace_back(std::string("a"));   // Inherited
        check("attrs-inherit", L("a", at), "(a: { inherit a; })");
    }

    // attrs-inheritfrom:  a: { inherit (a) x y; }  ->  (a: { inherit (a) x y; })
    {
        auto * at = m.add<Attrs>(false);
        at->inheritFromExprs.push_back(V("a"));
        int idx0 = 0;
        at->attrs.emplace_back(std::string("x"), idx0);   // InheritedFrom src 0
        at->attrs.emplace_back(std::string("y"), idx0);
        check("attrs-inheritfrom", L("a", at), "(a: { inherit (a) x y; })");
    }

    // attrs-dynamic:  a: { ${a} = 1; }  ->  (a: { "${a}" = 1; })
    {
        auto * at = m.add<Attrs>(false);
        at->dynamicAttrs.push_back({V("a"), m.add<Int>(1)});
        check("attrs-dynamic", L("a", at), "(a: { \"${a}\" = 1; })");
    }

    // select-dynamic:  a: b: a.${b}  ->  (a: (b: (a)."${b}"))
    check("select-dynamic",
        L("a", L("b",
            m.add<Select>(V("a"), std::vector<AttrName>{ AttrName(V("b")) }))),
        "(a: (b: (a).\"${b}\"))");

    std::printf("\n=== %d/%d checks passed ===\n", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
