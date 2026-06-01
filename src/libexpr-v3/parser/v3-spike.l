/* v3 parser Stage 1.4 — Tier-1 lexer (expression core).
 *
 * Reentrant flex scanner for the Tier-1 grammar (v3-spike.y): integer +
 * float literals, identifiers, the if/then/else + or keywords, the
 * operator tokens, parens, and `:`/`.`/`?`.  Token rules transcribed
 * from lexer.l's DEFAULT state (keywords lexer.l:127-136, operators
 * 139-147, ID/INT/FLOAT 155-176).  The 6 string/path/indented-string
 * states are deferred to Tier 2+.
 *
 * Keyword rules precede the ID rule so equal-length matches (e.g. "if")
 * resolve to the keyword (flex: longest-match, then first-rule); longer
 * identifiers ("iffy") still win by longest-match.
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
  #include <string>
%}

ID    [a-zA-Z_][a-zA-Z0-9_'\-]*
INT   [0-9]+
FLOAT (([1-9][0-9]*\.[0-9]*)|(0?\.[0-9]+))([Ee][+-]?[0-9]+)?

%%

  /* keywords (before ID; see header note) */
"if"        { return nix::v3::spike::SpikeParser::token::IF; }
"then"      { return nix::v3::spike::SpikeParser::token::THEN; }
"else"      { return nix::v3::spike::SpikeParser::token::ELSE; }
"or"        { return nix::v3::spike::SpikeParser::token::OR_KW; }

  /* multi-char operators (before single-char so they win) */
"=="        { return nix::v3::spike::SpikeParser::token::EQ; }
"!="        { return nix::v3::spike::SpikeParser::token::NEQ; }
"<="        { return nix::v3::spike::SpikeParser::token::LEQ; }
">="        { return nix::v3::spike::SpikeParser::token::GEQ; }
"&&"        { return nix::v3::spike::SpikeParser::token::AND; }
"||"        { return nix::v3::spike::SpikeParser::token::OR; }
"->"        { return nix::v3::spike::SpikeParser::token::IMPL; }
"//"        { return nix::v3::spike::SpikeParser::token::UPDATE; }
"++"        { return nix::v3::spike::SpikeParser::token::CONCAT; }

{ID}        { yylval->emplace<std::string>(yytext, (size_t) yyleng);
              return nix::v3::spike::SpikeParser::token::ID; }
{INT}       { yylval->emplace<int64_t>(std::strtoll(yytext, nullptr, 10));
              return nix::v3::spike::SpikeParser::token::INT_LIT; }
{FLOAT}     { yylval->emplace<double>(std::strtod(yytext, nullptr));
              return nix::v3::spike::SpikeParser::token::FLOAT_LIT; }

  /* single-char tokens returned as their char code */
[<>+\-*/!?.:()] { return yytext[0]; }

[ \t\r\n]+  { /* skip whitespace */ }
"#"[^\n]*   { /* skip line comment */ }

.           { return yytext[0]; }   /* let the parser reject */

%%
