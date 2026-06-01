/* v3 parser Stage 1.4 — grammar grow (Tier 1: expression core).
 *
 * PARSER_PROJECT_PLAN_2026-06-01.md §2 (Stage 1.4).  Grows the Stage 1.3
 * toolchain spike into the real parser by transcribing parser.y's
 * expression-core productions VERBATIM (same precedence declarations,
 * same productions) and rewriting only the action bodies to emit the v3
 * AST via the v3 ParserState.  Keeping the grammar structure identical
 * preserves parser.y's `%expect 0` conflict-freedom.
 *
 * Tier 1 coverage: integer/float literals, variables, the full operator
 * precedence tier (incl. the `<`/`>`/`<=`/`>=` -> __lessThan and unary
 * `-` -> `__sub 0` desugarings), function application (flattened via
 * makeCall), attribute selection (`.` + `or` default), has-attr (`?`),
 * simple lambda (`x: body`), and `if/then/else`.  This validates against
 * the 49-fixture operator-precedence battery
 * (test/parser-ti/fixtures/precedence).
 *
 * Tier 2+ (subsequent commits) add: strings + antiquotation, paths,
 * indented strings, attrsets/binds (via addAttr), lists, let/with/assert,
 * formals (via validateFormals), pipe operators, dynamic attr keys.
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
  // default: case; silence -Wswitch-enum for the generated code (matches
  // the real parser.y:17-19).  Push without pop.
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wswitch-enum"

  #include "v3/ast/expr.hh"
  #include "parser-state.hh"
  #include <vector>
  #include <string>
  typedef void * yyscan_t;
}

%code {
  #include "v3-spike-tab.hh"
  int yylex(nix::v3::spike::SpikeParser::value_type * yylval, yyscan_t scanner);
  using namespace nix::v3::ast;
}

/* Token value types (mirror parser.y). */
%token <std::string> ID   "identifier"
%token <std::string> STR  "string"
%token <int64_t>     INT_LIT   "integer"
%token <double>      FLOAT_LIT "float"
%token DOLLAR_CURLY "'${'"
%token IF "'if'" THEN "'then'" ELSE "'else'"
%token ASSERT "'assert'" WITH "'with'" LET "'let'" IN_KW "'in'" REC "'rec'"
%token OR_KW "'or'"
%token EQ "'=='" NEQ "'!='" LEQ "'<='" GEQ "'>='"
%token UPDATE "'//'" CONCAT "'++'" AND "'&&'" OR "'||'" IMPL "'->'"

%type <nix::v3::ast::Node *> expr expr_function expr_if expr_op
%type <nix::v3::ast::Node *> expr_app expr_select expr_simple
%type <nix::v3::ast::Attrs *> binds binds1
%type <std::vector<nix::v3::ast::Node *>> list
%type <nix::v3::ast::Node *> string_parts
%type <std::vector<nix::v3::ast::Node *>> string_parts_interpolated
%type <std::vector<nix::v3::ast::AttrName>> attrpath
%type <std::string> attr

/* Precedence — transcribed verbatim from parser.y:208-219. */
%right IMPL
%left OR
%left AND
%nonassoc EQ NEQ
%nonassoc '<' '>' LEQ GEQ
%right UPDATE
%left NOT
%left '+' '-'
%left '*' '/'
%right CONCAT
%nonassoc '?'
%nonassoc NEGATE

%%

start
  : expr { state->result = $1; }
  ;

expr
  : expr_function
  ;

expr_function
  : ID ':' expr_function { $$ = state->add<Lambda>($1, $3); }
  | ASSERT expr ';' expr_function { $$ = state->add<Assert>($2, $4); }
  | WITH expr ';' expr_function   { $$ = state->add<With>($2, $4); }
  | LET binds IN_KW expr_function { $$ = state->add<Let>($2, $4); }
  | expr_if
  ;

expr_if
  : IF expr THEN expr ELSE expr { $$ = state->add<If>($2, $4, $6); }
  | expr_op
  ;

expr_op
  : '!' expr_op %prec NOT { $$ = state->add<OpNot>($2); }
  | '-' expr_op %prec NEGATE {
      $$ = state->add<Call>(state->add<Var>(std::string("__sub")),
             std::vector<Node *>{ state->add<Int>(0), $2 });
    }
  | expr_op EQ expr_op    { $$ = state->add<BinOp>(Kind::OpEq,  "==", $1, $3); }
  | expr_op NEQ expr_op   { $$ = state->add<BinOp>(Kind::OpNEq, "!=", $1, $3); }
  | expr_op '<' expr_op {
      $$ = state->add<Call>(state->add<Var>(std::string("__lessThan")),
             std::vector<Node *>{ $1, $3 });
    }
  | expr_op LEQ expr_op {
      $$ = state->add<OpNot>(
             state->add<Call>(state->add<Var>(std::string("__lessThan")),
               std::vector<Node *>{ $3, $1 }));
    }
  | expr_op '>' expr_op {
      $$ = state->add<Call>(state->add<Var>(std::string("__lessThan")),
             std::vector<Node *>{ $3, $1 });
    }
  | expr_op GEQ expr_op {
      $$ = state->add<OpNot>(
             state->add<Call>(state->add<Var>(std::string("__lessThan")),
               std::vector<Node *>{ $1, $3 }));
    }
  | expr_op AND expr_op    { $$ = state->add<BinOp>(Kind::OpAnd,  "&&", $1, $3); }
  | expr_op OR expr_op     { $$ = state->add<BinOp>(Kind::OpOr,   "||", $1, $3); }
  | expr_op IMPL expr_op   { $$ = state->add<BinOp>(Kind::OpImpl, "->", $1, $3); }
  | expr_op UPDATE expr_op { $$ = state->add<BinOp>(Kind::OpUpdate, "//", $1, $3); }
  | expr_op '?' attrpath   { $$ = state->add<OpHasAttr>($1, $3); }
  | expr_op '+' expr_op {
      // `+` -> 2-element ConcatStrings (parser.y:311).
      $$ = state->add<ConcatStrings>(std::vector<Node *>{ $1, $3 });
    }
  | expr_op '-' expr_op {
      $$ = state->add<Call>(state->add<Var>(std::string("__sub")),
             std::vector<Node *>{ $1, $3 });
    }
  | expr_op '*' expr_op {
      $$ = state->add<Call>(state->add<Var>(std::string("__mul")),
             std::vector<Node *>{ $1, $3 });
    }
  | expr_op '/' expr_op {
      $$ = state->add<Call>(state->add<Var>(std::string("__div")),
             std::vector<Node *>{ $1, $3 });
    }
  | expr_op CONCAT expr_op { $$ = state->add<BinOp>(Kind::OpConcatLists, "++", $1, $3); }
  | expr_app
  ;

expr_app
  : expr_app expr_select { $$ = state->makeCall($1, $2); }
  | expr_select          { $$ = $1; }
  ;

expr_select
  : expr_simple '.' attrpath
    { $$ = state->add<Select>($1, $3, nullptr); }
  | expr_simple '.' attrpath OR_KW expr_select
    { $$ = state->add<Select>($1, $3, $5); }
  | expr_simple
  ;

expr_simple
  : ID {
      if ($1 == "__curPos") $$ = state->add<PosExpr>();
      else                  $$ = state->add<Var>($1);
    }
  | INT_LIT      { $$ = state->add<Int>($1); }
  | FLOAT_LIT    { $$ = state->add<Float>($1); }
  | '"' string_parts '"' { $$ = $2; }
  | '(' expr ')' { $$ = $2; }
  | REC '{' binds '}' { $3->recursive = true; $$ = $3; }
  | '{' binds1 '}'    { $$ = $2; }
  | '{' '}'           { $$ = state->add<Attrs>(false); }
  | '[' list ']'      { $$ = state->add<List>(std::move($2)); }
  ;

/* attrset bindings.  Tier 3-lite: `attrpath = expr;` only (wires the
 * proven ParserState::addAttr).  inherit / inherit-from / dynamic keys
 * are Tier 3b/Tier 2 (the latter needs the STRING lexer state). */
binds
  : binds1
  | /* empty */ { $$ = state->add<Attrs>(false); }
  ;

binds1
  : binds1 attrpath '=' expr ';'
    { $$ = $1; state->addAttr($1, std::move($2), $4, 0); }
  | attrpath '=' expr ';'
    { $$ = state->add<Attrs>(false); state->addAttr($$, std::move($1), $3, 0); }
  ;

list
  : list expr_select { $$ = std::move($1); $$.push_back($2); }
  | /* empty */      { $$ = std::vector<nix::v3::ast::Node *>{}; }
  ;

/* string literals + interpolation (parser.y:397-412).  A plain string
 * is a single String; an interpolated one is a ConcatStrings with
 * forceString=true. */
string_parts
  : STR { $$ = state->add<String>($1); }
  | string_parts_interpolated
    { $$ = state->add<ConcatStrings>(std::move($1), /*forceString=*/true); }
  | /* empty */ { $$ = state->add<String>(std::string("")); }
  ;

string_parts_interpolated
  : string_parts_interpolated STR
    { $$ = std::move($1); $$.push_back(state->add<String>($2)); }
  | string_parts_interpolated DOLLAR_CURLY expr '}'
    { $$ = std::move($1); $$.push_back($3); }
  | DOLLAR_CURLY expr '}'
    { $$ = std::vector<nix::v3::ast::Node *>{ $2 }; }
  | STR DOLLAR_CURLY expr '}'
    { $$ = std::vector<nix::v3::ast::Node *>{ state->add<String>($1), $3 }; }
  ;

attrpath
  : attrpath '.' attr { $$ = std::move($1); $$.emplace_back($3); }
  | attr              { $$ = std::vector<AttrName>{ AttrName($1) }; }
  ;

attr
  : ID    { $$ = $1; }
  | OR_KW { $$ = std::string("or"); }
  ;

%%

void nix::v3::spike::SpikeParser::error(const std::string & msg)
{
    throw nix::v3::ast::ParseError("v3 parse error: " + msg, 0);
}
