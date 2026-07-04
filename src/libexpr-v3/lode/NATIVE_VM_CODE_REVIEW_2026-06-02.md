# v3 native VM implementation — code review (15 findings)

**Date:** 2026-06-02
**Reviewed at:** `5d29d96e6` (+ uncommitted FFI settings-shim working-tree diff in ffi.cc / ffi.hh / primops.cc)
**Method:** xhigh-effort `/code-review` — 6 finder angles + 3 verifiers + 1 gap sweep, against the TW (tree-walker) oracle
**Scope:** the native `.nix → v3 IR` lowering (`cli/lower_v3.hh`, 957 LoC), the v3 parser (`parser/v3-parser.{y,l}`, `parser/parser-state.hh`), and the uncommitted FFI settings shims. This is the code that replaced the deleted `lower.cc` + the TW-AST→nix::Expr bridge (parser Stage 2).

**Headline:** 7 of 15 findings are the **"v3 silently accepts/computes what TW rejects"** class ([[#680-semantic-strictness]]) — they produce a *wrong drvPath or wrong value*, not a crash. None would surface in the 47-package drvPath sweep; they need adversarial inputs (shadowed `builtins`, overflow literals, mistyped dynamic keys, `inherit`+dotted collisions). This is consistent with the standing caveat that the native-path validation matrix is correctness-on-normal-input, not adversarial.

**Recommendation:** run a dedicated **strictness-parity audit pass** on the new parser/lowering (mirror of the #677–#693 TW-parity work) before the native path is considered fully validated. The four critical fast-path / dynamic-key / addAttr-inherit bugs trigger on ordinary Nix and should be fixed first.

---

## Method notes

- **Finder angles (Phase 1):** A line-by-line scan · B removed-behavior/TW-parity · C cross-file tracer · D C++ pitfalls · E position+laziness · F altitude+cleanup. Each surfaced ≤8 candidates.
- **Verify (Phase 2):** 1-vote 3-state (CONFIRMED / PLAUSIBLE / REFUTED) per candidate.
- **Sweep (Phase 3):** fresh-eyes gap pass — **ran `v3-eval` vs `nix-instantiate` on this host** and empirically confirmed findings 2, 3, 8, 12 (not source-reading inferences).

**Dropped as REFUTED (recorded so they aren't re-raised):**
- `inherit x` lowered eagerly → **REFUTED**: the value lowers to a `RecBindingSlotRef`; `OP_REC_BINDING_SLOT_REF` + `OP_ATTRS_REC_SET` store a slot pointer without forcing, so `let x = throw "boom"; in { inherit x; }` returns the attrset (matches TW laziness; the #686 fix is present).
- makePath relative-path `..` underflow → **REFUTED**: ported algorithm matches `nix::CanonPath` byte-for-byte across 17 inputs incl. `..`-past-root (both clamp at root). Only degenerate `basePath=="/"` + `"./"` differs (`/` vs `//`), an unreachable source dirname.
- unbound-var raw `std::runtime_error` "tryEval-uncatchable" → **REFUTED** as a behavioral divergence: TW's `tryEval` catches only `AssertionError`, NOT `UndefinedVarError` (which derives from `EvalError`), and TW raises it at `bindVars` (parse) time too. Same observable behavior; the raw-type + missing-position is a diagnostic-quality nit (see finding-adjacent note in §Hardening).

---

## Findings (ranked most-severe first)

### Critical — store-path-affecting wrong eval / silent permissiveness (#680 class)

#### 1. `lowerSelect` `builtins.<X>` fast-path ignores lexical shadowing
- **File:** `cli/lower_v3.hh:833`
- **Mechanism:** the fast-path fires on a purely syntactic check (`e->e` is a Var named `"builtins"` + single-symbol path resolving via `findPrimOp`), with **no scope-stack consultation**. `lowerVarByName` (line ~289) walks scopes innermost-first; the Select fast-path skips that.
- **Trigger:** `let builtins = { head = 1; }; in builtins.head` → v3 emits `LitPrimOp(__head)`; TW resolves `builtins` lexically → `1`.
- **Impact:** store-path-affecting wrong eval result. **CONFIRMED** (source + scope-check evidence).
- **Fix:** before the fast-path, verify `builtins` is not lexically/with-bound (reuse `lowerVarByName`'s scope check).

#### 2. Same fast-path resolves `__`-prefixed primops TW does not expose under `builtins`
- **File:** `cli/lower_v3.hh:837`
- **Mechanism:** `findPrimOp(symbol)` is called on the raw symbol, so `__`-prefixed primops (registered as bare globals under their `__name`) match as `builtins.__name`. TW exposes them under `builtins` *without* the `__` prefix and rejects `builtins.__name` as missing.
- **Trigger:** `builtins.__sub 5 2` → v3 returns 3; TW throws "attribute '__sub' missing". Same for `__lessThan`/`__mul`/`__findFile`.
- **Impact:** v3 silently accepts a non-existent `builtins` attribute path. **CONFIRMED empirically** (v3-eval vs nix-instantiate).
- **Fix:** the fast-path must verify the resolved name is actually a member of the `builtins` attrset (not just any global primop). Same code site as #1 — both are soundness holes in the one fast-path.

#### 3. Dynamic non-string attr key → silent `false`/default instead of type error
- **File:** `cli/lower_v3.hh:857` (HasAttrDyn lowering) + `vm.cc:8786` (handler)
- **Mechanism:** dynamic-key `?` and `or`-default `select` lower to `HasAttrDyn`, whose VM handler returns `false` for a non-string key (null/int/bool/path) instead of forcing-to-string-and-throwing.
- **Trigger:** `{ a = 1; }.${null} or 7` → v3 returns 7 (TW throws "expected a string but found null"); `{a=1;} ? ${true}` → v3 `false` (TW throws); `x ? a.${1}` → v3 `false` (TW throws).
- **Impact:** a mistyped computed key yields a wrong value/drvPath instead of the TW error. **CONFIRMED empirically.**
- **Fix:** force the dynamic key to a string (with the coercion error) before the has/select.

#### 4. `addAttr` duplicate-detection skips `inherit` definitions
- **File:** `parser/parser-state.hh:287` (leaf), `:423` (non-leaf descent), `:309` (nested merge)
- **Mechanism:** dup detection uses `findPlain`, which only matches `Plain`-kind defs and skips `Inherited`/`InheritedFrom`. TW keys its check on a symbol→def map covering all kinds (then `dynamic_cast<ExprAttrs>` to decide merge-vs-error).
- **Trigger:** `{ inherit a; a.b = 1; }` → v3 pushes a SECOND `a` AttrDef (malformed attrset, two `a` bindings); TW throws "attribute 'a' already defined". Same for `{ inherit (src) a; a.b = 1; }` and nested merges.
- **Impact:** silently builds a malformed attrset where TW is a parse error → wrong eval/drvPath. **CONFIRMED** (vs `parser-state.hh.upstream`).
- **Fix:** use the existing `findAny` (already used by `addInherit`/`addInheritFrom`) for the reverse-direction collision, mirroring TW's all-kinds check.

#### 5. INT literal overflow silently clamps
- **File:** `parser/v3-parser.l:137`
- **Mechanism:** `std::strtoll(yytext, nullptr, 10)` clamps to INT64_MAX on overflow and ignores `errno`. TW uses `string2Int` (boost::lexical_cast) which throws "invalid integer".
- **Trigger:** `9223372036854775808` → v3 silently produces 9223372036854775807; TW raises ParseError.
- **Impact:** wrong integer value vs parse error. **CONFIRMED** (vs `src/libexpr/lexer.l`).
- **Fix:** mirror TW — use `string2Int`/checked parse; throw on overflow.

#### 6. FLOAT literal has no errno/range check
- **File:** `parser/v3-parser.l:139`
- **Mechanism:** `std::strtod(yytext, nullptr)` with no `errno = 0` / `errno != 0` check. TW clears errno and throws "invalid float" on ERANGE.
- **Trigger:** `1e400` → v3 produces ExprFloat `inf`; TW throws.
- **Impact:** wrong value (inf) vs parse error. **CONFIRMED.**
- **Fix:** add the `errno = 0` before + `errno != 0` throw after, as TW does.

#### 7. HPATH (`~/...`) has no pure-eval gate
- **File:** `parser/parser-state.hh:190`
- **Mechanism:** the HPATH branch resolves `~/foo` against `homePath` with no pure-mode check; the v3 ParserState has no `pureEval`/`settings` member at all. TW throws "the path '%s' can not be resolved in pure mode".
- **Trigger:** `~/foo` under pure eval (flakes / `--pure-eval`) → v3 resolves and succeeds; TW errors.
- **Impact:** a flake referencing a home path parses+evals instead of erroring → impurity leaks into a "pure" eval. **CONFIRMED** (secondary sub-claim of the makePath candidate; the resolution-divergence sub-claim was REFUTED).
- **Fix:** thread the pure-eval flag into ParserState (or gate HPATH at lower time) and throw in pure mode.

### Medium

#### 8. Pipe operators `<|`/`|>` are not experimental-feature-gated
- **File:** `parser/v3-parser.l:132`
- **Mechanism:** pipe tokens emitted unconditionally; TW gates them behind `requireExperimentalFeature(Xp::PipeOperators)`.
- **Trigger:** `5 |> f` → v3 evaluates to 6; stock TW (without `--extra-experimental-features pipe-operators`) errors "experimental Nix feature 'pipe-operators' is disabled". **CONFIRMED empirically.**
- **Impact:** v3 silently accepts feature-gated syntax (silent permissiveness).
- **Fix:** gate the pipe lexer rules on the experimental feature.

#### 9. AST nodes carry no source position (structural)
- **File:** `parser/v3-parser.y:277` (Var/Call/Select/Lambda/If/With/Assert built with default noPos), `cli/lower_v3.hh:447` (lowerLambda never sets `posHandle`), `include/v3/ir.hh` (App/AttrSelect/If/Lambda structs have no position field)
- **Mechanism:** TW stamps `CUR_POS` on each of these node kinds; v3's grammar omits the Pos arg and the IR has nowhere to store it.
- **Trigger:** `(x: x.y) 1` / `builtins.head []` → TW reports `error: ... at /path:L:C`; v3's trace has no source frame. `«lambda @ pos»` prints as `«lambda»`.
- **Impact:** diagnostic-quality regression; partly store-path-relevant where `__curPos` is interpolated into a derivation. Static attr/formal positions ARE preserved (ordinary `unsafeGetAttrPos` works); the loss is on expression nodes + dynamic keys + path literals (`v3-parser.y:398`).

#### 10. Dynamic attr-key position hard-coded `0`
- **File:** `cli/lower_v3.hh:748` (rec), `:811` (non-rec)
- **Mechanism:** dynamic-key entries lowered with `/*pos*/ 0` instead of `posHandle(da.pos)`.
- **Trigger:** `builtins.unsafeGetAttrPos "a" (rec { ${"a"} = 1; })` → v3 yields `null`; TW returns `{file,line,column}`.
- **Impact:** `unsafeGetAttrPos` on dynamic keys returns null where TW has a position (a real builtin; can feed derivations).

#### 11. `internStr`/`stringPool` use process-global static deques (thread-unsafe + unbounded)
- **File:** `cli/lower_v3.hh:196` (internStr `pool`), `:340` (String case `stringPool`)
- **Mechanism:** two function-local `static std::deque<std::string>` back every IR `string_view` — process-lifetime, never freed, shared across all `LowererV3` instances, not thread-safe. Contradicts the per-instance design the comment implies.
- **Impact:** (a) memory-first-class: long-running eval importing many files leaks every path/string literal for process lifetime; (b) latent: future parallel/import eval races on the deque → torn node → IR `LitString` view points at corrupted bytes → wrong literal/drvPath.
- **Fix:** move the pool to a member owned by the CU/Lowerer with the right lifetime; if cross-instance sharing is intended, document it and add a lock for the parallel-eval future.

#### 12. Dangling `$` / `\` / `$\` at EOF inside a string → flex jam
- **File:** `parser/v3-parser.l:147`
- **Mechanism:** the exclusive STRING state has no rule for a trailing `$`/`\`/`$\` at EOF; with `%option nodefault` this hits flex's jam path. TW has a dedicated `<STRING>\$|\\|\$\\` rule returning EOF for a clean parse error.
- **Trigger:** source ending mid-escape (`"abc\` or `"abc$` at EOF) → v3 prints internal "flex scanner jammed" with no position; TW gives "syntax error, unexpected end of file". **CONFIRMED empirically.**
- **Impact:** truncated/corrupt input crashes the lexer instead of a clean parse error.

### Altitude / maintenance

#### 13. `canLowerV3` is now a dead gate and a regression hazard
- **File:** `cli/lower_v3.hh:81`
- **Mechanism:** the AST→nix::Expr bridge was deleted, so all 6 call sites do `if (!canLowerV3) throw` with no fallback. The predicate now masks the lowerer rather than protecting a fallback.
- **Cost:** a new Kind handled by `lowerExpr` but not added to `canLowerV3` silently downgrades a lowerable program to a hard throw — and the ~110-LoC predicate + 6 guards must stay in lockstep with `lowerExpr` forever.
- **Fix:** either delete the gate (let `lowerExpr` be the single source of truth, throwing its own clean error on a genuinely-unsupported kind) or generate both from one table.

#### 14. `resolvesViaWith` duplicates `lowerVarByName`'s resolution + base-env list
- **File:** `cli/lower_v3.hh:567`
- **Mechanism:** `resolvesViaWith` mirrors `lowerVarByName`'s resolution order (lexical → base-env consts → primop → with) with the base-env const list (`true`/`false`/`null`/`builtins`) hardcoded a second time.
- **Cost:** correctness-adjacent drift — if base-env resolution changes in one and not the other, `isTrivialForValue` mis-thunks a with-resolved var → the documented delayed-with cycle bug. No compiler help to catch divergence.
- **Fix:** single-source the resolution decision (one function returns the resolution kind; both call sites consume it).

#### 15. Doc-comments not attached to lambdas
- **File:** `parser/v3-parser.l:279`
- **Mechanism:** no doc-comment lexer rule or `docCommentDistance` tracking; `/** ... */` is lexed as a plain block comment. TW records it and `SET_DOC_POS` attaches it to the `ExprLambda`.
- **Trigger:** `/** doc */ x: x` then `:doc f` / builtins doc path → v3 reports no doc; TW returns the doc string.
- **Impact:** always-on diagnostic divergence (`:doc`, lambda documentation).

---

## Hardening note (REFUTED-but-worth-fixing)

The unbound-var path throws a raw `std::runtime_error` (`cli/lower_v3.hh:326`) with no source position. This is NOT a tryEval divergence (verified — TW can't catch `UndefinedVarError` via tryEval either, and raises it at bind time). BUT a raw `std::runtime_error` (not a `nix::Error`) escaping into a top-level catch that only handles `nix::Error` would produce a worse diagnostic or an abort — the fork-review B2 class ("wrap dispatch exits; std::runtime_error → tryEval-uncatchable / worse top-level message"). Low priority, but converting it to a positioned `nix::UndefinedVarError` aligns the message with TW and removes the raw-exception hazard.

---

## Cross-cutting observations

1. **The dominant pattern is silent permissiveness (findings 1–8).** Per [[#680-semantic-strictness]], "silent permissiveness is more dangerous than visible errors — audit anywhere v3 accepts what TW rejects." The new parser/lowering reintroduced this class at the parse/lower boundary. A focused strictness-parity sweep (the parser-stage analogue of the #677–#693 error-message-parity work) is the right closing move for the native path.

2. **Why the 47-package drvPath sweep missed these.** All of findings 1–8 require adversarial inputs that don't occur in well-formed nixpkgs: shadowed `builtins`, `builtins.__sub`, `.${null}`, overflow literals, `inherit`+dotted collisions, `~/` under pure eval, `|>` without the feature flag. The differential matrix validates correctness-on-normal-input; it is not an adversarial/property sweep. This matches the standing "HNE/M5 + soak waived" caveat — the validation surface is narrower than full TW parity.

3. **Position info (findings 9, 10) is a structural gap, not a bug per node.** The IR has no position field on App/AttrSelect/If/Lambda. Closing it is a small schema addition + threading `CUR_POS` through the grammar; until then, error traces and `«lambda @ pos»` cannot match TW. Static attr/formal positions ARE correctly preserved, so ordinary `unsafeGetAttrPos` works.

4. **Memory-first-class (finding 11).** The two process-global string deques are a latent leak that grows with import count — directly relevant to the M5/HNE RSS story this branch has been chasing. Worth folding into the next memory pass.

---

## Suggested fix ordering

1. **lowerSelect fast-path** (1 + 2) — one site, two soundness holes, store-path-affecting, ordinary-Nix trigger.
2. **Dynamic-key type error** (3) — store-path-affecting, ordinary trigger.
3. **addAttr inherit-dup** (4) — parse-error parity, ordinary trigger.
4. **INT/FLOAT overflow** (5, 6) — trivial lexer fix mirroring TW.
5. **HPATH pure-eval gate + pipe-operator gate** (7, 8) — purity/feature correctness.
6. **Position schema** (9, 10) — IR field + grammar threading.
7. **String-pool lifetime** (11) — memory + parallel-eval hardening.
8. **Lexer EOF rule** (12), **canLowerV3 dead gate** (13), **resolvesViaWith dedup** (14), **doc-comments** (15) — cleanups.

Add the adversarial cases above as permanent regression fixtures (per [[always-add-tests]]) — they are exactly the inputs the 47-package sweep cannot reach.

---

## Cross-references

- [[#680-semantic-strictness]] — the silent-permissiveness rule findings 1–8 fall under
- [[parser-stage1-complete-2026-06-01]] — the native parser/lowering this reviews
- [[ffi-consolidation-2026-06-02]] — the uncommitted FFI settings-shim diff (verified clean: global-vs-per-state preserved, mkStringValueOwned copies by value — no dangling)
- [[native-parser-feasibility-2026-06-01]] — the "byte-equal on 547 nixpkgs files, soak waived" validation context
- [[always-add-tests]] — every fix needs positive + negative + regression fixtures
- `parser/parser-state.hh.upstream` — the TW ParserState oracle used for the differential
- `src/libexpr/lexer.l` / `src/libexpr/parser.y` — canonical TW parser oracle

### Code anchors
- `cli/lower_v3.hh`: 81 (canLowerV3), 196/340 (string pools), 218 (resolvePos), 326 (unbound throw), 447 (lambda posHandle), 567 (resolvesViaWith), 748/811 (dynamic-key pos), 833/837 (builtins fast-path), 857 (HasAttrDyn)
- `parser/parser-state.hh`: 190 (HPATH), 287/309/423 (findPlain dup)
- `parser/v3-parser.l`: 132 (pipe), 137 (INT), 139 (FLOAT), 147 (STRING EOF), 279 (doc-comment)
- `parser/v3-parser.y`: 277 (node positions), 398 (path literal pos)
- `vm.cc:8786` (HasAttrDyn handler)

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
