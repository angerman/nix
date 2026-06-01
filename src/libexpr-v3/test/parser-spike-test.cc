/// @file
/// Stage 1.3/1.4 parser test — bison/flex → v3 AST end-to-end.
///
/// PARSER_PROJECT_PLAN_2026-06-01.md §2.  Drives the v3 bison/flex
/// parser (parser/v3-spike.{y,l}) and asserts the resulting v3 AST
/// `show()`s byte-equal to `nix-instantiate --parse`.
///
/// Two layers:
///   1. hardcoded arithmetic sanity (Stage 1.3 toolchain proof);
///   2. if argv[1] is a fixtures dir, sweep every *.nix in it and
///      byte-compare show() output vs the committed *.exp golden
///      (Stage 1.4 validation against the operator-precedence battery,
///      test/parser-ti/fixtures/precedence — 49 fixtures).
///
/// Per [[falsification-rule]]: kills "the v3 parser can't reproduce
/// TW's AST for the operator/lambda/select/app/has-attr core".
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/ast/expr.hh"
#include "parser-state.hh"
#include "v3-spike-decls.hh"  // v3-spike-tab.hh + YYSTYPE
#include "v3-spike-lex.hh"    // flex reentrant decls (needs YYSTYPE)

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace nix::v3::ast;

static int failures = 0, checks = 0;

/// Parse `text` through the v3 bison/flex parser into v3 AST.  The
/// ParserState owns the AST (Pool); caller shows it while alive.
static Node * v3parse(ParserState & st, const std::string & text) {
    yyscan_t scanner;
    yylex_init(&scanner);
    // yy_scan_string copies; the buffer is freed by yy_delete_buffer.
    YY_BUFFER_STATE buf = yy_scan_string(text.c_str(), scanner);
    nix::v3::spike::SpikeParser parser(scanner, &st);
    parser.parse();
    yy_delete_buffer(buf, scanner);
    yylex_destroy(scanner);
    return st.result;
}

static void check(const std::string & src, const std::string & golden) {
    ++checks;
    ParserState st;
    std::string got;
    try {
        got = showToString(v3parse(st, src));
    } catch (const std::exception & e) {
        ++failures;
        std::printf("  FAIL %-20s threw: %s\n", src.c_str(), e.what());
        return;
    }
    if (got == golden) std::printf("  ok   %-20s %s\n", src.c_str(), got.c_str());
    else { ++failures; std::printf("  FAIL %-20s\n    golden: %s\n    got:    %s\n",
                                   src.c_str(), golden.c_str(), got.c_str()); }
}

static std::string slurp(const std::filesystem::path & p) {
    std::ifstream f(p);
    std::ostringstream ss; ss << f.rdbuf();
    std::string s = ss.str();
    // trim trailing newline(s) so file content + golden compare cleanly
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

int main(int argc, char ** argv) {
    std::printf("=== bison/flex spike → v3 AST: arithmetic sanity ===\n");
    check("1 + 2",        "(1 + 2)");
    check("1 + 2 + 3",    "((1 + 2) + 3)");
    check("2 * 3",        "(__mul 2 3)");
    check("1 + 2 * 3",    "(1 + (__mul 2 3))");
    check("(1 + 2) * 3",  "(__mul (1 + 2) 3)");

    // Stage 1.4: sweep each fixture dir given (precedence battery,
    // tier3 battery, ...): parse every .nix via the v3 parser and
    // byte-compare show() to the committed .exp golden.
    for (int a = 1; a < argc; ++a) {
        std::filesystem::path dir(argv[a]);
        std::printf("\n=== fixture sweep (%s) ===\n", argv[a]);
        std::vector<std::filesystem::path> nixFiles;
        if (std::filesystem::is_directory(dir))
            for (auto & e : std::filesystem::directory_iterator(dir))
                if (e.path().extension() == ".nix") nixFiles.push_back(e.path());
        std::sort(nixFiles.begin(), nixFiles.end());
        for (auto & nf : nixFiles) {
            auto exp = nf; exp.replace_extension(".exp");
            if (!std::filesystem::exists(exp)) continue;
            check(slurp(nf), slurp(exp));
        }
    }

    std::printf("\n=== %d/%d checks passed ===\n", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
