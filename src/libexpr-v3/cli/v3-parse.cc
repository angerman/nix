/// @file
/// v3-parse — Stage 1.5 integration CLI for the v3-native parser.
///
/// PARSER_PROJECT_PLAN_2026-06-01.md §2 (Stage 1.5).  Parses a real .nix
/// FILE (or `--expr STRING`) with the v3-native bison/flex parser and
/// prints the resulting v3 AST via `show()`, byte-compatible with
/// `nix-instantiate --parse`.  Parsing a real file supplies the
/// source-file basePath (and `$HOME`) needed to resolve RELATIVE
/// (`./foo`) and HOME (`~/foo`) path literals — the piece the
/// string-buffer spike could not validate (Tier 4c deferred it here).
///
/// This is the Stage 1 validation surface: byte-compare the v3 AST vs
/// the TW AST over a real-file corpus.  It does NOT evaluate — that is
/// Stage 2 (collapse the v3 AST to IR).
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/ast/expr.hh"
#include "parser-state.hh"
#include "v3-spike-decls.hh"  // v3-spike-tab.hh + YYSTYPE
#include "v3-spike-lex.hh"    // flex reentrant decls (needs YYSTYPE)

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace nix::v3::ast;

/// Parse `text` through the v3 bison/flex parser into v3 AST.  The
/// ParserState owns the AST (Pool); the caller shows it while alive.
static Node * v3parse(ParserState & st, const std::string & text)
{
    yyscan_t scanner;
    yylex_init(&scanner);
    YY_BUFFER_STATE buf = yy_scan_string(text.c_str(), scanner);
    nix::v3::spike::SpikeParser parser(scanner, &st);
    parser.parse();
    yy_delete_buffer(buf, scanner);
    yylex_destroy(scanner);
    return st.result;
}

static void usage()
{
    std::fprintf(stderr,
        "usage: v3-parse <file.nix>      parse a file (basePath = its dir)\n"
        "       v3-parse --expr <expr>   parse a string (basePath = cwd)\n");
}

int main(int argc, char ** argv)
{
    ParserState st;
    std::string text;

    // $HOME for HOME-path (`~/foo`) resolution (mirrors TW getHome()).
    if (const char * home = std::getenv("HOME"))
        st.homePath = home;

    if (argc == 3 && std::string(argv[1]) == "--expr") {
        text = argv[2];
        // --expr: basePath = cwd (mirrors `nix-instantiate --expr`).
        st.basePath = std::filesystem::current_path().string();
    } else if (argc == 2) {
        std::filesystem::path p(argv[1]);
        std::ifstream f(p);
        if (!f) {
            std::fprintf(stderr, "v3-parse: cannot open %s\n", argv[1]);
            return 1;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        text = ss.str();
        // basePath = absolute dirname of the source file (TW basePath).
        std::error_code ec;
        auto abs = std::filesystem::absolute(p, ec);
        st.basePath = abs.parent_path().string();
    } else {
        usage();
        return 2;
    }

    try {
        Node * n = v3parse(st, text);
        if (!n) {
            std::fprintf(stderr, "v3-parse: empty parse\n");
            return 1;
        }
        // Trailing newline matches `nix-instantiate --parse` output.
        std::printf("%s\n", showToString(n).c_str());
    } catch (const std::exception & e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
