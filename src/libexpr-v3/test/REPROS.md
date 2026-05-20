# v3 repro fixture manifest

Per-fixture index: file → lesson it pins → original issue/commit.

This file is the source of truth for `test/run-repros.sh` (the automated
runner) and the human reference when investigating new bugs ("did we
ever see this shape before?").

Convention: every `repro-<topic>.nix` in `test/` MUST appear here with
at least the lesson + commit columns filled in.  New repros must be
added when a bug is fixed — see `LESSONS_LEARNED_2026-05-15.md` §4.8
on the bisect → fixture → permanent-guard workflow.

| Fixture | Lesson it pins | Original commit | Run modes |
|---|---|---|---|
| `repro-455.nix` | `pkgs ? lib` returns true (env-shape mismatch in `prev // overlay final prev`) | (RCA #455) | TW + v3-direct must agree |
| `repro-455-aliases.nix` | RCA #455 with aliases shape | (RCA #455) | TW + v3-direct must agree |
| `repro-495-broader-thunkify-bug.nix` | broader-thunkify upvalue bug (#495 / #496 / #497 / #498) | `b3a7c5e6e` family | v3-direct only |
| `repro-583-mapattrs-app-cache.nix` | mapAttrs Tag::App caching shape | (issue #583) | v3-direct only |
| `repro-583-tag-app-cache-negative-1.nix` | Tag::App cache: bad behavior under specific shape | issue #583 | v3-direct, negative |
| `repro-583-tag-app-cache-negative-2.nix` | Same family | issue #583 | v3-direct, negative |
| `repro-583-tag-app-cache-positive-1.nix` | Tag::App cache: correct behavior post-fix | issue #583 | v3-direct, positive |
| `repro-583-tag-app-cache-positive-2.nix` | Same family | issue #583 | v3-direct, positive |
| `repro-583-tag-app-cache-positive-3.nix` | Same family | issue #583 | v3-direct, positive |
| `repro-583-tag-app-cache-regression-1.nix` | Regression guard for fix | issue #583 | v3-direct, guard |
| `repro-a12b-op-call-iter-force.nix` | A12b OP_CALL iter-force fix | (A12b series) | v3-direct only |
| `repro-hello-name.nix` | Action plan Phase 1 exit criterion | (Phase 1 closure 2026-05-17) | v3-direct only |
| `repro-genericClosure-bytecode-filter.nix` | genericClosure callClosure WHNF-on-return | `b0a0ff2e1` (2026-05-17) | TW + v3-direct must match |
| `repro-removeAttrs-lazy-elements.nix` | removeAttrs element-WHNF force | within `7adc7e61f` | TW + v3-direct must match |
| `repro-isTrueValue-slot.nix` | Tag::Slot reaching OP_NOT via CFF_FORCE_WB_PTR_KEEP | within `7adc7e61f` | TW + v3-direct must match |
| `repro-app-memo-regression.nix` | Tag::App `evaluated`-field memo (pos + neg via gate) | `d3e41c13d` (2026-05-18) | both with + without `NIX_V3_NO_APP_MEMO=1` |
| `repro-hello-name-real.nix` | Real-nixpkgs hello.name smoke (Option 4 hybrid guard) | (session 2026-05-17/18) | v3-direct vs TW oracle |
| `repro-beta-reduce.nix` | IR Phase A semantic guard (8 patterns) | (session 2026-05-18) | TW + v3 ON + v3 OFF (NIX_V3_NO_BETA_REDUCE=1) all match |
| `repro-beta-reduce-perf.nix` | IR Phase A alloc-reduction guard | (session 2026-05-18) | v3 ON: 10 closures; v3 OFF: 19 closures (47% reduction) |
| `repro-primop-fold.nix` | IR Phase B semantic guard (11 patterns) | (session 2026-05-18) | TW + v3 ON + v3 OFF (NIX_V3_NO_PRIMOP_FOLD=1) all match; on static `length [1..10]`: insns 56→23, lists 1→0 |
| `repro-stream-fusion.nix` | IR Phase C semantic + perf guard (foldl'+map → __foldlMap) | (session 2026-05-18) | TW + v3 ON + v3 OFF (NIX_V3_NO_STREAM_FUSION=1) all match; N=100K perf budget 5s catches PrimOpCall-shape regression (60× slowdown) |
| `repro-lambda-lift.nix` | IR Phase D semantic + alloc guard (closure-free intern) | (session 2026-05-18) | TW + v3 ON + v3 OFF (NIX_V3_NO_LAMBDA_LIFT=1) all match; N=100 alloc guard: ON saves ~99 closure allocs vs OFF (224 vs 323) |
| `repro-path-with-context-coerce.nix` | Tag::Path coerceToString → /nix/store + `__structuredAttrs` env emission | `78fa43631` (2026-05-19) | TW + v3-direct must match |
| `repro-665-tostring-vs-derivcoerce.nix` | `toString` non-copying vs new `__derivCoerce` copying; bash bootstrap cascade fix | `7b2535fc9` (2026-05-19, #665) | TW + v3-direct must match |
| `repro-666-derivation-equality.nix` | `primops.cc` `valueEqual` derivation outPath special-case (was diverging from `vm.cc` `valueEqual`) — `builtins.elem` + `lib.unique` on derivations | `45da225c6` (2026-05-19, #666) | TW + v3-direct must match |
| `repro-667-assert-force.nix` | `OP_ASSERT` CFF_FORCE_RETRY iterative-force protocol (was the only bool-consuming opcode missing it) — unblocks gtk3 / firefox-unwrapped / firefox | `120def5bc` (2026-05-19, #667) | TW + v3-direct must match |
| `repro-668-defer-across-branch.nix` | #542 emit-time deferring leaked OP_SET_LOCAL ops INSIDE then-block via flushAllDeferred-on-block-entry; else-path saw mismatched stack depth → later OP_GET_LOCAL on Uninit slot → STR_CONCAT cascade → SIGTRAP on go.drvPath | (2026-05-19, #668) | TW + v3-direct must match |
| `repro-672-tojson-context.nix` | `builtins.toJSON` lost string context for interpolated paths / derivations (lib.generators.toLua → writeText drvs missing inputDrvs); also exercises `__toString` lookup path | (2026-05-19, #672) | TW + v3-direct must match |
| `repro-673-basename-dirof-context.nix` | `baseNameOf` / `dirOf` dropped string context on String input; preserved on Path input (no context to drop). Found during #672 audit. | (2026-05-19, #673) | TW + v3-direct must match |
| `repro-674-no-ctx-primops.nix` | `parseDrvName`/`splitVersion`/`getEnv`/`compareVersions` silently accepted contexted strings (TW rejects via `forceStringNoCtx`); also `toXML` dropped context. Fix mirrors TW's exact error string for the rejection path. | (2026-05-19, #674) | TW + v3-direct must match |
| `repro-669-lambda-contextual-name.nix` | `nix eval --impure` printer emits the contextual binding name for let/attr-bound lambdas (matches TW's `«lambda foo @ pos»`); anonymous lambdas stay nameless. Plumbed via new `LambdaDescriptor::contextualName` (lower.cc reads `ExprLambda::name`). | (2026-05-19, #669 follow-up) | TW + v3-direct must match |
| `repro-670-671-ghc-sphinx-bisection.nix` | Bisection result: `haskell.compiler.{ghc96,ghc98,ghc910,ghc984}` with `enableDocs=false` produce byte-identical drvPaths to TW. Sphinx/docs is the trigger for both #670 (SIGTRAP) and #671 (fake-store fallback). | (2026-05-19, #670/#671 bisection) | TW + v3-direct must match |
| `repro-670-671-python-mismatch.nix` | Root-cause guard: ghc94/96/98/910/984 must all produce byte-identical drvPaths to TW. Pre-fix the dangling-string_view bug in primDerivationStrictNative made ghc96+ non-deterministically fail. | (2026-05-20, #670/#671 FIX) | TW + v3-direct must match |
| `repro-675-tojson-shortcircuit.nix` | `nix eval --impure --json` on a derivation must emit just its outPath string (TW parity); pre-fix v3 recursed into the full transitive graph (>3 min, 0 bytes for hello.drvAttrs.src). | (2026-05-20, #675) | TW + v3-direct must match |
| `run-676-apply-cu-stability.sh` | `nix eval --impure --apply <fn>` must not SIGTRAP when the apply fn returns a non-arg-derived value. Pre-fix: RootResult moved via `std::optional::emplace` left closure `cu` pointers dangling at the pre-move stack address; dispatchLoop read garbage bytecode. Fix: heap-allocate CompilationUnit via `std::unique_ptr` so its address is move-stable. | (2026-05-20, #676) | TW + v3-direct must match (9 shapes via shell driver — `.nix` fixtures can't repro since the bug needs two `runRootExpr` calls) |
| `run-677-default-print-errors.sh` | Default-mode `nix eval --impure` must catch per-attr/elem errors as `«error: <msg>»` inline (TW `ValuePrinter` parity), emit verbatim `throw` message (no "v3 throw:" prefix), emit TW's exact `abort` phrasing, short-circuit derivation print without forcing `passthru.tests`, and dedup by caller-Value-address (not stack-local) so sibling structurally-equal lists both print in full. Pre-fix: upfront `forceDeep` propagated the throw to top-level and aborted the whole print. | (2026-05-20, #677) | TW + v3-direct must match (6 shapes via shell driver) |
| `run-678-error-message-parity.sh` | Core error-message text matches TW byte-for-byte for: attribute-missing (select + getAttr), head/tail empty list, elemAt out-of-bounds (with index + size), division by zero, add-string-to-integer. Pre-fix v3 emitted v3-specific debug names (`v3 OP_ATTRS_SELECT: ...`, `v3 primop head: ...`, etc.). | (2026-05-20, #678) | TW + v3-direct must match (8 shapes via shell driver) |
| `run-679-cosmetic-parity.sh` | `builtins.unsafeGetAttrPos` returns null for synthetic sources (`--expr`/`<stdin>`/`<unknown>`), matching TW's mkPos non-SourcePath behavior. Pre-fix v3 leaked `{ file = "<string>"; ... }` to users. Plus printClosureToken helper refactor. | (2026-05-20, #679) | TW + v3-direct must match (4 shapes via shell driver) |
| `run-680-semantic-strictness.sh` | Several SEMANTIC bugs: v3 silently accepted `"x" + 1` (→ `"x1"`), `null + 1` (→ `"1"`), `"${1}"` (→ `"1"`), `true + 1` (→ `"11"`), and `builtins.length "abc"` (→ 3). TW errors on all. Plus message alignment for infinite-recursion + comparison errors. | (2026-05-20, #680) | TW + v3-direct must match (10 shapes; semantic fixes + message alignment) |
| `run-681-formals-validation.sh` | Lambda with formals called with non-attrset arg must error `expected a set but found <type>` BEFORE body runs. Pre-fix v3 silently produced the body result for ellipsis-only lambdas (`({ ... }: 1) [ ]` returned 1) because `needForce` was false. Also matches TW's exact phrasing for missing/extra-arg errors with lambda contextualName resolution (`function 'foo' called without required argument 'b'`). | (2026-05-20, #681) | TW + v3-direct must match (9 shapes: 5 type-check + 2 missing + 2 extra) |
| `run-682-tofile-context.sh` | `builtins.toFile` must (a) reject contents with derivation context (TW-matching error); (b) include path context entries as refs in the resulting store path's CA hash. Pre-fix v3 ignored contents' context entirely → wrong-hash drvPath for any toFile with path/drv interpolation. **drvPath-affecting bug** — most serious class. | (2026-05-20, #682) | TW + v3-direct must match (3 positive + 2 negative shapes) |
| `run-683-div-float-zero.sh` | Float div-by-zero must raise `division by zero` matching TW (libexpr/primops.cc:4703 unconditional). Three sub-fixes: (a) primDiv now checks all 4 numeric combos (was Int/Int only); (b) opt_const_fold no longer folds float-div-by-zero to ±inf (the "Mirror tree-walker yields ±inf" comment was wrong — TW throws); (c) hashString unknown-algo error includes the algo-list hint. | (2026-05-20, #683) | TW + v3-direct must match (8 negative + 6 positive shapes) |
| `run-684-add-error-context.sh` | `addErrorContext` must preserve the wrapped exception's runtime type so `tryEval` catches AssertionError-class errors through it. Pre-fix v3 collapsed every catch into `std::runtime_error`, breaking `tryEval (addErrorContext "ctx" (throw "x"))` semantics (returned error instead of `{success=false; value=false;}`). | (2026-05-20, #684) | TW + v3-direct must match (4 positive + 2 negative shapes) |
| `run-685-dynamic-attr-name-context.sh` | Dynamic attr names (`{ ${s} = v; }`, `attrs.${s}`, `attrs ? ${s}`) must reject string context (TW's `forceStringNoCtx`). Pre-fix v3 silently accepted contexted strings as attr names and DISCARDED the context. Three opcode sites (INIT_DYN, SELECT_DYN, HAS_DYN) all needed the check. | (2026-05-20, #685) | TW + v3-direct must match (4 negative + 5 positive shapes) |

## Run all repros

The umbrella driver `test/all-v3-tests.sh` invokes the dedicated
`run-*-tests.sh` shell drivers for repro families that have them
(e.g. `run-583-tag-app-cache-tests.sh` for the #583 family,
`run-broader-thunkify-tests.sh` for #495-498, etc.).

For the standalone `.nix` fixtures listed above, the simple recipe is:

```bash
for f in src/libexpr-v3/test/repro-*.nix; do
  tw=$(build/src/nix/nix eval --impure -f "$f" 2>/dev/null)
  v3=$(NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 \
       build/src/nix/nix eval --impure -f "$f" 2>/dev/null)
  printf "%-60s TW=%-30s v3=%s\n" "$(basename "$f")" "$tw" "$v3"
  if [[ "$tw" != "$v3" ]]; then echo "DIVERGE"; fi
done
```

(Some fixtures are TW-only or v3-only by design — see the table.  The
driver above doesn't handle those; the per-family shell scripts do.)

## Coverage gaps (TODO)

The 2026-05-17/18 session identified the following missing repros;
they're listed here as homework rather than committed empty fixtures.
Once added, they go in the table above.

- (none currently; the 5 from the session are now committed)

If you fix a bug and don't add a repro: see Rule 0 in
`src/libexpr-v3/CLAUDE.md` (every commit must answer "what hypothesis
does this kill?") and `LESSONS_LEARNED_2026-05-15.md` §4.8 (bisect-to-
fixture workflow).

---

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0
