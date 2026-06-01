# v3-native parser (in-progress)

**Project plan:** [`../lode/PARSER_PROJECT_PLAN_2026-06-01.md`](../lode/PARSER_PROJECT_PLAN_2026-06-01.md)
**Status:** **Stage 1 COMPLETE.** `parser/v3-parser.{y,l}` is the real
v3-native parser (renamed from the `v3-spike` toolchain probe).  Full
Nix grammar (all 27 AST kinds, all 7 flex states, all ParserState
helpers); `show()` byte-equal to `nix-instantiate --parse` on
**151/151** synthetic fixtures, **260/273** lang files (0 parse
divergences), and **547/547** nixpkgs `lib/` + `build-support/` files
(0 divergences) — every non-match is bindVars (Stage 2).  See the
per-tier sections below; the historical scaffolding notes that follow
describe the original plan and are kept for context.

**Stage 2 integration — EVAL PARITY (143/143):** `NIX_V3_NATIVE_PARSER=1
v3-eval` parses natively, bridges the v3 AST → `nix::Expr`
(`cli/v3-to-nixexpr.hh`), then the EXISTING `bindVars` + `lowerNixExpr`
pipeline evaluates it.  **143/143 lang tests** under the native parser
(== the default TW path), bridge `--parse` 157/157 byte-equal.

File-local **positions** are tracked (bison `%locations` + a byte-offset
`ParserLoc` + flex `YY_USER_ACTION` offset accumulation; `PosExpr` /
attr-defs / formals carry offsets; the bridge maps them to `PosIdx` via
the source's `PosTable::Origin`).  `__curPos` + `builtins.unsafeGetAttrPos`
(on attrs AND function args) resolve byte-identical to TW.

Remaining: a fully-native AST→IR lowering (delete `lower.cc`'s
`nix::Expr` path) — an optimization to retire the bridge; the parser is
functionally complete (parses + evaluates + positions, 143/143).

## Files

| File | Origin | Status |
|---|---|---|
| `parser.y` | `src/libexpr/parser.y` | Forked verbatim.  Actions inside `{ ... }` blocks will be rewritten to emit v3 AST/IR. |
| `lexer.l` | `src/libexpr/lexer.l` | Forked verbatim.  Lexer state machine + token kinds are unchanged. |
| `parser-state.hh.upstream` | `src/libexpr/include/nix/expr/parser-state.hh` | Forked verbatim.  Will be ported to a v3-flavored `parser-state.hh` (Symbol/Pos table adapters). |

## What's preserved (per grammar-reuse strategy)

* 51 productions + 19 nonterminals
* 14 precedence levels + `%expect 0` LALR(1) cleanliness
* 7 Flex states + antiquotation push/pop + path lexer modes
* Indented-string `stripIndentation` algorithm
* The doc-comment positioning logic
* The cursed-`or` wart
* Operator precedence + associativity

## What's changed (Stage 1)

* Action bodies inside `{ ... }` blocks: replace `state->exprs.add<ExprXxx>(...)` with `state->v3ast.add<v3Xxx>(...)`.
* `ParserState` swapped for `v3ParserState`:
  * `exprs` → `v3ast` (v3 AST allocator)
  * `symbols` → v3 globalSymbolTable adapter
  * `positions` → v3 PosTable adapter (file-local spans, NOT cache-stable PosIdx)
  * `at(loc)` → v3 PosHandle (file-local)
* Build system: Meson `custom_target` rules for bison/flex output linked into v3 dylib.

## What's changed (Stage 2)

* Action bodies emit `ir::Module` directly instead of v3 AST.
* v3 AST class hierarchy retired.
* `lower.cc`'s `nix::Expr`-consuming code path deleted (~2-3K LoC retirement target).

## Required external context

* `parser-scanner-decls.hh` — bison/flex glue (upstream `src/libexpr/`)
* Bison ≥ 3.0 (LALR1.cc skeleton)
* Flex ≥ 2.6 (reentrant + bison-bridge + bison-locations + stack + extra-type)

## Build wiring (TODO)

`src/libexpr-v3/meson.build` adds `custom_target` rules mirroring `src/libexpr/meson.build` for the bison + flex outputs.  Both Bison and Flex tools come from the existing flake.nix devShell.

## Stage 1.1 status — v3 AST COMPLETE (show())

`include/v3/ast/expr.hh` holds the v3-owned AST.  ALL 27 `nix::Expr`
Kinds are implemented except the two non-parsed ones (InheritFrom —
a TW-internal pseudo-var that never appears in show() output;
BlackHole — a runtime sentinel).  ConcatLists folds into BinOp("++").

Validated by `test/ast-show-test.cc` (**32/32** show() checks
byte-equal to TW), wired as the `v3-ast-show-test` meson test.
Coverage: Int, Float, String (+ printLiteralString port), Path, Var,
Call, Select (static + dynamic `${}` keys), OpHasAttr, Lambda (simple
+ formals lexicographic), List, Attrs (plain/rec/inherit/inheritFrom/
dynamic/empty + showBindings), Let, With, If, Assert, OpNot,
ConcatStrings, Pos, and all 7 BinOps (==,!=,&&,||,->,//,++).

The remaining Stage 1.1 → Stage 1.4 work is now: 1.2 ParserState
(symbol interning — names are inline std::string today), 1.3 build
wiring (bison/flex → libnixexprv3), 1.4 action rewrite (target THIS
AST instead of nix::Expr).

### The `show()` contract (from `nixexpr.cc:26-262` + MakeBinOp)

Every node's `show()` must reproduce TW byte-for-byte.  Reference:

| Node | show() format |
|---|---|
| `Int` | `<n>` |
| `Float` | `<f>` (default ostream double — verify formatting) |
| `String` | `printLiteralString` (escaped, quoted) |
| `Path` | `<pathStrView>` |
| `Var` | `<name>` |
| `Select` | `(<e>).<path>` then ` or (<def>)` if default |
| `OpHasAttr` | `((<e>) ? <path>)` |
| `Attrs` | `[rec ]{ <bindings> }` — bindings sorted by symbol; `inherit`/`inherit (e)` groups first, then `k = v; `, then dynamic `"${e}" = v; ` |
| `List` | `[ (<e1>) (<e2>) ... ]` (each elem parenthesized) |
| `Lambda` simple | `(<arg>: <body>)` |
| `Lambda` formals | `({ a, b ? d, ... }[ @ arg]: <body>)` — formals LEXICOGRAPHIC |
| `Call` | `(<fun> <a1> <a2> ...)` |
| `Let` | `(let <bindings>in <body>)` |
| `With` | `(with <attrs>; <body>)` |
| `If` | `(if <c> then <t> else <e>)` |
| `Assert` | `assert <c>; <body>` (NOTE: no outer parens) |
| `OpNot` | `(! <e>)` |
| `ConcatStrings` (`+`) | `(<e1> + <e2> + ...)` |
| `BinOp` (==,!=,&&,\|\|,->,//,++) | `(<e1> <op> <e2>)` |
| `Pos` (`__curPos`) | `__curPos` |

### Parser-action desugarings (locked by the precedence battery)

| Surface | AST |
|---|---|
| `a * b` | `Call(Var "__mul", [a, b])` |
| `a / b` | `Call(Var "__div", [a, b])` |
| `a - b` | `Call(Var "__sub", [a, b])` |
| `-a` | `Call(Var "__sub", [Int 0, a])` |
| `a < b` | `Call(Var "__lessThan", [a, b])` |
| `a > b` | `Call(Var "__lessThan", [b, a])` (swapped) |
| `a <= b` | `OpNot(Call(Var "__lessThan", [b, a]))` |
| `a >= b` | `OpNot(Call(Var "__lessThan", [a, b]))` |
| `a + b` | `ConcatStrings([a, b])` |

These come from `parser.y` actions (lines 298-313).  The v3 parser
actions (Stage 1.4) must emit the SAME desugarings.

### Stage 1.1 remaining — DONE (all node kinds landed)

All show()-producing node kinds are implemented + tested (32/32).

### Stage 1.2 status — ParserState `addAttr` landed

`parser-state.hh` (the v3 ParserState) hosts the attrset-merge
construction `addAttr` (port of parser-state.hh.upstream:195/248),
validated by `test/ast-addattr-test.cc` (**10/10** vs TW `--parse`):
nested-path creation, two-path merge, merge-into-set, deep-merge,
two-attrset merge, dynamic-key path, + duplicate-definition detection
(`{ a.b=1; a.b=2; }` throws "attribute 'a.b' already defined").

### Stage 1.2 — self-contained helpers DONE (addAttr, validateFormals, stripIndentation)

`parser-state.hh` now hosts all three semantic helpers, validated by
`test/ast-addattr-test.cc` (**19/19** vs TW):
* `addAttr` — attrset-merge + dup detection (8 + 2 checks)
* `validateFormals` — dup-arg + @-binding collision (5 checks)
* `stripIndentation` — indented-string dedent (4 checks)

The `stripIndentation` tests also document the **IND_STRING_OPEN
lexer contract** (lexer.l:209 `''( *\n)?` consumes the leading
spaces+newline after `''`, so the token content has no leading
newline) — a Stage 1.3 lexer requirement.

Stage 1.2 remaining: symbol interning (names are inline std::string
today — a Stage 1.4 integration decision).

### Stage 1.3 status — bison/flex toolchain WIRED (spike green)

The bison/flex → v3 build integration is proven.  `parser/v3-parser.{y,l}`
is a minimal arithmetic grammar (same lalr1.cc skeleton +
`api.value.type variant` as the real parser.y, minus locations)
emitting v3 AST; `test/parser-test.cc` parses it and asserts
`show()` byte-equal to `nix-instantiate --parse` (**6/6**: `1 + 2 * 3`
→ `(1 + (__mul 2 3))`, etc.).  Wired via meson custom_target
(`v3-parser-tab`, `v3-parser-lex`) mirroring libexpr, `unity=off`.

This retires the #1 project risk — the toolchain path (bison/flex in
devshell → meson → generated C++ compiles → flex/bison glue
[`v3-parser-decls.hh` YYSTYPE] → links → runs → v3 AST) all works.
Integration gotchas locked: `-Wswitch-enum` pragma in `%code requires`
(matches parser.y:17), `unity=off`, and the YYSTYPE glue header.

### Stage 1.4 status — Tier 1 (expression core) LANDED

`parser/v3-parser.{y,l}` grew from the arithmetic toolchain spike into
the real parser's **expression core**, transcribing parser.y's
productions + precedence VERBATIM (so `%expect 0` holds) and rewriting
only the actions to emit v3 AST.

Tier 1 covers: integer/float literals, variables (+ `__curPos`), the
full operator precedence tier (incl. `<`/`>`/`<=`/`>=` → `__lessThan`
and unary `-` → `__sub 0` desugarings), application (flattened via
`makeCall`), select (`.` + `or`), has-attr (`?`), simple lambda
(`x: body`), and `if/then/else`.

Validated by `test/parser-test.cc` (**54/54**): 5 arithmetic
sanity + a sweep of all **49 operator-precedence fixtures**
(test/parser-ti/fixtures/precedence) byte-equal to `nix-instantiate
--parse`.  No bison conflicts (faithful transcription preserved
parser.y's `%expect 0`).

### Stage 1.4 Tier 3-lite — lists, attrsets, let/with/assert LANDED

`v3-parser.{y,l}` gained: lists (`[ ... ]`), attrsets (`{ attrpath =
expr; }`, `rec`, empty) wiring the proven `addAttr` (so `{ a.b=1;
a.c=2; }` → nested merge works through the grammar), `let ... in`,
`with`, `assert`.  No new lexer states (DEFAULT-state tokens +
let/in/with/assert/rec keywords + `{ } [ ] ; = ,`).  Still `%expect 0`.

Validated by the new tier3 battery (test/parser-ti/fixtures/tier3, 13
fixtures) — total parser-test now **67/67** byte-equal to TW.

### Stage 1.4 Tier 2 — strings + antiquotation LANDED

`v3-parser.{y,l}` gained string literals + interpolation: the flex
`STRING` exclusive state (`%option stack`), `${...}` antiquotation
push/pop (`${` pushes DEFAULT, `}` pops), the two content rules
(general + trailing-`$`), `v3UnescapeStr` (port of lexer.l:48-75), and
the `string_parts`/`string_parts_interpolated` grammar (interpolation
→ `ConcatStrings` with `forceString=true`).  `{`/`}` are now
state-managed (mirror lexer.l).  Still `%expect 0`.

Validated by the tier2 battery (12 fixtures) — total parser-test
now **79/79** byte-equal to TW: plain/empty strings, `\n`/`\"`/`\$`
escapes, mid/start/only/multi interpolation, strings in lists + attrs.

### Stage 1.4 Tier 3b — inherit / inherit-from LANDED

`binds1` gained `inherit a b;` (→ `Inherited` AttrDefs, wiring
`ParserState::addInherit`) and `inherit (e) x y;` (→ `InheritedFrom`,
wiring `addInheritFrom` + the `inheritFromExprs` source list), plus the
`attrs` name-list production — all transcribed verbatim from
parser.y:477-535 (incl. the `binds`-not-`binds1` accumulator on the
INHERIT productions so `{ inherit a; }` parses via empty-binds).  Still
`%expect 0`.

Validated by the tier3b battery (9 fixtures) — total parser-test
now **88/88** byte-equal to TW: `inherit a;`, `inherit a b;`,
`inherit (x) a b;`, combined inherit + inherit-from in one set,
empty `inherit;` (→ `{ }`), inherit-in-`let`, inherit-in-`rec`, and the
inherit-before-plain show ordering.

### Stage 1.4 Tier 3c — string / dynamic attr keys LANDED

`string_attr` (`"…"` reusing `string_parts`, or bare `${e}`) wired into
`attrpath` (all 4 parser.y alternatives) for keys + selects + has-attr.
A `string_attr` is a STATIC symbol key iff it is a plain `String`
literal (`ParserState::strAttrName` decides — String⇒static, else
dynamic).  The lexer gained a default-context `${` → `DOLLAR_CURLY`
rule (mirrors lexer.l's INITIAL/INSIDE_DOLLAR_CURLY) so `${e}` lexes
OUTSIDE string literals (dynamic keys / `x.${e}`).

Crucially this needed `printIdentifier` (port of print.cc:90-111):
TW quotes attr/inherit/path names that are empty, reserved keywords, or
non-identifiers — `{ "foo" = 1; }` shows as `{ foo = 1; }` but
`{ "a b" = 1; }` / `{ "if" = 1; }` / `{ "" = 1; }` / `{ "foo.bar" = 1; }`
stay quoted.  Routed through `showBindings` (plain + inherit names) and
`showAttrPath` (select/has-attr static symbols).

Validated by the tier3c battery (12 fixtures) — total parser-test
now **100/100** byte-equal to TW: static string keys (ident-collapse +
quoted), `${e}` dynamic keys, nested + mixed static/dynamic paths,
interpolated keys (`"pre${x}"` → `"${("pre" + x)}"`), and dynamic/string
keys in select + has-attr.

### Stage 1.4 Tier 4a — formals LANDED

`{ a, b ? d, ... } @ args: body` argument patterns — the most common
nixpkgs construct.  Three expr_function productions (`formal_set ':'`,
`formal_set '@' ID ':'`, `ID '@' formal_set ':'`) + `formal_set` /
`formals` / `formal`, transcribed verbatim from parser.y:586-612, wiring
the proven `ParserState::validateFormals` (dup-arg + `@`-collision) and a
`buildFormals` PFormal→Formal converter.  Lexer gained `...` (ELLIPSIS)
and `@`.  **`%expect 0` survives the `{`-attrset-vs-formal-set
disambiguation** in the subset grammar — the chief formals risk, cleared.

Validated by the tier4 battery (14 fixtures) — total parser-test
now **114/114** byte-equal to TW: single/multi formals, defaults (incl.
default referencing another formal), ellipsis (`...` / `a, ...`), empty
`{ }` (→ two-space `{  }`), `@`-binding both positions (TW normalizes
`args@{…}` → `{…} @ args`), lexicographic formal sorting, nested formal
lambdas, formal-lambda application.

### Stage 1.4 Tier 4b — indented strings LANDED

`''…''` strings — the IND_STRING exclusive lexer state (transcribed from
lexer.l:209-232) + the now-wired `ParserState::stripIndentation`.
IND_STRING_OPEN `''` optionally swallows the opening line's trailing
` *\n`; the general content rule sets `hasIndentation=true` (dedent
participant); the escapes `''$`/`$`→`$`, `'''`→`''`, `''\x`→unescaped,
lone `'`→`'` set false; `${` opens an antiquotation, `''` closes.  The
`ind_string_parts` grammar assembles `IndStringSegment`s and calls
`stripIndentation`.  Fixed a latent `stripIndentation` bug (it passed
`pos` into the `forceString` arg slot; now explicit `forceString=true`,
matching parser-state.hh.upstream:428).  Still `%expect 0`.

Validated by the tier4b battery (9 fixtures) — total parser-test
now **123/123** byte-equal to TW: `''foo''`, empty `''''`, multiline
dedent (uneven indent), the `''$`/`'''` escapes, `${x}` interpolation
(single + multiline).  This completes the strings story (plain +
interpolated + indented) and wires the LAST unwired proven helper.

### Stage 1.4 Tier 4c — paths LANDED (deterministic subset)

`./foo` / `/abs` / `~/x` / `<spath>` / interpolated `./a/${x}` — the last
3 flex states (PATH_START / INPATH / INPATH_SLASH), transcribed verbatim
from lexer.l:235-315 (the `yyless(0)` interpolated-path-start rewind +
the `PATH_END` re-parse on the path-terminating char + the trailing-slash
error).  Grammar: `path_start` (PATH/HPATH) + the 3 expr_simple path
productions (`path_start PATH_END`, interpolated, SPATH→`__findFile`).

**Resolution split** (a string-buffer spike has no source-file basePath
/ $HOME — per the agreed design):
* ABSOLUTE paths (`/foo`) + `<spath>` resolve deterministically NOW —
  `makePath`/`canonAbs` (CanonPath: `/a/./b`→`/a/b`, `/a/b/../c`→`/a/c`)
  and SPATH→`(__findFile __nixPath "…")`, golden-tested vs TW.
* RELATIVE (`./foo`) + HOME (`~/x`) paths LEX + PARSE correctly (stored
  as literals — verified no-crash) but their RESOLUTION is DEFERRED to
  integration, where a real file supplies basePath.

Validated by the tier4c battery (9 fixtures) — total parser-test
now **132/132** byte-equal to TW: abs simple/multi/deep, CanonPath
`.`/`..` collapse, `<nixpkgs>`/`<a/b>`, abs-interpolated `/foo/${x}`,
path-as-application-arg.  The trailing-slash cases `/foo/` + `/a//b` are
TW parse errors — the INPATH_SLASH rule reproduces that rejection.

### Stage 1.4 Tier 4d — pipe / cursed-or / let{} LANDED (grammar COMPLETE)

The remaining expr-grammar warts:
* pipe operators `|>` / `<|` (parser.y:287-294) — `a |> b` = `b a`,
  `a <| b` = `a b`, via `makeCall`.  Lexer emits PIPE_INTO/PIPE_FROM
  ungated (feature policy is an integration concern).
* cursed-or (parser.y:341, NixOS/nix#11118) — `expr_simple OR_KW` →
  `Call(f, [Var("or")])`; the ambiguity WARNING is a diagnostic, omitted
  (AST/show() identical).
* `let { … }` (parser.y:386) — desugars to `(rec { … }).body`.

Validated by the tier4d battery (9 fixtures) — total parser-test
now **141/141** byte-equal to TW: `a |> b`→`(b a)`, `a <| b`→`(a b)`,
pipe chains both directions, `f or`→`(f or)`, `let { body = 1; }`→
`(rec { body = 1; }).body`.  **The expr grammar surface is now complete**
(all 27 AST node kinds + all 7 flex states + all 3 ParserState helpers).

### Stage 1.5 — INTEGRATION (real-file validation) LANDED

`cli/v3-parse.cc` parses a real .nix FILE with the v3-native parser and
prints `show()`, byte-comparable to `nix-instantiate --parse`.  Parsing
a file supplies the source-file `basePath` (+ `$HOME`), so RELATIVE
(`./foo`) and HOME (`~/foo`) path literals now resolve byte-equal to TW
(the piece Tier 4c deferred).  `ParserState` gained `basePath`/`homePath`
and `makePath` resolves against them.

`test/run-parser-sweep.sh` sweeps the lang corpus: **MATCH 260 / DIFF 0 /
V3-FAIL 0** over the 273 parseable files (the 5 TW-FAIL + 2 parse-fail
residuals are all `undefined variable` = bindVars, which `v3-parse` does
not run — that is Stage 2 / lowering); **39/41 parse-fail correctly
rejected** (the 2 remainders are bindVars too).

The sweep surfaced + fixed **6 parser correctness gaps** the synthetic
fixtures never exercised — all now regression-guarded by the tier5
battery (**parser-test = 151/151**):
1. **block comments** `/* … */` (incl. `/** doc */`) — lexer.l:343
2. **URL literals** `http://…` → string — the `URI` token (lexer.l:316)
3. **string-keyed inherit** `inherit (e) "x"` / `${"x"}` — `attrs
   string_attr` (static→name, dynamic→TW's "not allowed in inherit")
4. **inherit-from merge** — the InheritedFrom `fromIdx` shift on attrset
   merge (`{ m={inherit(y)d;}; m={inherit(x)c;}; }` keeps the two
   sources distinct)
5. **non-ASCII bytes** — `return (unsigned char) yytext[0]` (lexer.l:348)
   so high bytes aren't negative token values bison reads as EOF
6. **dynamic-in-let** — `${e}` in `let` is a TW parse error
   (parser.y:270); `${"a"}` (static) stays allowed

### Remaining (Stage 2)

bindVars (variable→De-Bruijn resolution) + collapse the v3 AST to IR
(replacing lower.cc's `nix::Expr` path); rename v3-parser → v3-parser;
optional opt-in via `NIX_V3_NATIVE_PARSER=1` in v3-eval.
* Then: wire into `v3-eval --parse` behind `NIX_V3_NATIVE_PARSER=1`;
  validate the 68 fixtures + 263-file sweep + 143 lang tests; rename
  v3-parser → v3-parser; retire the throwaway arithmetic framing.

## How to inspect upstream actions

Look at `parser.y` lines 222-680 — every production has a `{ ... }` action block.  Each action does some combination of:

* `state->exprs.add<ExprXxx>(...)` — allocate a new `nix::Expr` subclass in `state->exprs`'s PMR arena
* `state->symbols.create(stringtoken)` — intern a symbol
* `state->at(yylhs.location)` — convert lexer location to PosIdx
* `state->positions[...]` — read position table
* `SET_DOC_POS(node, @loc)` — record doc-comment position

These are the API points to swap to v3 equivalents.

## Acceptance criteria (Stage 1)

Per `PARSER_PROJECT_PLAN_2026-06-01.md §2.2`:

* Byte-equal AST shape on full nixpkgs `legacyPackages` (≥10K packages)
* Byte-equal AST shape on 320-case lang corpus + parse-okay/parse-fail goldens
* 143/143 lang tests PASS under v3-parser opt-in
* Byte-equal drvPath on hello / firefox / python3 / HNE / M5
* Byte-equal eval output on 144 eval-okay lang cases

## Falsification criteria

* Any nixpkgs AST divergence that isn't position-info-only → FALSIFY (specific case not handled)
* Wall regression > 30 % per .nix parse → FALSIFY (parser perf concern)
* Lang-test regression under v3-parser opt-in → FALSIFY

## Pre-committed thresholds

Per `PARSER_PROJECT_PLAN §8`:

* Stage 1 max calendar duration: 14 weeks
* Stage 1 SHIP gate pass rate: 100%
* Stage 1 AST byte-equal rate on nixpkgs ≥10K: 99.99%
* Stage 1 acceptable parser wall regression: ≤ 30%
* Stage 2 max calendar duration: 6 weeks
* Stage 2 LoC deletion target: ≥ 2K LoC retired from lower.cc

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
