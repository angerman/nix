# Comprehensive Semantic Audit: v3 VM vs TW (2026-05-11)

## Method

Six parallel review agents inspected disjoint semantic surfaces of the
v3 bytecode VM (`src/libexpr-v3/`) against the tree-walker reference
(`src/libexpr/eval.cc` + `src/libexpr/primops.cc`). Each agent produced
a structured report at:

- `force_blackhole_semantics.md` — Agent 1 (force, WHNF, blackhole,
  exception propagation)
- `attrset_semantics.md` — Agent 2 (attribute sets, `//`,
  inherit-from, `__functor`, ordering)
- `function_semantics.md` — Agent 3 (lambdas, formals, calls,
  closure capture)
- `scope_select_let_semantics.md` — Agent 4 (var lookup, with-stack,
  let-rec, ExprSelect)
- `operators_strings_semantics.md` — Agent 5 (binary ops, comparison,
  string/path coercion)
- `errors_tryeval_primops_semantics.md` — Agent 6 (exception types,
  `tryEval`, error traces, primop fidelity)

After collecting reports, I spot-checked the highest-impact claims
against actual source (boolean opcode handling, missing-formal
validation, `//` operand order, `primAddErrorContext` rewrap,
`primV3CallBridge1` fallback inconsistency, and TW's
`handleEvalExceptionForThunk` / v3's acknowledged gap). Every spot
check confirmed the agent's claim — see "Verified by re-reading code"
markers below.

The numbering in this document is **independent** of each sub-report's
numbering; cross-refs (e.g. "A1-D2") use `Agent<n>-<their-id>`.

---

## Critical findings (high impact, correctness-affecting)

### C1. Non-Bool LHS silently accepted by `&&`, `||`, `->`, `if`, `OP_BRANCH_FALSE`
- **Verified by re-reading code.** `vm.cc:2138, 2146, 2155, 2164`:
  each opcode tests `if (v.isBool() && v.payload.i == 0/1)` — a
  non-Bool value falls through the `&&` to the **non-jump** branch:
  - `null && true` → `true` (RHS) on v3; **TW raises TypeError**
    (`eval.cc:1439-1465` `evalBool`).
  - `if null then x else y` → `x` on v3 (vm.cc:2164: non-Bool ≠ 0,
    no jump, then-branch runs); **TW raises TypeError**.
- **Impact**: silently changes program meaning, not just error
  messages. A latent typo (`if undefinedVar` mis-typed as something
  else, `assert someAttr` where `someAttr` is mistyped) takes a
  different branch instead of being reported as ill-typed.
- **Source**: Agent 5 D2, D3.

### C2. Missing-required formal not validated (silently returns body value)
- **Verified by re-reading code** (`vm.cc:3398-3413`): the validation
  loop iterates `forcedArg.payload.bindings` and checks each arg name
  against `desc->formals` — i.e., it detects **extras**, not missing
  required formals.
- A non-default required formal is only enforced when the body
  references it (via the synthetic `LetRec` thunk + `AttrSelect`).
- **Behaviour gap**:
  - `({a, b}: 42) {a = 1;}` → v3 returns `42`; **TW raises TypeError**
    ("function called without required argument 'b'").
  - When the body does reference the missing formal, both fail, but
    v3's error is the generic "v3 OP_ATTRS_SELECT: attribute not
    found" — no formal name, no lambda name, no position.
- **Test impact**: `tests/functional/lang/eval-fail-missing-arg.err`
  and `eval-fail-empty-formals.err` will diverge.
- **Source**: Agent 3 D1, D2.

### C3. `primAddErrorContext` erases exception type → breaks `tryEval` transitivity
- **Verified by re-reading code** (`primops.cc:2712-2725`): catches
  `BlackholeError` and rethrows as `BlackholeError` (good), but
  catches `std::exception &` and rethrows as **plain
  `std::runtime_error`** — destroying `AssertionError` /
  `ThrownError` typing.
- Downstream `tryEval` catches only `AssertionError`-class
  exceptions, so:
  - `tryEval (addErrorContext "ctx" (throw "x"))` → `tryEval` does
    **not catch**; the exception escapes.
- **Impact**: nixpkgs `lib/modules.nix` wraps every config evaluation
  in `addErrorContext`. Any module-evaluation `throw` that is
  intended to be catchable by a downstream `tryEval` is no longer
  catchable on v3.
- **Source**: Agent 6 #1.

### C4. `primV3CallBridge1` rewraps non-Blackhole exceptions inconsistently
- **Verified by re-reading code** (`primops.cc:3017-3019`):
  `fallbackToTreeWalker` does `throw std::runtime_error(ex.what())`
  when `ex` is **not** a `BlackholeError` or `fallbackExpr` is null.
  Sibling `primV3ForceAttr` (`primops.cc:3404-3405`) does the **right
  thing** — `throw;` to rethrow the original.
- **Result**: an `AssertionError`/`ThrownError` thrown across the
  bridge1 surface gets retyped to `runtime_error`; the same exception
  thrown across the force-attr surface keeps its type.
- **Source**: Agent 6 #2.

### C5. `OP_STR_CONCAT` accepts numeric / Bool / Null in `+` mode
- **TW** (`eval.cc:2613-2614`): when first element is non-string,
  passes `coerceMore=false` to `coerceToString` — Bool/Int/Float/Null
  trigger `TypeError`.
- **v3** (`vm.cc:6682`): unconditionally calls `coerceToString(p,
  forceStr)` whose body (`vm.cc:608-664`) accepts Int/Float/Bool/Null
  in all cases.
- **Behaviour gap**: `"foo" + 1` → `"foo1"` on v3; **`TypeError` on
  TW**.
- **Source**: Agent 5 #1.

### C6. `path + "context-string"` silently drops context
- **TW** (`eval.cc:2627-2631`): `path + string-with-store-context`
  raises an error.
- **v3** (`vm.cc:6663-6664, 6691-6697`): path-arm under
  `forceStr=false` accumulates context from String parts but the
  result is a Path with **no context payload** — the offending
  context is silently dropped.
- **Impact**: derivations using `./path + "${storedrv}"` patterns
  lose the runtime dep info.
- **Source**: Agent 5 (matches section, flagged as confirmed).

### C7. Eval-order of `//` reversed (TW evaluates RHS first, v3 evaluates LHS first)
- **Verified by re-reading code**. TW `eval.cc:2487`
  (`e2->evalForUpdate(...)` first, then walks the left spine). v3
  `lower.cc:2068-2069` (`lowerBinOp` emits `forceVal(lowerExpr(e->e1))`
  first, then e2).
- **Repro**: `(throw "lhs") // (throw "rhs")` → TW throws `"rhs"`;
  v3 throws `"lhs"`.
- **Severity**: rarely hit by real code but visible in tests that
  pin operand order.
- **Source**: Agent 2 D1.

### C8. `Tag::PrimOpApp` not chased at `OP_ATTRS_SELECT` / `OP_ATTRS_HAS` head
- v3's select/has-attr force only `Tag::App`, `Tag::Thunk`,
  `Tag::Slot` (`vm.cc:5805-5807`, `:5334-5337`). `Tag::PrimOpApp`
  passes through without force, then the `isAttrs()` check fails and
  the user sees "not an attrset" instead of forcing the curried
  primop to a result value.
- **Repro shape** (theoretical, not constructed): something where a
  partially-applied primop ends up as the select head — e.g.
  `((builtins.foldl' f) or {}).x` — but it's not clear any real Nix
  code can land in this configuration.
- **Source**: Agent 4 SA-3.

---

## Significant correctness divergences (narrower impact)

### S1. Tag::Blackhole as a propagable Value (no TW counterpart)
- **v3** has TWO blackhole representations: `ThunkState::Blackhole`
  (parallel to TW's `eBlackHole`) **plus** a `Tag::Blackhole` value
  singleton (`vm.cc:4443-4446`, `:8062-8103`). The latter is pushed
  for cross-VMState Black thunks and can propagate through subsequent
  ops (where TW would always throw at the cycle).
- **Downstream** ops that don't handle `Tag::Blackhole` (e.g.
  `coerceToString` at `vm.cc:641` falls into the default branch)
  produce a different error message at a different op than TW's
  "infinite recursion" at the originating force.
- Gated by `NIX_V3_NO_BLACKHOLE_AS_VALUE=1` (default-on as a value).
- **Source**: Agent 1 D3.

### S2. STG "WHNF recovery" returns approximate partial Bindings
- When a Black thunk is on the current frame **and** a partial
  Bindings is registered via `publishToNearestBlackThunkFrame`, v3
  returns `pickLargestLayer(chain)` as a `Tag::Attrs` and marks the
  frame `CFF_TAINTED` so OP_RETURN doesn't memoize
  (`vm.cc:8132-8283`, `:4471-4485`).
- TW has no equivalent — it throws.
- **Two readers can observe different snapshots** of the same logical
  thunk depending on when publishing happened relative to the read.
- This is the architecture behind the `#455`/`#496`/`#498`/`#546`/
  `#558` cluster — deliberate, but a real semantic divergence.
- Gated by `NIX_V3_NO_STG_WHNF=1` (default-on).
- **Source**: Agent 1 D6, Agent 2 D2.

### S3. `OP_ATTRS_REC_INIT` publishes partial Bindings for ALL non-empty attrsets
- **v3 emit** (`emit.cc:702-744`): emits `OP_ATTRS_REC_INIT` (not
  `OP_ATTRS_INIT`) for **every** non-empty attrset, regardless of
  whether the attrset is `rec`. The opcode publishes the
  in-construction Bindings into the partial-bindings registry of the
  surrounding Black thunk.
- **TW** has no analog.
- **Consequences**:
  - A `with`-lookup inside the same thunk can see a partial sibling
    attrset.
  - An `inherit-from` clause publishes with `mkNull` placeholders in
    the IF slots (`vm.cc:5060`); a chain-peek before the trailing
    `OP_ATTRS_REC_SET` returns `null`.
  - The registry-wide "chain peek" at `vm.cc:5654-5682` can return a
    value from a completely unrelated thunk's chain.
- **Source**: Agent 2 D2, Agent 4 CD-6.

### S4. `OP_WITH_LOOKUP` swallows per-scope `BlackholeError`
- **v3** (`vm.cc:759-772`, `:799-808`): catches `BlackholeError` from
  the per-scope `forceValue`, records `anyBlackholed`, continues to
  the outer scope. Only throws if NO scope resolved the name.
- **TW** (`eval.cc:946-962`): the very first `InfiniteRecursionError`
  from `forceAttrs(*env->values[0])` propagates immediately.
- **Result**: v3 can resolve a name from an outer with-scope where
  TW would have died.
- **Source**: Agent 1 D5.

### S5. Inherit-from from-exprs not unconditionally thunkified
- **TW** (`eval.cc:1513-1523`): `from->maybeThunk(state, up)`
  unconditionally — every from-expr is lazy.
- **v3** (`lower.cc:1686-1822`): default mode thunkifies only a
  narrow `self.X` heuristic (gated by `NIX_V3_SELF_DOT_MAX_LEVEL=4`),
  or under env-var overrides (`NIX_V3_STG`,
  `NIX_V3_INHERIT_FROM_THUNK_ALL`, `NIX_V3_LAMBDA_SKIP`).
- Non-matched from-exprs are lowered **eagerly** — runs at attrset
  construction time, can force a sibling mid-construction (the
  cycle that S2's STG WHNF recovery is designed to mask).
- The dependency chain S5 → S3 → S2 is the architectural root of the
  `#455`/`#496`/`#498`/`#546`/`#548` family.
- **Source**: Agent 2 D3, Agent 4 CD-2.

### S6. Function equality returns `false` instead of `AssertionError`
- **TW** (`eval.cc:3344-3346`): raises `AssertionError("distinct
  functions and immediate comparisons of identical functions compare
  as unequal")` for ANY function comparison.
- **v3** (`vm.cc:539-546`): direct comparison returns `false`; inside
  a container returns `true` iff closure pointers match. Never
  throws.
- **Behavioural gap**: `let f = (x: x); in f == f` → `false` on v3;
  raises on TW. Visible to programs guarding equality with `tryEval`.
- **Source**: Agent 3 D6.

### S7. `valueLess` lacks Path branch
- TW's `CompareValues` (`primops.cc:903-907`) handles
  `nPath: pathStrView() < pathStrView()`. v3's `valueLess`
  (`vm.cc:557-589`) has no Path branch → throws "unsupported operand
  types".
- **Impact**: `builtins.sort builtins.lessThan [./a ./b]` fails on
  v3.
- **Source**: Agent 5 #7.

### S8. `__toString` returning non-string falls through to `outPath`
- **TW** (`eval.cc:2922-2940`): recursively coerces the result of
  `__toString`, so `__toString` returning a Path copies-to-store.
- **v3** (`vm.cc:6643-6651`): only accepts string return; otherwise
  falls through to try `outPath` — masking the user's intent.
  - Separately, `outPath` returning a `Tag::Path` (`vm.cc:6653-6660`)
    skips `copyPathToStore` even in interpolation context.
- **Source**: Agent 5 #9, #10.

### S9. `builtins.throw` / `builtins.abort` reject non-string args
- **TW** (`primops.cc:1136-1167`): coerces via `coerceToString` — so
  `throw ./path` or `throw { __toString = self: "msg"; }` works.
- **v3** (`primops.cc:790-796`, `:1439-1444`): hard-requires
  `isString()`.
- **Source**: Agent 5 #6.

### S10. v3 standalone has fake-store paths for `builtins.path` and `derivationStrict`
- `primops.cc:6241-6248` and `:4900-4904`: when `state.nixEvalState`
  is null, v3 returns synthetic `/v3-fake-store/...` paths (FNV-1a,
  non-cryptographic).
- TW always uses the real store.
- **Impact**: only relevant for v3-standalone tests; bridged v3 uses
  TW's store paths.
- **Source**: Agent 6 #13.

### S11. `primReadFile` doesn't scan content for store-path references
- **TW** (`primops.cc:2237-2263`): `PathRefScanSink::fromPaths(refs)`
  scans file content to add Opaque context entries.
- **v3** (`primops.cc:2510-2568`): only forwards input context.
- **Impact**: derivation runtime-dep inference from files read by
  `builtins.readFile`. Real-world impact in nixpkgs.
- **Source**: Agent 6 #8.

---

## Error-quality divergences (cosmetic, but pervasive)

These are NOT correctness bugs, but every user-visible Nix error from
v3 is **materially worse** than TW. Every v3 `throw` is one of:

- `std::runtime_error("v3 OP_*: <generic message>")` — 89 sites in
  `vm.cc`, 91 in `primops.cc`.
- No source position attached.
- No "while evaluating attribute X" trace chain (TW chains
  `addErrorTrace` at ~50+ sites).
- No `withSuggestions` / "did you mean ...?" for typos.
- Not derived from `nix::EvalError`, `nix::TypeError`,
  `nix::AssertionError`, `nix::StackOverflowError`, or
  `nix::InfiniteRecursionError` — only `ThrownError` and
  `BlackholeError` are typed.
- "v3 throw: " / "v3 abort: " prefix on `builtins.throw`/`abort`
  messages (`primops.cc:795`, `:1444`).
- `OP_ASSERT` says only `"v3 OP_ASSERT: assertion failed"` — TW
  renders the failing condition + source pos + frame (with
  `assertEqValues` diff for `assert a == b`).
- Stack overflow: hard-coded `kMaxCallDepth = 5000`
  (`vm.cc:96`), no `settings.maxCallDepth` plumbing; TW default
  10000, settable.

### Golden tests known to drift
- `eval-fail-missing-arg.err.exp` (Agent 3 D1).
- `eval-fail-empty-formals.err.exp` (Agent 3 D2).
- Likely many more `*.err.exp` files since most v3 errors carry no
  position info.

---

## Performance / observability gaps (not correctness)

- **No slot mutation on first force in `OP_GET_LOCAL_FORCE`**
  (`vm.cc:1869-1914`). Subsequent reads pay an extra Tag::Thunk →
  Evaluated indirection. TW writes WHNF in place. (Agent 1 D7.)
- **Profiler hooks not wired**: `NIX_COUNT_CALLS`,
  `EvalProfiler::preFunctionCallHook` / `postFunctionCallHook`,
  `nrFunctionCalls`, `functionCalls[lambda]`, `primOpCalls`,
  `primOpTimerStack`, `--trace-function-calls`, `debugTraceStacker`
  — none invoked from v3. The entire profile feature is silently
  inert under v3. (Agent 3 D7.)
- **`trylevel` not tracked** in v3 → REPL debugger cannot skip
  inside-try frames. (Agent 6 #6.)
- **v3-specific tail-call iteration cap**: 10⁷ (`vm.cc:3507-3513`).
  Programs that work on v3 may exhaust at unexpected depth. TW has no
  TCO; the same program either hits `max-call-depth` or C-stack
  overflow. (Agent 3 S4.)
- **`__curPos` eager line/column ints**, synthetic "<stdin>" /
  "<string>" / "<unknown>" file strings (`lower.cc:678-708`); TW emits
  `null` on non-SourcePath origins and lazy `App(lineOfPos, posV)`.
  (Agent 4 CD-1, CD-5.)

---

## Subtle divergences (low practical impact)

- **`forceValueDeep` cycle detection keyed differently**: TW keys by
  Value pointer (`eval.cc:2730-2774`), v3 keys by container pointer
  (Bindings*/ListVec*) (`primops.cc:1457-1480`). v3 inserts
  **after** force; TW inserts **before**. Trace ordering on
  cycle-then-throw scenarios differs. (Agent 1 D4.)
- **`primGenericClosure` keys by tag-prefixed string hash**
  (`primops.cc:2201-2228`); TW uses `CompareValues`. Different error
  messages on mixed-type keys; v3 silently accepts `Tag::Bool` keys
  TW would reject. (Agent 6 #10.)
- **JSON parse errors are nlohmann's `parse_error`, not
  `nix::JSONParseError`** (`primops.cc:6674`). No position trace.
  (Agent 6 #15.)
- **`primReadDir` reports `"unknown"`** for files where the
  filesystem returns DT_UNKNOWN; TW resolves via `lstat`.
  (Agent 6 #28.)
- **`primScopedImport`** silently drops scope names that aren't
  identifiers / are Nix keywords (`primops.cc:6406-6429`). (Agent 6
  #9.)
- **`primTypeOf`** has dead-code fallbacks `"thunk"` and `"unknown"`
  (`primops.cc:728, 734-735`). (Agent 6 #14.)
- **Path equality ignores `pathAccessor`** (`vm.cc:495` vs
  `eval.cc:3414-3417`). (Agent 5 #8.)
- **Integer overflow throws `runtime_error`**, not `EvalError`.
  (Agent 5 suspect.)
- **`OP_APPLY_OVERRIDES` allocates new Bindings on add-path**
  invalidating partial-chain registry entries that point at the old
  Bindings (`vm.cc:5206`). Probably benign today; future opcode that
  preserves the pointer across mutation would expose. (Agent 2 S3.)

---

## v3-extensions (no TW analog — not divergences in the bug sense)

- **Tail-call optimization** + 10M tail-call iteration cap
  (`vm.cc:3507-3513`). TW has no TCO.
- **Partial-Bindings registry & chain peek** (`vm.cc:1316-1660`,
  `:5286-5378`) with `largest-layer-wins` heuristic. v3-only,
  enabling lib.fix-style fixed-points to make progress despite
  intermediate Black thunks.
- **Bridge thunk type** (`Tag::Bridge`) and `tryDispatchTWLambdaInV3`
  shortcut (`vm.cc:2865-2873`, `:8586-8592`).
- **Various opcodes with no TW counterpart**:
  `OP_ATTRS_LET_REC_INIT`, `OP_ATTRS_REC_INIT_TAIL`,
  `OP_APPLY_OVERRIDES`, `OP_THUNK_SET_LOCAL_THROUGH_CELL`,
  `OP_LIT_BUILTINS`, etc.

---

## Cross-cutting themes

Three architectural choices recur across multiple agents' findings:

1. **No typed-error machinery**. All ~180 throw sites in v3 raise
   `std::runtime_error` (with two exceptions, `BlackholeError` and
   `ThrownError`/`AssertionError`/`AbortError`). This means:
   - Every error loses TW's typed-catch routing.
   - Every error loses source position.
   - Every error loses "while evaluating ..." trace chains.
   - Bridge code that catches `std::exception` and rewraps
     (C3, C4) silently breaks `tryEval` transitivity.

2. **Eager-vs-lazy asymmetry in attrset construction**. v3's
   `OP_ATTRS_REC_INIT` publishes EVERY non-empty attrset to the
   surrounding Black thunk's partial-bindings registry, and v3's
   `inherit-from` lowering is heuristic-gated (S5). The partial-
   bindings chain-peek then becomes the "safety net" that catches
   the cycles eager lowering creates (S2). TW lazily thunkifies
   from-exprs and never publishes partial state — no safety net
   needed. The whole class of `lib.fix` / `lib.extends` /
   `lib.systems.elaborate` bugs (#455/#496/#498/#546/#548/#558)
   traces to this asymmetry.

3. **Bridge boundary disagrees with itself**. Different bridge
   surfaces (primAddErrorContext, primV3CallBridge1, primV3ForceAttr,
   ffi::applyClosure) handle re-thrown exceptions differently — some
   preserve the type, some erase it, some erase the position. The
   right policy is uniform `throw;` on re-entry; this is the smallest
   fix touching the most user-visible behaviour.

---

## Recommendations (prioritized)

### P0 — silent correctness gaps

1. **Add type-check to boolean opcodes** (`OP_AND_BRANCH`,
   `OP_OR_BRANCH`, `OP_IMPL_BRANCH`, `OP_BRANCH_FALSE`). Throw a
   `TypeError` on non-Bool LHS to match TW. (C1)
2. **Add missing-required-formal validation** to `OP_CALL` after
   the extras check (`vm.cc:3398-3413`). Iterate `desc->formals`;
   for each non-default formal, verify presence in the bindings.
   Throw with formal name + lambda name + position. (C2)
3. **Fix `primAddErrorContext`** to preserve exception type: nested
   `try`/`catch` for each known v3-typed exception
   (`AssertionError`, `ThrownError`, `BlackholeError`, `AbortError`)
   so `tryEval` transitivity survives. (C3)
4. **Fix `primV3CallBridge1::fallbackToTreeWalker`** — replace `throw
   std::runtime_error(ex.what())` with `throw;` to match
   `primV3ForceAttr`. (C4)
5. **Restrict `OP_STR_CONCAT` coercion in `+` mode** to match TW's
   `coerceMore=false` behaviour. (C5)
6. **Forbid `path + string-with-context`** to match TW's error. (C6)

### P1 — error-quality / observability

7. **Migrate v3 throw sites to `nix::EvalError` / `TypeError` /
   `AssertionError` / `InfiniteRecursionError` / `StackOverflowError`
   hierarchy** with positions and lambda names. Pull from
   `desc->name` / `desc->posHandle` / `desc->formals[i].name`.
   This restores `.err.exp` parity for golden tests.
8. **Wire `kMaxCallDepth` to `settings.maxCallDepth`** so
   `--option max-call-depth N` applies to v3.
9. **Wire profiler hooks into `OP_CALL` / `OP_CALL_PRIMOP` /
   `callClosure`** when `EvalProfiler::getNeededHooks()` signals
   demand. Also bump `nrFunctionCalls`, `functionCalls[lambda]`,
   `primOpCalls[name]` to restore `NIX_COUNT_CALLS` output.
10. **Add a minimal error-trace stack** so the most common "while
    evaluating attribute X" / "while calling builtin FOO" /
    "while evaluating the value passed for the lambda argument"
    breadcrumbs survive. A thread-local vector pushed at
    OP_CALL_PRIMOP and selected OP_ATTRS_* sites, drained on throw,
    is mechanically straightforward and would close 80% of the
    error-quality gap.
11. **Make `OP_ASSERT` retain the failed condition + position**
    (lower-time capture into a side-table indexed by the assert IP).
    `assert a == b; body` should also produce the `assertEqValues`
    diff.
12. **Replace `path + string-context` silent drop** with TW's error.

### P2 — narrower correctness

13. **Add Path branch to `valueLess`** (S7).
14. **Make `==`/`!=` on functions raise `AssertionError`** matching
    TW (S6).
15. **Make `__toString` recursively coerce its result** as TW does;
    make `outPath` returning a Path call `copyPathToStore` in
    interpolation context (S8).
16. **Make `builtins.throw` / `builtins.abort` call `coerceToString`
    on their arg** (S9).
17. **Scan `primReadFile` content for store-path refs** (S11).

### P3 — architectural follow-ups

18. The `OP_ATTRS_REC_INIT`-publishes-everything + STG-WHNF-recovery
    + chain-peek triad (S2, S3, S5) is the single largest semantic
    divergence area. The structurally-cleaner replacement is the
    cell-update mechanism (`Thunk::cell`, `OP_RETURN`'s
    `*cell = retVal` at `vm.cc:6973-6979`). When/if v3 transitions
    to cell-update-everywhere, the partial-bindings registry can
    be retired.
19. **`Tag::Blackhole` as propagable value** (S1) — replacing this
    with the typed `InfiniteRecursionError` everywhere is cleaner
    but requires audit of cross-VMState producers; not a one-CL
    change.

---

## What I tempered after critical review

- Agent 1's **S2-S6 "suspect" hazards** (cell pointer not cleared,
  nUpvalues preserved after tail clear, inner revert path corner)
  are real corner-case audits but I couldn't construct concrete
  repros from code-reading alone. They go into the "follow-up"
  pile, not the recommendation list. Documented in
  `force_blackhole_semantics.md`.
- Agent 2's **S6 largest-layer-wins** (`NIX_V3_NO_LARGEST_PEEK`
  toggle) is a v3-internal heuristic, not directly user-observable —
  documented but not promoted to a recommendation.
- Agent 3's **D8 `__functor` mechanic difference** (single vs
  two-step call) was flagged but converges on the happy path; not
  a real divergence for any user-writable Nix.
- Agent 4's **CD-7 `Kind::Other` for let-rec** is inert today; I
  preserved it as a "latent footgun" note rather than a
  recommendation.
- Agent 4's **SA-4 `unbound variable` divergence** was deemed dead
  code in practice (TW's `bindVars` raises earlier on parsed input);
  not a recommendation unless registry parity drifts.

---

## Files inspected (for the spot-check verification pass)

- `/Users/angerman/Projects/iohk/nix/src/libexpr-v3/vm.cc:2133-2166` (boolean opcodes)
- `/Users/angerman/Projects/iohk/nix/src/libexpr-v3/vm.cc:3338-3416` (formals validation)
- `/Users/angerman/Projects/iohk/nix/src/libexpr-v3/lower.cc:2058-2072` (binop emit)
- `/Users/angerman/Projects/iohk/nix/src/libexpr/eval.cc:2415-2495` (TW `//` eval-order)
- `/Users/angerman/Projects/iohk/nix/src/libexpr-v3/primops.cc:2700-2727` (addErrorContext)
- `/Users/angerman/Projects/iohk/nix/src/libexpr-v3/primops.cc:3000-3030` (bridge1 fallback)
- `/Users/angerman/Projects/iohk/nix/src/libexpr-v3/primops.cc:3400-3414` (forceAttr fallback)
- `/Users/angerman/Projects/iohk/nix/src/libexpr/eval.cc:2665-2685` (TW handleEvalExceptionForThunk)
- `/Users/angerman/Projects/iohk/nix/src/libexpr-v3/vm.cc:8418-8432` (v3's acknowledged Failed gap)
