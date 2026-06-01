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
%option stack

%s DEFAULT
%x STRING

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

  // State stack management (mirrors lexer.l:98).  `%s DEFAULT` is an
  // inclusive state so unprefixed rules (keywords/operators/...) are
  // active in INITIAL + DEFAULT; `${` and `{` push DEFAULT so the
  // embedded expression is lexed with those rules; `}` pops back.
  #define PUSH_STATE(s) yy_push_state(s, yyscanner)
  #define POP_STATE()   yy_pop_state(yyscanner)

  // Port of lexer.l:48-75 unescapeStr: unescape a string-literal chunk
  // (\\n \\r \\t \\" \\\\ \\$ ...) + normalise CR / CRLF to LF, in place.
  // Returns the unescaped std::string.
  static std::string v3UnescapeStr(const char * s, size_t length) {
      std::string out;
      out.reserve(length);
      for (size_t i = 0; i < length; ) {
          char c = s[i++];
          if (c == '\\') {
              char e = s[i++];
              if      (e == 'n') out += '\n';
              else if (e == 'r') out += '\r';
              else if (e == 't') out += '\t';
              else               out += e;
          } else if (c == '\r') {
              out += '\n';
              if (i < length && s[i] == '\n') ++i;  // cr/lf -> lf
          } else {
              out += c;
          }
      }
      return out;
  }
%}

ID    [a-zA-Z_][a-zA-Z0-9_'\-]*
INT   [0-9]+
FLOAT (([1-9][0-9]*\.[0-9]*)|(0?\.[0-9]+))([Ee][+-]?[0-9]+)?
ANY   .|\n

%%

  /* keywords (before ID; see header note) */
"if"        { return nix::v3::spike::SpikeParser::token::IF; }
"then"      { return nix::v3::spike::SpikeParser::token::THEN; }
"else"      { return nix::v3::spike::SpikeParser::token::ELSE; }
"assert"    { return nix::v3::spike::SpikeParser::token::ASSERT; }
"with"      { return nix::v3::spike::SpikeParser::token::WITH; }
"let"       { return nix::v3::spike::SpikeParser::token::LET; }
"in"        { return nix::v3::spike::SpikeParser::token::IN_KW; }
"rec"       { return nix::v3::spike::SpikeParser::token::REC; }
"inherit"   { return nix::v3::spike::SpikeParser::token::INHERIT; }
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

  /* String literals (mirrors lexer.l:190-200).  `"` opens the STRING
   * exclusive state; `${` inside opens DEFAULT for the embedded expr;
   * `}` pops back; `"` closes.  Content is unescaped via v3UnescapeStr.
   * The two content rules handle a string ending in `$` (trailing
   * context) vs the general case. */
\"          { PUSH_STATE(STRING); return '"'; }
<STRING>([^\$\"\\]|\$[^\{\"\\]|\\{ANY}|\$\\{ANY})*\$/\" |
<STRING>([^\$\"\\]|\$[^\{\"\\]|\\{ANY}|\$\\{ANY})+ {
              yylval->emplace<std::string>(v3UnescapeStr(yytext, (size_t) yyleng));
              return nix::v3::spike::SpikeParser::token::STR;
            }
<STRING>\$\{  { PUSH_STATE(DEFAULT); return nix::v3::spike::SpikeParser::token::DOLLAR_CURLY; }
<STRING>\"    { POP_STATE(); return '"'; }

  /* state-managed braces (mirrors lexer.l:184-188): `{` pushes DEFAULT
   * so the contents lex with the inclusive-state rules; `}` pops
   * (no-op pop at INITIAL — the stack-bottom marker). */
\{          { PUSH_STATE(DEFAULT); return '{'; }
\}          { if (YY_START != INITIAL) POP_STATE(); return '}'; }

  /* other single-char tokens returned as their char code */
[<>+\-*/!?.:()] { return yytext[0]; }
[\[\];=,]       { return yytext[0]; }

[ \t\r\n]+  { /* skip whitespace */ }
"#"[^\n]*   { /* skip line comment */ }

.           { return yytext[0]; }   /* let the parser reject */

%%
