# REVIEW_2026-05-11: Errors / tryEval / Primops Semantics

Scope: error-/exception-type semantics, `builtins.tryEval`, error trace
formatting, and primop fidelity between **TW** (`src/libexpr/eval.cc` +
`src/libexpr/primops.cc`) and **v3** (`src/libexpr-v3/{vm.cc,primops.cc,v3_hook.cc}`,
plus `include/v3/errors.hh`).

Audit date: 2026-05-11.  Reviewer: agent run (cited file:line).

## Summary

- **`tryEval` itself is correct in v3** (`primops.cc:6837-6870`): catches
  `v3::AssertionError` (covers v3 `throw`/`assert`) **and**
  `nix::AssertionError` (catch bridged from TW).  `AbortError` is a plain
  `std::runtime_error`, so it propagates -- matches TW's `nix::Abort`.
  However, the surrounding machinery (bridge1, addErrorContext, hook
  fallback) silently REWRAPS thrown exceptions as
  `std::runtime_error`, which **breaks tryEval transitivity across
  every TW<->v3 bridge boundary**.
- **Error traces are essentially absent** in v3.  TW chains
  `addErrorTrace`/`addTrace` pervasively (~30+ sites in eval.cc, 20+ in
  primops.cc) to attach "while evaluating attribute X" / "while calling
  the foo builtin" / "from call site".  v3 throws bare
  `std::runtime_error("v3 primop FOO: ...")` with no chained traces, no
  source position, no "while evaluating ..." context.  Visible
  divergence in every user-facing error message.
- **Source positions are not carried** through v3 bytecode.  v3 reserves
  but does not emit `OP_POS` (`bytecode.hh:284-286`); per-attr positions
  are stored in a side-table and recoverable via
  `unsafeGetAttrPos` (`primops.cc:1498`), but `assert` errors don't
  carry the condition expression, primop errors don't carry a
  caller `Pos`, and `addErrorContext` cannot attach traces in the TW
  shape.  This is structural, not a bug to fix in a single CL.

## Confirmed Divergences

### Errors / tryEval

1. **`primAddErrorContext` LOSES exception type** for non-blackhole errors.
   `primops.cc:2719-2725` wraps any `std::exception` as
   `throw std::runtime_error(msg + "\n" + ex.what())`.  An
   `AssertionError` thrown inside the wrapped body becomes a plain
   `runtime_error` -- the surrounding `tryEval` will NOT catch it.
   TW's `prim_addErrorContext` (`libexpr/primops.cc:1175-1188`) calls
   `e.addTrace(...)` and `throw;` on the SAME exception object, type
   preserved.

   *Impact: SEVERE.*  Every nixpkgs module evaluation wraps in
   `lib.modules`' `addErrorContext`.  If a downstream `tryEval` is
   intended to recover from `throw`/`assert` inside a config, v3
   prevents the catch from firing.

2. **`primV3CallBridge1` REWRAPS exceptions** on the catch path.
   `primops.cc:3017-3019`'s `fallbackToTreeWalker` lambda:
   ```
   if (!fallbackExpr || !dynamic_cast<const BlackholeError *>(&ex))
       throw std::runtime_error(ex.what());
   ```
   When a v3 closure was bridged to TW (via `__v3_call_bridge_1`) and
   the body threw `AssertionError`/`ThrownError`, the type is erased to
   plain `runtime_error` on its way back into TW.  TW's
   `tryEval` will NOT catch.

   *Impact: SEVERE in mixed-evaluator runs.*  Sister handlers
   `primV3ForceAttr` (`primops.cc:3404-3405`) and `primV3ForceListElem`
   correctly `throw;` (rethrow original) -- only bridge1 has this bug.
   The two surfaces disagree.

3. **Hook-level v3 throws are swallowed + replayed via TW.**
   `v3_hook.cc:2295-2322`: when `run(*cu)` throws, the eval hook
   blacklists, then calls `e->eval(state, state.baseEnv, v)` to retry on
   TW.  If v3 raised a SPURIOUS error (a v3 bug), TW silently succeeds
   and the bug is masked.  If v3's error was legitimate (e.g., a
   user `throw "msg"`), TW reproduces the same throw at C-stack-deeper
   nesting -- error message position info matches TW shape but
   re-evaluation cost is paid.  Test coverage cannot distinguish
   "v3 was wrong" from "v3 was right and TW agrees" without
   `V3_DEBUG_HOOK=1`.

   *Impact: MEDIUM.*  Acceptable as a soft-fallback, but masks v3
   correctness regressions in CI.

4. **`OP_ASSERT` error message is generic.**
   `vm.cc:6724`: `throw AssertionError("v3 OP_ASSERT: assertion failed")`.
   TW (`eval.cc:2290`):
   `state.error<AssertionError>("assertion '%1%' failed", exprStr)
        .atPos(pos).withFrame(env, *this).debugThrow()`
   -- includes the source-rendered condition + position + env frame.
   v3 strips all of it; user sees `"v3 OP_ASSERT: assertion failed"`.

5. **v3 `throw`/`abort` add a leading `"v3 throw: "` / `"v3 abort: "`
   prefix** (`primops.cc:795`, `primops.cc:1444`).  TW emits just the
   user message + position.  Visible in any error output.

6. **`trylevel` is not tracked.**  TW (`primops.cc:1317`) bumps
   `state.trylevel` so the `debugRepl` can skip when inside a try.  v3
   has no equivalent.  Non-essential for non-debug usage; missing for
   parity.

7. **Stack overflow / infinite recursion is `std::runtime_error`.**
   `vm.cc:3422-3424`, `vm.cc:4726-4727`, `vm.cc:7780-7781`,
   `vm.cc:8317`.  TW uses `nix::StackOverflowError`
   (`include/nix/expr/eval-error.hh:69`) and
   `nix::InfiniteRecursionError` -- both `EvalError` subclasses; not
   `AssertionError`, so `tryEval` ignores both.  v3's plain
   `runtime_error` is the same w.r.t. tryEval, but lacks the
   typed-error machinery (no position, no trace chain) and is
   distinguishable in C++ catches that TW callers may rely on.

### Primops

8. **`primReadFile` does not scan content for store-path references.**
   v3 `primops.cc:2510-2568` forwards only the input's existing string
   context.  TW `libexpr/primops.cc:2237-2263` uses
   `PathRefScanSink::fromPaths(refs)` to detect store-path mentions in
   the file content and add them as Opaque context entries.
   Affects derivation runtime-deps inference from `builtins.readFile`'d
   files.  *Real-world impact: medium-to-high in nixpkgs.*

9. **`primScopedImport` silently drops scope names** that aren't valid
   plain identifiers or that are Nix keywords.
   `primops.cc:6406-6429` synthesizes a `let X = __scope__.X; ...`
   wrapper and skips names that don't match `[a-zA-Z_][a-zA-Z0-9_'-]*`
   or that equal `if/then/else/assert/with/let/in/rec/inherit/or`.
   TW (`libexpr/primops.cc:408-430`) installs every name into the
   parser's `StaticEnv` regardless.  *Impact: low.*  Real nixpkgs uses
   plain identifiers; pathological tests may diverge.

10. **`primGenericClosure` keys via string-tagged hash, not
    `CompareValues`.**  `primops.cc:2201-2228` converts each key to a
    tag-prefixed string and uses `unordered_set<std::string>`.  This
    means v3 accepts `Tag::Bool` keys (TW rejects in `CompareValues`,
    `libexpr/primops.cc:919-927`) and produces different error messages
    on mixed-type keys (TW: "cannot compare X with Y"; v3:
    "cannot compare keys of incompatible types").  *Impact: low.*

11. **`primForceDeepRec` (deepSeq) does not attach error traces.**
    v3 `primops.cc:1460-1480` is plain recursive force.  TW
    `forceValueDeep` (`eval.cc:2730-2773`) wraps each attr / list elem
    in try/catch and calls `addErrorTrace(e, i.pos, "while evaluating
    the attribute '%1%'")` / `"while evaluating list element at index
    %1%"` before rethrowing.  v3 errors thrown deep inside a deepSeq
    have no breadcrumb chain.

12. **`primToString` (and the `toStringCoerceCtx` helper) does not call
    `__toString self`.**  `primops.cc:629-647` explicitly comments
    "v3 doesn't yet wire calling __toString from a primop context --
    fall through to outPath only."  TW (`eval.cc:2922-2940`) calls
    `__toString` first, then recursively coerces the result.  *Impact:
    medium.*  Any user-defined `__toString` (some nixpkgs helpers,
    custom pretty-printers) returns the wrong value or hits "no
    outPath" via v3.

13. **`primPath` has a fake-store fallback.**  `primops.cc:6241-6248`:
    when `state.nixEvalState` is null, returns
    `Tag::Path = "/v3-fake-store/<name>"`.  Real Nix would have errored.
    Likewise `primDerivationStrict` (`primops.cc:4900-4904`) produces
    `"/v3-fake-store/<fnv-hash>-<name>(.drv)"` fake outputs.  Distinct
    from TW; relevant when v3 runs standalone (no TW EvalState wired).

14. **`primTypeOf` returns `"thunk"` / `"unknown"` rather than throwing.**
    `primops.cc:728,734-735`.  TW's `prim_typeOf` (after forcing)
    returns one of seven well-defined names; never `"thunk"` or
    `"unknown"`.  v3 force-pre-evaluates args (it has no
    `lazyArgs` bit), so `Tag::Thunk` shouldn't show up at this point,
    but the dispatch leaves the fallback string.  *Impact:
    theoretical.*

15. **JSON errors are not typed.**  v3 `primops.cc:6674` calls
    `nlohmann::json::parse(..., allow_exceptions=true)`; an invalid
    JSON throws nlohmann's `parse_error` (a std::exception, not a
    `nix::JSONParseError`) and propagates raw.  TW
    (`libexpr/primops.cc:2789-2793`) wraps as `JSONParseError` + adds
    a position trace `"while decoding a JSON string"`.

16. **`primGenList` is lazy in v3 (App entries), `prim_genList` in TW
    is also lazy.**  Match.  Listed for traceability.

17. **`primFoldl'` strictness:** Neither TW (`libexpr/primops.cc:4128-4147`)
    nor v3 (`primops.cc:941-956`) forces the accumulator between
    iterations explicitly inside the primop.  Both rely on the
    underlying call returning a WHNF value.  Match in practice;
    contradicts both docstrings' "evaluated immediately, even for
    intermediate values" claim, but consistent.

18. **`primFunctionArgs` formals attrs:** v3 (`primops.cc:6516-6532`)
    records per-attr position metadata via `recordAttrPos`;
    `unsafeGetAttrPos` (`primops.cc:1498-1530`) can recover it.  TW
    (`libexpr/primops.cc:3673-3674`) uses
    `attrs.insert(i.name, getBool(i.def), i.pos)`.  Both report
    `(name -> bool hasDefault)` with positions.  Match.

19. **`primMatch` / `primSplit` use `std::regex::extended`.**  v3
    (`primops.cc:2275`) and TW (`libexpr/primops.cc:5058`) both use the
    same POSIX extended grammar via `std::regex`.  Match.  (Memo to
    earlier doc: TW does NOT use boost::regex/ECMAScript here.)

20. **`primSeq` / `primDeepSeq` lazyArgs bitmask is `0b10`** (arg 1
    lazy, arg 0 strict).  `primops.cc:8022-8023`.  The dispatcher
    pre-forces arg 0 at `vm.cc:6779-6782`; the primop body doesn't
    re-force.  Matches TW semantics where `prim_seq` and `prim_deepSeq`
    both force args[1] inside but rely on caller-forcing for args[0].
    Match.

### FFI / Bridge

21. **`ffi.cc:applyClosure`** (`ffi.cc:284-290`) flattens any thrown
    exception into `Fallible<Value>{EvalError{e.what()}}`.  Type info
    lost at the FFI boundary -- consumers can't tell `AssertionError`
    from `Abort` from `TypeError`.  Comment at `ffi.cc:288` flags this
    as FFI plan §A5 future work.

## Suspect Areas

22. **`clearBlackMarksOnException` correctness on nested `tryEval`s.**
    `vm.cc:7023-7070` is the WC-37 fix; the project memory note marks
    it RESOLVED.  Confirmed: catch path unwinds vm.frames, valueStack,
    and withStack.  The `#557` hardening at `vm.cc:8447-8449` swallows
    secondary exceptions during cleanup -- correct policy, but masks
    bugs in `clearBlackMarksOnException` itself (any `partialBindingsRegistry`
    corruption silently survives).  Recommend stderr trace gated on
    a debug env var, not pure swallow.

23. **`primTryEval` does NOT explicitly call
    `clearBlackMarksOnException` after its catch** (`primops.cc:6837-6870`).
    Today this is correct because `forceValue` does it internally (in
    its thunk-frame catch at `vm.cc:8430-8454`).  But if `forceValue`
    is ever inlined differently or its catch contract changes, the
    primTryEval invariant becomes invisibly dependent on it.
    Recommend an explicit guard in primTryEval mirroring
    `dispatchAndClear`.

24. **`primAddErrorContext` lazyArgs comment.**  `primops.cc:8071`:
    `lazyArgs=0b10` -- arg 1 (the wrapped value) is lazy.  But the
    body forces arg 1 inside the try (`primops.cc:2710`).  So the
    laziness gate is "force only inside the try so addErrorContext can
    catch", which IS the TW shape.  Match.  Filed as suspect only to
    flag the implicit dependency of correctness on lazyArgs being
    exactly `0b10` -- not `0b00` (would crash before try) or `0b11`
    (the message would never be coerced even on error).  A unit test
    for this bit value would catch a future regression.

25. **`primReadFile` NUL-byte check is post-read.**
    `primops.cc:2553-2554`: reads the whole file then rejects.  TW
    (`libexpr/primops.cc:2241-2244`) raises EvalError with a position
    trace; v3 raises plain `runtime_error`.  Behavior matches; error
    text and type differ.

26. **No restriction on `primPath` / `primReadFile` argument lazy
    forcing.**  Both are registered without lazyArgs (force-by-default
    via dispatcher).  But TW uses `realisePath` which goes through
    `coerceToString` (handles `__toString`, `outPath`).  v3 only
    handles `Tag::String` / `Tag::Path` directly (`primops.cc:2513-2515`,
    `primops.cc:6226-6230`) and bridges through TW for the broader
    shapes.  Suspect-to-investigate: does a non-bridge v3 standalone
    accept `{ outPath = "/foo"; }` as a `readFile` argument?  TW does
    (via the same coercion).  Almost certainly fails in v3 standalone.

27. **`primDerivationStrict` "fake store" path uses a NON-cryptographic
    FNV-1a hash.**  `primops.cc:4886-4895`.  Adequate for tests that
    just compare drvPath strings; emphatically not equivalent to TW's
    content-addressed store paths for any real build.  Documented and
    bracketed but worth flagging since `eval-okay-eq-derivations` and
    similar pin the behaviour.

28. **`primReadDir` returns directory-iterator types without `lstat`.**
    `primops.cc:2578-2587` uses `std::filesystem::directory_iterator`'s
    cached type; comment notes `is_symlink` first so links are
    correctly reported.  But certain filesystems / platforms (older
    Linux ext4, network filesystems) populate the dirent type as
    DT_UNKNOWN and require a follow-up `lstat`.  v3 reports
    `"unknown"`; TW (`libexpr/primops.cc`'s primReadDir, not shown
    here but using `path.readDirectory()`) explicitly resolves via
    `lstat` and may return the precise kind.  Edge case but
    distinguishable.

## Matches

- **`primTryEval` catches `AssertionError` only** (and `nix::AssertionError`
  across the bridge), as required.  `AbortError` correctly propagates.
  `primops.cc:6851-6858`.
- **`primThrow` is `ThrownError` (subclass of v3 `AssertionError`); 
  `primAbort` is `AbortError` (plain `runtime_error`).**  Catchability
  by tryEval is correct.  `errors.hh:30-46`.
- **`primTypeOf` strings** for `Int/Float/Bool/Null/String/Path/set/list/lambda`
  match TW's `showType`.  `primops.cc:712-737`.
- **`primMatch` / `primSplit` regex flavour** -- both use
  `std::regex::extended`.  Same engine, same syntax.
- **`primFunctionArgs`** returns `{name -> bool}` with positions and the
  ellipsis distinction omitted (Nix has no ellipsis distinction --
  `functionArgs ({...}: ...) = {}` per docstring).  Match.
- **`primSeq` / `primDeepSeq`** semantics + lazyArgs match.
- **`primFoldl'`** behavior matches (both rely on call-return as WHNF).
- **`primGenericClosure` order is BFS** in both implementations
  (`primops.cc:2230-2242` uses `std::deque`; TW uses
  `std::list`/`front/pop_front`).  Match in iteration order, key
  matching strategy differs (see divergence 10).
- **`primGenList` is lazy** in both.  v3 uses `Tag::App`; TW uses
  thunk values.  Match in observable laziness.
- **`primPathExists` returns `false` on `RestrictedPathError`** in both
  TW and v3 (when nixEvalState is wired).  `primops.cc:2117-2119`.
- **WC-37 (clearBlackMarksOnException ghost frames)** is fully
  unwinding `vm.frames`, `vm.valueStack`, `vm.withStack` to the
  pre-throw bases.  `vm.cc:7039-7070`.  Confirmed RESOLVED.

## Recommendations (prioritised)

1. **Fix `primAddErrorContext` and `primV3CallBridge1` to preserve
   exception type** (Divergences 1 and 2).  Both are user-visible
   correctness bugs; both have a small mechanical fix:
   - `primAddErrorContext`: nested `try`/`catch` per concrete v3 type
     (`AssertionError`, `ThrownError`, `BlackholeError`, fallback to
     wrap).
   - `primV3CallBridge1::fallbackToTreeWalker`: replace the `throw
     std::runtime_error(ex.what())` with `throw;` to rethrow the
     original.  (The substring-typing era is gone -- the typed
     `BlackholeError` is already the routing key, so the rewrap is
     unnecessary.)

2. **Add a tryEval-transitivity test** that exercises the bridge path:
   `builtins.tryEval (addErrorContext "ctx" (throw "x"))` should give
   `{success=false; value=false;}` in both TW and v3 modes.

3. **Wire a minimal error-trace stack in v3** so the most common
   user-visible "while evaluating the attribute X" / "while calling the
   FOO builtin" traces survive.  Could be a thread-local vector pushed
   in OP_CALL_PRIMOP and selected OP_ATTRS_* sites, drained on throw to
   prepend to `runtime_error::what()`.  Doesn't need to match TW
   byte-for-byte, but should produce something better than a single
   line.

4. **Add `__toString` handling to v3's `toStringCoerceCtx`** so
   `builtins.toString { __toString = self: "hi"; }` produces `"hi"`
   without bridging.  Comment at `primops.cc:629-632` already flags
   this as TODO.

5. **Scan readFile content for store references** in `primReadFile`
   (Divergence 8) -- nixpkgs depends on this for runtime-dep capture
   from configs / scripts.
