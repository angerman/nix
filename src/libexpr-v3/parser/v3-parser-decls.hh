#pragma once
/// @file
/// v3 parser Stage 1.3 spike — scanner/parser glue header.
///
/// Mirrors TW's parser-scanner-decls.hh: includes the bison-generated
/// parser header and defines `YYSTYPE` as the parser's variant
/// value_type, which the flex-generated reentrant scanner
/// (bison-bridge) references for `yylval`.  Must be included BEFORE the
/// flex header (v3-parser-lex.hh), whose yyget_lval/yyset_lval/yylex
/// declarations use YYSTYPE.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3-parser-tab.hh"

// bison's lalr1.cc + `api.value.type variant` exposes the value type
// as Parser::value_type (no YYSTYPE macro); the flex bison-bridge
// scanner needs YYSTYPE.  (Same shape as parser-scanner-decls.hh.)
using YYSTYPE = ::nix::v3::parser::Parser::value_type;

// With %locations the bison-bridge scanner also references YYLTYPE
// (the location type) in yylex / yyget_lloc declarations.
using YYLTYPE = ::nix::v3::parser::Parser::location_type;
