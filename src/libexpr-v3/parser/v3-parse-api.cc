/// @file
/// v3-native parser public entry point (PARSER_PROJECT_PLAN §5.3).
///
/// This is the ONE place the flex/bison glue lives outside the generated
/// sources.  It is compiled into the standalone `v3-parser` static
/// library (`unity=off`, so the recursive flex/bison generated-header
/// dependency doesn't break a unity build) which is linked into both the
/// main `libnixexprv3` library (for the import/flake parse sites) and the
/// parser CLIs.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3-parse-api.hh"

#include "nix/util/error.hh"

// Order matters: v3-parser-decls.hh defines YYSTYPE/YYLTYPE from the
// bison parser's value/location types, which the flex header references.
#include "v3-parser-decls.hh"  // v3-parser-tab.hh + YYSTYPE
#include "v3-parser-lex.hh"    // flex reentrant decls (needs YYSTYPE)

namespace nix::v3::parser {

nix::v3::ast::Node * parseString(nix::v3::ast::ParserState & st, const std::string & text)
{
    // Reentrant flex scanner over an in-memory buffer (mirrors TW's
    // parseExprFromString glue + the CLI's former v3ParseInto).
    yyscan_t scanner;
    yylex_init(&scanner);
    YY_BUFFER_STATE buf = yy_scan_string(text.c_str(), scanner);
    Parser parser(scanner, &st);
    parser.parse();
    yy_delete_buffer(buf, scanner);
    yylex_destroy(scanner);
    if (!st.result)
        throw nix::Error("v3-native parser produced no expression");
    return st.result;
}

}  // namespace nix::v3::parser
