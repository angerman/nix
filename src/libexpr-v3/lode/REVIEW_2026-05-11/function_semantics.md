# Function / Lambda / Closure Semantics — TW vs v3 Audit (2026-05-11)

Scope: function/lambda/closure semantics only. Read-only audit of
`src/libexpr/eval.cc` (TW) vs `src/libexpr-v3/lower.cc` + `vm.cc` (v3).

## Summary

- v3 OP_CALL and v3 lower-time formal handling implement the core Nix
  function calling protocol (currying via single-arg OP_CALL, formals
  with defaults via synthetic LetRec thunk, ellipsis, `@arg` binding,
  PrimOpApp partial application, `__functor` dispatch). Semantically
  most cases are equivalent for code that succeeds.
- The bulk of divergences are in ERROR-PATH fidelity: v3 raises
  `std::runtime_error` (not `TypeError`/`EvalError`/`StackOverflowError`
  /`AssertionError`), without source position, without lambda name,
  without value print, and uses different wording. Several
  TW-expected error sites (missing-required-formal, value-not-a-set
  for the lambda arg, attribute-not-found, function-comparison) are
  affected.
- One semantic (non-error) divergence: TW raises an `AssertionError`
  on `f == f` and on `f1 == f2` for ANY function/closure direct
  comparison; v3 silently returns `false` for direct comparison and
  `true` for identical-closure-by-pointer comparison inside a
  container. The container-side equality is also a divergence (TW
  raises there too).

---

## Confirmed divergences

### D1. Missing-required-formal: no eager validation, no rich error

- **TW** `src/libexpr/eval.cc:1914-1924` — at call time TW iterates
  `formals->formals`; for each formal whose `def == nullptr` AND
  whose name is not present in the call attrset it raises
  `TypeError("function '%1%' called without required argument
  '%2%'", lambdaName, formalName).atPos(lambda.pos)
  .withTrace(pos, "from call site").withFrame(...).debugThrow()`.
  Validation runs BEFORE the body, before any default thunks build.

- **v3** `src/libexpr-v3/lower.cc:1268-1301`, `vm.cc:3338-3416` —
  v3's lowerer builds one synthetic thunk per formal whose body is
  `if hasAttr(param, X) then param.X else <default>` for
  defaulted formals, or just `AttrSelect(param, X)` for non-default
  formals. OP_CALL's formals validation (`vm.cc:3398-3413`) only
  checks for EXTRA arguments (non-ellipsis). It does NOT iterate
  formals to verify presence of required ones. A missing
  non-default formal triggers a generic `AttrSelect` failure ONLY
  if the body actually demands the formal.

- **Diffs**:
  1. Error wording: TW says `function 'anonymous lambda' called
     without required argument 'y'`; v3 says `v3 OP_ATTRS_SELECT:
     attribute not found` (`vm.cc:5732`).
  2. No position, no lambda name, no formal name in v3 message.
  3. Error class: `TypeError` vs `std::runtime_error` — affects
     `tryEval` catch and the error-printer machinery.
  4. **Semantic divergence**: if the body does NOT reference the
     missing formal, v3 silently succeeds; TW raises eagerly.

- **Repro (TW raises, v3 succeeds)**:
  ```nix
  ({a, b}: 42) {a = 1;}
  ```
  TW prints `function 'anonymous lambda' called without required
  argument 'b'`. v3 returns 42 (`b` is never referenced in the body).

- **Repro (both fail but different messages)**:
  ```nix
  ({a, b}: b) {a = 1;}
  ```
  TW: `function 'anonymous lambda' called without required argument
  'b'` at the lambda's position.
  v3: `v3 OP_ATTRS_SELECT: attribute not found` (no name, no pos,
  no class).

- **Golden test affected**:
  `tests/functional/lang/eval-fail-missing-arg.err.exp`.

### D2. Extra-formal error: different error class, different format

- **TW** `src/libexpr/eval.cc:1934-1953` — raises
  `TypeError("function '%1%' called with unexpected argument '%2%'",
  lambdaName, attrName).atPos(lambda.pos).withTrace(pos, "from call
  site").withSuggestions(suggestions).withFrame(...)`. Includes a
  `Suggestions::bestMatches` did-you-mean.

- **v3** `src/libexpr-v3/vm.cc:3409-3411` (OP_CALL) and
  `vm.cc:3657-3659` (OP_TAIL_CALL) — raises
  `std::runtime_error("v3 OP_CALL: function called with unexpected
  argument '" + nm + "'")`. No lambda name, no position, no
  suggestions, no frame, wrong error class.

- **Repro**:
  ```nix
  (foo@{ }: 1) { a = 3; }
  ```
  TW (`tests/functional/lang/eval-fail-empty-formals.err.exp`):
  ```
  … from call site
    at /pwd/lang/eval-fail-empty-formals.nix:1:1:
  error: function 'anonymous lambda' called with unexpected
  argument 'a' at /pwd/lang/eval-fail-empty-formals.nix:1:2:
  ```
  v3: `v3 OP_CALL: function called with unexpected argument 'a'`.

- **Golden test affected**:
  `tests/functional/lang/eval-fail-empty-formals.err.exp`.

### D3. Lambda-arg-not-a-set: different error path

- **TW** `src/libexpr/eval.cc:1898` — calls
  `forceAttrs(*args[0], lambda.pos, "while evaluating the value
  passed for the lambda argument")` which raises
  `TypeError("expected a set but found %1%: %2%", showType(v),
  ValuePrinter(...))` at `eval.cc:1479`.

- **v3** `src/libexpr-v3/vm.cc:3397-3414` — calls `forceValue` (not
  `forceAttrs`) on the arg. The `forcedArg.isAttrs()` check at line
  3398 silently SKIPS the extra-arg validation for non-attrset
  args. The arg is then bound to `local[0]` as a non-attrset; the
  body's formal access via the synthetic thunks does `AttrSelect`
  on the non-attrset, raising `runtime_error("v3
  OP_ATTRS_SELECT: not an attrset")` at `vm.cc:5419` — different
  message, different class, no position.

- **Repro**:
  ```nix
  ({a}: a) "foo"
  ```
  TW: `expected a set but found a string: "foo"` with position.
  v3: `v3 OP_ATTRS_SELECT: not an attrset`.

### D4. Not-a-function callee: different error class + message

- **TW** `src/libexpr/eval.cc:2162-2167` —
  `TypeError("attempt to call something which is not a function but
  %1%: %2%", showType(vCur), ValuePrinter(...)).atPos(pos)
  .debugThrow()`.

- **v3** `src/libexpr-v3/vm.cc:2989` (OP_CALL) and `vm.cc:8646`
  (callClosure) — `runtime_error("v3 OP_CALL: callee is not a
  closure")` / `runtime_error("v3 callClosure: not callable")`. No
  type, no value print, no position, wrong class.

- **Repro**:
  ```nix
  42 1
  ```
  TW: `error: attempt to call something which is not a function but
  an integer: 42`.
  v3: `error: v3 OP_CALL: callee is not a closure`.

### D5. Stack-overflow on infinite recursion: different limit + class

- **TW** `src/libexpr/include/nix/expr/eval-inline.hh:226` and
  `eval-settings.hh:330` — uses `settings.maxCallDepth` (default
  **10000**, user-settable via `max-call-depth`). Raises
  `StackOverflowError("stack overflow; max-call-depth exceeded")`
  at `eval-error.hh:72`.

- **v3** `src/libexpr-v3/vm.cc:96` (`kMaxCallDepth = 5000`),
  `vm.cc:3422-3424` — hard-coded 5000, raises
  `runtime_error("v3 OP_CALL: stack overflow; call depth
  exceeded 5000")`. Ignores the `max-call-depth` setting.

- **Behavioral divergence**: a deeply-recursive evaluator that
  works in TW with `--option max-call-depth 50000` will hit v3's
  hard cap. A program that exhausts at depth 6000 in TW (using
  default 10000) succeeds, but exhausts at 5000 in v3.

- **Repro**: `let rec = n: if n == 0 then 0 else 1 + rec (n-1); in
  rec 6000` — TW: 6000. v3: throws.

### D6. Function equality: AssertionError vs silent comparison

- **TW** `src/libexpr/eval.cc:3344-3346` — for ANY function-type
  comparison (`nFunction`, which is lambdas, primops, and
  primopapps): raises `AssertionError("distinct functions and
  immediate comparisons of identical functions compare as
  unequal").debugThrow()`. This fires even for `f == f` where both
  sides are the same closure value.

- **v3** `src/libexpr-v3/vm.cc:539-546` —
  ```c++
  case Tag::Closure:
  case Tag::PrimOp:
  case Tag::PrimOpApp:
      if (!insideContainer) return false;
      return a.payload.closure == b.payload.closure;
  ```
  - Direct comparison returns `false` (no throw).
  - Inside a container: returns `true` iff the pointers match
    (false otherwise, no throw).

- **Diff**:
  - `(f: f) 1 == (f: f) 1` (two fresh closures): TW raises
    AssertionError; v3 returns `false`.
  - `let f = (x: x); in f == f`: TW raises AssertionError; v3
    returns `false`.
  - `[(f: f)] == [(f: f)]`: TW raises (inside a list, equality
    recurses into elements, hits nFunction case, raises); v3
    compares closure pointers and returns `false`.

- This is a real semantic divergence visible to programs that
  guard with `tryEval` on equality.

### D7. Profiling / call-count hooks not invoked

- **TW** `src/libexpr/eval.cc:1802-1808` — `callFunction` invokes
  `profiler.preFunctionCallHook(...)` and posts
  `postFunctionCallHook(...)` in a `Finally` block. Also bumps
  `nrFunctionCalls` (`eval.cc:1959`) and `functionCalls[lambda]`
  (`incrFunctionCall`, eval.cc:1961), and `primOpCalls[fn->name]`
  (`eval.cc:2013`), and primop self-time tracking
  (`primOpTimerStack`, `eval.cc:2016-2058`).

- **v3** `src/libexpr-v3/vm.cc` OP_CALL / OP_TAIL_CALL /
  callClosure — no profiler hook invocations, no `nrFunctionCalls`
  increment, no `functionCalls` tracking, no `primOpCalls`,
  no primop timer stack.

- **Behavioral divergence**: `NIX_COUNT_CALLS=1` produces zero data
  for any function call that runs through v3 bytecode. The
  `EvalProfiler` (used by `nix-instantiate --eval-profile-file`
  and the eval-profile features at `eval-profiler.cc`) cannot
  observe v3 calls.

- `--trace-function-calls` / `debugTraceStacker` infrastructure
  (TW eval.cc:1968-1976) is also not invoked from v3, so v3 calls
  do not appear in REPL backtraces and do not feed
  `function-trace.cc`.

### D8. `__functor` curried call: same effect, different mechanic

- **TW** `src/libexpr/eval.cc:2141-2158` — when vCur is an attrset
  with `__functor`, TW builds `args2 = [&vCur_copy, args[0]]` and
  recursively calls `callFunction(*functor->value, args2, vCur,
  functor->pos)` PASSING BOTH ARGS AT ONCE. callFunction's loop
  will dispatch on `functor` (likely a Lambda `self: arg: ...`)
  in two iterations. After the recursion returns the loop forces
  vCur and consumes ONE arg from the outer arg vector.

- **v3** `src/libexpr-v3/vm.cc:2941-2955` — two SEPARATE
  `callClosure` calls: `callClosure(forced, fun)` then
  `callClosure(firstStep, arg)`. Each pushes a new frame.

- For ordinary Nix `__functor = self: arg: ...` shape the result
  is identical. The difference would only surface if `__functor`
  is somehow a multi-arg primop wanting both args atomically —
  which is not allowed by Nix's protocol. **Practically a
  match**; flagged for completeness.

- Note: v3's `__functor` branch only fires inside OP_CALL
  (`vm.cc:2941`) and `callClosure` (`vm.cc:8549-8557`). TW's
  `forceFunction` does NOT auto-dispatch __functor; it only
  reports the value `isFunctor` (eval.cc:2848 checks
  `isFunctor(v)` for the type guard). v3's call sites mirror this.

### D9. Strictness optimizer side-effect (potential)

- v3's `opt_strictness.cc` rewrites `Force{v}` to `VarRef{v}` for
  bindings whose RHS is in the WHNF whitelist (literals, Lambda,
  AttrSet, ListExpr, arithmetic, etc.). The whitelist
  (`opt_strictness.cc:72-110`) deliberately EXCLUDES App,
  MkThunk, AttrSelect, AttrSelectDyn, WithLookup,
  RecBindingSlotRef, PrimOpCall, If/Assert/With — all the things
  that can produce a thunk.

- The Lambda case is correct because lowering produces a Lambda
  IR node only for the lambda CONSTRUCTION (the `MakeClosure`-
  side); the result is always Tag::Closure (WHNF).

- I could not find a case where this optimizer eliminates a Force
  that TW would observe. Conservative whitelist; no divergence.

### D10. PrimOp strict args forced eagerly at call site

- **TW** `src/libexpr/eval.cc:2026` /
  `eval.cc:2107` — invokes primop with unforced args. Primops
  force whichever args they need inside their bodies via
  `state.forceValue(*args[i], ...)` calls inline.

- **v3** `src/libexpr-v3/vm.cc:2762-2765` (OP_CALL) /
  `vm.cc:6779-6782` (OP_CALL_PRIMOP) — forces every non-`lazyArgs`
  bit of `po->arity` BEFORE invoking. Uses `po->lazyArgs` bitmask
  (`primops.cc:8004,8022,8023,8036,8071,...`) to skip force on
  lazy args (`tryEval`, `foldl'`, `seq`, `deepSeq`,
  `addErrorContext`).

- **Practical effect**: for primops correctly marked, v3 matches
  TW. Risk vector: any TW primop that DOESN'T force its first arg
  at entry — v3's eager force would surface an error earlier or
  cause a thunk to fire that wouldn't otherwise. I did not
  enumerate all primops to verify.

---

## Suspect areas

### S1. `@arg` binding identity

Both TW (`eval.cc:1905-1906`) and v3 (`lower.cc:1251-1254`) bind
`@arg` to the RAW incoming attrset value, not a defaulted-applied
copy. This matches Nix semantics. However, the v3 binding goes
through the lambda's `param` slot which is the same `Value` as the
caller-side OP_CALL arg. TW's `args[0]` is also the caller-side
pointer. In v3, if the caller passes a Tag::Slot or Tag::Thunk and
`hasFormals` triggers the `forceValue` at `vm.cc:3397`, the
forced value is then bound to `local[0]` (`vm.cc:3430`) — the
`@arg` reference resolves to the FORCED attrset, not the original
thunk. In TW, `args[0]` is the original `Value*` pointer; after
`forceAttrs(*args[0], ...)` the thunk is MUTATED in place to the
attrset. References to `@arg` later in the body would see the
forced attrset in both cases. **Probably matches** but the
identity (is `@arg` the SAME `Value*` the caller passed?) is
NOT preserved in v3 because v3 binds the forced VALUE, not the
caller's slot pointer.

If user code does pointer-identity tricks (impossible in pure Nix
but conceivable via debug tooling / __functor patterns), divergence
could surface. Low priority.

### S2. Closure upvalue capture during synthetic LetRec for formals

`lower.cc:1303-1326` builds a synthetic `LetRec` that allocates one
inner FuncId per formal whose body captures the recScope. The
inner FuncId is then captured as a `MakeThunk`-style child. The
free-vars-analysis (`ir.cc:312-478`) must close over the formal
thunks' references to the outer lambda's body scope (defaults can
reference outer-scope bindings via `scopes` walk in `lowerExpr`).
Mostly looks correct, but it is a different mechanism from TW's
`i.def->maybeThunk(*this, env2)` (TW just creates a thunk that
captures env2 directly).

Specific concern: if a default expression references the lambda's
own enclosing `with` scope, v3 must capture the with-stack at
formal-thunk-construction time. The MkThunk handling in lower.cc
sets up `freeVars` and `lexicalWiths`; the formal thunks are
embedded via LetRec entries `thunkBody = thunkFids[i]`
(`lower.cc:1322`) — those FuncIds go through the same emit pipe
as regular lambdas, so they should pick up captures correctly.
**Have not verified end-to-end** for a formal-default
that references a `with`-scope name. Suspect area.

### S3. `forceValue` chasing through Tag::Slot during call

`vm.cc:2717-2720`: when fun is Tag::Slot, force chases through.
But the FORCE of the slot mutates the slot's storage to the
resolved value. If two callers share the slot (slot identity
across siblings), one caller's force is visible to others.
Matches TW's in-place thunk update model. Match.

### S4. Tail-call iteration cap

`vm.cc:3507-3513`: kMaxTailCalls = 10^7. TW has no TCO so this
limit doesn't exist on TW. A v3 program with a 10M-iter tail-
recursive loop hits this limit (`v3 OP_TAIL_CALL: tail-call
iteration limit exceeded`). On TW, the same program would either:
- Hit max-call-depth (10000 by default) and raise StackOverflow.
- C-stack-overflow.

This isn't strictly a divergence in the "v3 is wrong" sense — v3
extends what's expressible. But programs relying on a stack-
overflow at depth 10000 (e.g. recursion-depth probes) will see
different counters from v3.

### S5. Bridge thunk callee — `tryDispatchTWLambdaInV3` shortcut

`vm.cc:2865-2873` and `vm.cc:8586-8592` — when fun is a Bridge
thunk wrapping a TW lambda whose body has a v3 cache entry, v3
dispatches the body LOCALLY without bridging the arg through
v3ToTreeWalker. The arg's `Tag::Slot`/`Tag::Thunk` is preserved.

This is a v3-side optimization that bypasses TW for the function
body. It risks a divergence if the v3 body cache entry was lowered
in a different scope context than the TW call would see (e.g.
upvalue indexing mismatch). Documented under #515. Has been
implicated in past cycle bugs (#509/#515). Audit out of scope.

---

## Matches

These were verified equivalent between TW and v3:

- **M1. Currying mechanic**: `f a b c` lowers to a chain of three
  single-arg `OP_CALL`s in v3 (`lower.cc:1490-1493`,
  `emit.cc:531-540`). TW's `callFunction` iterates `args` in its
  while loop (`eval.cc:1825`). Both eventually invoke the
  underlying lambda or primop with one arg at a time, accumulating
  through partial application.

- **M2. Partial primop application**: both produce a primop-app
  value when arity > totalArgs (TW `eval.cc:2003-2006` /
  `2076-2079` `makeAppChain`; v3 `vm.cc:2733-2742`).

- **M3. Plain `x: body` lambda**: both bind the arg WITHOUT
  forcing (TW `eval.cc:1955-1956`; v3 lower at `lower.cc:1350-
  1368`, OP_CALL skip-force branch).

- **M4. `{a, b}: body` (non-ellipsis)**: both force the arg to
  attrs at entry (TW `eval.cc:1898`; v3 `vm.cc:3338-3398`), then
  bind defaults via per-formal lazy thunks (TW
  `i.def->maybeThunk`; v3 synthetic LetRec).

- **M5. `{a, b, ...}: body` (ellipsis)**: extra keys are
  accepted. v3 SKIPS the eager force at OP_CALL by default
  (`vm.cc:3341` `needForce = !desc->ellipsis || s_eagerArgForce`)
  — matching TW's "force then ignore extras" behavior in effect,
  but achieving it lazily. Gated `NIX_V3_EAGER_ARG_FORCE=1` makes
  v3 behave like TW (force at entry). Both observable behaviors
  succeed equally on well-typed inputs.

- **M6. Default with sibling-formal reference**:
  `{a, b ? a}: b` applied with `{a = 1;}` → 1 in both. v3 routes
  the default expression through `recScope` so `a` lookup
  resolves via the rec attrset (`lower.cc:1280-1295`).

- **M7. Mutually-recursive defaults**:
  `{a ? b, b ? a}: a` applied with `{}` → blackhole/infinite-rec
  in both. v3's per-formal thunks form a cycle just like TW's
  per-formal env2 thunks.

- **M8. `@arg` binds raw attrset** (not defaulted): both v3 and
  TW bind `@arg` to the raw incoming arg (before defaults
  applied). TW eval.cc:1905-1906; v3 lower.cc:1251-1254.

- **M9. `__functor` auto-dispatch**: both auto-call `__functor
  self arg` when callee is attrset with `__functor`. TW
  eval.cc:2141-2158; v3 vm.cc:2941-2955. (See D8 for mechanic
  difference.)

- **M10. forceFunction**: TW has explicit `forceFunction`
  (eval.cc:2844-2857) used by callback primops (map, filter,
  foldl', etc.). v3 uses `forceValue` inline plus the OP_CALL
  type-dispatch. Functionally equivalent for non-error cases;
  diverges on the "not a function" error path (D4).

- **M11. PrimOp lazy-arg masks**: TW primops force inside their
  bodies; v3 marks the same primops with `lazyArgs` (foldl' bit
  0b010, seq 0b10, etc.) and forces strict args at dispatch. Bit
  layout matches TW's call-site demand expectations
  (`primops.cc:8004` etc.). Has been carefully maintained.

- **M12. Tail-call correctness**: v3's OP_TAIL_CALL truncates
  the with-stack and reseats `cur.withStackBase` before pushing
  the callee's captured-withs (`vm.cc:3750-3757`) — fixed under
  Phase-13 HIGH-1. Matches a TW-equivalent fresh-frame's
  with-stack semantics.

- **M13. Closure capture**: v3's `Closure::upvalues[]` (sized to
  `desc->nUpvalues`) carries the freeVars in the order
  `ir::Function::freeVars` was computed. Free-vars analysis
  (`ir.cc:312-478`) traverses bindings recursively. TW captures
  the entire `Env*`. Both correctly preserve all bindings
  reachable from the lambda body, including transitive `with`-
  scoped names (via `Closure::capturedWiths` at
  `vm.cc:2188-2202`). No missed references identified in the
  audit window.

- **M14. Stack-overflow on `(x: x x) (x: x x)`**: both raise.
  TW via `addCallDepth`; v3 via `kMaxCallDepth`. Different limit
  and error class, but the failure mode is the same kind. See
  D5.

---

## Notes on test-suite impact

These golden tests in `tests/functional/lang/` will exhibit
different `.err` output under v3 vs TW (the `.err.exp` was
authored against TW):

- `eval-fail-missing-arg.err.exp` — D1.
- `eval-fail-empty-formals.err.exp` — D2.

If/when v3 becomes default-on for these tests, either:
- Make v3 raise the same `TypeError` strings (preferred — restores
  observable equivalence; requires plumbing position/name into v3's
  formals validation and migrating from `runtime_error` to the
  `EvalError` hierarchy).
- Or fork the expected outputs.

The latter would break the principle that v3 is a drop-in for TW.
The former is mechanically straightforward (lower.cc already has
`pos` handles for formals; the AST `ExprLambda*` is captured in
`m.functions[fid].astLambda`).

---

## Recommended near-term remediations (priority order)

1. **High**: D1+D2+D3+D4 — wrap the relevant v3 throw sites in
   `EvalError`/`TypeError` with positions and lambda names. Pull
   the lambda name from `desc->name` and position from
   `desc->posHandle`; pull the formal name from
   `desc->formals[i].name`. This restores `.err.exp` parity.
2. **High**: D1 — eager missing-formal validation at OP_CALL after
   the forceAttrs (currently OP_CALL only checks extras, not
   missing). Add a second loop iterating `desc->formals` to detect
   missing non-default formals. This restores the eager-validation
   contract and fixes the unused-missing-formal silent-success bug.
3. **Medium**: D5 — wire `kMaxCallDepth` to
   `settings.maxCallDepth` so `--option max-call-depth N`
   controls v3 too. Raise as `StackOverflowError`.
4. **Medium**: D6 — make v3's `valueEqual` for closures/primops
   raise the same `AssertionError` TW does.
5. **Low**: D7 — wire profiler hooks into OP_CALL /
   callClosure if NIX_COUNT_CALLS or
   `EvalProfiler::getNeededHooks()` indicates demand. Match TW's
   `nrFunctionCalls`/`functionCalls`/`primOpCalls` bookkeeping.
