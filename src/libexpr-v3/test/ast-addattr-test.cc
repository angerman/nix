/// @file
/// Stage 1.2 unit test — v3 ParserState::addAttr attrset-merge.
///
/// PARSER_PROJECT_PLAN_2026-06-01.md §2 (Stage 1.2).  Drives
/// `ParserState::addAttr` with the (attrpath, value) sequences the
/// parser actions produce, then asserts the resulting Attrs show()
/// matches TW `nix-instantiate --parse` BYTE-FOR-BYTE — covering the
/// attrset-merge construction (`{ a.b = 1; a.c = 2; }` → nested) and
/// duplicate-definition detection.
///
/// Standalone: includes only the header-only ParserState + AST.
/// Build: `c++ -std=c++23 -I include -I parser test/ast-addattr-test.cc`.
///
/// Per [[falsification-rule]]: kills "v3 addAttr can't reproduce TW's
/// attrset-merge + dup-detection semantics".
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "parser-state.hh"

#include <cstdio>
#include <string>

using namespace nix::v3::ast;

static int failures = 0, checks = 0;

static void checkShow(const char * name, const Node * n, const std::string & golden)
{
    ++checks;
    std::string got = showToString(n);
    if (got == golden) std::printf("  ok   %-22s %s\n", name, got.c_str());
    else { ++failures; std::printf("  FAIL %-22s\n    golden: %s\n    got:    %s\n",
                                   name, golden.c_str(), got.c_str()); }
}

static void checkThrows(const char * name, void (*fn)(), const char * wantSubstr)
{
    ++checks;
    try { fn(); std::printf("  FAIL %-22s (expected throw)\n", name); ++failures; }
    catch (const ParseError & e) {
        std::string w = e.what();
        if (w.find(wantSubstr) != std::string::npos)
            std::printf("  ok   %-22s throws: %s\n", name, w.c_str());
        else { ++failures; std::printf("  FAIL %-22s wrong msg: %s (want substr '%s')\n",
                                       name, w.c_str(), wantSubstr); }
    }
}

// path helpers
static std::vector<AttrName> P(std::initializer_list<const char *> syms) {
    std::vector<AttrName> p;
    for (auto * s : syms) p.emplace_back(std::string(s));
    return p;
}

// dup-detection negative tests need their own state; use file-scope fns.
static void dupLeaf() {
    ParserState st;
    auto * a = st.add<Attrs>(false);
    st.addAttr(a, P({"a", "b"}), st.add<Int>(1), 0);
    st.addAttr(a, P({"a", "b"}), st.add<Int>(2), 0);   // dup a.b
}
static void dupPlain() {
    ParserState st;
    auto * a = st.add<Attrs>(false);
    st.addAttr(a, P({"a"}), st.add<Int>(1), 0);
    st.addAttr(a, P({"a"}), st.add<Int>(2), 0);        // dup a
}

int main()
{
    std::printf("=== Stage 1.2 v3 addAttr vs TW --parse goldens ===\n");

    // simple:  { b = 1; a = 2; }  ->  { a = 2; b = 1; }
    {
        ParserState st; auto * a = st.add<Attrs>(false);
        st.addAttr(a, P({"b"}), st.add<Int>(1), 0);
        st.addAttr(a, P({"a"}), st.add<Int>(2), 0);
        checkShow("simple", a, "{ a = 2; b = 1; }");
    }

    // nested-path:  { a.b.c = 1; }  ->  { a = { b = { c = 1; }; }; }
    {
        ParserState st; auto * a = st.add<Attrs>(false);
        st.addAttr(a, P({"a", "b", "c"}), st.add<Int>(1), 0);
        checkShow("nested-path", a, "{ a = { b = { c = 1; }; }; }");
    }

    // merge-two-paths:  { a.b = 1; a.c = 2; }  ->  { a = { b = 1; c = 2; }; }
    {
        ParserState st; auto * a = st.add<Attrs>(false);
        st.addAttr(a, P({"a", "b"}), st.add<Int>(1), 0);
        st.addAttr(a, P({"a", "c"}), st.add<Int>(2), 0);
        checkShow("merge-two-paths", a, "{ a = { b = 1; c = 2; }; }");
    }

    // merge-into-set:  { a = { b = 1; }; a.c = 2; }  ->  { a = { b = 1; c = 2; }; }
    {
        ParserState st; auto * a = st.add<Attrs>(false);
        auto * inner = st.add<Attrs>(false);
        st.addAttr(inner, P({"b"}), st.add<Int>(1), 0);
        st.addAttr(a, P({"a"}), inner, 0);
        st.addAttr(a, P({"a", "c"}), st.add<Int>(2), 0);
        checkShow("merge-into-set", a, "{ a = { b = 1; c = 2; }; }");
    }

    // deep-merge:  { a.b.c = 1; a.b.d = 2; }  ->  { a = { b = { c = 1; d = 2; }; }; }
    {
        ParserState st; auto * a = st.add<Attrs>(false);
        st.addAttr(a, P({"a", "b", "c"}), st.add<Int>(1), 0);
        st.addAttr(a, P({"a", "b", "d"}), st.add<Int>(2), 0);
        checkShow("deep-merge", a, "{ a = { b = { c = 1; d = 2; }; }; }");
    }

    // merge-two-sets:  { a = {b=1;}; a = {c=2;}; }  ->  { a = { b = 1; c = 2; }; }
    {
        ParserState st; auto * a = st.add<Attrs>(false);
        auto * s1 = st.add<Attrs>(false);
        st.addAttr(s1, P({"b"}), st.add<Int>(1), 0);
        auto * s2 = st.add<Attrs>(false);
        st.addAttr(s2, P({"c"}), st.add<Int>(2), 0);
        st.addAttr(a, P({"a"}), s1, 0);
        st.addAttr(a, P({"a"}), s2, 0);   // merges s2 into s1
        checkShow("merge-two-sets", a, "{ a = { b = 1; c = 2; }; }");
    }

    // merge-rec-inner:  { a.b = 1; a = {c=2;}; }  ->  { a = { b = 1; c = 2; }; }
    {
        ParserState st; auto * a = st.add<Attrs>(false);
        st.addAttr(a, P({"a", "b"}), st.add<Int>(1), 0);
        auto * s2 = st.add<Attrs>(false);
        st.addAttr(s2, P({"c"}), st.add<Int>(2), 0);
        st.addAttr(a, P({"a"}), s2, 0);
        checkShow("merge-rec-inner", a, "{ a = { b = 1; c = 2; }; }");
    }

    // dynamic-path:  { a.${k} = 1; }  ->  { a = { "${k}" = 1; }; }
    {
        ParserState st; auto * a = st.add<Attrs>(false);
        std::vector<AttrName> path;
        path.emplace_back(std::string("a"));
        path.emplace_back(st.add<Var>(std::string("k")));   // dynamic ${k}
        st.addAttr(a, path, st.add<Int>(1), 0);
        checkShow("dynamic-path", a, "{ a = { \"${k}\" = 1; }; }");
    }

    // --- duplicate-definition detection (negative) ---
    checkThrows("dup-leaf",  dupLeaf,  "attribute 'a.b' already defined");
    checkThrows("dup-plain", dupPlain, "attribute 'a' already defined");

    std::printf("\n=== %d/%d checks passed ===\n", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
