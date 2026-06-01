/* v3 parser Stage 1.3 — bison/flex toolchain spike.
 *
 * PARSER_PROJECT_PLAN_2026-06-01.md §2 (Stage 1.3).  Minimal grammar
 * that de-risks the bison/flex -> v3 build integration: same lalr1.cc
 * C++ skeleton + api.value.type variant as the real parser.y, emitting
 * the v3 AST (include/v3/ast/expr.hh) via the v3 ParserState
 * (parser/parser-state.hh).  Locations are omitted for the spike
 * (positions = noPos); the real parser.y adds them in Stage 1.4.
 *
 * Grammar: arithmetic over integer literals + parens with the standard
 * precedence (+ looser than *, both left-assoc), enough to prove the
 * whole toolchain path compiles, links, runs, and produces v3 AST that
 * `show()`s byte-equal to `nix-instantiate --parse`.  Stage 1.4 swaps
 * this for the full parser.y grammar + rewritten actions.
 *
 * Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
 *   Input Output Group.
 * SPDX-License-Identifier: Apache-2.0
 */

%skeleton "lalr1.cc"
%require "3.0"
%define api.namespace { nix::v3::spike }
%define api.parser.class { SpikeParser }
%define api.value.type variant
%define parse.error detailed
%parse-param { void * scanner }
%parse-param { nix::v3::ast::ParserState * state }
%lex-param { void * scanner }
%expect 0

%code requires {
  // bison emits switch statements over the full symbol-kind enum with a
  // default: case; silence -Wswitch-enum for the generated code (same as
  // the real parser.y:17-19).  Pushed without pop — applies to the
  // generated header + the .cc that includes it.
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wswitch-enum"

  #include "v3/ast/expr.hh"
  #include "parser-state.hh"
  typedef void * yyscan_t;
}

%code {
  #include "v3-spike-tab.hh"
  // Flex reentrant + bison-bridge scanner entry (matches lexer.l's
  // YY_DECL shape, minus locations).
  int yylex(nix::v3::spike::SpikeParser::value_type * yylval, yyscan_t scanner);
  using namespace nix::v3::ast;
}

%token <int64_t> INT
%type <nix::v3::ast::Node *> expr term factor

%%

start
  : expr { state->result = $1; }
  ;

expr
  : expr '+' term {
      // `+` desugars to a 2-element ConcatStrings (left-assoc), matching
      // parser.y; `1 + 2 + 3` -> ((1 + 2) + 3).
      $$ = state->add<ConcatStrings>(std::vector<Node *>{ $1, $3 });
    }
  | term { $$ = $1; }
  ;

term
  : term '*' factor {
      // `*` desugars to `__mul a b` (parser.y:308 makeCall pattern).
      $$ = state->add<Call>(
             state->add<Var>(std::string("__mul")),
             std::vector<Node *>{ $1, $3 });
    }
  | factor { $$ = $1; }
  ;

factor
  : INT          { $$ = state->add<Int>($1); }
  | '(' expr ')' { $$ = $2; }
  ;

%%

void nix::v3::spike::SpikeParser::error(const std::string & msg)
{
    throw nix::v3::ast::ParseError("v3 spike parse error: " + msg, 0);
}
