# v3 VM Correctness State — 2026-05-11

## Summary

- **Smoke functional parity**: 142/142 `eval-okay-*` lang tests pass
  under v3-eval (`run-lang-tests.sh`). `run-fail-tests.sh` reports
  103/109 as "raised error", but the gate is **loose**: any non-zero
  exit with a stderr matching `error|aborted|throw|assert|fail` is
  accepted. The exact `.err.exp` golden file is **not** compared, so
  C2-class divergences (right exit code, wrong message/position) go
  undetected.
- **v3-direct on real nixpkgs**: still broken. `(import <nixpkgs>{}).hello.name`
  under `NIX_V3_DIRECT_EVAL=1` now fails with a new symptom —
  `error: v3 STR_CONCAT: cannot coerce type to string (tag=7)` —
  representing yet another floor in the lib.fix/extends cycle stack
  that began at `callPackage` (#516) and moved through `texlive` →
  `libsForQt5` → `qt5` (#558).
- **STG default-on (#547)** plus the slot/cell mechanism gives v3
  parity with TW on the lang corpus and v3-fhook parity on full
  nixpkgs eval. v3-direct still lags because of the unresolved
  eager-vs-lazy asymmetry in inherit-from from-expressions
  (S5/S3/S2 cluster).
- **Today's audit** (`COMPREHENSIVE_REPORT.md`) verified 8 P0/critical
  silent-correctness gaps and ~11 narrower divergences. None of
  these were addressed by the #558 commit run (which targets the
  qt5/STG WHNF recovery surface, not the validated semantic gaps).
- **Error quality is materially worse than TW everywhere**: ~180 v3
  throw sites raise `std::runtime_error` rather than typed
  `nix::EvalError`/`TypeError`/`AssertionError`. Position info is
  missing on most; no "while evaluating attribute X" trace chain.
  This is acknowledged but unfixed.
- **96 unique `NIX_V3_*` env-var gates** in source. 5 STG runtime
  gates flipped default-on by #547. Bisect kill-switches
  (`NIX_V3_NO_*`) cover most opt passes. Per `REVIEW_2026-05-09.md`
  Agent 4, 77 of 96 are missing from USAGE.md.

---

## Test suite status

### `tests/functional/lang/` (143 eval-okay, 109 eval-fail, 13 parse-okay, 41 parse-fail)

| runner | scope | result |
|---|---|---|
| `tests/functional/lang.sh` (upstream) | TW reference; compares `.err.exp` and `.exp` exactly | Not invoked under v3. |
| `src/libexpr-v3/test/run-lang-tests.sh` | eval-okay only; compares **stdout** to `.exp` or to TW's `--eval --strict` output | **142/142 pass** (1 disabled upstream: `eval-okay-tail-call-1.exp-disabled`) |
| `src/libexpr-v3/test/run-fail-tests.sh` | eval-fail only; checks non-zero exit + stderr matches loose regex `error|aborted|throw|assert|fail` | **103/109 raised error, 6 silent** |
| Parse tests | Not run through v3 (v3-eval has no `--parse` mode that exits same way as nix-instantiate) | Not measured. |

**Silent passes on eval-fail (6 of 109)** — these tests exit 0 from v3
where TW raises:

- `eval-fail-abs-path-fatal`
- `eval-fail-derivation-structuredAttrs-stack-overflow`
- `eval-fail-home-path-fatal`
- `eval-fail-short-path-literal`
- `eval-fail-toJSON-stack-overflow`
- `eval-fail-url-literal`

Of these, three are about deprecated path-syntax warnings becoming
errors (v3 likely accepts the old syntax), two are stack-overflow
tests (v3 has different limits / its own TCO), and one is a URL-literal
deprecation. None overlap the C-series audit findings.

### `src/libexpr-v3/test/run-*.sh` (regression / feature tests)

Total 42 shell scripts. Categories:

**Issue-tagged regression tests** (run-NNN-... pattern):
- `run-456-chase-cycle-tests.sh` — #456 chase-cycle issue
- `run-458-rec-slot-capture-tests.sh` — #458 rec-slot capture
- `run-558-emit-order-tests.sh` — #558 emit-order (qt5 territory)

**Feature / mechanism tests:**
- `run-bridge-attr-lookup-tests.sh`, `run-bridge-stack-uaf-tests.sh`,
  `run-bridge-thunk-after-force-tests.sh`, `run-bridge1-shortcut-tests.sh`
- `run-broader-thunkify-tests.sh`, `run-cell-update-protocol-tests.sh`
- `run-cutover-parity-tests.sh`, `run-cutover-tests.sh`
- `run-direct-eval-tests.sh` (26 hand-written shapes — passes)
- `run-disk-cache-tests.sh`, `run-drv-parity.sh`
- `run-evalscope-tests.sh`, `run-fix-inherit-from-self-tests.sh`
- `run-gate-removal-tests.sh`, `run-inherit-from-laziness-tests.sh`
- `run-intrinsic-recognition-tests.sh`
- `run-lang-tests.sh`, `run-lazy-bridge-arg-tests.sh`
- `run-let-rec-publish-split-tests.sh`, `run-lexical-withs-tests.sh`
- `run-mutual-circular-formals-tests.sh`, `run-nursery-tests.sh`
- `run-on-demand-root-shapes.sh`, `run-on-demand-root-tests.sh`
- `run-self-dot-thunkify-tests.sh`, `run-thunk-all-regression-tests.sh`
- `run-tw-lambda-bridge-tests.sh`
- `run-wc-laziness-tests.sh`, `run-wc38-nixpkgs-probe.sh`

**Known-fail / inventory / lint:**
- `known-fail-callpackage-with.sh` — exits 0 if bug present, 1 if "fixed"
- `inventory-stg-mode.sh` — STG flag inventory
- `lint-no-inline-getenv.sh`
- `wc38-bisect-harness.sh`
- `bench-*.sh` — performance benchmarks
- `evalscope-handles.cc`, `drv-preflight.cc`, `smoke.cc` — C++ unit tests

**Explicitly KNOWN-FAIL today (per latest run):**
`known-fail-callpackage-with.sh` — script accepts ANY of these symptoms
as expected failure under `NIX_V3_DIRECT_EVAL=1` on full nixpkgs:
- `OP_WITH_LOOKUP: name 'callPackage' not found in with-scope`
- `OP_WITH_LOOKUP: cycle while resolving 'callPackage'`
- `OP_WITH_LOOKUP: cycle while resolving 'texlive'`
- `OP_WITH_LOOKUP: cycle while resolving 'libsForQt5'`
- `OP_ATTRS_SELECT: attribute not found`

When invoked on 2026-05-11, the script reports a **NEW symptom**:
`v3 STR_CONCAT: cannot coerce type to string (tag=7)`. This is a
floor change the known-fail catalogue hasn't yet recorded.

**Soft KNOWN-FAIL marker** (in code, not in test infra):
`lower.cc:1762` — comment "(level >= 1 self-dot via intermediate `let`)
is a known-fail" for `NIX_V3_SELF_DOT_MAX_LEVEL >= 1` patterns.

### `src/libexpr-v3/Makefile`

The actual `Makefile` only builds + runs `smoke.cc` (a single C++ unit
test). `make check` runs the optimized smoke binary; `make check-debug`
runs the ASan/UBSan-instrumented variant. It does NOT invoke
`run-*.sh` shell tests — those are invoked manually or via top-level
meson/CI.

---

## Env-var gates

Full list extracted from `vm.cc`, `lower.cc`, `primops.cc`,
`v3_hook.cc`. 96 unique gates per `REVIEW_2026-05-09.md`. Grouped by
intent.

### Master mode switches

| Var | Default | Effect |
|---|---|---|
| `NIX_USE_V3` | off | Top-level hook gate — v3 owns parse-lower-compile dispatch. |
| `NIX_USE_V3_FORCE` | off | Wire v3 into the forceValue hook (opt-in, 3-6% regression noted on real workloads). |
| `NIX_USE_V3_CALL` | off (no-op alias) | Back-compat for early call hook. |
| `NIX_V3_DIRECT_EVAL` | off | v3 owns ALL eval (no TW fallback at leaves); used for v3-direct testing. |
| `NIX_V3_INVERT_EVAL` | off | #454 Phase E inverted eval entry (opt-in). |
| `NIX_V3_BRIDGE_CLOSURE` | off | TW closure bridging into v3 (opt-in, opt-in for testing). |
| `NIX_V3_FIBER_BRIDGE` | off | Fiber-based bridge experiment. |

### STG (post-#547 default-on flips)

| Var | Default | Effect |
|---|---|---|
| `NIX_V3_NO_STG` | (off → STG ON) | Master STG kill — restores legacy hook-eager path |
| `NIX_V3_STG_KEEP_HOOKS` | off | Keep the eval/call hooks active alongside STG (debug) |
| `NIX_V3_NO_STG_WHNF` | (off → STG WHNF ON) | Disable the WHNF-recovery / partial-Bindings pick-largest-layer pickLargestLayer mechanism (S2 in COMPREHENSIVE_REPORT) |
| `NIX_V3_NO_BLACKHOLE_AS_VALUE` | (off → ON) | Disable Tag::Blackhole as a propagable Value (S1) |
| `NIX_V3_NO_CALL_HOOK_EAGER` | (off → eager ON) | Force the call-hook to fire eagerly on every call entry |
| `NIX_V3_NO_ACTIVE_V3_VM_REFUSE` | (off → refuse ON) | Disable refusing TW re-entry while v3 VM is active |

### Optimization toggles (most default-on, kill-switch style)

| Var | Polarity | What it disables |
|---|---|---|
| `NIX_V3_NO_OPTIMISE` | off → opt ON | All optimizer passes |
| `NIX_V3_NO_PRECOMPILE` | off → on | Precompile pipeline |
| `NIX_V3_PARSE_PRECOMPILE` | off (opt-in) | Eagerly populate cache for every Expr at parse time |
| `NIX_V3_NO_CONTENT_CACHE` | off → on | Content-keyed bytecode cache |
| `NIX_V3_DISK_CACHE` | off (opt-in) | Persistent disk-cache layer |
| `NIX_V3_NO_INVERT_EVAL` | off → on | Inverted eval pipeline |
| `NIX_V3_NO_INLINE_REC_SLOT` | off → on | Inline RecBindingSlotRef |
| `NIX_V3_NO_REC_SLOT_CAPTURE` | off → on | Rec-slot capture during lambda lower |
| `NIX_V3_NO_LIFT_LAMBDA` | off → on | Lambda hoisting |
| `NIX_V3_NO_LIFT_ATTRSLIST` | off → on | AttrsList hoisting |
| `NIX_V3_NO_LIFT_WRC` | off → on | With-rec-closure hoisting |
| `NIX_V3_LIFT_IRWPC` | off | Inline-rec-with-publish-clear lift (opt-in) |
| `NIX_V3_NO_LIFT_IRWPC` | off → matches above gate | Opt-out for the above |
| `NIX_V3_NO_INTRINSIC_RECOGNISE` | off → on | Fix/Extends/Compose recognition pass |
| `NIX_V3_INTRINSIC_DISPATCH` | off (opt-in) | Native Fix/Extends dispatch (not yet default; not yet correct on lib.fix d3b at level=1) |
| `NIX_V3_NO_LAMBDA_SKIP` | off → on | Lambda-skip pass |
| `NIX_V3_NO_OP_CALL_BRIDGE_SHORTCUT` | off → on | OP_CALL bridge shortcut |
| `NIX_V3_NO_TW_LAMBDA_INV3` | off → on | Dispatch TW-lambda in v3 path |
| `NIX_V3_NO_BINOP_FORCE` | off → on | Some BinOp force optimization |
| `NIX_V3_NO_PATH_COMPRESS` | off → on | Path-compression in dispatch |
| `NIX_V3_NO_CLOSURE_POOL` | off → on | Closure pool reuse |
| `NIX_V3_NO_SHORTCIRCUIT` | off → on | Short-circuit dispatch in v3 hook |
| `NIX_V3_NO_GETFORCE_SUPER` | off → on | OP_GET_LOCAL_FORCE super-call path |
| `NIX_V3_NO_UPDATE_TAIL` | off → on | Update tail emission optimization |
| `NIX_V3_NO_COMPLEX_FROM_THUNK` | off → on | Complex-from-expr thunkify rule |
| `NIX_V3_NO_PUBLISH` (used in comments) | — | Partial publish (publish-everywhere) |

### Inherit-from / from-expr thunkification (the S5 surface)

| Var | Default | Effect |
|---|---|---|
| `NIX_V3_SELF_DOT_MAX_LEVEL` | 4 | self-dot heuristic depth |
| `NIX_V3_SELF_DOT_LIMIT` | unset | Hard-cap on self-dot thunkify count |
| `NIX_V3_SELF_DOT_SKIP_NTH` | unset | Skip Nth self-dot for bisect |
| `NIX_V3_INHERIT_FROM_THUNK_ALL` | off | Unconditional thunkify ALL inherit-from from-exprs (matches TW; triggers parse.nix re-eval blowup) |
| `NIX_V3_NO_INHERIT_FROM_THUNK_ALL` | off | Opt-out of above |
| `NIX_V3_NO_INHERIT_FROM_THUNK` | off → on | Inherit-from thunkify (master) |
| `NIX_V3_INHERIT_FROM_THUNK_FILTER` | unset | Filter substring for bisect |
| `NIX_V3_THUNK_CALL_ON_SELECT_VAR` | off | Accept `ExprCall(ExprSelect(Var, ...), ...)` as a Var-rooted from-expr head |
| `NIX_V3_THUNK_ALL_TRIVIAL` | off | Thunkify all trivial from-exprs |
| `NIX_V3_LAMBDA_SKIP` | off | Lambda-skip lower pass |

### Bridge / FFI / depths

| Var | Default | Effect |
|---|---|---|
| `NIX_V3_BRIDGE_PRIMOP_DEPTH` | (no cap) | Cap re-entry depth in bridge primops |
| `NIX_V3_BRIDGE1_DEPTH` | (no cap) | Cap re-entry depth for bridge1 |
| `NIX_V3_BRIDGE1_REENTRY_MAX` | (no cap) | Per-thunk reentry cap for bridge1 |
| `NIX_V3_FORCE_CHAIN_DEPTH` | (no cap) | Force-chain cap |
| `NIX_V3_FORCE_CHAIN_REENTRY_MAX` | (no cap) | Force-chain reentry cap |
| `NIX_V3_FALLBACK_CHAIN_DEPTH` | (no cap) | Fallback chain cap |
| `NIX_V3_EAGER_BRIDGE_MAX` | (no cap) | Eager-bridge cap |
| `NIX_V3_NO_REFUSE_FORMALS_BRIDGE` | off → refuse ON | Allow formals-closure bridge |
| `NIX_V3_TW_LAMBDA_BRIDGE` | off | TW-lambda bridge wired through v3 |
| `NIX_V3_LAZY_BRIDGE_ARG` | off | Lazy bridge-arg |
| `NIX_V3_EAGER_ARG_FORCE` | off | Force args eagerly at call (TW semantics opt-in) |

### Runtime / GC / cache tuning

| Var | Default | Effect |
|---|---|---|
| `NIX_V3_NURSERY` | off | Cheney nursery |
| `NIX_V3_NURSERY_SCAVENGE` | off | Nursery scavenge mode |
| `NIX_V3_PHASEB_FAIL_LIMIT` | 1 | SubExprCacheEntry blackholeFailureCount limit |
| `NIX_V3_PRECOMPILE_MAX_FNS` | 200 | Precompile fn cap |
| `NIX_V3_SKIP_THRESHOLD` | unset | Runaway-compile skip threshold |
| `NIX_V3_NO_CACHE_CANDIDATE_FLAG` | off → flag ON | #455 diagnostic skip |
| `NIX_V3_NO_LAMBDA_ROOT_MAP` | off → ON | Lambda root-map |
| `NIX_V3_LEAKED_BLACK_RECOVER` | off | Leaked-black recovery (opt-in) |
| `NIX_V3_EARLY_PUBLISH` | off | Early-publish opcode |
| `NIX_V3_CELL_EVERYWHERE` | off | Cell-update mechanism everywhere (replaces partial-bindings registry, opt-in) |
| `NIX_V3_PRIMOP_DUMP` | off | Dump primop stats at exit |

### Debug-only (V3_DBG_*, ~60 vars)

Mostly diagnostic prints (`V3_DBG_FORCE_*`, `V3_DBG_WITH`,
`V3_DBG_OP_CALL`, `V3_DBG_FORCE_INSIDE_X`, `V3_DBG_TAINT`,
`V3_DBG_FORCE_NAME/POS/FILE`, etc.). Not load-bearing for
correctness; not listed exhaustively here.

---

## Real-world status

### `(import <nixpkgs>{}).hello.name` under v3-direct (`NIX_V3_DIRECT_EVAL=1`)

**Broken**. Cycle floor has moved through this sequence as fixes landed
through 2026-05-09 → 2026-05-11:

1. `OP_WITH_LOOKUP: name 'callPackage' not found in with-scope`
   (CALLPACKAGE_BUG_2026-05-09.md — wrong with-target captures the
   `{prev}` recAttrs from lib.extends).
2. After #546 OP_ATTRS_REC_INIT split: shifts to
   `cycle while resolving 'callPackage'` (Black thunk reach).
3. After 0931b77a3 (curried-call thunkify): `'callPackage'` → `'texlive'`.
4. After eae55d149 (ExprSelect-with-Var head): `'texlive'` → `'libsForQt5'`.
5. After Phase 1 STG ladder (ff629384b..1214a40b0):
   `'libsForQt5'` → `OP_ATTRS_SELECT: attribute not found` (qt5,
   #558 territory).
6. **As of 2026-05-11 invocation**: known-fail script reports
   `v3 STR_CONCAT: cannot coerce type to string (tag=7)` — yet another
   new symptom. The bug stack continues to peel layers.

The underlying gap is documented in `V3_DIRECT_NIXPKGS_2026-05-09.md`:
v3-direct's emit/lower forces some from-expr / `with`-thunk that TW
keeps lazy. The architectural fix (S5 in COMPREHENSIVE_REPORT) needs
both inherit-from cache reordering AND a fix for the downstream
`lib/systems/parse.nix` re-eval explosion that opens up when the
cycle is bypassed.

### `(import <nixpkgs>{}).hello.name` under v3-fhook (`NIX_USE_V3=1`)

**Works** post-#547 STG default-on flip. This is the path that's
actively serving real-world workloads.

### `lib.fix` / `lib.extends`

- `lib.fix (self: {x = 1; y = self.x + 1;})` — works on v3-direct
  (the rec-attrs path).
- `lib.fix (lib.extends overlay base)` with 1 stage — works.
- Multi-stage extends with `with final;` — works.
- Multi-stage extends with allPackages-shape (5+ levels) + by-name
  overlay + curried-call inherit-from clauses — **fails** (the
  symptom above).

### `callPackage` (synthetic minimal)

- Synthetic `self.callPackage <path> { args }` — works.
- Real nixpkgs `_internalCallByNamePackageFile` after 83 successful
  packages, on the 84th (hello), trips the wrong-with-target bug.
  Re-confirmed in `CALLPACKAGE_BUG_2026-05-09.md` §3.

### cardano-node

Not measured in the past 6 days per `REVIEW_2026-05-09.md` Agent 2.
Last working data point: `phaseB fail-limit = 1` (memory note,
2026-05-04) recovered ~1s on cardano-node v3-fhook.

---

## Audit-finding regression check (today's `COMPREHENSIVE_REPORT.md` vs current code)

I picked four C-series findings and spot-checked whether the
behaviour is observable on `v3-eval` standalone (the cleanest
test surface, no TW fallback at boundaries). All four reproduced
as the audit claims.

### C1 — non-Bool LHS silently accepted by `&&`, `||`, `if`

**Verified reproduced today** on standalone `v3-eval`:

```
$ v3-eval --expr 'null && true'   →  true        (v3)
$ nix-instantiate --eval -E 'null && true'  →  TypeError "expected a Boolean but found null" (TW)

$ v3-eval --expr 'if null then 1 else 2'  →  1   (v3)
$ nix-instantiate --eval -E 'if null then 1 else 2'  →  TypeError (TW)
```

The audit's claim at `vm.cc:2138, 2146, 2155, 2164` (test
`v.isBool() && v.payload.i == 0/1` falls through silently on non-Bool)
is real and visible on bare expressions today.

**Catchable by existing tests?** No.
- `eval-okay-logic.nix` exercises only Bool LHS (`!false && (true || false) -> true`).
- `eval-fail-assert-nested-bool.nix` is about `assert` on `==` of Bool —
  doesn't probe a non-Bool LHS of `&&`/`||`/`if`/`->`.
- `eval-fail-not-throws.nix` (`!(throw "uh oh!")`) checks `!` on a
  thrown value — different surface.

No `eval-fail-*` test in the lang corpus exercises non-Bool LHS of
`&&` / `||` / `->` / `if`. **Gap is silent.**

Note: when the same expression runs through `nix-instantiate
--eval` with `NIX_USE_V3=1`, the error fires because the top-level
parse/eval string path stays in TW (the v3 hook is not yet
intercepting the `--eval -E` entry for all opcodes). The bug is
real but masked by the current hook surface — it surfaces only
when v3 owns the dispatch, which is `NIX_V3_DIRECT_EVAL=1`,
`v3-eval` standalone, or any future opt-in mode that routes the
expression through v3's bytecode for boolean opcodes.

### C2 — missing-required-formal silently returns body value

**Verified reproduced today** on standalone `v3-eval`:

```
$ v3-eval --expr '({a, b}: 42) {a = 1;}'   →  42  (v3)
$ nix-instantiate --eval -E '({a, b}: 42) {a = 1;}'
    →  TypeError "function 'anonymous lambda' called without required argument 'b'"  (TW)
```

When the body **does** reference the missing formal (as in
`eval-fail-missing-arg.nix`, body `x + y + z`), v3 fails with
`v3 OP_ATTRS_SELECT: attribute not found` — a generic error with no
mention of the formal name `y`, the lambda name, or the source
position.

**Catchable by existing tests?** Partially.
- `eval-fail-missing-arg.nix` (body references missing arg): v3 throws
  `OP_ATTRS_SELECT: attribute not found`. `run-fail-tests.sh` only
  checks that an error of *some* kind is raised — its loose regex
  matches "error" so this test counts as passing. The **golden
  `.err.exp` file** (which says "function 'anonymous lambda' called
  without required argument 'y'") is **never compared** by the v3
  test driver. Under upstream `tests/functional/lang.sh` against TW,
  the .err.exp would match.
- `eval-fail-empty-formals.nix` (`(foo@{}: 1) { a = 3; }` — the
  opposite case, extras-only): v3 throws
  `v3 OP_CALL: function called with unexpected argument 'a'` — also
  passes the loose gate but with a different error message
  (TW says "function 'anonymous lambda' called with unexpected
  argument 'a'", v3 omits the lambda name).
- **No test exercises** the body-doesn't-reference-missing-formal case
  that's the cleanest C2 repro — e.g., `({a, b}: 42) {a = 1;}`. That
  silent success goes undetected.

### C5 — `OP_STR_CONCAT` accepts numeric/Bool/Null in `+` mode

**Verified reproduced today** on standalone `v3-eval`:

```
$ v3-eval --expr '"foo" + 1'                       →  "foo1"  (v3)
$ nix-instantiate --eval -E '"foo" + 1'
    →  TypeError "cannot coerce an integer to a string: 1"  (TW)
```

**Catchable by existing tests?** No.
- `eval-okay-concat.nix` and similar exercise `+` on values TW
  already accepts (string + string, list ++ list).
- No `eval-fail-*` test exercises `"string" + integer` /
  `"string" + bool` / `"string" + null`. **Gap is silent.**

### C7 — `//` evaluation order reversed (TW evaluates RHS first; v3 evaluates LHS first)

**Verified reproduced today** on standalone `v3-eval`:

```
$ v3-eval --expr '(throw "lhs") // (throw "rhs")'
    →  v3 throw: lhs                  (v3)
$ nix-instantiate --eval -E '(throw "lhs") // (throw "rhs")'
    →  error: rhs                     (TW)
```

**Catchable by existing tests?** No.
- No `eval-fail-update-order` / `eval-fail-throw-update-order` test in
  the lang corpus. No test pins `//` operand order. **Gap is silent
  and would require a new test.**

### Cross-cutting: did #558 cleanup land any of the audit fixes?

Reading the recent commits:

```
1214a40b0 v3 #558: V3_DBG_FORCE_FILE filter + nested allDeps cascade trace
43bd25340 v3 #558: V3_DBG_TAINT diagnostic + investigation findings
b4a99d59a v3 #558: V3_DBG_FORCE_NAME / V3_DBG_FORCE_POS diagnostic gates
ab3357f2a v3 #558: pickLargestLayer helper — STG-true chain collapse
783020080 v3 #558: CFF_TAINTED — STG re-entrancy for thunks that observed in-flight WHNF
```

These all target the **#558 cluster** (STG WHNF recovery / partial-
Bindings chain, libsForQt5/qt5 territory — the architectural S2/S3/S5
surface, not the C-series silent gaps). **None of C1, C2, C5, C7 has
been addressed by the recent commit run.** The COMPREHENSIVE_REPORT
explicitly lists them as P0 *unfixed* recommendations.

---

## What's still broken (synthesis)

### High-impact silent correctness gaps (P0, COMPREHENSIVE_REPORT)

1. **C1 non-Bool boolean ops** — `&&`/`||`/`->`/`if`/`OP_BRANCH_FALSE`
   silently fall through on non-Bool LHS. No lang test catches it.
2. **C2 missing-required-formal** — silently returns body value when
   body doesn't reference the missing formal; produces wrong error
   text when it does. `eval-fail-missing-arg.err.exp` would drift but
   v3 test driver doesn't compare it.
3. **C3 `primAddErrorContext` erases exception type** — breaks
   `tryEval` transitivity for `addErrorContext "..." (throw "x")`.
   Real-world impact: nixpkgs `lib/modules.nix` wraps every
   module-eval in `addErrorContext`.
4. **C4 `primV3CallBridge1` rewraps non-Blackhole exceptions** as
   `runtime_error`; sibling `primV3ForceAttr` does `throw;` correctly.
5. **C5 `+` coerces too liberally** — `"foo" + 1` returns `"foo1"`.
6. **C6 `path + string-with-context`** silently drops context payload.
7. **C7 `//` evaluation order reversed** vs TW.
8. **C8 `Tag::PrimOpApp` not chased** at `OP_ATTRS_SELECT` head.

### Architectural divergences from TW (intentional but real)

9. **S1 `Tag::Blackhole` as a Value** — propagable, no TW analog.
   `NIX_V3_NO_BLACKHOLE_AS_VALUE=1` opts out.
10. **S2 STG WHNF recovery via `pickLargestLayer`** — returns
    approximate partial Bindings; two readers can see different
    snapshots. `NIX_V3_NO_STG_WHNF=1` opts out.
11. **S3 `OP_ATTRS_REC_INIT` publishes for ALL non-empty attrsets**,
    not just `rec` — the partial-Bindings registry chain is structural.
12. **S4 `OP_WITH_LOOKUP` swallows per-scope `BlackholeError`** —
    continues to outer with-scope where TW would have thrown.
13. **S5 inherit-from from-exprs heuristic-thunkified** — TW thunkifies
    unconditionally. Root of the
    `#455`/`#496`/`#498`/`#516`/`#546`/`#548`/`#558` cluster.

### v3-direct on real nixpkgs

14. **Full nixpkgs hello.name under v3-direct fails** with a moving
    error symptom (currently `STR_CONCAT: cannot coerce type to string`
    per 2026-05-11 run, previously `OP_ATTRS_SELECT: attribute not
    found` for qt5).
15. **lib/systems/parse.nix re-eval explosion** under broader
    thunkify — 60K re-evaluations vs 1 expected; arena climbs to 56 GB.
    Blocks any Phase 2 attempt at fixing the inherit-from cache.
16. **libsForQt5 closure deferred** until runtime perf absorbs the
    50M-thunk eval volume the bypass exposes.

### Error quality

17. **All ~180 v3 throw sites raise `std::runtime_error`** except
    `BlackholeError` / `AssertionError` / `ThrownError` / `AbortError`.
    Lost: typed-catch routing, source positions, "while evaluating
    attribute X" trace chains, "did you mean" suggestions.
18. **`OP_ASSERT` says only "assertion failed"** — no condition diff,
    no position.
19. **`kMaxCallDepth = 5000` hard-coded** (`vm.cc:96`); `--option
    max-call-depth N` doesn't reach v3.
20. **Profiler hooks not wired** — `NIX_COUNT_CALLS` /
    `--trace-function-calls` / `debugTraceStacker` all silently inert
    under v3.

### Test infrastructure

21. **`run-fail-tests.sh` uses loose-regex match** — C2-class
    "different error text but exit-code-matches" goes undetected. The
    upstream `tests/functional/lang.sh` (which does compare `.err.exp`
    exactly) is not invoked against v3.
22. **6 silent passes** on eval-fail (deprecated path / URL / stack-
    overflow tests). Not C-series, but worth tracking.
23. **No tests for C1/C5/C7** in the lang corpus — these gaps don't
    have golden files at all.

---

## Honest verdict

v3 is **correctly evaluating a structurally-large fraction of real
Nix** (142/142 lang corpus, all v3-direct micro-shapes, v3-fhook
full nixpkgs) but **diverges silently** on several semantic
surfaces that golden tests don't cover. The 142/142 number is real
but narrow; it measures `.exp` stdout parity for value-only
positive tests, not error parity, not error-message parity, not
operand-order parity, not coercion parity, not formals-validation
parity.

The v3-direct path is the canonical eval-owns-everything mode, and
it cycles on real nixpkgs at a moving error floor. That cycle is
the architectural S5 surface — eager inherit-from from-expr
lowering creates a partial-Bindings dependency that the STG WHNF
recovery has to mask, and the recovery is the source of all the
S1-S5 divergences.

The C-series silent gaps from today's audit (boolean type-check,
missing-formal, `+` coercion, `//` order, etc.) are mechanically
small fixes that would each take a handful of lines + a new
`eval-fail-*` test, and they have nothing to do with the S-series
architectural work. They are simply unprioritised because no test
catches them.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group.
SPDX-License-Identifier: Apache-2.0
