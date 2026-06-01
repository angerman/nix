/// @file
/// Stage 1.3 toolchain spike test — bison/flex → v3 AST end-to-end.
///
/// PARSER_PROJECT_PLAN_2026-06-01.md §2 (Stage 1.3).  Drives the
/// minimal bison/flex parser (parser/v3-spike.{y,l}) and asserts the
/// resulting v3 AST `show()`s byte-equal to `nix-instantiate --parse`
/// for arithmetic expressions — proving the WHOLE toolchain path:
/// bison + flex in the devshell → meson custom_target → generated C++
/// compiles → flex/bison glue links → parser runs → v3 AST out.
///
/// Once this is green, Stage 1.4 grows the grammar from this seed
/// (swap in parser.y's full productions + rewritten actions).
///
/// Per [[falsification-rule]]: kills "bison/flex can't be wired into
/// the v3 build to produce a runnable v3-AST-emitting parser".
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/ast/expr.hh"
#include "parser-state.hh"
#include "v3-spike-decls.hh"  // v3-spike-tab.hh + YYSTYPE
#include "v3-spike-lex.hh"    // flex reentrant decls (needs YYSTYPE)

#include <cstdio>
#include <string>

using namespace nix::v3::ast;

static int failures = 0, checks = 0;

/// Parse `text` through the spike bison/flex parser into v3 AST.
/// The ParserState owns the AST (Pool); caller shows it while alive.
static Node * spikeParse(ParserState & st, const char * text) {
    yyscan_t scanner;
    yylex_init(&scanner);
    YY_BUFFER_STATE buf = yy_scan_string(text, scanner);
    nix::v3::spike::SpikeParser parser(scanner, &st);
    parser.parse();
    yy_delete_buffer(buf, scanner);
    yylex_destroy(scanner);
    return st.result;
}

static void check(const char * src, const char * golden) {
    ++checks;
    ParserState st;
    std::string got;
    try {
        got = showToString(spikeParse(st, src));
    } catch (const std::exception & e) {
        ++failures;
        std::printf("  FAIL %-14s threw: %s\n", src, e.what());
        return;
    }
    if (got == golden) std::printf("  ok   %-14s %s\n", src, got.c_str());
    else { ++failures; std::printf("  FAIL %-14s\n    golden: %s\n    got:    %s\n",
                                   src, golden, got.c_str()); }
}

int main() {
    std::printf("=== Stage 1.3 bison/flex spike → v3 AST vs TW --parse ===\n");
    // Goldens captured from `nix-instantiate --parse`.
    check("1 + 2",        "(1 + 2)");
    check("1 + 2 + 3",    "((1 + 2) + 3)");
    check("2 * 3",        "(__mul 2 3)");
    check("1 + 2 * 3",    "(1 + (__mul 2 3))");
    check("(1 + 2) * 3",  "(__mul (1 + 2) 3)");
    check("1 * 2 * 3",    "(__mul (__mul 1 2) 3)");
    std::printf("\n=== %d/%d checks passed ===\n", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
