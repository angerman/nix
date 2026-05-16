# V3 vs TW: Operators, Comparisons, Arithmetic, Strings, Paths

Audit date: 2026-05-11
Scope: operator opcodes (`OP_ADD`, `OP_SUB`, `OP_MUL`, `OP_DIV`, `OP_EQ`, `OP_NEQ`, `OP_LESS`, `OP_NOT`, `OP_AND_BRANCH`, `OP_OR_BRANCH`, `OP_IMPL_BRANCH`, `OP_BRANCH_FALSE`, `OP_STR_CONCAT`, `OP_LIST_CONCAT`, `OP_ATTRS_UPDATE`, `OP_ATTRS_UPDATE_TAIL`, `OP_ASSERT`) and their TW equivalents in `src/libexpr/eval.cc` (`ExprOpEq/NEq/And/Or/Impl/Update/ConcatLists/Not`, `ExprConcatStrings`, `ExprIf`, `ExprAssert`, `eqValues`, `CompareValues` in `primops.cc`).

## Summary
- **v3 OP_STR_CONCAT permits Int/Float/Bool/Null in `+` mode that TW rejects.** v3's coerceToString unconditionally accepts them; TW passes `coerceMore=false` to `coerceToString`, so `"foo" + 1` succeeds in v3 (yields `"foo1"`) and throws `TypeError` in TW.
- **v3 boolean opcodes (`OP_AND_BRANCH`, `OP_OR_BRANCH`, `OP_IMPL_BRANCH`, `OP_BRANCH_FALSE`) silently treat non-Bool LHS as the non-jump case.** TW's `evalBool` raises `TypeError` if the condition is not a Bool. `OP_NOT` and `OP_ASSERT` *do* throw via `isTrueValue`, but with a generic `runtime_error` rather than the `TypeError` shape callers expect.
- **v3 `valueLess` lacks Path comparison.** TW's `CompareValues` supports `nPath` via `pathStrView()`; v3 falls into the "unsupported operand types" throw for paths. v3 also has no `assertEqValues` diagnostic for `assert a == b; body`, and `valueEqual` compares Paths by string only (ignoring `pathAccessor`).

## Confirmed divergences

### 1. `+` operator: numeric/null/bool coercion in non-interpolation mode
- TW `src/libexpr/eval.cc:2550-2647` (`ExprConcatStrings::eval`): when `forceString=false` and first element is non-string, calls `state.coerceToString(..., coerceMore=false, ...)` (line 2613-2614). The `coerceMore` flag is **false**, so `coerceToString` (eval.cc:2995-3033) throws `TypeError` on Bool/Int/Float/Null/List.
- v3 `src/libexpr-v3/vm.cc:6682` (`OP_STR_CONCAT`): always invokes `coerceToString(p, forceStr)` regardless of `forceStr`. v3's `coerceToString` (vm.cc:608-664) returns `"1"/""` for Bool, `to_string(int)` for Int, etc.

Effect: `"foo" + 1` succeeds with `"foo1"` in v3; throws `TypeError` in TW.
Also `"foo" + null` and `"foo" + true` differ similarly.

### 2. `&&`, `||`, `->`: non-bool LHS silently accepted in v3
- TW `eval.cc:2318-2337` routes through `EvalState::evalBool` (eval.cc:1439-1465), which throws `TypeError` "expected a Boolean but found %1%" for non-Bool.
- v3 `vm.cc:2133-2156`:
  - `OP_AND_BRANCH` (2133-2141): `if (v.isBool() && v.payload.i == 0) ip = operand; else pop`. Non-bool LHS → falls through to RHS without checking.
  - `OP_OR_BRANCH` (2142-2148): symmetric: non-bool LHS falls through to RHS.
  - `OP_IMPL_BRANCH` (2150-2156): non-bool LHS → fall-through; never pushes the early-true result.

Effect: `null && true` returns `true` in v3, raises `TypeError` in TW. Same for `||`, `->`. Note that the RHS is `evalBool`-ed by TW too, so the type error fires regardless of which side is wrong.

### 3. `if c then ...` and unwrapped `OP_BRANCH_FALSE`: non-bool condition
- TW `eval.cc:2264-2267` (`ExprIf::eval`): uses `evalBool`, throws on non-bool.
- v3 `vm.cc:2160-2166` (`OP_BRANCH_FALSE`): `if (v.isBool() && v.payload.i == 0) ip = operand;` — non-bool is treated as truthy (no jump), executes the then-branch silently.

Effect: `if null then "yes" else "no"` returns `"yes"` in v3, throws in TW.

### 4. `assert c; body`: condition type-check
- TW `eval.cc:2270-2293` routes through `evalBool` → TypeError on non-bool. v3 `vm.cc:6722-6726` calls `isTrueValue(c)` (vm.cc:591-595) which throws `runtime_error("v3: expected bool")` — same boolean coverage but loses the TypeError class (test infrastructure that catches `nix::TypeError` would miss it).

### 5. `assert a == b; body`: no fancy diff message in v3
- TW `eval.cc:2272-2292` introspects `cond` for `ExprOpEq` and on failure calls `assertEqValues` (eval.cc:3170-3378) for a structured diff trace ("string 'a' is not equal to string 'b'", with attribute-level traces).
- v3 has no equivalent. `lower.cc:2825-2834` (`lowerAssert`) lowers `ExprAssert` as `Assert{cond, bodyBlock}` and forces `cond`; on failure `OP_ASSERT` throws a single-line `AssertionError("v3 OP_ASSERT: assertion failed")` regardless of cond shape. Diagnostic UX regression.

### 6. `builtins.throw`/`builtins.abort`: argument coercion
- TW `src/libexpr/primops.cc:1136-1167` calls `coerceToString` on the argument — so `throw ./path` or `throw { __toString = self: "msg"; }` works.
- v3 `src/libexpr-v3/primops.cc:790-796, 1439-1444`: `if (!args[0].isString()) typeError("throw"|"abort", "string");` — rejects everything but plain strings.

Exception class hierarchy itself matches: v3's `ThrownError` derives from `AssertionError` (`src/libexpr-v3/include/v3/errors.hh:33-37`) — `tryEval` catches it. `AbortError` does NOT derive from `AssertionError` (errors.hh:42-46), so `tryEval` does not catch it — matches TW.

### 7. `<`/`<=`/`>`/`>=` on Paths
- TW `src/libexpr/primops.cc:861-936` (`CompareValues`) explicitly handles `case nPath: return v1->pathStrView() < v2->pathStrView();` (line 903-907).
- v3 `vm.cc:557-589` (`valueLess`): no Path branch; falls into `throw std::runtime_error("v3 OP_LESS: unsupported operand types")`.

Effect: `[ ./a ./b ]` sorted with `builtins.sort builtins.lessThan` would fail in v3.

### 8. `==`/`!=` on Paths: accessor not compared
- TW `eval.cc:3414-3417` (`eqValues`): `v1.pathAccessor() == v2.pathAccessor() && v1.pathStrView() == v2.pathStrView()`.
- v3 `vm.cc:495` (`valueEqual`): `case Tag::Path: return std::string_view(a.payload.path) == std::string_view(b.payload.path);`. No accessor concept (v3 stores Path as `const char *`, see `include/v3/value.hh:77`).

Effect: equivalent in single-accessor evals (the common case), but tools that overlay multiple `SourceAccessor`s could see same-string paths from different roots compare equal in v3 only.

### 9. `${attr}` coercion: __toString returning non-string falls through to outPath
- TW `eval.cc:2922-2940` (`tryAttrsToString`): calls `coerceToString` on `__toString`'s result. If `__toString` returns a path, TW recurses into the Path branch of `coerceToString` (eval.cc:2960-2970) and returns the canonical path or copies to store. `tryAttrsToString` succeeds (returns the string); the outer call site does not try `outPath`.
- v3 `vm.cc:6643-6651`: only accepts `__toString` if the return is `Tag::String`. If `__toString` returns a Path/Int/etc., v3 falls through to try `outPath` instead — masking the intent.

### 10. `${attr}` with outPath: missing copyToStore on Path
- v3 `vm.cc:6653-6660`: `outPath` returning a `Tag::Path` does `out.append(forced.payload.path); continue;` — bypasses both the `copyPathToStore` invocation and the store-path context attachment that the Path-arm at line 6665-6681 normally does for `forceStr=true`.
- TW does `coerceToString(p, ...)` recursively, which copies the path and records context.

Effect: an attrset whose `outPath = ./local;` interpolated with `${pkg}` should copy `./local` to the store; v3 instead embeds the raw filesystem path with no context.

## Suspect areas

### Path normalization in `path + string`
- v3 `vm.cc:6691-6697`: `std::filesystem::path(out).lexically_normal().string()` + manual trailing-slash strip.
- TW `eval.cc:2632-2637`: `state.rootPath(CanonPath(resultStr))` — Nix's `CanonPath` constructor.
- Possible divergence at edge cases (e.g., embedded `.` / `..`, multiple slashes). Both should normalize, but the algorithms are not 1:1. Worth an empirical check with paths like `./a/b/../c`, `/a/b/./`, `/a//b/c`.

### `string + path` context handling
- v3 OP_STR_CONCAT (vm.cc:6663-6664): only accumulates context entries from `Tag::String` parts via `lookupStringContextEntries(p.payload.str)`. Paths get `copyPathToStore` only when `forceStr=true`.
- For plain `+`: when the first element is a string and a later part is a Path, TW with `copyToStore=true` (eval.cc:2614 `firstType == nString`) copies the path. v3 OP_STR_CONCAT: `forceStr=false` so the Path-arm (vm.cc:6665) is skipped. Effect: `"prefix" + ./local` produces `"prefix./local"` in v3 with no store-copy and no context entry, where TW would produce `"prefix/nix/store/<hash>-local"` with context.
- This is **probably a confirmed divergence** but I'm flagging as Suspect because I haven't traced through `forceStr` plumbing for every parser-emitted `+` path; if the parser sets `forceString=true` for any non-trivial form (it doesn't — see parser.y:312) the divergence stands.

### Integer overflow on `OP_DIV`: only INT64_MIN / -1
- v3 `vm.cc:2051-2070`: only catches `INT64_MIN / -1`. Other arithmetic (OP_ADD/SUB/MUL) catches signed overflow via `__builtin_*_overflow`. OP_STR_CONCAT line 6605 uses `__builtin_add_overflow`. Match with TW which uses `NixInt::valueChecked` and throws "integer overflow in adding %1% + %2%" (eval.cc:2580-2585).
- However v3 throws `std::runtime_error("v3 OP_ADD: integer overflow")` rather than `EvalError` — class hierarchy divergence. Same applies to OP_SUB / OP_MUL / OP_STR_CONCAT.

### Float equality
- Both use raw `==` on `double`. v3 vm.cc:491 (`Tag::Float`), TW eval.cc:3460-3461 (`nFloat`). Same NaN behavior (NaN != NaN both implementations). Match.

### `// ` (OP_ATTRS_UPDATE) value laziness
- Both leave RHS attribute values lazy: TW eval.cc:2382-2412 just copies bindings; v3 vm.cc:5878 calls `mergeBindings` (vm.cc:666-703) which copies entries unmodified. Match.
- v3 has additional collapsing logic for `Tag::Thunk` in Blackhole state with partialBindings (vm.cc:5856-5875) — this is v3-only support for STG-style partial-construction and should be semantics-preserving when fully resolved.

### `++` (OP_LIST_CONCAT) element laziness
- TW eval.cc:2508-2547 (`ExprOpConcatLists::eval` → `concatLists`): copies element pointers; does not force the elements. Match.
- v3 vm.cc:4846-4872: copies element Values into a new ListVec. Force-on-receive at lines 4851-4858 forces only the **lists themselves**, not their elements. Match.

### Force discipline at OP_EQ / OP_NEQ / OP_LESS
- v3 lowers `==`/`!=` via `lowerBinOp` (lower.cc:2058-2072) which inserts emit-time `forceVal` on both operands. v3 lowers `<` via the primop fast path at lower.cc:1427-1430 which also forces. Both ops then enter `valueEqual` / `valueLess` which themselves force on container recursion. Match.

## Matches

- **Empty-attrset short-circuit in `//`**: TW eval.cc:2344-2353 returns the other operand verbatim when one side is empty. v3 `mergeBindings` (vm.cc:666-703) just iterates — no early short-circuit, but the loop trivially returns a copy of the non-empty side. Semantically equivalent, perf-wise v3 always allocates a new Bindings.
- **Derivation equality (`{ outPath=..., type="derivation" } == drv`)**: both compare `outPath` when both sides are derivations (`isDrv` check). TW eval.cc:3432-3438, v3 vm.cc:511-528.
- **Integer/float type-compatibility in `==`/`!=`**: both treat `1 == 1.0` as `true`. TW eval.cc:3394-3398, v3 vm.cc:485-486.
- **`==`/`!=` between different types returns false**: TW eval.cc:3401-3402, v3 vm.cc:484-487. Functions return false (v3 returns true only for `insideContainer` with identical pointer, matching TW's `&v1 == &v2` short-circuit — v3 vm.cc:539-546 has this exact logic).
- **Lambdas are unequal**: TW eval.cc:3453-3454 returns false; v3 vm.cc:539-545 same.
- **String contexts merge**: v3 OP_STR_CONCAT lines 6663-6664 (string parts) and 6674-6680 (path parts under `forceStr`) accumulate context entries via `lookupStringContextEntries` / `copyPathToStore`. Then dedup + attach to result string (line 6712-6717). Matches TW's `copyContext`.
- **Path context-on-path arithmetic forbidden**: TW eval.cc:2627-2631 rejects `path + "string-with-store-path-context"`. v3 omits this check — see "Suspect" path-arith section. Actually checking again: v3 OP_STR_CONCAT doesn't differentiate; it would happily build a Path output with the underlying context attached only to the (discarded) String result. Since the result is a Path with no context payload, the offending context is silently dropped. **This is actually a divergence** — but lower priority than the others above.
- **Branch laziness in `if`**: emit.cc:652-670 emits then/else into separate blocks behind `OP_BRANCH_FALSE` + `OP_JUMP`. Unselected block's bytecode is never executed. Match with TW (only `then` or `else_` is `eval`'d).
- **List laziness on construction**: v3 lower.cc:2095-2105 thunkifies elements. Match with TW.
- **Short-circuit emission**: v3 lower.cc:2940-2954 emits RHS into a separate block; only entered when the branch op falls through. Match with TW's `evalBool` chained logic.
- **Throw/Abort exception hierarchy**: v3 `ThrownError : AssertionError` (errors.hh:33-37) caught by `tryEval`. `AbortError` not derived from `AssertionError`, NOT caught by `tryEval` (primops.cc:6846-6855 catches only `AssertionError` variants). Matches TW (`ThrownError` derived from `AssertionError`, `Abort` from `EvalError`).
- **`__lessThan`/`__sub`/`__mul`/`__div` primop fast path**: parser.y:302-315 desugars binops to `__lessThan`/`__sub`/etc. primop calls; v3 lower.cc:1395-1431 detects these and emits direct `Less`/`Sub`/`Mul`/`Div` IR. v3 builds the equivalent VM ops; semantics preserved.
- **Search path `<name>`**: parser.y:365-369 desugars to `__findFile __nixPath "name"`. v3 implements `__findFile` (primops.cc:1861-1898), `__nixPath`. Match.
