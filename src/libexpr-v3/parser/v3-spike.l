/* v3 parser Stage 1.3 — bison/flex toolchain spike lexer.
 *
 * Minimal reentrant flex scanner for the spike grammar (v3-spike.y):
 * integer literals, +, *, parens; whitespace skipped.  Mirrors the
 * real lexer.l's reentrant + bison-bridge options, minus the location
 * tracking and the 7 string/path states (Stage 1.4 ports those).
 *
 * Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
 *   Input Output Group.
 * SPDX-License-Identifier: Apache-2.0
 */

%option 8bit
%option reentrant bison-bridge
%option noyywrap
%option never-interactive
%option nodefault
%option nounput noinput

%top {
  // Brings in v3-spike-tab.hh + defines YYSTYPE (the parser variant
  // value_type) which the bison-bridge scanner references for yylval.
  #include "v3-spike-decls.hh"
}

%{
  #include "v3/ast/expr.hh"
  #include "parser-state.hh"
  #include <cstdlib>
  // bison-bridge sets *yylval; the variant value_type comes from the
  // generated parser header.
%}

%%

[0-9]+      { yylval->emplace<int64_t>(std::strtoll(yytext, nullptr, 10));
              return nix::v3::spike::SpikeParser::token::INT; }
"+"         { return '+'; }
"*"         { return '*'; }
"("         { return '('; }
")"         { return ')'; }
[ \t\r\n]+  { /* skip whitespace */ }
.           { return yytext[0]; }   /* let the parser reject */

%%
