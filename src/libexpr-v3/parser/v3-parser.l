/* v3 parser Stage 1.4 — Tier-1 lexer (expression core).
 *
 * Reentrant flex scanner for the Tier-1 grammar (v3-parser.y): integer +
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
%x IND_STRING
%x PATH_START
%x INPATH
%x INPATH_SLASH

%top {
  // Brings in v3-parser-tab.hh + defines YYSTYPE (the parser variant
  // value_type) which the bison-bridge scanner references for yylval.
  #include "v3-parser-decls.hh"
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

  /* path char-classes (lexer.l:108-113), transcribed verbatim. */
PATH_CHAR   [a-zA-Z0-9\.\_\-\+]
PATH        {PATH_CHAR}*(\/{PATH_CHAR}+)+\/?
PATH_SEG    {PATH_CHAR}*\/
HPATH       \~(\/{PATH_CHAR}+)+\/?
HPATH_START \~\/
SPATH       \<{PATH_CHAR}+(\/{PATH_CHAR}+)*\>
URI         [a-zA-Z][a-zA-Z0-9\+\-\.]*\:[a-zA-Z0-9\%\/\?\:\@\&\=\+\$\,\-\_\.\!\~\*\']+

%%

  /* keywords (before ID; see header note) */
"if"        { return nix::v3::parser::Parser::token::IF; }
"then"      { return nix::v3::parser::Parser::token::THEN; }
"else"      { return nix::v3::parser::Parser::token::ELSE; }
"assert"    { return nix::v3::parser::Parser::token::ASSERT; }
"with"      { return nix::v3::parser::Parser::token::WITH; }
"let"       { return nix::v3::parser::Parser::token::LET; }
"in"        { return nix::v3::parser::Parser::token::IN_KW; }
"rec"       { return nix::v3::parser::Parser::token::REC; }
"inherit"   { return nix::v3::parser::Parser::token::INHERIT; }
"or"        { return nix::v3::parser::Parser::token::OR_KW; }

  /* multi-char operators (before single-char so they win) */
"=="        { return nix::v3::parser::Parser::token::EQ; }
"!="        { return nix::v3::parser::Parser::token::NEQ; }
"<="        { return nix::v3::parser::Parser::token::LEQ; }
">="        { return nix::v3::parser::Parser::token::GEQ; }
"&&"        { return nix::v3::parser::Parser::token::AND; }
"||"        { return nix::v3::parser::Parser::token::OR; }
"->"        { return nix::v3::parser::Parser::token::IMPL; }
"//"        { return nix::v3::parser::Parser::token::UPDATE; }
"++"        { return nix::v3::parser::Parser::token::CONCAT; }
"..."       { return nix::v3::parser::Parser::token::ELLIPSIS; }  /* formals (lexer.l:137) */
  /* pipe operators (lexer.l:148-153).  TW gates these on the
   * `pipe-operators` experimental feature; the spike does not gate
   * (feature policy is an integration concern, not a parse concern). */
"<|"        { return nix::v3::parser::Parser::token::PIPE_FROM; }
"|>"        { return nix::v3::parser::Parser::token::PIPE_INTO; }

{ID}        { yylval->emplace<std::string>(yytext, (size_t) yyleng);
              return nix::v3::parser::Parser::token::ID; }
{INT}       { yylval->emplace<int64_t>(std::strtoll(yytext, nullptr, 10));
              return nix::v3::parser::Parser::token::INT_LIT; }
{FLOAT}     { yylval->emplace<double>(std::strtod(yytext, nullptr));
              return nix::v3::parser::Parser::token::FLOAT_LIT; }

  /* String literals (mirrors lexer.l:190-200).  `"` opens the STRING
   * exclusive state; `${` inside opens DEFAULT for the embedded expr;
   * `}` pops back; `"` closes.  Content is unescaped via v3UnescapeStr.
   * The two content rules handle a string ending in `$` (trailing
   * context) vs the general case. */
\"          { PUSH_STATE(STRING); return '"'; }
<STRING>([^\$\"\\]|\$[^\{\"\\]|\\{ANY}|\$\\{ANY})*\$/\" |
<STRING>([^\$\"\\]|\$[^\{\"\\]|\\{ANY}|\$\\{ANY})+ {
              yylval->emplace<std::string>(v3UnescapeStr(yytext, (size_t) yyleng));
              return nix::v3::parser::Parser::token::STR;
            }
<STRING>\$\{  { PUSH_STATE(DEFAULT); return nix::v3::parser::Parser::token::DOLLAR_CURLY; }
<STRING>\"    { POP_STATE(); return '"'; }

  /* Indented strings `''…''` (mirrors lexer.l:209-232).  IND_STRING_OPEN
   * `''` optionally swallows the trailing ` *\n` of the opening line;
   * stripIndentation (ParserState) does the dedent.  Content vs escapes:
   * the general rule sets hasIndentation=true (participates in dedent);
   * `''$`/`$` -> literal `$`, `'''` -> `''`, `''\x` -> unescaped x, lone
   * `'` -> `'` all set false.  `${` opens an antiquotation (push DEFAULT);
   * `''` closes (pop). */
\'\'(\ *\n)?  { PUSH_STATE(IND_STRING); return nix::v3::parser::Parser::token::IND_STRING_OPEN; }
<IND_STRING>([^\$\']|\$[^\{\']|\'[^\'\$])+ {
                yylval->emplace<nix::v3::ast::IndStr>(
                    nix::v3::ast::IndStr{std::string(yytext, (size_t) yyleng), /*hasIndentation=*/true});
                return nix::v3::parser::Parser::token::IND_STR;
              }
<IND_STRING>\'\'\$ |
<IND_STRING>\$  { yylval->emplace<nix::v3::ast::IndStr>(nix::v3::ast::IndStr{std::string("$"), false});
                return nix::v3::parser::Parser::token::IND_STR; }
<IND_STRING>\'\'\' { yylval->emplace<nix::v3::ast::IndStr>(nix::v3::ast::IndStr{std::string("''"), false});
                return nix::v3::parser::Parser::token::IND_STR; }
<IND_STRING>\'\'\\{ANY} {
                yylval->emplace<nix::v3::ast::IndStr>(
                    nix::v3::ast::IndStr{v3UnescapeStr(yytext + 2, (size_t) yyleng - 2), false});
                return nix::v3::parser::Parser::token::IND_STR; }
<IND_STRING>\$\{ { PUSH_STATE(DEFAULT); return nix::v3::parser::Parser::token::DOLLAR_CURLY; }
<IND_STRING>\'\' { POP_STATE(); return nix::v3::parser::Parser::token::IND_STRING_CLOSE; }
<IND_STRING>\'  { yylval->emplace<nix::v3::ast::IndStr>(nix::v3::ast::IndStr{std::string("'"), false});
                return nix::v3::parser::Parser::token::IND_STR; }

  /* Paths (mirrors lexer.l:235-315).  Three states: PATH_START handles a
   * path that STARTS with an antiquotation segment (`./${x}`, `~/${x}`)
   * via a yyless(0) rewind; INPATH/INPATH_SLASH carry the continuation.
   * The `yylloc->unstash()` location bookkeeping is omitted (the spike
   * tracks no positions).  Resolution (the PATH/HPATH grammar actions)
   * is deterministic for ABSOLUTE paths + <spath>; relative/home need a
   * source-file basePath the string-buffer spike lacks (deferred). */
{PATH_SEG}\$\{ |
{HPATH_START}\$\{ {
                PUSH_STATE(PATH_START); yyless(0);
              }
<PATH_START>{PATH_SEG} {
                POP_STATE(); PUSH_STATE(INPATH_SLASH);
                yylval->emplace<std::string>(yytext, (size_t) yyleng);
                return nix::v3::parser::Parser::token::PATH;
              }
<PATH_START>{HPATH_START} {
                POP_STATE(); PUSH_STATE(INPATH_SLASH);
                yylval->emplace<std::string>(yytext, (size_t) yyleng);
                return nix::v3::parser::Parser::token::HPATH;
              }
<PATH_START>{ANY} |
<PATH_START><<EOF>> {
                /* unreachable: PATH_START is entered only after the
                 * yyless(0) rewind of a {PATH_SEG}/{HPATH_START} match. */
                POP_STATE(); return (unsigned char) yytext[0];
              }
{PATH} {
                if (yytext[yyleng-1] == '/') PUSH_STATE(INPATH_SLASH);
                else                         PUSH_STATE(INPATH);
                yylval->emplace<std::string>(yytext, (size_t) yyleng);
                return nix::v3::parser::Parser::token::PATH;
              }
{HPATH} {
                if (yytext[yyleng-1] == '/') PUSH_STATE(INPATH_SLASH);
                else                         PUSH_STATE(INPATH);
                yylval->emplace<std::string>(yytext, (size_t) yyleng);
                return nix::v3::parser::Parser::token::HPATH;
              }
<INPATH,INPATH_SLASH>\$\{ {
                POP_STATE(); PUSH_STATE(INPATH); PUSH_STATE(DEFAULT);
                return nix::v3::parser::Parser::token::DOLLAR_CURLY;
              }
<INPATH,INPATH_SLASH>{PATH}|{PATH_SEG}|{PATH_CHAR}+ {
                POP_STATE();
                if (yytext[yyleng-1] == '/') PUSH_STATE(INPATH_SLASH);
                else                         PUSH_STATE(INPATH);
                yylval->emplace<std::string>(yytext, (size_t) yyleng);
                return nix::v3::parser::Parser::token::STR;
              }
<INPATH>{ANY} |
<INPATH><<EOF>> {
                /* non-path char ends the path: emit PATH_END + re-parse
                 * the char in the default context (lexer.l:296-305). */
                POP_STATE(); yyless(0);
                return nix::v3::parser::Parser::token::PATH_END;
              }
<INPATH_SLASH>{ANY} |
<INPATH_SLASH><<EOF>> {
                throw nix::v3::ast::ParseError("path has a trailing slash", 0);
              }
{SPATH} {
                yylval->emplace<std::string>(yytext, (size_t) yyleng);
                return nix::v3::parser::Parser::token::SPATH;
              }
{URI} {
                // URL literal (lexer.l:316) -> a string (parser.y:372);
                // the deprecation lint is omitted (diagnostic only).
                yylval->emplace<std::string>(yytext, (size_t) yyleng);
                return nix::v3::parser::Parser::token::URI;
              }

  /* `${` in the default context = a dynamic attr key / select / has-attr
   * (`{ ${e} = …; }`, `x.${e}`, `x ? ${e}`).  Mirrors lexer.l's
   * INITIAL/INSIDE_DOLLAR_CURLY `\$\{` rule: emit DOLLAR_CURLY + push
   * DEFAULT so the embedded expr lexes; the matching `}` pops.  (The
   * STRING-state `${` is handled by the <STRING> rule above; this one
   * is for `${` OUTSIDE a string literal.)  2-char match beats the
   * `$` fall-through by flex longest-match. */
\$\{        { PUSH_STATE(DEFAULT); return nix::v3::parser::Parser::token::DOLLAR_CURLY; }

  /* state-managed braces (mirrors lexer.l:184-188): `{` pushes DEFAULT
   * so the contents lex with the inclusive-state rules; `}` pops
   * (no-op pop at INITIAL — the stack-bottom marker). */
\{          { PUSH_STATE(DEFAULT); return '{'; }
\}          { if (YY_START != INITIAL) POP_STATE(); return '}'; }

  /* other single-char tokens returned as their char code.  `@` is the
   * formal-set @-binding separator (`args@{…}` / `{…}@args`).  Cast to
   * unsigned char (lexer.l:348) so high bytes don't become negative
   * token values (which bison would treat as EOF). */
[<>+\-*/!?.:()@] { return (unsigned char) yytext[0]; }
[\[\];=,]        { return (unsigned char) yytext[0]; }

[ \t\r\n]+  { /* skip whitespace */ }
\#[^\r\n]*  { /* skip line comment (lexer.l:342) */ }
  /* block comments `/* … *​/` incl. `/** … *​/` doc comments (the long-
   * comment regex, lexer.l:343): non-nesting; `*`s only end the comment
   * when immediately followed by `/`.  Beats `{PATH}` / `/` by longest
   * match. */
\/\*([^*]|\*+[^*/])*\*+\/  { /* skip block comment */ }

.           { return (unsigned char) yytext[0]; }   /* let the parser reject (unsigned: lexer.l:348) */

%%
