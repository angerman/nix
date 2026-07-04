# v3-native Nix parser feasibility audit

**Date:** 2026-06-01
**Author:** session synthesis (3-agent: TW parser inventory + test suite coverage + existing-parser survey + critical review)
**Status:** STRATEGIC FEASIBILITY ASSESSMENT — feasible (3-6 months) but multi-month opportunity cost against active memory work; primary architectural justification is NOT effort but PosIdx-cache determinism
**Triggering question:** "The team mentioned that they might consider implementing a Nix parser. How hard would this be? Is there a sufficiently strong test-suite to make this feasible?"

Companion docs:
- [`FFI_BRIDGE_INVENTORY_2026-05-31.md`](FFI_BRIDGE_INVENTORY_2026-05-31.md) — current FFI surface (parser is permitted FFI leaf per V3-NATIVE)
- [`T4_1_TW_RETENTION_AUDIT_2026-06-01.md`](T4_1_TW_RETENTION_AUDIT_2026-06-01.md) — TW retention (parser arena contribution)
- [`LINKING_DESIGN_2026-05-17.md`](LINKING_DESIGN_2026-05-17.md) — references PosIdx as module-local in cell representation
- [`GC_STRATEGY_INTEGRATED_2026-05-30.md`](GC_STRATEGY_INTEGRATED_2026-05-30.md) — 6-layer strategy (parser is Layer 5+ scope)
- [`ERROR_UX_DESIGN_2026-05-20.md`](ERROR_UX_DESIGN_2026-05-20.md) — Diagnostic design (parser must integrate)

---

## 1. TL;DR

**Feasible:** REVISED 2026-06-01 (post-user-pushback) — **reuse `parser.y` + `lexer.l` grammar; rewrite only the actions to emit v3 IR.** This is dramatically cheaper than a from-scratch parser:
- **Path A (cautious, two stages): 12-16 weeks** — Stage 1 (8-10 wk) parallel v3 AST emission; Stage 2 (4-6 wk) collapse to direct IR emission
- **Path B (direct): 10-14 weeks** — actions emit `ir::Module` directly, skip AST entirely; HIGH risk because no intermediate surface for validation
- **Original from-scratch estimate (3-6 months / Lix's PEGTL 6 months) is moot** — reusing parser.y means 6 months of grammar-correctness work is free

**Test sufficiency: TW remains as oracle** (Agent B's "oracle dies" framing was wrong; TW coexists with v3 in the same binary). Existing 144-case lang differential + 72-package nixpkgs sweep + 320 gtest continue working with parser-path opt-in. New test infra: **~8-10 days** (AST-shape diff harness + position-info coverage + operator precedence battery), NOT the 22-35 days I previously claimed.

**Test sufficiency: PARTIAL.** Every existing differential test in the v3 stack uses TW's parser as the oracle (144-case lang sweep + 72-package nixpkgs drvPath sweep + 320 gtest + property fuzzer). **Replacing TW kills the oracle.** Before any parser swap can be validated, the team needs **22-35 engineering-days of new test infrastructure** (AST-shape differential runner, position-info golden suite, differential fuzzer, error-message golden tests, full-nixpkgs parse sweep).

**Strategic positioning:** the strongest argument for a native parser is **NOT effort feasibility** — it is that **TW's PosIdx is not cache-stable across builds** (depends on `addOrigin` load-order). v3's bytecode disk cache key today involves PosIdx-bearing structures, making the cache content-NOT-addressable by source. A v3-native parser emitting file-relative spans makes the cache content-addressable. This is an architectural unlock independent of effort/test concerns.

**Opportunity cost:** 3-6 months is a strategic pivot away from the active memory work (M5 watchdog gap ~1.7 GB; 11 GC/memory falsifications in 3 weeks; derivationStrict native completion projected to eliminate 311 MB HNE retention at source). Parser work yields determinism + reduced FFI surface; it does NOT directly close the M5 watchdog.

**Recommendation (REVISED 2026-06-01):** the grammar-reuse path makes this competitive with the active memory work. **Path A (12-16 weeks)** is approximately the same calendar time as the GC strategy's Week 6 M5-watchdog target. Strategic case for proceeding strengthens if (a) bytecode-disk-cache determinism is valued, (b) full TW retention elimination (per T4_1_TW_RETENTION_AUDIT) is on the roadmap, or (c) the end goal of `.nix → v3 IR` collapse (Option c, partial deletion of lower.cc's 3,446 LoC) is strategic. The from-scratch / PEGTL alternative is now strictly dominated by grammar reuse.

## 1.5 Section added 2026-06-01 — grammar reuse correction

The user pushed back on the synthesis with two points: (a) reuse the existing parser.y / lexer.l rather than rewrite the grammar; (b) TW remains as a functional oracle since it coexists with v3 in the same binary.

Both pushbacks are correct. The original synthesis treated this as a from-scratch parser problem; that was the wrong framing.

**What we keep from TW (Bison grammar reuse):**
- `parser.y`'s 51 productions + 19 nonterminals + 14 precedence levels + `%expect 0` LALR(1) cleanliness
- `lexer.l`'s 7 Flex states + antiquotation push/pop + path lexer modes + indented-string handling
- The entire grammar-correctness investment (decades for TW; "thousands of core hours of fuzzing" for Lix's PEGTL alternative — both made free by reuse)

**What we change:**
- Actions inside `{ ... }` blocks in parser.y: instead of `new ExprAttrs(...)` → emit v3 AST/IR node directly
- `ParserState` helpers: swap TW Symbol/Pos tables for v3 equivalents
- Build system: link bison/flex output into v3 binary (already happens in the same repo)

**Estimated action rewrite:** 51 productions × ~30 LoC = ~1.5 KLoC mechanical C++. ParserState v3-side: ~300 LoC. **6-10 weeks for Stage 1 (parallel AST emission); +4-6 weeks for Stage 2 (collapse to IR).**

**On PEGTL vs grammar reuse:** PEGTL wins on (a) no external build-tool, (b) header-only modern C++ idiom, (c) ~20% measured parser perf (Lix). Loses on (a) PEG semantics ≠ LALR (easy to write subtly-different grammar), (b) slow template compile time, (c) no declarative precedence, (d) weaker error recovery. **For v3 specifically: grammar reuse is strictly better than PEGTL.** PEGTL's perf advantage (20% parser wall) is a small fraction of total eval wall; the build-tool / modern-idiom benefits don't justify throwing away nixpkgs-validated grammar.

**On TW-as-oracle:** Agent B's claim that "all existing differential tests use TW as oracle, so a new parser invalidates the oracle" was overstated. TW coexists with v3 in the same binary; v3 ships TW as the FFI-leaf evaluator. The existing 320 gtest + 144 lang differential + 72-package nixpkgs sweep + property fuzzer all continue working with a parser-path opt-in. New differential test infrastructure for parser swap: **~8-10 days** (AST-shape diff harness + ~20 position-info golden fixtures + operator-precedence battery), NOT the 22-35 days originally estimated.

**Updated SHIP gate (Stage 1):** byte-equal-drvPath on full nixpkgs `legacyPackages` (≥10K packages) under v3-parser path. byte-equal-AST on 320 lang corpus + parse-okay/parse-fail goldens. Differential fuzzer optional (existing nixpkgs corpus is the strongest signal given TW oracle).

**Updated SHIP gate (Stage 2):** v3-parser-emits-IR-directly; lower.cc's nix::Expr-consuming code path retired in stages; SHIP gate identical to Stage 1.

**End goal achievement:** Path A two-stage reaches `.nix → v3 IR` collapse in 12-16 weeks. Path B one-stage reaches it in 10-14 weeks but at HIGH risk (no intermediate surface for validation).

---

## 2. How hard? — code-verified effort estimate

### 2.1 TW parser surface (Agent A inventory)

| File | LoC (raw) | LoC (non-comment) | Role |
|---|---|---|---|
| `parser.y` | 683 | ~562 | Bison grammar (LALR1.cc, C++ skeleton) |
| `lexer.l` | 365 | ~277 | Flex lexer (reentrant, 6 stack states) |
| `parser-state.hh` | 441 | ~319 | ParserState, indentation stripping, addAttr, validateFormals |
| `nixexpr.hh` | 1024 | ~749 | AST node hierarchy (27 Kind discriminants) |
| `pos-table.hh` + `pos-idx.hh` | 192 | ~136 | PosIdx/PosTable opaque types |
| **TOTAL parsing surface** | **~2.8 KLoC** | **~1.4 KLoC** | |

**Bison rules:** 19 nonterminals, ~51 productions. `%expect 0` (no shift/reduce conflicts), no `%glr-parser`, no mid-rule actions.

**Flex states:** 7 — `DEFAULT`, `STRING`, `IND_STRING`, `INPATH`, `INPATH_SLASH`, `PATH_START`, `REPL_BINDINGS_MODE`. Uses `%option stack` for nested push/pop.

**AST node count:** 27 kinds — Int, Float, String, Path, Var, InheritFrom, Select, OpHasAttr, Attrs, List, Lambda, Call, Let, With, If, Assert, OpNot, OpUpdate, ConcatStrings, Pos, BlackHole, OpEq, OpNEq, OpAnd, OpOr, OpImpl, OpConcatLists.

### 2.2 Hard parts of Nix grammar (Agent A)

10 features ranked by replication difficulty:

| # | Feature | Complexity | Why |
|---|---|---|---|
| 1 | Antiquotation `${...}` in strings + paths | HIGH | Lexer state-machine couples to parser; can't be done by simple regex |
| 2 | Indented strings `''` | HIGH | ~110 LoC `stripIndentation()` algorithm |
| 3 | Path syntax (PATH/HPATH/SPATH/URI) | HIGH | Disambiguates with identifiers + arithmetic; mid-path interpolation |
| 4 | Operator precedence (14 levels, `!` + `->` quirks) | MEDIUM | Mechanical; `-x` and `<` lowered to primop calls |
| 5 | Dynamic attribute keys | MEDIUM | `${expr}.value` + `DynamicAttrDef`; `ToBeStringyExpr` deferred-allocation |
| 6 | `a.b.c or default` + cursed-or wart | MEDIUM | ~50 LoC backwards-compat for nixpkgs `or` function |
| 7 | Float vs int disambiguation | LOW | Well-defined regex |
| 8 | Identifier syntax (hyphens, primes) | LOW | Falls out of Flex longest-match |
| 9 | Comments + doc comments | MEDIUM | Doc-comment positioning attaches to lambdas |
| 10 | REPL bindings mode | LOW | Single-shot start-token injection |

### 2.3 v3's parser-invocation surface — just 6 call sites

| Site | File:line | Entry | Purpose |
|---|---|---|---|
| 1 | `primops.cc:8738,8740` | `parseExprFromFile` | scopedImport legacy fallback |
| 2 | `primops.cc:9207,9209` | `parseExprFromFile` | primImport main parse (after disk-cache miss) |
| 3 | `primops.cc:10042` | `parseExprFromString` | scopedImport synthetic wrapper |
| 4 | `v3_call_flake.cc:127` | `parseExprFromString` | callFlakeV3 (in-memory call-flake.nix) |
| 5 | `bytecode_primops.cc:172` | `parseExprFromString` | bytecode-primop installer |
| 6 | `cli/v3-eval.cc:255` | `parseExprFromFile` | v3-eval CLI top-level |

The pattern is uniform across all 6: parse → `bindVars` → `lowerNixExpr(e, ns.symbols, ns.positions)` → `ir::optimise` → `ir::computeFreeVars` → `compile` → `run`. v3 NEVER constructs `nix::Expr` directly; everything flows through TW's parser.

### 2.4 Position information coupling (KEY FINDING)

**Per Agent A:** PosIdx parity is NOT required for v3. Every `lower.cc` consumer of position info goes through `PosTable::operator[]` (`lower.cc:746, 2277, 2299, 2311, 3435`) which produces `{file, line, column}`, then v3 hashes that triple through `recordPosSnapshot` (`lower.cc:763`) into a separately-interned pool.

**Per Agent C:** TW's PosIdx is already **NOT cache-stable across builds**. `PosTable::addOrigin` (`pos-table.hh:79-87`) assigns each origin a `uint32_t offset` equal to cumulative size of all previously-loaded origins. Different import orderings → different PosIdx values for the same source position.

**Synthesis:** PosIdx is build-dependent within TW itself. A v3-native parser doesn't need to match it. The contract is just: "produce a `nix::Expr` whose `getPos()` resolves through some `PosTable` to the right `{file, line, column}`."

This finding alone drops 1-2 weeks from a naïve estimate that assumed PosIdx parity was required.

### 2.5 Effort estimate breakdown (Agent A)

For drop-in `nix::Expr`-producing v3-native parser:

| Component | LoC | Notes |
|---|---|---|
| Recursive-descent / Pratt parser | 1,500-2,200 | Mirrors 51 Bison productions; Pratt for operators |
| Hand-rolled lexer | 700-1,000 | 7-state stack; ~30 token kinds |
| Position info (PosTable wrapper + line tracking) | 200-300 | Track inline, sidestepping on-demand recompute |
| Indented string stripping | 100-150 | Port of ParserState |
| Error reporting + recovery | 300-500 | Panic-mode sync per parser.y:124-134 |
| nix::Expr construction shims | 200-400 | PMR allocator integration, vtable stubs |
| **First working version subtotal** | **3,000-4,500 LoC** | |
| Doc-comments + cursed-or + REPL mode + lints | 200-400 | Edge-case parity |

**Person-weeks:**
- First working version (parses 95% of nixpkgs): 4-6 weeks
- Production-hardened (byte-identical AST on 100% of nixpkgs + error message parity within trivial whitespace): +6-10 weeks
- **Total: 10-16 person-weeks** for a drop-in replacement

### 2.6 Lix precedent (Agent C — the load-bearing calibration)

Lix shipped a PEGTL parser rewrite (Gerrit !1118) in **~6 months by lachrimae (single primary author)** with:
- ~1.5K LoC PEGTL grammar replacing 683-line parser.y + 365-line lexer.l
- **Byte-identical AST** to Bison parser (validated by "thousands of core hours of fuzzing")
- 20% pure parsing speedup; 4-10% full-eval speedup

This is the strongest evidence the team's effort estimate is realistic. The Lix case falsifies "years of bug parity" worry.

### 2.7 Other implementations (Agent C survey)

| Project | LoC | Technique | Effort | Notes |
|---|---|---|---|---|
| TW (cppnix) | ~2.0K | Bison + Flex | accreted ~16 yrs | reference |
| **Lix** | ~1.5K | **PEGTL** | **~6 months** | byte-identical AST; 20% perf win |
| **rnix-parser** (Tvix) | ~1.86K | recursive-descent over rowan | ~3mo to v0.1, 7+ yrs to v0.14 | lossless CST; documented cppnix divergences |
| Tvix eval | uses rnix | n/a | ~12 mo + 12 mo parity | "Tvix currently ignores [parser] test cases" |
| hnix-parser | ~1K (Haskell) | Megaparsec | ~6mo (Wiegley initial) | known semantic gaps |
| nixel | thin wrapper | FFI to cppnix | weeks | preserves comments/whitespace |
| nixfmt-rs parser | ~K LoC Rust | hand-written recursive-descent | ~3-6 months | chose NOT to use rnix; cited "easier-to-read errors" |

---

## 3. Test suite sufficiency

### 3.1 What exists (Agent B inventory)

**`tests/functional/lang/` corpus**: 320 `.nix` files
- `parse-okay-*`: **13** (parser-only positive, with golden AST)
- `parse-fail-*`: **41** (parser-only negative, with golden error)
- `eval-okay-*`: 144 (parse + eval, stdout golden)
- `eval-fail-*`: 116 (parse + eval, error golden)

**Avg size**: 306 B; median 84 B. Tests are tiny snippets, not stress tests.

**v3 differential testing (all 7 mechanisms):**
- `run-cutover-parity-tests.sh:38-95` — every `lang/eval-okay-*.nix` through TW + v3, byte-compare stdout (~144 cases)
- `run-lang-tests.sh` — `v3-eval` direct vs `.exp` golden
- `run-759-nixpkgs-drvpath-sweep.sh` — **72 nixpkgs packages**, drvPath byte-compare
- `property/property_tests.py` — TW vs `NIX_V3_DIRECT_EVAL=1` random-input parity per primop class
- `derivation-parity.sh`, `run-cutover-tests.sh`, `run-drv-parity.sh`, `run-direct-eval-tests.sh`
- 320 gtest unit tests (`src/libexpr-tests/`) — `eval(...)` Nix strings + value-shape assertions
- IR-fixture FileCheck suite (`test/ir-fixtures/`, 20 fixtures)

### 3.2 The critical finding (Agent B headline)

**ALL existing differential infrastructure uses TW's parser as the oracle.** The 320 gtest, 144 lang cases, 72-package nixpkgs sweep, property fuzzer, IR fixtures — every one calls `parseExprFromString` from `src/libexpr/parser.y`. **A new parser invalidates this oracle relationship.** TW + v3 agreeing today proves nothing about a third parser.

### 3.3 Edge case gaps (Agent B audit)

Likely UNTESTED corners:
1. **Antiquotation depth** — `"${"${c}"}"`. No `*antiq*` test files exist.
2. **Operator precedence corners** — 14 precedence levels, ZERO dedicated precedence tests. `a // b // c` right-assoc, `-a ? b`, `!a ? b`, `a == b == c` nonassoc — none covered.
3. **Path tokens** — `~/foo/bar`, `<nixpkgs/lib>`, multi-component spath. Only negative tests exist.
4. **Float literals** — only ONE `eval-okay-float.nix`.
5. **Indent edge cases** — tabs/spaces mixed, leading-tab-only, BOM, CRLF-in-indented-string.
6. **Dynamic attrs** — all 5 forms (`{ ${x} = ... }`, `let ${x} = ... in`, `attrs.${x}`, `attrs ? ${x}`, `inherit (s) ${x}`). Only REJECTION tested.
7. **Position info** — only 6 `unsafeGetAttrPos`/`__curPos` tests. None cover positions inside `if`/`let`/`lambda`/`with`/`assert`/`tryEval`.
8. **String escapes** — only `\$`/`\${` partially tested. Full matrix missing.
9. **Mid-line column accuracy** — only EOF position tested.
10. **Comments** — single test; nested `/* /* */ */` (which Nix doesn't nest) lexer race not tested.

### 3.4 Required new infrastructure (Agent B)

For shipping a new parser with confidence:

| # | Build | Effort | Why |
|---|---|---|---|
| 1 | AST-shape pretty-printer + differential runner | 3-5 d | byte-diff vs TW; reuse `parse-okay-*.exp` |
| 2 | `v3-eval --parse` flag + IR-fixture harness | 2 d | mirror `nix-instantiate --parse` |
| 3 | **Position-info golden suite** (~50 new fixtures) | 5-8 d | the existing 6 are anemic |
| 4 | **Differential fuzzer** | 5-10 d | LESSONS §4.9 item 6 ("not yet built") |
| 5 | Error-message golden tests | 3 d | extend 41 `.err.exp` for new parser |
| 6 | **Full-nixpkgs parse sweep** (~80K parses) | 2-4 d | beyond the 72-package sweep |
| 7 | Operator precedence battery (~80 fixtures) | 2 d | currently ZERO dedicated tests |
| 8 | Comment-preservation tests | 1 d | doc-comment positioning |

**Total: 22-35 engineering-days of new test infra BEFORE the parser swap can be validated.**

### 3.5 Verdict: PARTIAL

Existing infrastructure is **excellent for EVAL correctness** (TW is oracle). It is **inadequate for PARSER replacement** because every existing test uses TW to parse.

A new parser COULD ship with confidence IF:
- 54 `parse-*-*.nix` files bit-equal AST in v3 (16% of edge cases observable from inventory)
- 144 `eval-okay-*.nix` ALL produce byte-identical eval output through v3-parser + v3-eval
- Full nixpkgs `legacyPackages` produces byte-identical drvPath for ≥10K packages
- Position-info golden suite (~50 new fixtures) catches drift
- 24-hour differential fuzzer run lands no divergences

**Without items 3-5 from §3.4**, the project ships on test surface the existing parser already passes — leaving corners (operator precedence, position info, antiquote nesting) untested against the new implementation.

Risk profile: **materially different from any v3 change to date.** Parser bugs manifest as silently-incorrect downstream evaluations (cf. #682 toFile context — first store-path-affecting bug — and #670/671 dangling string_view — non-determinism + phantom outPath = corruption signature).

---

## 4. Strategic positioning — should we do this?

### 4.1 The strongest argument FOR (Agent A + C synthesis)

**TW's PosIdx is build-dependent.** Per Agent C: `PosTable::addOrigin` assigns offsets equal to cumulative size of previously-loaded origins. Different import orderings → different PosIdx for the same source.

**Consequence for v3 bytecode disk cache:** the cache key today involves PosIdx-bearing structures. **The cache is content-NOT-addressable by source.** Two builds traversing imports in different order produce spurious cache misses.

**A v3-native parser emitting `(file_id, file_relative_span)` makes the cache content-addressable.** This is an architectural unlock for:
- Reproducible builds across machines
- AOT cache distribution (R8a / shared bytecode cache)
- Cross-process determinism (LINKING_DESIGN_2026-05-17 §"PosIdx becomes module-local")

**This justifies the native-parser investment on its own,** independent of effort/test concerns.

### 4.2 Other arguments

| Argument | Strength |
|---|---|
| PosIdx determinism (above) | STRONG; architecturally unlocking |
| Reduced FFI surface | MEDIUM; parser is permitted FFI leaf per V3-NATIVE rule |
| Lower memory (arena allocation vs Boehm) | LOW; per BRIDGE_TELEMETRY parser arena is small |
| Direct-to-IR optimization (skip nix::Expr) | LOW; AST→IR lowering is not a bottleneck |
| Better error messages | MEDIUM; ERROR_UX_DESIGN_2026-05-20 already plans this |

### 4.3 Arguments AGAINST

| Argument | Strength |
|---|---|
| **Opportunity cost vs active memory work** | STRONG; 3-6 months is a multi-month pivot |
| Parser bugs cause silent corruption (cf. #682 / #670-671) | STRONG; risk profile different from any v3 change to date |
| Test suite requires 22-35 days new infra BEFORE start | MEDIUM; bounded but real |
| Future Nix syntax extensions force dual maintenance | MEDIUM; pipe operators already differ across forks |
| Doc-comment positioning + cursed-or wart maintenance | LOW; well-localized edge cases |

### 4.4 The opportunity cost calculation

**Current memory work** (per yesterday's `GC_STRATEGY_INTEGRATED_2026-05-30`):
- M5 watchdog gap: ~1.7 GB
- 11 GC/memory falsifications in 3 weeks
- **derivationStrict native completion (FFI_BRIDGE_INVENTORY action #1): projected to eliminate 311 MB HNE retention at source. ~1-2 weeks.**
- Layer 1 page-release falsified twice (yesterday + today)
- The "prevent allocation" pivot identified this morning

**3-6 months of parser work = 12-24 weeks** = at least 6× the derivationStrict native completion + 3× the entire FFI kill plan + ~3× the integrated GC strategy's planned timeline.

**Parser work does NOT directly close the M5 watchdog.** It enables determinism unlocks that compound LATER.

### 4.5 Recommendation

**DEFER unless the bytecode-disk-cache determinism becomes a blocker.** Specifically:
- IF: reproducible-builds-across-machines becomes a hard requirement (e.g., shared cache distribution per R8a)
- OR: the team plans to ship AOT compilation where cache stability matters
- THEN: parser project is well-scoped at 3-6 months given Lix precedent + minimal v3 invocation surface

**Otherwise, complete the active memory work first.** The parser is permitted FFI leaf per V3-NATIVE; it stays in TW until the determinism case forces action.

**If the project starts:** the prerequisites are
1. Build the 22-35 days of new test infrastructure FIRST (per §3.4)
2. Use Lix's PEGTL+fuzzing approach as calibration
3. Target Option (a) drop-in nix::Expr-producing parser (per Agent A's verdict)
4. File-relative spans, NOT PosIdx parity (per Agent A + C synthesis)
5. Pre-commit SHIP gate: byte-identical AST on full nixpkgs `legacyPackages` (≥10K packages)
6. Pre-commit FALSIFY criterion: any drvPath divergence on any nixpkgs package

---

## 5. Critical agent review

### 5.1 Agent A (TW parser inventory + v3 integration)

**Strong:**
- Code-verified 6-call-site invocation surface
- PosIdx parity not required is the load-bearing surprise
- Lix precedent calibration explicit
- LoC breakdown for production-hardened version

**Where I push back:**
- "Allocate 30% to integration, 70% to grammar" assumes engineer has Bison/Flex background — bias if not
- Effort estimate doesn't account for the 22-35 days new test infra Agent B identifies

### 5.2 Agent B (test suite coverage)

**Strong:**
- "ALL existing differential tests use TW as oracle" is the headline finding
- Edge case gap analysis is concrete (14 specific gaps)
- Required new test infrastructure with day-counts

**Where I push back:**
- "PARTIAL" verdict undersells the risk profile — for parser changes specifically, "PARTIAL" is closer to "INADEQUATE without new infra"
- Differential fuzzer at 5-10 days is optimistic; Lix's "thousands of core hours of fuzzing" suggests ongoing campaign not one-shot build

### 5.3 Agent C (existing parser survey)

**Strong:**
- Lix evidence is the strongest single calibration data point
- TW PosIdx not cache-stable is the architectural insight that justifies the project
- Per-implementation table is comprehensive

**Where I push back:**
- "Recommend recursive-descent + Pratt" is a design preference; PEGTL (Lix's choice) is the proven-shipped alternative — should be Plan B not dismissed
- "File-relative spans" recommendation is sound but Agent A's vtable-shim concern wasn't addressed

### 5.4 Synthesis-level observations

1. **PosIdx parity not required + TW PosIdx not cache-stable** is a powerful synthesis. Both agents independently surfaced different sides of the same architectural fact. The combined insight is: v3-native parser SHOULD diverge from TW PosIdx, because TW's PosIdx is broken-by-design for cache purposes anyway.

2. **The test-suite finding inverts the usual feasibility argument.** Normally "small parser surface + similar projects shipped" suggests easy. But "existing tests use TW as oracle" means the bar is HIGHER than for any other v3 change. Parser bugs are silent-corruption-class.

3. **Strategic positioning is the deciding factor.** Effort + tests are tractable; the question is whether 3-6 months is the right next investment given the M5 watchdog work outstanding.

---

## 6. Honest limits

- **Effort estimate (3-6 months)** assumes single competent engineer with C++ + Bison/Flex experience. Calendar time with normal interruptions could double.
- **Lix precedent (6 months)** uses PEGTL — different technique; LoC count may not transfer directly to a recursive-descent rewrite.
- **Test-suite gap analysis (22-35 days)** assumes existing IR-fixture FileCheck infrastructure carries; if it doesn't, add weeks.
- **PosIdx-not-cache-stable finding** is from `pos-table.hh:79-87` (Agent C). v3's cache implementation may or may not actually depend on PosIdx bytes — need to verify by reading `disk_cache.cc` serialization paths.
- **The "determinism unlocks cache distribution" claim** assumes R8a-style shared bytecode cache becomes a goal. If never pursued, the determinism argument's strength drops.
- **Parser bugs cause silent corruption** is a real risk profile (cf. #682 toFile context — first store-path-affecting bug — that took weeks to root-cause). The risk is higher than any other v3 change.
- **`bindVars` lives in TW** today; replacing it (option b/c per Agent A) adds ~200 LoC and complicates the lowering pipeline. Option (a) leaves it TW-side.
- **Future Nix syntax extensions** (pipe operators already differ between cppnix and Lix) impose ongoing dual-maintenance cost. The parser project becomes "permanent 2× maintenance" unless v3 ever becomes the canonical Nix implementation.
- **3-6 months parser work does NOT close M5 watchdog.** The yield is determinism + architectural cleanup, not RSS. Opportunity cost vs active memory work is significant.
- **The vtable-shim concern** (Agent A): if v3 inherits from `nix::Expr` to produce drop-in, must implement virtual `eval`/`bindVars`/`maybeThunk` as unreachable stubs. Linker visibility / vtable layout fragility may surface during integration.
- **No mention of `tests/lang2026/`** found — either it doesn't exist or I missed it. Verify before committing to test-coverage estimates.

---

## 7. Cross-references

- [`FFI_BRIDGE_INVENTORY_2026-05-31.md`](FFI_BRIDGE_INVENTORY_2026-05-31.md) — parser is FFI LEAF in V3-NATIVE rule
- [`T4_1_TW_RETENTION_AUDIT_2026-06-01.md`](T4_1_TW_RETENTION_AUDIT_2026-06-01.md) — TW arena retention from parser
- [`LINKING_DESIGN_2026-05-17.md`](LINKING_DESIGN_2026-05-17.md) §"PosIdx becomes module-local" — design already expects PosIdx not globally identical
- [`ERROR_UX_DESIGN_2026-05-20.md`](ERROR_UX_DESIGN_2026-05-20.md) — Diagnostic struct integration target
- [`GC_STRATEGY_INTEGRATED_2026-05-30.md`](GC_STRATEGY_INTEGRATED_2026-05-30.md) — parser is Layer 5+ scope (not on critical path)
- [`IR_CHECK_INFRASTRUCTURE_PLAN_2026-05-18.md`](IR_CHECK_INFRASTRUCTURE_PLAN_2026-05-18.md) — IR fixture infrastructure (reusable for parser test infra)

### Code anchors
- `src/libexpr/parser.y` — Bison grammar (562 LoC non-comment)
- `src/libexpr/lexer.l` — Flex lexer (277 LoC non-comment)
- `src/libexpr/include/nix/expr/parser-state.hh` — ParserState (319 LoC; includes stripIndentation:323-429)
- `src/libexpr/include/nix/expr/nixexpr.hh` — AST classes (749 LoC; Kind enum at 109-138)
- `src/libutil/include/nix/util/pos-table.hh:79-87` — `addOrigin` (the not-cache-stable mechanism)
- `src/libutil/include/nix/util/pos-table.hh:103-108` — "very expensive" `operator[]`
- `src/libexpr-v3/lower.cc:743-766` — `posIdxToHandle` (the {file,line,col} translation)
- `src/libexpr-v3/include/v3/ffi.hh:227-233` — declared eventual extraction target
- v3 parser invocation sites: `primops.cc:8738/8740/9207/9209/10042`; `v3_call_flake.cc:127`; `bytecode_primops.cc:172`; `cli/v3-eval.cc:255`

### Methodology
- [[falsification-rule]] — parser project must answer "what hypothesis does this kill"
- [[memory-first-class]] — parser is wall+determinism lever, not primary RSS lever
- [[measure-twice-cut-once]] §3 — pre-commit SHIP gate before commit
- [[same-host-bisect]] — verify drvPath byte-equality on same host before claiming parity

### External references
- Lix Gerrit !1118 (PEGTL parser rewrite)
- rnix-parser (github.com/nix-community/rnix-parser)
- Tvix eval README ("Tvix currently ignores [parser] test cases")
- nixfmt-rs parser thread (NixOS Discourse)

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
