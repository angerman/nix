# v3-native parser project plan

**Date:** 2026-06-01
**Per:** user directive "pivot to the .nix → v3 ast parser" + synthesis correcting the original DEFER recommendation to a 12-16 week scope via grammar reuse
**Status:** ACTIVE PROJECT — multi-month strategic effort.  Replaces FFI_KILL_TODO Tier 5 (T5.1-T5.3) which were placeholder items.
**Predecessor docs:** `NATIVE_PARSER_FEASIBILITY_2026-06-01.md` §1.5 (grammar-reuse correction), `T4_1_TW_RETENTION_AUDIT_2026-06-01.md` (the blocker behind Tier 4)
**Pre-committed scope:** 12-16 weeks total (Path A two-stage); Path B (one-stage direct-to-IR) explicitly NOT recommended

---

## 0. The end state

After completion:

* **`.nix` source → v3 IR directly**, no TW `nix::Expr` in the pipeline
* **TW's parser state retired** (`mem.exprs` arena, `symbols`, `positions`, `positionToDocComment`) — per `T4_1_TW_RETENTION_AUDIT_2026-06-01`
* **`lower.cc`'s 3,446 LoC `nix::Expr`-consuming code path deleted**
* **PosIdx becomes file-local** (file-relative spans) → bytecode disk cache content-addressable by source → reproducible-builds + AOT cache distribution + cross-process determinism

The v3 evaluator's parser-side FFI surface drops to ZERO crossings into TW.  TW remains in the binary only for store/fetcher/path-realisation leaves.

---

## 1. Strategic framing

### 1.1 Why this is the right pivot NOW (user synthesis)

Per the FFI audit summary § "What's next":

* **Tier 1 cleanups are exhausted** post Phase 1-3 (T1.3, T1.4, install-bridge elimination).  All structurally-eliminable v3→TW sites in the histogram have been retired.
* **Tier 2 (lazy result) and Tier 4 (TW release) are BOTH blocked behind v3-native parser.**  T2.2/T2.3 are high-risk on the current bridge surface; T4.1-T4.6 require eliminating TW from the parse pipeline.
* **Grammar reuse cuts the original 3-6 month estimate to 10-16 weeks.**  Bison grammar is declarative; the actions are mechanical.

### 1.2 Why grammar reuse, not from-scratch

Per the user's correction to the original synthesis:

| Keep from upstream | Change to v3 |
|---|---|
| `parser.y`'s 51 productions + 19 nonterminals | Action bodies inside `{ ... }` blocks |
| 14 precedence levels + `%expect 0` LALR(1) | `ParserState` helpers (Symbol/Pos tables) |
| `lexer.l`'s 7 Flex states + antiquotation push/pop | Build integration into v3 binary |
| Path syntax + indented strings + cursed-or wart | — |

Grammar correctness investment (decades of TW + "thousands of core hours of fuzzing" for Lix's PEGTL alternative) is made free.

### 1.3 Why NOT PEGTL

Per `NATIVE_PARSER_FEASIBILITY_2026-06-01 §1.5`: PEGTL's measured 20% parser-perf win is a small fraction of total eval wall; the build-tool / modern-idiom benefits don't justify throwing away nixpkgs-validated LALR grammar.  PEG semantics ≠ LALR makes it easy to write a subtly-different grammar without realizing.

### 1.4 Why Path A (two-stage) over Path B (direct)

* Path A (Stage 1 v3-AST emission + Stage 2 collapse to IR): 12-16 weeks; **intermediate validation surface** (byte-compare v3 AST vs TW AST)
* Path B (one-stage direct-to-IR): 10-14 weeks; HIGH risk — no intermediate surface; any IR-shape divergence is hard to debug without an AST to compare

Path A's Stage 1 v3-AST is scaffolding that gets DELETED at end of Stage 2.  Net deliverable is identical.

---

## 2. Stage 1 — parser.y action rewrite emitting v3 AST (8-10 weeks)

### 2.1 Sub-stages

| # | Sub-stage | Effort | Gate |
|---|---|---|---|
| 1.0 | **Test infrastructure** (8-10 days per `NATIVE_PARSER_FEASIBILITY §3.4` corrected) | 8-10 d | AST-shape diff harness + ≥20 position-info goldens + operator precedence battery in place |
| 1.1 | **v3 AST class hierarchy** (`include/v3/ast/expr.hh` and friends) | 5-7 d | header compiles; 27 Kind discriminants mirror nix::Expr |
| 1.2 | **v3 ParserState** (Symbol/Pos table adapters) | 5-7 d | API surface identical to `state->exprs.add<...>` / `state->symbols.create` / `state->at(loc)` |
| 1.3 | **Parser.y + lexer.l fork into v3 tree** | 2-3 d | identical Bison output; v3-flavored linker symbols |
| 1.4 | **Action rewrite — atoms + literals** (ExprInt/Float/String/Path/Var) | 3-5 d | parse-okay-int / parse-okay-string golden round-trip |
| 1.5 | **Action rewrite — composites** (Attrs, List, Lambda, Call, Let, With, If, Assert) | 10-14 d | full 320-lang corpus AST byte-equal |
| 1.6 | **Action rewrite — operators** (14 precedence levels, OpEq, OpUpdate, OpConcat, OpImpl, OpAnd, OpOr, OpNot, OpHasAttr, ConcatStrings) | 7-10 d | operator-precedence battery (~80 fixtures) green |
| 1.7 | **Indented string + path + antiquotation** (the HIGH-complexity §2.2 features) | 7-10 d | parse-okay-indent and parse-okay-path goldens green |
| 1.8 | **Doc-comments + cursed-or + REPL bindings + error reporting** | 5-7 d | parse-fail-* goldens byte-equal |
| 1.9 | **Full-nixpkgs sweep validation** (≥10K packages) | 3-5 d | byte-equal-AST across nixpkgs |

**Total Stage 1: 55-78 calendar days = 8-11 weeks at 1 engineer + buffer.**

### 2.2 Stage 1 SHIP gate (pre-committed)

* **Byte-equal AST shape on full nixpkgs `legacyPackages`** (≥10K packages) under v3-parser path
* **Byte-equal AST shape on 320-case lang corpus** + parse-okay/parse-fail goldens
* **143/143 lang tests** PASS under v3-parser opt-in path (`NIX_V3_NATIVE_PARSER=1`)
* **byte-equal drvPath** on hello / firefox / python3 / HNE / M5
* **byte-equal eval output** on the 144 eval-okay lang cases

### 2.3 Stage 1 falsification criteria

* Any AST shape divergence on the full-nixpkgs sweep that isn't a position-info-only difference → FALSIFY (specific case not handled)
* Wall regression > 30 % on a single .nix parse → FALSIFY (parser perf concern)
* Lang-test regression on any of the 320 cases (under v3-parser opt-in) → FALSIFY

### 2.4 Stage 1 gate is opt-in throughout

The new parser ships behind `NIX_V3_NATIVE_PARSER=1`.  Default OFF.  TW parser remains the production path through Stage 1.  Flip the default only when the SHIP gate is met AND there has been a 1-week soak period under the opt-in flag across multiple workloads.

---

## 3. Stage 2 — collapse v3 AST to direct IR emission (4-6 weeks)

### 3.1 Sub-stages

| # | Sub-stage | Effort | Gate |
|---|---|---|---|
| 2.1 | **Identify lower.cc paths consumed by v3 AST** (the 3,446 LoC nix::Expr-consuming code) | 2-3 d | dependency map produced |
| 2.2 | **Replace v3 AST emission with `ir::Module` emission in actions** | 10-14 d | parser actions emit IR; v3 AST hierarchy stops being instantiated |
| 2.3 | **Retire v3 AST hierarchy + the lower.cc paths it consumed** | 3-5 d | -3K LoC deletion |
| 2.4 | **Stage 2 SHIP gate re-run** | 2-3 d | same as Stage 1 |

**Total Stage 2: 17-25 days = 3-5 weeks at 1 engineer + buffer.**

### 3.2 Why Stage 2 isn't risky after Stage 1

* Stage 1 validated parser correctness (AST byte-equal)
* The v3 AST → ir::Module walker already exists in `lower.cc` (it processes nix::Expr today; the v3-AST equivalent is a near-mechanical copy)
* Collapsing the AST→IR step into the parser actions is local refactor work
* TW parser remains in tree throughout (escape hatch via env var)

### 3.3 Stage 2 SHIP gate (pre-committed)

Identical to Stage 1 SHIP gate (§2.2).  No new criteria — Stage 2's deliverable is internal restructuring with no observable change.

---

## 4. Test infrastructure (pre-Stage-1, ~8-10 days)

Per `NATIVE_PARSER_FEASIBILITY §3.4` corrected estimate.  These are the additions REQUIRED before Stage 1.1 can start (because validation depends on them):

| # | Build | Effort | Why |
|---|---|---|---|
| TI.1 | AST-shape pretty-printer + differential runner | 3-4 d | byte-diff v3 AST vs TW AST on the 54 parse-okay/parse-fail goldens |
| TI.2 | `v3-eval --parse` flag + IR-fixture harness | 1-2 d | mirror `nix-instantiate --parse` |
| TI.3 | ≥20 position-info golden fixtures | 2-3 d | the existing 6 are anemic; need positions for if/let/lambda/with/assert/tryEval |
| TI.4 | Operator precedence battery (~80 fixtures) | 1-2 d | currently ZERO dedicated tests |
| TI.5 | Full-nixpkgs parse-sweep harness (extends 72-pkg sweep to ≥10K) | 1 d | run mode + diff mode |

**Total TI: 8-12 days.  Bundled into Stage 1.0.**

### 4.1 What we DO NOT need to build first

Per the user's correction: TW remains as oracle.  The existing 144-case lang differential + 72-package nixpkgs sweep + 320 gtest + property fuzzer all continue working through v3-parser opt-in.  We don't need a "from-scratch oracle".

---

## 5. Code organization

### 5.1 Where the new code lives

```
src/libexpr-v3/
├── parser/
│   ├── parser.y           ← copied from src/libexpr/parser.y, actions rewritten
│   ├── lexer.l            ← copied from src/libexpr/lexer.l, mostly unchanged (state machine reusable)
│   ├── parser-state.hh    ← v3 ParserState (port of TW's)
│   └── parser-state.cc    ← stripIndentation port + addAttr port + validateFormals port
├── include/v3/
│   ├── ast/
│   │   ├── expr.hh        ← v3 AST class hierarchy (Stage 1 scaffolding)
│   │   ├── ast-builder.hh ← state->exprs.add<...> equivalent
│   │   └── ast-fwd.hh     ← forward declarations
│   └── parser.hh          ← public API (parseExprFromFile, parseExprFromString)
└── parser.cc              ← integration into v3 (replaces TW's parseExprFromFile calls)
```

### 5.2 Build integration

Per `parser.y` requirements:
* Bison 3.x (LALR1.cc skeleton — same as upstream)
* Flex (reentrant + bison-bridge + bison-locations + stack + extra-type)
* Meson `custom_target` rules mirroring `src/libexpr/meson.build`

Build wiring adds ~50 lines to `src/libexpr-v3/meson.build`.

### 5.3 The 6 v3-side call sites that consume the new parser

Per `NATIVE_PARSER_FEASIBILITY §2.3`:

| Site | File:line | Today |
|---|---|---|
| 1 | `primops.cc:8738/8740` | `ns.parseExprFromFile` (scopedImport legacy) |
| 2 | `primops.cc:9207/9209` | `ns.parseExprFromFile` (primImport main) |
| 3 | `primops.cc:10042` | `ns.parseExprFromString` (scopedImport synthetic) |
| 4 | `v3_call_flake.cc:127` | `ns.parseExprFromString` (callFlakeV3) |
| 5 | `bytecode_primops.cc:172` | `ns.parseExprFromString` (bytecode primop installer) |
| 6 | `cli/v3-eval.cc:255` | `ns.parseExprFromFile` (v3-eval CLI) |

Each site needs to dispatch: `if (NIX_V3_NATIVE_PARSER) v3::parser::parseExpr*(...)  else ns.parseExpr*(...)`.

Stage 1: opt-in via env var.  Stage 2: flip default.  Long-term: delete TW path.

---

## 6. Risks + mitigations

| Risk | Likelihood | Mitigation |
|---|---|---|
| Bison version drift (v3 parser uses different Bison than TW) | LOW | Pin Bison version in flake.nix; mirror upstream pin |
| Position-info byte-equal divergence | MEDIUM | Accept divergence per `NATIVE_PARSER_FEASIBILITY §2.4` — TW PosIdx isn't cache-stable anyway; v3 uses file-relative spans |
| Indented-string stripping bug (the HIGH-complexity feature) | MEDIUM | Port `stripIndentation` verbatim from upstream; add ≥50 indented-string fixtures |
| Cursed-or wart misimplementation | LOW | Copy upstream behavior 1:1; flag with comment |
| Doc-comment positioning drift | LOW | Verify with the 6 existing doc-comment-positioning tests |
| Operator precedence subtle bug | HIGH-impact / LOW likelihood | TI.4 operator-precedence battery (~80 fixtures) is the primary defense |
| Antiquotation depth bug | MEDIUM | Add ≥20 nested-antiquotation fixtures |
| Stage 2 IR-emission walker has subtle differences from lower.cc | MEDIUM | Bring lower.cc behavior over verbatim; AST byte-equal validation continues to apply |
| **Opportunity cost vs active memory work** | STRONG | This is a PIVOT — accept that the M5 watchdog work pauses |
| Parser bugs cause silent corruption (cf. #682 toFile, #670-671 dangling string_view) | MEDIUM | Mandatory full-nixpkgs sweep before Stage 2 default-flip + week-long soak |
| Future Nix syntax extensions (pipe operators differ across forks) | MEDIUM | Track upstream parser.y; cherry-pick changes |

---

## 7. Falsification criteria (per Rule 0)

Per `[[falsification-rule]]`, each milestone must answer "what hypothesis does this kill":

| Milestone | Hypothesis killed if SHIP | Hypothesis killed if FALSIFIED |
|---|---|---|
| TI.1-TI.5 land | "Existing test infra is sufficient for parser validation" — KILLED.  New infra is in place. | If test infra build is harder than 10 days, the whole-project scope shifts. |
| Stage 1.4 (atoms) | "Parser actions can be mechanically rewritten" | If atom-action rewrite reveals architectural complexity, escalate scope. |
| Stage 1.9 (full sweep) | "Grammar reuse delivers byte-equal AST on real workloads" | If any nixpkgs package diverges, isolate + fix or revise scope. |
| Stage 2 SHIP | "v3 can emit IR directly from parser, eliminating lower.cc dependency on nix::Expr" | If Stage 2 reveals AST→IR walker has hidden TW dependencies, scope changes. |

---

## 8. Pre-commit thresholds (measure-twice-cut-once)

Per `[[measure-twice-cut-once]]`:

| Threshold | Value | Set BEFORE |
|---|---|---|
| Stage 1 max calendar duration | 14 weeks | TI.1 start |
| Stage 1 SHIP gate per-test pass rate | 100% | TI.1 start |
| Stage 1 AST byte-equal rate on nixpkgs ≥10K | 99.99% (allows ≤1 in 10K to be position-info-only diff) | TI.1 start |
| Stage 1 acceptable parser wall regression | ≤ 30 % | TI.1 start |
| Stage 2 max calendar duration | 6 weeks | Stage 1 SHIP gate met |
| Stage 2 LoC deletion target | ≥ 2K LoC retired from lower.cc | Stage 1 SHIP gate met |

Threshold recalibration only allowed per `[[threshold-recalibration-rule]]`: original premise must be measurably wrong.

---

## 9. What this kills (per Rule 0, looking forward)

* **"v3-native parser is multi-month at 3-6 months"** — KILLED by grammar reuse: 12-16 weeks.
* **"PEGTL is the right v3 parser technique"** — REFUTED: grammar reuse is strictly better for v3's scope.
* **"TW retention after parse is structurally unavoidable"** (`T4_1_TW_RETENTION_AUDIT`) — KILLED at project completion.  Stage 2 deletes the parse path entirely.
* **"Bytecode disk cache is content-NOT-addressable because PosIdx is build-dependent"** (`NATIVE_PARSER_FEASIBILITY §4.1`) — KILLED at Stage 2.

---

## 10. What this does NOT kill (out of scope)

* The active memory work (M5 watchdog gap) — Tier 2.2/2.3 (BP1/BP2 lazy result) and Tier 1.5 (BP1/BP2 fallback retirement) remain queued.  Parser project does NOT directly close the M5 watchdog.
* The bridge-table retention pattern — bridge tables persist for non-parser FFI (fetchers, store leaves).  Parser project removes parser-side bridges only.
* `cppnix` upstream parser maintenance — Nix syntax extensions in upstream will still need to be tracked.  v3 fork accepts dual-maintenance cost.
* The fake-store path in `primDerivationStrict` (T1.2) — load-bearing for `v3-eval` tests; deferred regardless of parser project.

---

## 11. Engineering bandwidth + scheduling

* **This is a single-track project for one engineer.** ~13 weeks at 1 engineer.  Calendar time 14-16 weeks accounting for normal interruptions.
* **Pauses the FFI Tier 2/3/4 active work for the duration.** That work is queued; resumes post-Stage 2.
* **Memory work can continue in parallel IF a second engineer is available.** Otherwise sequential after Stage 2.

### 11.1 Calendar mapping

```
Week 1-2:    Test infrastructure (TI.1-TI.5)
Week 3-4:    Stage 1.1-1.3 (AST hierarchy + ParserState + parser.y fork)
Week 5-8:    Stage 1.4-1.6 (actions: atoms, composites, operators)
Week 9-10:   Stage 1.7-1.8 (strings, paths, errors)
Week 11:     Stage 1.9 (full-nixpkgs sweep)
Week 12:     Stage 1 SHIP gate + 1-week soak
Week 13-14:  Stage 2.1-2.3 (collapse to IR)
Week 15:     Stage 2 SHIP gate + 1-week soak
Week 16:     Default flip + retirement of TW path
```

---

## 12. Acceptance criteria for THIS plan itself

This plan is a hypothesis like any other.  Per Rule 0, it must answer "what kills me":

* **If TI.1-TI.5 takes > 15 days**, the project's calendar overruns.  Pause + re-scope.
* **If Stage 1.4 atom-action rewrite uncovers architectural issues** (e.g., TW's parser is more entangled with TW's symbol table than the audit predicted), pause + re-investigate before proceeding.
* **If Stage 1.9 full-nixpkgs sweep shows > 0.01% AST divergence** that isn't position-info-only, pause + fix the specific case (do NOT loosen SHIP gate).
* **If the user explicitly pivots back to memory work mid-project**, queue the parser-project state and resume.

---

## 13. Cross-references

### Predecessors
* `NATIVE_PARSER_FEASIBILITY_2026-06-01.md` — feasibility audit + grammar-reuse correction
* `T4_1_TW_RETENTION_AUDIT_2026-06-01.md` — why this project unblocks Tier 4
* `FFI_AUDIT_2026-06-01.md` — empirical FFI surface (what this project eventually retires)
* `FFI_KILL_TODO_2026-06-01.md` — original TODO Tier 5 placeholder
* `FFI_KILL_TODO_STATUS_2026-06-01.md` — FFI work checkpoint at pivot

### Strategic alignment
* `LINKING_DESIGN_2026-05-17.md` — "PosIdx becomes module-local" — design already expects this transition
* `ERROR_UX_DESIGN_2026-05-20.md` — Diagnostic struct integration target for parser errors
* `GC_STRATEGY_INTEGRATED_2026-05-30.md` — parser is Layer 5+ scope (not on critical path for M5)

### Upstream code anchors (the input we're rewriting)
* `src/libexpr/parser.y` — Bison grammar (562 LoC non-comment)
* `src/libexpr/lexer.l` — Flex lexer (277 LoC non-comment)
* `src/libexpr/include/nix/expr/parser-state.hh` — ParserState (319 LoC; stripIndentation:323-429)
* `src/libexpr/include/nix/expr/nixexpr.hh` — AST classes (749 LoC; Kind enum at 109-138)
* `src/libutil/include/nix/util/pos-table.hh:79-87` — `addOrigin` (the not-cache-stable mechanism)

### Methodology
* `[[falsification-rule]]` — Rule 0; every milestone has a kill criterion
* `[[measure-twice-cut-once]]` §3 — pre-committed thresholds
* `[[same-host-bisect]]` — verify drvPath byte-equality on same host before claiming parity
* `[[memory-first-class]]` — parser is wall+determinism lever, NOT primary RSS lever

### External
* Lix Gerrit !1118 — PEGTL parser rewrite (calibration for "drop-in parser is feasible")
* rnix-parser — recursive-descent + rowan (alternative technique reference)

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
