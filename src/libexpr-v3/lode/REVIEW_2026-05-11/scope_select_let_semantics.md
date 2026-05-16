# Scope / Select / Let / With audit — TW vs v3

Cross-impl audit of lexical scope, the with-stack, let-bindings, var lookup, and selector semantics between:

- TW: `src/libexpr/eval.cc` + `src/libexpr/nixexpr.cc::bindVars` (static analyser).
- v3: `src/libexpr-v3/lower.cc` (lowering) + `src/libexpr-v3/vm.cc` (OP_WITH_*, OP_ATTRS_*, OP_GET_LOCAL/_UPVALUE, OP_REC_BINDING_SLOT_REF).

Note on opcode naming: the task brief references `OP_LOAD_VAR` / `OP_LOAD_UPVAL`; the actual v3 opcodes are `OP_GET_LOCAL`, `OP_GET_LOCAL_FORCE`, `OP_GET_UPVALUE`, `OP_GET_UPVALUE_FORCE` (`vm.cc:1868`, `:1872`, `:1938`, `:1946`).

## Summary

- **Var lookup, lexical/with shadowing**: v3 inherits TW's static analyser (`bindVars` in `nixexpr.cc:315-349` sets `ExprVar::level/displ/fromWith`). Both consume those fields identically — lexical names hit `resolveVar(level, displ)` (`lower.cc:797`, `:267-340`), with-bound names emit `OP_WITH_LOOKUP` (`lower.cc:793-796`, `vm.cc:715-1000`). The withStack walks innermost→outermost honoring per-frame `withStackBase` (`vm.cc:722`, `:3473`), so shadowing rules match TW.
- **let-rec + inherit-from + with-targets**: still has several latent or opt-in divergences. v3 thunkifies inherit-from from-exprs only via gated paths (default thunkifies a narrow `self.X` heuristic with `NIX_V3_SELF_DOT_MAX_LEVEL=4`, with `NIX_V3_STG` / `NIX_V3_INHERIT_FROM_THUNK_ALL` / `NIX_V3_LAMBDA_SKIP` widening to TW's blanket behavior — `lower.cc:1735-1822`). TW always thunkifies (`eval.cc:1520` via `from->maybeThunk`). v3 also leaks `Kind::Other` for let-rec scopes (`lower.cc:2492`) — currently inert but a footgun for any future kind-driven heuristic.
- **Selector + position semantics**: `x.y or z` matches TW's missing-attr-vs-force-error distinction. Missing-attr error messages diverge (TW: typed `EvalError` with position + suggestions + frame; v3: opaque `std::runtime_error`). `__curPos` / `ExprPos`: TW emits `null` for stdin/string origins and lazy `App(lineOfPos, posV)` thunks for line/column; v3 unconditionally builds `{file, line, column}` with eager ints and synthetic file strings (`"<stdin>"`, `"<string>"`, `"<unknown>"`).

## Confirmed divergences

### CD-1. `__curPos` / `ExprPos` shape on non-SourcePath origins
- **TW** (`eval.cc:989-999`, `mkPos`): on non-`SourcePath` origin returns `v.mkNull()`. On SourcePath origin, builds a 3-attr Bindings with `file = string(path.abs())`, but `line` and `column` are LAZY (`App(lineOfPos, posV)` / `App(columnOfPos, posV)` — see `primops.cc:3338-3346`).
- **v3** (`lower.cc:678-708`, `lowerPosAttrs`): unconditionally synthesizes `{file, line, column}` regardless of origin (`<stdin>`, `<string>`, `<unknown>` placeholders for non-Source). Both `line` and `column` are EAGER `LitInt`s.
- **Impact**: programs that do `(__curPos == null)` in REPL or eval-string contexts get different answers. `tryEval (__curPos.line)` evaluates eagerly under v3, lazily under TW. The `<unknown>` fallback further diverges any code that branches on `__curPos.file`.

### CD-2. Inherit-from thunkification policy
- **TW** (`eval.cc:1513-1523`, `buildInheritFromEnv`): unconditionally `from->maybeThunk(state, up)` — every inherit-from source is a thunk.
- **v3** (`lower.cc:1686-1822`, `pushInheritFromCache` + `isSelfDotPattern`): only thunkifies eagerly under specific conditions:
  - `NIX_V3_NO_INHERIT_FROM_THUNK=1` disables all thunkify (off by default — so thunkify IS enabled by default for the matched patterns, but not blanket).
  - Default mode: only `self.X`-shaped from-exprs where head ExprVar resolves to a `Kind::Lambda` scope at `level <= NIX_V3_SELF_DOT_MAX_LEVEL` (default 4) get thunkified (`lower.cc:1808-1821`).
  - `NIX_V3_LAMBDA_SKIP`, `NIX_V3_INHERIT_FROM_THUNK_ALL`, `NIX_V3_STG` widen to blanket thunkify.
- **Impact**: under default mode, an inherit-from source that is NOT a self-dot Var-headed `ExprSelect` (e.g., a function call, complex expression, or a from-expr whose head is a let-binding rather than a lambda param) is lowered eagerly. This evaluates the from-expr during attrset construction, which can force a sibling rec-attrset that's still being built — the latent class of regressions documented in CALLPACKAGE_BUG_2026-05-09.md, #498 (always-thunkify regression), #548 (inherit-from lazy). v3 has had to compensate at runtime via the partial-Bindings registry and the chain-peek paths in `withLookup` (`vm.cc:737-770`) and `OP_ATTRS_SELECT` (`vm.cc:5246-5378`); TW needs none of those.

### CD-3. Missing-attribute error class and metadata
- **TW** (`eval.cc:1720-1730`): throws typed `EvalError("attribute '%1%' missing", name).atPos(pos).withSuggestions(bestMatches(...)).withFrame(env, *this).debugThrow()`. Includes:
  - Symbol name in message.
  - Source position.
  - Best-match suggestions ("did you mean...?").
  - Frame info for the debugger.
- **v3** (`vm.cc:5732`, `:5419`): throws `std::runtime_error("v3 OP_ATTRS_SELECT: attribute not found")`. No position, no suggestions, no frame info, no symbol name in the user-visible string (some routes have a debug-only `V3_DBG_SELECT_FAIL` block that prints to stderr but does NOT enrich the thrown error).
- **Impact**: user-facing error quality on attribute typos is materially worse on v3. Tests that `assertThrowsKind<EvalError>` may also fail (caught as runtime_error vs EvalError).

### CD-4. Lambda formals lowering: rec-attrset wrap (vs TW's flat Env)
- **TW** (`eval.cc::callFunction` path; `nixexpr.cc:461-486` for `bindVars`): for a lambda `{ a, b ? c }: body`, `arg` (if any) and each formal get their own displacement in a single Env (`StaticEnv` sized `(formals.size() + (arg ? 1 : 0))`). Defaults can reference siblings via lexical displacement on the SAME env.
- **v3** (`lower.cc:1178-1325`): for a default-bearing formals lambda, builds a SYNTHETIC `LetRec` (rec-attrset!) named `formalsRec` with one entry per formal. Each formal becomes a thunk `if hasAttr(param, X) then param.X else default`. The lambda body's scope (`recScope.kind = Lambda`, `recScope.recAttrsVar = formalsRec`) makes formal references go via `AttrSelect(formalsRec, name)` + Force.
- **Impact** (the #516 root cause): a `level/displ` reference to a formal under v3 returns a Tag::Slot or AttrSelect — NOT a direct env value. When that lambda-param slot is captured into a closure that also captures a let-rec's `recSlotVar`, the two `Tag::Slot` references get conflated. The fix-up paths in `lower.cc:2880-2895` distinguish the cases but the underlying representation gap remains. Tests in `run-self-dot-thunkify-tests.sh` and lib.extends are the canary.

### CD-5. Position metadata on attrs from `__curPos`
- **TW** (`eval.cc:991-997`): file string is a `mkString` of `path->path.abs()` with NO context (no source-context). Line/column lazy via thunks tied to a `PosIdx` (`primops.cc:3340-3346`).
- **v3** (`lower.cc:686-702`, `posIdxToHandle` at `:740-763`): file string is interned by `interpStr` into a static `std::deque<std::string>` pool, then wrapped in `LitString`. Eager line/column ints. Position-handle side-table is populated separately for `builtins.unsafeGetAttrPos`.
- **Impact**: minor — affects programs that introspect position-attr laziness or context. `unsafeGetAttrPos` semantics confirmed equivalent via the side-table path; the divergence is only on the `__curPos`/`ExprPos` path.

### CD-6. Runtime peeks at partial Bindings (cycle masking) — v3 only
- **TW** has NO equivalent: when forceAttrs hits a Black thunk, it propagates `InfiniteRecursionError` / `BlackholeError`. The lookup fails fast.
- **v3** has multiple peek paths that "see through" Black thunks:
  - `withLookup` peek (`vm.cc:737-769`, `:763-769` catch-after-throw).
  - `OP_ATTRS_SELECT` chain-peek BEFORE force (`vm.cc:5268-5333`).
  - `OP_ATTRS_SELECT` chain-peek AFTER force (`vm.cc:5358-5378`).
  - `tryBridgeAttrLookup` for Bridge thunks (`vm.cc:5224-5234`).
- **Impact**: v3 may successfully resolve a name in a `with self;` over a let-rec mid-construction that TW would reject with infinite recursion. In the canonical `lib.fix self: { x = self.y; y = 1; }` shape this is BENIGN: v3 returns the partial Bindings's `y` value, equivalent to what would happen if the user had not used `with`. But it diverges from TW behavior — TW evaluates `self.x` lazily, never hits the cycle, while v3's eager from-expr lowering forces `self` mid-construction, hits the cycle, then escapes via peek. Hides bugs that TW would surface.

### CD-7. `Kind::Let` / `Kind::Rec` scope tagging — half-implemented
- **Defined** in `lower.cc:78-84`: enum has Lambda, Let, Rec, With, Other.
- **Set** only on Lambda (`lower.cc:1176`, `:1249`) and With (`lower.cc:2927`). The `recScope` built by `lowerLetRecCapture` at `lower.cc:2492` leaves `kind` defaulted to `Kind::Other`.
- **Used** at `lower.cc:1818` (`isSelfDotPattern` checks `sc.kind != Scope::Kind::Lambda`).
- **Impact**: today inert — no code branches on `Kind::Let` or `Kind::Rec`. Latent footgun: any new heuristic that wants "is this a let-rec scope?" gets `Kind::Other` and silently dispatches wrong.

## Suspect areas (worth a follow-up audit)

### SA-1. `ListVec * capturedWiths` materialization order vs TW's `parentWith` chain
- TW's runtime walk uses the AST chain `fromWith->parentWith` + `prevWith` displacement (`eval.cc:946-962`) to traverse exactly the lexically enclosing withs, in inner→outer order.
- v3's `collectLexicalWiths()` (`lower.cc:214-229`) walks `scopes` outer→inner front-to-back, then materializes a `ListVec` that `pushCapturedWiths` (`vm.cc:1204-1209`) replays onto the runtime withStack outer-first. `OP_WITH_LOOKUP` then walks innermost-first (`vm.cc:723`).
- Order looks correct on inspection. BUT `collectLexicalWiths` walks ALL `Kind::With` scopes including those that cross a `Kind::Lambda` boundary (comment at `lower.cc:218-222` is explicit about this). TW's runtime walk stops at the lambda boundary because the closure was made with its `fromWith` pointing only at withs lexically in scope at the closure expression. Audit whether v3 includes withs from outside the maker function in the captured chain — and if so whether `emit.cc`'s freeVars computation drops them.
- Specifically: if a closure body has NO fromWith refs, `collectLexicalWiths` could still include outer withs into `capturedWiths`. That's harmless if `OP_WITH_LOOKUP` never fires; but it does grow the captured-list size and could perturb `OP_MAKE_CLOSURE`'s `nWiths` operand encoding.

### SA-2. Force semantics inside `with`-stack on Tag::Slot entries
- `withLookup` for Tag::Slot (`vm.cc:729-779`): reads `*p` into a local `derefed`, forces locally, but does NOT write back to `*p`. Comment at `:732-735` says this is intentional (slot may mutate via OP_APPLY_OVERRIDES).
- TW's analogue: `lookupVar` does `forceAttrs(*env->values[0], ...)` which writes back into the env slot. Subsequent lookups skip the force.
- v3 re-forces on every lookup that crosses a Tag::Slot with-entry that wraps a thunk. After the FIRST force, `*p` may have been mutated (the thunk transitions to Evaluated and its `cell` is set), so the local copy on subsequent calls is also cheap (Evaluated path). Net cost should be a few extra branches per lookup, not a re-evaluation. Worth a benchmark — perceived perf gap on with-heavy workloads might trace here.

### SA-3. `OP_FORCE` and `Tag::PrimOpApp` chain
- v3's `OP_ATTRS_HAS` and `OP_ATTRS_SELECT` force only `Tag::App`, `Tag::Thunk`, `Tag::Slot` (`vm.cc:5805-5807`, `:5334-5337`). They do NOT force `Tag::PrimOpApp`.
- TW's `forceValue` on a `tApp` (which encodes PrimOpApp too in TW's tagging) chases the application. If v3 ever lands a `Tag::PrimOpApp` value at the head of a select chain (e.g., from a curried primop in attrset position), the `isAttrs()` check fails immediately and the select reports "not an attrset" instead of forcing and re-checking.
- I did not find a concrete reproducer — but the asymmetry is real. Audit: do any v3 IR paths produce a `Tag::PrimOpApp` at the SELECT operand? `OP_PRIMOP_APP` always pushes the curried value; if that value is then SELECTed before being CALLed (which user code can write: `(builtins.foldl' f) // x`), we'd hit this.

### SA-4. The `unbound variable` divergence at lower time
- TW's `bindVars` (`nixexpr.cc:344-345`) raises `UndefinedVarError("undefined variable '%1%'", name).atPos(pos).debugThrow()` at PARSE time.
- v3's `lowerVar` (`lower.cc:831`) raises `std::runtime_error("v3 lower: unbound variable '" + name + "'")` at LOWER time — but only fires if TW's bindVars already passed (so by then the var has been resolved to either lexical or fromWith). The fallback to base-env is hit only when `resolveVar` returns `kInvalid` (level falls off scopes) AND the name doesn't match `true/false/null/builtins` AND `findPrimOp(name)` returns null. That should never happen on TW-parsed input, so the `runtime_error` is dead code in practice.
- **BUT** if `findPrimOp(name)` returns null for a primop TW knows about (registry asymmetry), v3 reports the wrong error. Audit primop registration parity once we have a clean list.

### SA-5. `prev // overlay final prev` env-shape mismatch (#455 territory)
- The memory note for #455 pins the cardano-node OD failure to an env-shape mismatch in `prev // overlay final prev` at `lib/fixed-points.nix:327`.
- Looking at `lowerLet` (`lower.cc:2087-2092`) — it calls `lowerLetRec` with `isRec=true, hasBody=true`. The let-body's `prev` resolves via `recAttrsVar` (rec-attrset path), so `OP_ATTRS_SELECT(recAttrsVar, prev)` → forces the prev thunk → `f final` returns. Then `prev // overlay final prev` is an `Update` operation.
- TW's path: `ExprLet` builds an env, `env2.values[displ] = e->maybeThunk(...)`. `prev` is at displ 0 in env2. Body's `prev` is a direct displ access. The thunk shares the env. No publication / rec-attrset roundtrip.
- The shape mismatch is that v3 makes `prev` an entry in a Bindings whose other entries are placeholders (until OP_ATTRS_REC_SET fills them) — meanwhile TW's env has only `prev`'s slot. If a sub-closure captures the rec-attrset (via slot-capture mechanics) and later reads it, it sees `{prev}` only — divergent from TW which has no such snapshot to leak.

## Matches (verified parity)

### M-1. Static analyser ownership
v3 reuses TW's `Expr::bindVars` (`nixexpr.cc:286-554`) for `ExprVar::level/displ/fromWith`, `ExprWith::parentWith/prevWith`, `ExprAttrs::AttrDef::displ`. v3's lower-time `resolveVar(level, displ)` (`lower.cc:267-340`) consumes these fields. No re-implementation; no opportunity for inconsistency on the analyser pass itself.

### M-2. with-stack innermost-first walk
- TW (`eval.cc:946-963`): inner `fromWith` first, then walk `parentWith` chain outward.
- v3 (`vm.cc:723`): `for (i = vm.withStack.size(); i-- > base; )` walks top-of-stack (innermost) first.
Both find the innermost `with` that defines `name`, matching nix's documented semantics.

### M-3. Lexical shadowing over `with`
TW's `bindVars` (`nixexpr.cc:327-339`) prefers lexical bindings over `withLevel` in the static analyser: if a `curEnv->find(name)` succeeds in a non-with env, the var is bound to that lex slot — even if there are enclosing `with`s. The `fromWith` field stays null. v3's `lowerVar` checks `fromWith` first (`lower.cc:793`); since the analyser sets it null for lexically-bound names, v3 routes through `resolveVar`. **Verified parity** by inspection.

### M-4. with-target lazy force
- TW (`eval.cc:2255-2262`): `ExprWith::eval` stores `attrs->maybeThunk(state, env)` in env slot, defers force until first miss.
- v3 (`lower.cc:2836-2937` + `vm.cc:5960-6050`): `lowerWith` pushes the lowered `attrs` VarId (no eager force); `OP_WITH_PUSH` just stores the value on the with-stack; `OP_WITH_LOOKUP` (`vm.cc:756-759`) forces only when scanning the entry on a lookup miss.

### M-5. let-rec mutual references
- TW (`eval.cc:1622-1644`): all bindings get thunks pointing to the new env (env2), so a binding's body can reference siblings via displacement on env2. Mutual references work.
- v3 (`lower.cc:2602-2627`): each binding's def is lowered with `recScope` pushed (`isRec=true`), so refs to siblings get `AttrSelect(recAttrsVar, sibling_name)`. The runtime fills `recAttrsVar`'s Bindings via OP_ATTRS_REC_INIT + OP_ATTRS_REC_SET BEFORE entering the body block. Mutual refs work.

### M-6. `with x; inherit a;` resolves `a` via x
- TW: `inherit a` is parsed as an AttrDef with `Kind::Inherited` whose `def.e = ExprVar(a)`. `chooseByKind(env, env, inheritEnv)` returns `env` for Inherited (`nixexpr.cc:443`), so the ExprVar is bound in the parent env. If parent env has a `with`, the var's bindVars sets `fromWith` accordingly.
- v3: `lowerLetRecCapture` line 2618-2623 handles `Kind::Inherited` by lowering `def.e` (the ExprVar) in the PARENT scope, no `recScope` push. ExprVar's `fromWith` field is honored.
**Parity** confirmed for this case.

### M-7. `x.y or z` distinguishes "missing key" from "force throws"
- TW (`eval.cc:1712-1717`): for each step in path, with `def`: `forceValue(*vAttrs, pos)`. If forceValue throws (e.g., assert failure), the exception propagates (the try/catch at line 1740 is for `addErrorTrace` decoration only — it re-throws). Only `type() != nAttrs || missing key` triggers the default branch.
- v3 (`lower.cc:2750-2770`, vm.cc `OP_ATTRS_HAS` `:5786-5810`): emits `if HasAttr then Select else default`. `OP_ATTRS_HAS` forces `App | Thunk | Slot` (`:5805-5807`), and if forceValue throws, propagates. If forced value is not isAttrs, HasAttr returns false → default (matches TW's `vAttrs->type() != nAttrs` branch).
**Parity** on the exception-vs-missing distinction.

### M-8. Nested select short-circuit on default
- TW (`eval.cc:1708-1736`): the loop iterates `getAttrPath()`. On the FIRST step that misses (with default), it `def->eval` and returns — doesn't continue stepping.
- v3 (`lower.cc:2731-2776`, `emitSelectChain`): nested If: at each step, then-branch recurses into `emitSelectChain(got, path, defaultExpr, pathIdx+1)`; else-branch evaluates default and returns. Short-circuit at first miss.
**Parity**.

### M-9. SymbolTable byte-equality interning
- TW uses `nix::SymbolTable` — interns by exact string match.
- v3's `ir::globalSymbolTable` (`ir.cc:64-79`) uses `std::unordered_map<std::string, SymbolId>` with case-sensitive byte-equality hash + eq (`ir.cc:51-62`).
Both case-sensitive, no normalization. **Parity**.

### M-10. `builtins` resolution
- TW: `builtins` is in the static base env at displ 0 (`eval.cc:594-597`), so the bare name resolves lexically with no special-casing in eval. The attrset itself is built up by `addConstant` / `addPrimOp` (`eval.cc:519`, `:588`).
- v3 (`lower.cc:808-820`): when `resolveVar` falls off the scope stack and the name is `"builtins"`, emit `OP_LIT_BUILTINS`. The runtime materializes a singleton via `getBuiltinsValue()` (`vm.cc:7078-7120`), which iterates `allRegisteredPrimOps()` filtering out `__`-prefixed names — matching TW's "no `__`-prefixed entries in `builtins`" rule (cf. `vm.cc:7082-7092` comment quoting `eval-okay-builtins`).
**Parity**.

### M-11. `__sub` / `__add` etc. as bare names
v3's primop registry registers both stripped (`add`) AND `__`-prefixed (`__add`) variants for the arithmetic / comparison primops (`primops.cc:7993-7997`). So a bare `__sub` reference in user code resolves via `findPrimOp("__sub")` to the same `primSub` function pointer.
**Parity** (assuming the registry parity audit at SA-4 confirms all of TW's `__`-aliases are present).

### M-12. `withStack` per-frame floor
- TW: closures hold an `Env*` pointer that is independent of the caller's env chain.
- v3: per-frame `withStackBase` (`vm.cc:3473`, `:3485`) is set on every `OP_CALL` to the with-stack top BEFORE `pushCapturedWiths`. `OP_WITH_LOOKUP` bound by this base (`vm.cc:722-723`). Caller's withs are invisible to the callee body.
**Parity** in design.

### M-13. `__curPos` source attribution
For `__curPos` evaluated at a real source location (not stdin/string/REPL), v3 produces the same `{file=<abspath>, line, column}` shape as TW. Mismatch only at non-SourcePath origins (CD-1).

---

Files inspected (absolute paths):
- `/Users/angerman/Projects/iohk/nix/src/libexpr/eval.cc`
- `/Users/angerman/Projects/iohk/nix/src/libexpr/nixexpr.cc`
- `/Users/angerman/Projects/iohk/nix/src/libexpr/include/nix/expr/nixexpr.hh`
- `/Users/angerman/Projects/iohk/nix/src/libexpr/primops.cc`
- `/Users/angerman/Projects/iohk/nix/src/libexpr-v3/lower.cc`
- `/Users/angerman/Projects/iohk/nix/src/libexpr-v3/vm.cc`
- `/Users/angerman/Projects/iohk/nix/src/libexpr-v3/emit.cc`
- `/Users/angerman/Projects/iohk/nix/src/libexpr-v3/ir.cc`
- `/Users/angerman/Projects/iohk/nix/src/libexpr-v3/primops.cc`
- `/Users/angerman/Projects/iohk/nix/src/libexpr-v3/include/v3/bytecode.hh`
