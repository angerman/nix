# Error-message UX design for v3 — design proposal

**Date**: 2026-05-20. **Status**: design proposal, not committed. NO CODE in this doc.

**Premise**: Nix error messages are notoriously bad. The team's current TW-parity sprint (#677-#681) is correctly matching cppnix byte-for-byte, but cppnix's errors are themselves bad. v3 has an opportunity to **do better than TW** — making error UX a genuine differentiator alongside perf. This doc synthesizes prior art (rustc, Elm, Roc, GHC, TypeScript, Python 3.11+, Tvix, Lix, HNix), inventories today's pain points, and proposes a phased implementation.

Per the §1.7 "make it work right before making it fast" rule from LESSONS_LEARNED, error-message work IS correctness work — making the user's mental model align with v3's actual behavior. It's the natural extension of the in-flight TW-parity work.

## 1. Today's pain points (inventoried)

Concrete examples from real GitHub issues + Discourse threads:

| Pain | Example | Citation |
|---|---|---|
| Type-coercion: no value preview | `value is a function while a set was expected` (no name, no location of the offending value) | nix#963 (open since 2016); Discourse #46902 |
| Attribute-miss: caret on first token, no parent context | `foo.outputs.packages.${system}.foo` errors with caret at `foo`, not the failing component | nix#9636 (open) |
| Infinite recursion: no cycle identity | `error: infinite recursion encountered` with no participant identification | nix#6361, Discourse #18470 |
| `--show-trace` noise | 770-line trace masking "no i686-linux musl bootstrap binaries" | nixpkgs#239351 |
| `nix eval` vs `nix repl` divergence | Same expression produces error in one, success in the other | Discourse #14339 |
| Assertion: doubled echo | Expression repeated twice in the error output | Lix 2.92 release notes (now fixed there) |
| `callPackage` arg-mismatch: blames lib internals | `called without required argument 'fetchFromGithub', at lib/customisation.nix:69:16` — points at Nix infrastructure, not the user's `default.nix` | nixpkgs#79877 |
| String-coerce a derivation: no fix suggestion | `cannot coerce a set to a string: { drvAttrs = «thunk»; ...}` — no hint about `${pkg}/bin/...` or `lib.getExe pkg` | nix#561, Discourse #56730 |

**Pattern**: v3 inherits ALL of these from cppnix today. The team's #677-#681 work matches cppnix's bad output byte-for-byte. The opportunity: **diverge from TW where v3 can produce strictly better output, without breaking string-match consumers**.

## 2. Prior art tour

### 2.1 Rust (`rustc`) — the gold standard

- Every AST/HIR/MIR node carries a `Span` (file + byte range + macro context), threaded through `SourceMap`.
- `DiagCtxt` API builds `Diag { severity, code, primary_span, secondary_spans, child_notes, suggestions[applicability] }`.
- `Applicability` enum: `MachineApplicable` / `MaybeIncorrect` / `HasPlaceholders` / `Unspecified` — enables `cargo fix`.
- Rendered to terminal AND JSON (`--error-format=json`) from same struct.
- `#[derive(Diagnostic)]` for boilerplate-free error types.
- Error codes `error[E0308]` with `rustc --explain E0308`.
- References: [RFC 1644](https://rust-lang.github.io/rfcs/1644-default-and-expanded-rustc-errors.html), [rustc-dev-guide diagnostics](https://rustc-dev-guide.rust-lang.org/diagnostics.html).

### 2.2 Elm — "Compiler Errors for Humans" (Czaplicki 2015)

- Conversational first-person tone: "I cannot find a `getName` variable."
- Category headers: `-- TYPE MISMATCH ---`, `-- NAMING ERROR ---`.
- "Hint:" paragraphs with specific suggestions.
- `elm/error-message-catalog` — a GitHub repo of *bad source files* where every commit must demonstrably produce a better error.
- Reference: [Compiler errors for humans](https://elm-lang.org/news/compiler-errors-for-humans).

### 2.3 Roc — Elm's discipline, harder

- Goals: errors readable by beginners; tell users what to TYPE NEXT, not just what's wrong.
- Stateful parser: "I'm partway through parsing a function argument list..."
- One-error-at-a-time discipline (avoids cascades).
- Action-oriented hints ("Try writing `→` here").

### 2.4 Haskell GHC

- Typed holes (`_`): print expected type + every binding in scope.
- "Perhaps you meant..." Levenshtein suggestions.
- `-fdefer-type-errors`: lazy languages benefit by deferring errors to force time.
- `HasCallStack` constraint for opt-in lightweight stack traces.
- Cost-centre stacks under `+RTS -prof -fprof-auto` for thunk-creation provenance.
- Reference: [Marlow on GHC 8.0 stack traces](https://simonmar.github.io/posts/2016-02-12-Stack-traces-in-GHCi.html).

### 2.5 TypeScript

- Multi-span "related information": primary + N secondary spans pointing at declarations, imports, prior occurrences (TS 3.0+, [microsoft/TypeScript#25257](https://github.com/microsoft/TypeScript/issues/25257)).
- "Did you mean X?" with declaration anchor.
- Type-as-string expansion (one level): show `{ name: string, age: number }` not `Person`.
- "Excessively deep" cliff: detect runaway recursion as a distinct error class with own diagnostic.
- LSP hover info as primary UI; compiler emits structured JSON, editor renders.

### 2.6 Python 3.11+

- **PEP 657**: fine-grained `(start_line, end_line, start_col, end_col)` per bytecode instruction. Renders `~~~~^^^^` under the EXACT failing subexpression. Cost: ~22% larger `.pyc`.
- **PEP 678**: `__notes__` — mutable list on exception object; callers `add_note()` as they unwind.
- "Did you mean" for `NameError`/`AttributeError` (3.10+); implemented in `Python/suggestions.c`.

### 2.7 Tvix (Rust Nix evaluator, TVL)

The closest existing precedent for v3:

- `tvix_eval::SourceCode` — tracks every parsed source for diagnostic rendering.
- **`WithSpan` trait** — converts `Result<T, ErrorKind>` to `Result<T, Error>` using current call-frame span. Chains VM frame stack into a cause chain automatically.
- `codemap_diagnostic` crate — multi-span errors with carets, labels.
- Commit `9b1a266` "chain error spans for thunk errors" — explicit two-span design: thunk body + forcing site both rendered.

This is the **cleanest lazy-eval pos-threading design** in any production Nix evaluator. v3 should adopt the pattern.

### 2.8 Lix 2.91/2.92 — what cppnix forks have already shipped

- 2.91: explicit-throw diagnosed as such; integer-overflow message; attribute-path component identified.
- 2.92: parent-contents printed inline for missing attrs; selection-error caret on failing component (not chain start); no redundant assertion echo.
- Reference: [Lix 2.92 release notes](https://docs.lix.systems/manual/lix/nightly/release-notes/rl-2.92.html).

**Lesson**: don't reinvent; port what Lix has + go further.

### 2.9 cppnix's own improvements

- `ErrorInfo` + `Trace` + `LinesOfCode` + `Suggestions` in `libutil/error.hh` (Eelco's nix-errors-enhancement project, ~2020-21).
- Nix 2.21: started adding value pointer to traces.
- Nix 2.24 (2024-07): force re-eval on display; debugger prints file:line.
- `Suggestions::bestMatches` (libutil/suggestions.cc, Levenshtein, max-dist 2) wired at 3 sites: `eval.cc:1679`, `eval.cc:1874`, `attr-path.cc:93`.
- `addErrorContext` builtin: AST-side hook for users, but gated behind `--show-trace`.

## 3. The architectural foundation: structured `Diagnostic`

The single most important change. Replace v3's current `throw std::runtime_error("...")` pattern with:

```cpp
struct Diagnostic {
    ErrorCode code;                          // e.g. NIX_E0101 for missing-attr
    Severity severity;                       // Error / Warning / Note
    PosIdx primarySpan;                      // where the error fires
    std::vector<RelatedSpan> related;        // {span, message} secondary labels
    std::vector<Note> notes;                 // structured prose annotations
    std::vector<Suggestion> suggestions;     // {span, replacement, applicability}
    std::optional<ValuePreview> offending;   // truncated repr of bad value (if applicable)
};
```

Why this is load-bearing:
- Terminal renderer formats it one way (color, ASCII art, hierarchy).
- LSP/JSON consumer renders it another way (machine-parseable).
- Tests match on `code` not regex on prose.
- Future tooling (auto-fix, hover-info, `nix --explain`) all plug into the same struct.
- Easy migration: throw a `Diagnostic` wrapped in `EvalError` exception; existing catches still work; new renderer prefers `Diagnostic` payload when present.

**Estimated cost**: 1 week for the struct + renderer + initial JSON output + 5-10 throw-site migrations as worked examples. The rest of the throw-site migration (~50 sites in v3) happens incrementally as each error path gets improved.

## 4. The lazy-language win: two-span errors

The fundamental Nix UX gap: an error fires deep inside a forced thunk, but the message points only at the *force site*. The *binding/creation site* is lost.

**Tvix's pattern** (`WithSpan` trait): every `OP_FORCE`-driven re-entry into the dispatch loop carries `(currentPos, thunkOriginPos)` on the C frame. On throw, attach both. Render as:

```
error[N0303]: attribute 'fetchFromGithub' missing
   --> default.nix:42:12
    |
 42 |     fetchFromGithub,
    |     ^^^^^^^^^^^^^^^ no such attribute in this argument set
    |
    = bound here:
    --> lib/customisation.nix:7:5
      |
    7 |     callPackage = autoArgs.callPackage or (path: overrides: ...);
      |     --------------- the argument set was constructed here
```

**v3 has the data already**: `vm.frames` has the chain; `Thunk` allocations have access to the lower-time `PosIdx`. The work is: store the binding pos in the Thunk's slot side-table; attach it as `RelatedSpan` at force-time error throw.

**Estimated cost**: 1 week including the Thunk side-table change.

## 5. Five concrete techniques ranked by impact / cost

| # | Technique | Impact | Cost | Where in v3 |
|---|---|---|---|---|
| 1 | **Typed error hierarchy + structured `Diagnostic`** | foundational | 1 week | `errors.hh` extension + `Diagnostic.hh` new + renderer in `print.cc` |
| 2 | **Two-span lazy errors** (force site + binding site) | high (Nix-specific UX win) | 1 week | Thunk side-table + `OP_FORCE` slot |
| 3 | **Parent-attrset preview + Levenshtein** at attribute-miss | high (highest QoL/effort ratio) | 3 days, ~150 LOC | `vm.cc` OP_GET_ATTR / OP_GET_ATTR_PROBE miss paths |
| 4 | **Error codes + `nix --explain`** corpus | medium (compounds with #1) | 3 days infra + 1-2 days/code for explanations | `Diagnostic.code` + new CLI subcommand |
| 5 | **Smart trace summarisation** (collapse module-system frames, promote user frames) | high on real-world workloads | 1 week | print-time heuristic over `vm.frames` |

**Total Phase A (techniques 1-3)**: ~3 weeks of focused work; delivers the highest-impact wins.

**Total Phase A+B (techniques 1-5)**: ~5-6 weeks. After this, v3 is meaningfully better than cppnix on error UX.

## 6. Three before/after examples (design proposal)

### A. Attribute not found

**Today (cppnix and v3 inherited):**
```
error:
       … while calling the 'getAttr' builtin
         at /nix/store/.../lib/attrsets.nix:25:5:

       error: attribute 'fetchFromGithub' missing
```

**Proposed v3:**
```
error[N0101]: attribute 'fetchFromGithub' not found
  --> pkgs/ripgrep/default.nix:3:5
   |
 3 |     fetchFromGithub,
   |     ^^^^^^^^^^^^^^^ no such attribute in the argument set
   |
   = note: the set provided by callPackage contains 1284 attributes;
           closest names are:
             - fetchFromGitHub      (distance 1; case mismatch)
             - fetchFromGitLab      (distance 4)
             - fetchFromBitbucket   (distance 8)
   = help: rename to `fetchFromGitHub` — Nix attribute names are case-sensitive
   = explain: `nix --explain N0101` for more on attribute resolution
```

### B. Type / coercion error

**Today (cppnix and v3 inherited):**
```
error:
       … while evaluating a path segment
         at /home/u/cfg.nix:14:9:

       error: cannot coerce a set to a string: { drvAttrs = «thunk»; ... }
```

**Proposed v3:**
```
error[N0203]: cannot coerce attribute set into string
  --> cfg.nix:14:9
   |
14 |     path = "${pkgs.firefox}";
   |             ^^^^^^^^^^^^^^^ this is a derivation, not a path
   |
   = bound here:
   --> <nixpkgs>/pkgs/applications/networking/browsers/firefox/default.nix:5:1
   |
 5 | { lib, stdenv, ... }: stdenv.mkDerivation { ... }
   | ----------------- the derivation
   |
   = note: pkgs.firefox is a derivation. To use it in a path context, write
           one of:
             "${pkgs.firefox}/bin/firefox"   (the executable)
             "${lib.getExe pkgs.firefox}"    (canonical entrypoint)
             "${pkgs.firefox.outPath}"       (the store path)
```

### C. Infinite recursion

**Today (cppnix and v3 inherited):**
```
error: infinite recursion encountered

       at /nix/store/.../lib/fixed-points.nix:95:7
```

**Proposed v3:**
```
error[N0301]: infinite recursion forcing thunk
  --> overlay.nix:7:5
   |
 7 |     foo = self.foo + 1;
   |     ^^^^^^^^^^^^^^^^^^ this binding refers to itself with no base case
   |
   force chain (most recent first):
     overlay.nix:7:5      foo = self.foo + 1
     fixed-points.nix:95  fix' = f: let x = f x; in x
     default.nix:12       pkgs = import <nixpkgs> { overlays = [ ./overlay.nix ]; }
   |
   = note: `self.foo` resolves to this same `foo` binding through `fix`.
           A recursive attribute needs a terminating case, e.g.:
             foo = (super.foo or 0) + 1;
```

## 7. Implementation phases

### Phase A (3 weeks, highest impact)

| Sub-task | Effort |
|---|---|
| A.1: `Diagnostic` struct + renderer in `print.cc` | 3 days |
| A.2: `--error-format=json` CLI flag | 1 day |
| A.3: Migrate 5-10 most-common throw sites to typed errors (`TypeError`, `AttrNotFoundError`, `ArgumentMismatchError`, `CoercionError`) | 3 days |
| A.4: Levenshtein + parent-preview at `OP_GET_ATTR` miss paths | 3 days |
| A.5: Two-span Tvix-style `WithSpan` for thunk-force errors | 1 week |

**Phase A exit criterion**: the three before/after examples above produce the "Proposed v3" output. cutover-parity still 142/142 (string-match-only tests may need updating to use `code` instead of regex).

**Rule 0**: Phase A kills the hypothesis "Nix error messages must be cppnix-shaped." Falsified by producing strictly better output for the same input.

### Phase B (2-3 weeks, compounding wins)

| Sub-task | Effort |
|---|---|
| B.1: Error code corpus (~25 codes, `N0001`-`N0299`) | 2 days |
| B.2: `nix --explain Nxxxx` CLI subcommand + Markdown corpus | 3 days |
| B.3: `addErrorContext` shown by default (per nix#7553) | 1 day |
| B.4: Smart trace summarisation (collapse module-system frames, promote user frames) | 1 week |
| B.5: `nix eval` derivation short-circuit (per Discourse #14339) | 3 days |

**Phase B exit criterion**: top-10 most-cited bad-error issues from GitHub have v3 outputs that resolve the complaint.

### Phase C (deferred; ~2-3 weeks; do after Phases A-B settle)

| Sub-task | Effort |
|---|---|
| C.1: PEP 657-style sub-expression spans (per-bytecode-op pos ranges) | 1-2 weeks |
| C.2: PEP 678 `__notes__`-style propagation for trace context | 3 days |
| C.3: LSP integration (consume `--error-format=json`) | 1 week |
| C.4: Snapshot-test corpus à la Elm's `error-message-catalog` | 1 week |

**Phase C** unlocks editor / IDE integration as a downstream consequence.

## 8. Self-critique

Following the LESSONS_LEARNED §1.7 discipline:

**(C1)** This is correctness work, not perf work, so it's appropriate for the current TW-parity phase. ✓ Aligned with §1.7.

**(C2)** Effort estimates are typically 30-50% optimistic for me. Phase A's "3 weeks" is probably 4-5 weeks realistic. Phase B's "2-3 weeks" is probably 3-4 weeks. Total 7-9 weeks for Phases A+B.

**(C3)** I'm proposing 9 sub-tasks across Phases A+B. The team's recent velocity (Phase 1.6 in 2 hours; IR Phases A-H in 1 day) suggests they might compress this dramatically. Realistic if compressed: 3-4 weeks for Phases A+B.

**(C4)** Risk: the `Diagnostic` struct refactor touches ~50 throw sites. Doing all 50 in one PR is risky; incrementally migrating throw sites as their error paths get improved is safer.

**(C5)** Risk: cppnix string-match tests in nixpkgs CI may break if v3 produces different (better) text. Need a `--error-format=cppnix-compat` legacy mode for ~6 months while downstream adapts. Or: opt-in to new errors via env var (`NIX_V3_NEW_ERRORS=1`) initially.

**(C6)** The "parent-attrset preview" for sets with 1000+ attrs (e.g., nixpkgs top level) needs careful truncation — Lix caps at 8 names alphabetically. Same for v3.

**(C7)** Levenshtein on attribute names is a known winner but has edge cases: case-insensitive variants (`fetchFromGithub` vs `fetchFromGitHub` is distance 1 by Levenshtein, the fix is "case-sensitive" not "spelling"). Need to distinguish case vs spelling distance.

**(C8)** Error codes: Nix doesn't currently have them. Allocating `N0001`-`N0299` is making up a numbering system. Choose carefully — these become public API. Reserve ranges:
- `N0001-N0099` syntax / parse errors
- `N0100-N0199` name resolution / attribute access
- `N0200-N0299` type / coercion errors
- `N0300-N0399` evaluation / recursion errors
- `N0400-N0499` IFD / build / store errors
- `N0500-N0599` derivation / formals / argument errors
- `N0600-N0699` IO / file / path errors
- `N0700-N0799` resource limits (heap / CPU / wall)
- `N0800-N0899` reserved
- `N0900-N0999` internal / panic / assertion failures

**(C9)** The "before/after" examples I drafted are aspirational; some details (e.g., the exact "bound here" rendering for derivations) need to be hand-tuned against real failure modes. The team should iterate via Elm-style snapshot testing.

## 9. Cross-references

- This doc relates to but is independent of the four-pillar / Phase 1.6 / cardano-node work tracks.
- TW-parity issues #677/#678/#680/#681 are valuable for *cppnix-string-match-compatibility* but don't preclude doing better.
- Reference implementations: [Tvix's WithSpan](https://docs.tvix.dev/rust/tvix_eval/vm/trait.WithSpan.html), [codemap_diagnostic](https://docs.tvix.dev/rust/codemap_diagnostic/index.html).
- Existing v3 infrastructure: `PosIdx32` in `alloc.hh:76`, side-table at `alloc.hh:694-697`, error types in `errors.hh`.
- `Suggestions::bestMatches` in cppnix `libutil/suggestions.cc` (Levenshtein, max-distance 2) — port pattern to v3.
- Lix's release notes for prior-art: [2.91](https://lix.systems/blog/2024-08-12-lix-2.91-release/), [2.92](https://docs.lix.systems/manual/lix/nightly/release-notes/rl-2.92.html).

## 10. Source bibliography

Compiled from 3 research agents (2026-05-20):

**Rust / Elm / Roc**:
- [Rust RFC 1644 — Default and expanded rustc errors](https://rust-lang.github.io/rfcs/1644-default-and-expanded-rustc-errors.html)
- [rustc-dev-guide — Diagnostics](https://rustc-dev-guide.rust-lang.org/diagnostics.html)
- [Elm — Compiler Errors for Humans (Czaplicki 2015)](https://elm-lang.org/news/compiler-errors-for-humans)
- [Caleb Meredith — Writing Good Compiler Error Messages](https://calebmer.com/2019/07/01/writing-good-compiler-error-messages.html)
- [codespan-reporting](https://github.com/brendanzab/codespan)
- [ariadne](https://github.com/zesterer/ariadne)
- [miette](https://docs.rs/miette/latest/miette/)

**Haskell / TS / Python**:
- [PEP 657 — Include Fine Grained Error Locations in Tracebacks](https://peps.python.org/pep-0657/)
- [PEP 678 — Enriching Exceptions with Notes](https://peps.python.org/pep-0678/)
- [GHC User's Guide: Typed Holes](https://ghc.gitlab.haskell.org/ghc/doc/users_guide/exts/typed_holes.html)
- [GHC.Stack (HasCallStack)](https://hackage.haskell.org/package/base/docs/GHC-Stack.html)
- [Marius Schulz — Spelling Correction in TypeScript](https://mariusschulz.com/blog/spelling-correction-in-typescript)
- [TypeScript related-error-spans (microsoft/TypeScript#25257)](https://github.com/microsoft/TypeScript/issues/25257)
- [DWARF support in GHC (part 3) — Well-Typed](https://well-typed.com/blog/2020/04/dwarf-3/)

**Nix-specific**:
- [Proposal for improving Nix error messages — Discourse](https://discourse.nixos.org/t/proposal-for-improving-nix-error-messages/6305)
- [Tvix evaluator docs](https://docs.tvix.dev/rust/tvix_eval/index.html)
- [Tvix: chain error spans for thunk errors](https://code.tvl.fyi/commit/tvix?h=refs/r/5092&id=9b1a266197d4edb55d40415464f7106c72ad6149)
- [Lix 2.91 release notes](https://lix.systems/blog/2024-08-12-lix-2.91-release/)
- [Lix 2.92 release notes](https://docs.lix.systems/manual/lix/nightly/release-notes/rl-2.92.html)
- [nix#963 / #9636 / #6361 / #7552 / #7553 / #239351](https://github.com/NixOS/nix/issues)

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.
