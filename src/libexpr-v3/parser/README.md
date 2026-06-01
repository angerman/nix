# v3-native parser (in-progress)

**Project plan:** [`../lode/PARSER_PROJECT_PLAN_2026-06-01.md`](../lode/PARSER_PROJECT_PLAN_2026-06-01.md)
**Status:** SCAFFOLDING — files copied from upstream as Stage 1 starting point.  Action rewrite NOT yet begun.

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
