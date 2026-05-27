# v3 — Open Work Inventory (2026-05-11)

This report focuses **only on what is NOT done**. It pairs the
2026-05-11 audit (`COMPREHENSIVE_REPORT.md`, C1-C7 + S1-S11) and the
day's `#558` Phase 3.3a-3.3g cleanup with what's actually in the
working tree. Every claim was verified against source at HEAD
(`ba2476e73`, 2026-05-11 16:45). It does NOT celebrate the cleanup
wins — other agents handle that.

---

## Summary — the seven items that block shipping

Ranked by importance (highest first):

1. **Full nixpkgs eval does not complete in v3-direct or v3-fhook
   modes.** `(import <nixpkgs> {}).hello.name` and any deeper
   nixpkgs/cardano workload either cycles (default settings) or
   **times out >120s** (post-Phase-3.2 with `NIX_V3_INHERIT_FROM_THUNK_ALL`
   + `NIX_V3_NO_PARTIAL_BINDINGS` default-on). The "cell-update
   everywhere" replacement (Phase 1.5 `Thunk::shapeCell`) is
   **opt-in only** (`NIX_V3_CELL_EVERYWHERE`) and does NOT fire for
   the outer `x`-thunk in `lib.fix toFix` (no `OP_ATTRS_REC_INIT` in
   its body). See `CELL_UPDATE_EVERYWHERE_2026-05-12.md` §"Phase 1.5
   landed + scope decision".
2. **C1-C7 audit findings are real and unfixed in HEAD.** Spot-checked
   every one against current `vm.cc` / `primops.cc` line numbers (see
   §"Audit findings still real" below). C2 (missing-required-formal
   not validated) and C3 (`primAddErrorContext` erases exception
   type) are the most impactful because they silently change program
   behaviour (`tryEval` transitivity broken on nixpkgs `lib/modules.nix`
   `addErrorContext` wrapping).
3. **Profiler / observability completely inert under v3.** No call
   site invokes `EvalProfiler::preFunctionCallHook`,
   `nrFunctionCalls`, `functionCalls[lambda]`, `primOpCalls`, or
   `--trace-function-calls`. Zero hits in `*.cc` outside the
   `lode/` audit files. `kMaxCallDepth = 5000` hardcoded at
   `vm.cc:96`; `settings.maxCallDepth` not threaded.
4. **77 of 96 `NIX_V3_*` env vars are load-bearing toggles that
   suggest unresolved underlying issues.** Many `NIX_V3_NO_*`
   variants default-on a tactical workaround whose root cause was
   never closed (see §"Env-var gates of concern"). `NIX_V3_INHERIT_FROM_THUNK_ALL`
   was flipped default-on for cycle-elimination but trades 100x
   slowdown on nixpkgs (per `PROGRESS_PERF_TRAJECTORY.md` and
   `CELL_UPDATE_EVERYWHERE_2026-05-12.md` Phase 2 baselines).
5. **`v3_hook.cc` is 4147 LOC of bridge surface with inconsistent
   exception-rewrap policy across primAddErrorContext / primV3CallBridge1
   / primV3ForceAttr / ffi::applyClosure** (audit C3, C4 — both
   verified). Per the audit's "Cross-cutting themes §3", the right
   policy is uniform `throw;` re-throw on bridge re-entry. Not
   landed.
6. **Standalone v3 has fake-store fallbacks** for `derivationStrict`
   (`primops.cc:4944-4950`) and `builtins.path` (`primops.cc:6287`).
   Real derivations require `state.nixEvalState` to be wired — i.e.,
   the v3-standalone story is **only viable for non-derivation
   evaluation**. Even `v3-eval` lang tests pass 142/142 (parity
   semantics), but `hello.drvPath` has never been measured under
   any v3 mode per `REVIEW_2026-05-09.md` §6.
7. **Inversion plan (#454/#458) is structurally pinned on a bug v3
   has never solved.** `V3_NATIVE_CONSTRAINT_2026-05-09.md` forbids
   the TW-routing escape valve; `V3_DIRECT_NIXPKGS_2026-05-09.md`
   documents that the cycle is robust to every optimisation toggle
   (8 distinct `NIX_V3_NO_*` switches). The cell-everywhere
   architecture is the agreed v3-native replacement but stays
   opt-in pending Phase 2 (lazy inherit-from at perf cost) +
   Phase 3 (registry retirement) + Phase 4 alloc work to claw back
   the 100x slowdown.

---

## Known-broken / known-fail scenarios

### Tests that intentionally assert failure (must be promoted, not
just maintained):

- **`test/known-fail-callpackage-with.sh`** — exits 0 if
  `(import nixpkgs {}).hello.name` *still fails* under
  `NIX_V3_DIRECT_EVAL=1`. Accepts five symptom shapes:
  `OP_WITH_LOOKUP: name 'callPackage' not found`, `OP_WITH_LOOKUP:
  cycle while resolving 'callPackage' | 'texlive' | 'libsForQt5'`,
  and `OP_ATTRS_SELECT: attribute not found`. After today's
  Phase 3.3 cleanup, the current symptom is the
  `OP_ATTRS_SELECT` form (per the script's comment block).
- **`test/repro-455.nix`** — `lib.fix (self: with self; { ... })`
  with ≥5 bindings; fails under
  `NIX_V3_ON_DEMAND_ROOT=1 NIX_V3_SKIP_THRESHOLD=0`. Originally
  `#455`; per project memory `project_455_root_cause`, root-caused
  to env-shape mismatch in `prev // overlay final prev` but **not
  fixed**.
- **`test/repro-495-broader-thunkify-bug.nix`** — `lib.systems.elaborate
  "x86_64-linux"` under `NIX_V3_SELF_DOT_MAX_LEVEL=1`. The file's
  header note says "RESOLVED 2026-05-07 via #496 + #497" but the
  176-line investigation log at the bottom contradicts that —
  documents a residual "21-thunk simultaneous thunkify" upvalue
  corruption that's "pure mystery: skipping any single thunkify makes
  the bug disappear, no matter which one". Status mixed.

### nixpkgs-class scenarios documented as not working in any mode:

- `(import nixpkgs {}).hello.name` — cycles default, times out >120s
  with `NIX_V3_INHERIT_FROM_THUNK_ALL=1`.
- `attrnames-pkgs` — times out.
- `hello.drvPath` — never measured under v3 (no test in
  `run-direct-eval-tests.sh`).
- `cardano-node.name` — last measured 2026-05-04 (six days ago).

### Test infrastructure caveats:

- `run-lang-tests.sh` has a `KNOWN_SKIP` map (empty today) — the
  comment "Currently no tests are skipped" is accurate per
  `vm.cc`-side, but upstream `eval-okay-tail-call-1.exp-disabled`
  is skipped by file-marker convention.
- `bench/workloads.toml` has `skip-by-default` on every full-nixpkgs
  workload (`hello-name`, `attrnames-pkgs`, `hello-drvPath`,
  `git-name`, `firefox-drvPath`) — they're invisible to the
  default bench run. Until the cycle closes, the bench numbers
  *literally* describe `lib`-only workloads.

---

## Architectural debt — where the cleanup is partial

### `Tag::Blackhole` value flow still exists (audit S1)

- Singleton declared at `value.cc:26` as `Value::vBlackhole`.
- Used as a propagating sentinel at `vm.cc:7401-7420` when forcing a
  foreign-VMState Black thunk, gated by `NIX_V3_NO_BLACKHOLE_AS_VALUE`
  (default-on).
- Downstream consumers handle Blackhole in `gc.cc:165, 330`,
  `primops.cc:422, 692, 743, 3268, 4048, 5926, 6708, 6804`,
  `v3_hook.cc:2426, 3079, 3834-3883`, `vm.cc:459, 550, 3682, 3879,
  7401, 7736`, `print.cc:94, 212`.
- TW has no equivalent. Audit S1 unchanged.

### `CFF_TAINTED` is dead code

- Declared in `vm.hh:55`. Read at `vm.cc:3573` in OP_RETURN. **Never
  written anywhere** (`grep "fFlags |= CFF_TAINTED\|flags |= CFF_TAINTED"`
  returns nothing). The Phase 3.3c cleanup cleared the only setter
  ("STG WHNF recovery"). The reader can be deleted along with the
  field — partial cleanup.

### Partial-Bindings comments persist after function deletion

- Phases 3.3b/3.3g deleted `publishToNearestBlackThunkFrame`,
  `publishToAllThunkFrames`, `pickLargestLayer`,
  `lookupInPartialChain`, `partialBindingsRegistry`. But six
  comments in `vm.cc` (lines 649, 1162, 4608, 4686, 4809, 5317,
  7686) still reference these by name as if they exist, plus
  `emit.cc:941`. Reader bait.

### `Thunk::shapeCell` is opt-in and partial

- Field added (`closure.hh:131-156`) gated by
  `NIX_V3_CELL_EVERYWHERE` env-var (`vm.cc:4587`).
- Wired ONLY at `OP_ATTRS_REC_INIT` (`vm.cc:4598`) and
  `OP_ATTRS_REC_INIT_TAIL` (`vm.cc:4694`-ish).
- Read at `vm.cc:3602` in OP_RETURN.
- Phase 1.5 design doc admits: "v3-direct nixpkgs still fails the
  same way ... because x's body doesn't directly fire
  OP_ATTRS_REC_INIT (its body just calls f), so x's shapeCell stays
  at the sentinel". The cell-everywhere fix as designed does NOT
  close the original libsForQt5 cycle — the cycle's real cause
  is "eager forcing that produces the cycle" (eager inherit-from).
  Cell-everywhere is correct STG semantics; the remaining work is
  on the laziness side.

### Bytecode opcode bloat from #558 split

- `OP_ATTRS_LET_REC_INIT` (`vm.cc:4602-4644`) and
  `OP_ATTRS_REC_INIT_TAIL` (`vm.cc:4645-4710`) were added by #546
  / #558. Each is "bytecode-identical to OP_ATTRS_REC_INIT" except
  for the publish/shapeCell write path. Three opcodes that
  collapse into one with a flag word. Per `REVIEW_2026-05-09.md`
  agent 2: "v3 bytecode is 2.5× bloated; 50 ops where ~20 suffice".

### `lower.cc` self-dot thunkify heuristic

- `NIX_V3_SELF_DOT_MAX_LEVEL` default 4 (`lower.cc:1815`).
- Code comment at `lower.cc:1778`: "TODO: root-cause and"…(truncated
  in the file). The ONLY explicit `TODO` marker in the v3 codebase
  is this one. Root-cause for the level≥1 self-dot bug has been
  open since 2026-05-07 (`repro-495-broader-thunkify-bug.nix`).

### File-size guideline ignored

- `vm.cc` 8121 LOC, `v3_hook.cc` 4147 LOC, `primops.cc` 8219 LOC,
  `lower.cc` 3017 LOC. CLAUDE.md states "Keep files small if
  possible up to 1000 lines". No split is currently in progress
  (per `REVIEW_2026-05-09.md` agent 4).

---

## Env-var gates of concern (still load-bearing)

The full list is in `REVIEW_2026-05-09.md` agent 4 (77 flags missing
from USAGE.md). Highlights of gates whose presence indicates an
underlying unsolved problem:

### Diagnostic-but-shipped (default-on workarounds)

| Gate | Default | What disabling it breaks |
|---|---|---|
| `NIX_V3_NO_BLACKHOLE_AS_VALUE` | OFF (i.e. flow-as-value ON) | Foreign-VMState Black thunk handling reverts to thrown `BlackholeError`; bridge1 fallback retry chains grow C-stack unboundedly |
| `NIX_V3_NO_PARTIAL_BINDINGS` | ON (registry off as of #558 Phase 3.2) | Old chain-peek logic; replaced by shapeCell but cell-everywhere is OFF by default — so disabling NO_PARTIAL_BINDINGS today means "use nothing" |
| `NIX_V3_INHERIT_FROM_THUNK_ALL` | ON as of Phase 3.2 | Falls back to narrow self.X heuristic; reintroduces cycles on full nixpkgs but compiles `lib`-only fast |
| `NIX_V3_NO_REFUSE_FORMALS_BRIDGE` | OFF (i.e. refusal ON) | Removing refusal lets cardano-node progress further into deeper TW infinite-recursion (per `primops.cc:3972-3981`) |
| `NIX_V3_NO_ACTIVE_V3_VM_REFUSE` | OFF (i.e. refusal ON) | Two sites (`v3_hook.cc:1594, 3280`); guards re-entry while v3 VM is active |
| `NIX_V3_NO_LAMBDA_REENTRY_GUARD` | OFF (i.e. guard ON) | `v3_hook.cc:3306` |
| `NIX_V3_NO_WRONG_SHAPE_REFUSE` | OFF (refusal ON) | `v3_hook.cc:3603` — refuses to bridge wrong-shape values |
| `NIX_V3_NO_REFUSE_REC_CAPTURE_LAMBDA` | OFF (refusal ON) | `v3_hook.cc:3644` |
| `NIX_V3_NO_STG` (and `NIX_V3_STG_KEEP_HOOKS`) | various | `v3_hook.cc:1573-1594, 3142-3143` (STG flag flipped default-on by #547) |

Pattern: each "`NO_*` default-OFF means safety-net default-ON" gate
is a workaround promoted to permanent infrastructure. None of them
have been retired despite the audit calling out 30+ negation flags.

### Opt-in features still gated

| Gate | Purpose | Status |
|---|---|---|
| `NIX_V3_CELL_EVERYWHERE` | Mid-body cell update for cycle visibility | Phase 1.5 prototype, default OFF; primary "v3-native replacement for partial-Bindings registry"; doesn't fully close the cycle by itself |
| `NIX_V3_TW_LAMBDA_BRIDGE` | Real Tag::tLambda bridge for formals closures | Default OFF; #493 prerequisite for lambda-skip default-on but unvalidated for general traffic |
| `NIX_V3_LAMBDA_SKIP` | Skip v3-internal lambda dispatch | Default OFF; documented to surface "captured-with chain breaks at file boundaries" cycle (`INVERSION_PLAN_2026-05-08.md` c) |
| `NIX_V3_DIRECT_EVAL` | CmdEval bypasses TW eval | Default OFF; functional only on the 26-shape `run-direct-eval-tests.sh`; cycles on nixpkgs |
| `NIX_V3_NURSERY` / `NIX_V3_NURSERY_SCAVENGE` | Cheney young-gen GC | Default OFF; Phase D/E unfinished — cell-write barrier missing (`CHENEY_NURSERY_DESIGN.md` §Phase D risk) |
| `NIX_V3_FIBER_BRIDGE` | Fiber-based applyClosure re-entry | Default OFF; `primops.cc:3048` |

### Diagnostic gates with no production purpose

24 `V3_DBG_*` gates exist purely for tracing. These are fine but
they pollute the env-var namespace and several use static-cached
`std::getenv` (potential lint target):

- `V3_DBG_FORCE_NAME`, `V3_DBG_FORCE_POS`, `V3_DBG_FORCE_FILE`,
  `V3_DBG_FORCE_SITE`, `V3_DBG_FORCE_INSIDE_X`, `V3_DBG_TAINT`
  (added in last 24 h per the recent commits `1214a40b0`, `b4a99d59a`,
  `783020080`, `43bd25340`).

The fact that we needed three new "trace force calls by name/pos/file"
gates *yesterday* (2026-05-11) is itself a signal — the team is still
diagnosing the same cycle class with finer-grained tracing rather than
closing it.

---

## Bridge layer issues — the `v3_hook.cc` surface

`v3_hook.cc` is 4147 lines. Per the audit Cross-cutting theme #3
("Bridge boundary disagrees with itself") and project memory
(`project_v3_ffi_plan`, `project_493_step3d_with_stack`,
`feedback_v3_native_constraint`):

### Confirmed bridge bugs (audit C3, C4)

- **`primAddErrorContext` erases exception type** (`primops.cc:2712-2725`).
  Catches `std::exception &`, rethrows as plain `std::runtime_error`.
  Destroys `AssertionError` / `ThrownError` typing. `tryEval
  (addErrorContext "ctx" (throw "x"))` now doesn't catch.
  **Impact: every nixpkgs `lib/modules.nix` `_module.config`
  evaluation wraps in `addErrorContext`; any catchable `throw`
  becomes uncatchable on v3.**
- **`primV3CallBridge1::fallbackToTreeWalker` rewraps non-Blackhole
  exceptions inconsistently** (`primops.cc:3017-3019`). Sibling
  `primV3ForceAttr` (`primops.cc:3404-3405`) does the right thing
  (`throw;`). Two policies on adjacent surfaces.

### Wider bridge weaknesses (project memory)

- **Bridge cycles** from `primV3CallBridge1::depth` — gated by
  `NIX_V3_BRIDGE1_DEPTH=8` (`primops.cc:2933`),
  `NIX_V3_BRIDGE1_REENTRY_MAX` (`primops.cc:3105`),
  `NIX_V3_BRIDGE_PRIMOP_DEPTH` (`primops.cc:165`),
  `NIX_V3_FORCE_CHAIN_DEPTH` (`primops.cc:219`),
  `NIX_V3_FORCE_CHAIN_REENTRY_MAX` (`primops.cc:242`),
  `NIX_V3_FALLBACK_CHAIN_DEPTH` (`primops.cc:3474`),
  `NIX_V3_EAGER_BRIDGE_MAX` (`primops.cc:3734, 3836`). **Seven
  separate depth-limit gates**. Each is a tactical limit because
  bridge re-entry can grow unboundedly. Architecturally, this is
  symptomatic of cross-VMState cycle pathology rather than design.
- **`withStack` non-propagation** through bridged TW thunks
  (project memory `project_493_step3d_with_stack`). When v3 bridges
  out to TW (e.g. via `primV3CallBridge1`), the v3 with-stack does
  not propagate. Re-entry from TW sees an empty with-stack. The
  bridge surface accepts this and routes failure through the
  fallback retry chain. Architectural, not a one-CL fix.
- **`willReturnClosure` static detector** (`v3_hook.cc:1364-1393`)
  walks an Expr AST to decide whether v3 should evaluate it. The
  walk is incomplete (`Let`, `With`, `Assert`, `If`-then/else
  unioned). Pre-WC-11, this was a fallback heuristic; today
  `liftWillReturnClosure` (`v3_hook.cc:1982`) makes it secondary
  to `invertEval` but the code paths still coexist.
- **`tryDispatchTWLambdaInV3`** (`vm.cc:2400, 7860`; defined at
  `v3_hook.cc:4016`) is a v3-only fast path that bypasses the
  bridge for TW lambdas it can dispatch internally. It "REJECTs"
  silently when the lambda's shape isn't supported, falling back
  to the regular bridge. Reject paths haven't been audited for
  parity with the regular bridge.

### FFI is ghost structure

Per `REVIEW_2026-05-09.md` agent 5:

- 38 callable entry points declared in `ffi.hh`.
- 6 implemented in `ffi.cc`.
- **0 production call sites outside `ffi.cc` itself.**
- `applyClosure` only reached from the test
  (`test/evalscope-handles.cc`).
- Categories A/B/F (Parser, Symbol/Pos, Eval entry) obsoleted by
  the inversion plan; H (EvalError), I (Settings), J (Logger), K
  (PrimOp flags), L (StringContext), M (BlockingFFI) — Agent 5
  recommended PAUSE on those; no `STATUS: PAUSED` header has been
  added since.

---

## Audit findings still real (cross-reference C1-C7, S1-S11)

Each item below was re-verified against HEAD source. Notation:
"open" = still present and reproducible; "stale" = the function/code
the audit cited has been removed.

### Critical (correctness-affecting)

| # | Status | Notes |
|---|---|---|
| C1 (non-Bool LHS to `&&`/`||`/`->`/`if`/`OP_BRANCH_FALSE`) | **OPEN** | `vm.cc:2138, 2146, 2155, 2164` unchanged. `null && true` → `true` on v3; `TypeError` on TW. |
| C2 (missing required formal not validated) | **OPEN** | `vm.cc:3398-3413` only checks extras, not missing. `({a,b}: 42) {a=1;}` → 42 on v3. Golden tests `eval-fail-missing-arg.err`, `eval-fail-empty-formals.err` will diverge. |
| C3 (`primAddErrorContext` erases exception type) | **OPEN** | `primops.cc:2712-2725`. Breaks `tryEval` transitivity for nixpkgs module evaluation. |
| C4 (`primV3CallBridge1` rewraps non-Blackhole inconsistently) | **OPEN** | `primops.cc:3017-3019`. Sibling at `primops.cc:3404-3405` is correct. |
| C5 (`OP_STR_CONCAT` accepts non-string in `+`) | **OPEN** | `vm.cc:6682` calls coerceToString unconditionally; TW passes `coerceMore=false`. `"foo" + 1` → `"foo1"` on v3. |
| C6 (`path + "context-string"` drops context) | **OPEN** | `vm.cc:6663-6664, 6691-6697`. Path-arm under `forceStr=false` strips String context, drops to no-context Path. |
| C7 (eval-order of `//` reversed) | **OPEN** | `lower.cc:2068-2069` emits LHS-first; TW does RHS-first (`eval.cc:2487`). Visible in tests that pin operand order. |
| C8 (PrimOpApp not chased at OP_ATTRS_SELECT/HAS head) | **OPEN** | `vm.cc:5805-5807, :5334-5337` chase Tag::App / Thunk / Slot only. |

### Significant (narrower impact)

| # | Status | Notes |
|---|---|---|
| S1 (Tag::Blackhole as propagable Value, no TW counterpart) | **OPEN** | `value.cc:26` singleton + ~14 consumer dispatch sites unchanged. Gated `NIX_V3_NO_BLACKHOLE_AS_VALUE` default-on. |
| S2 (STG WHNF recovery returns approximate partial Bindings) | **STALE → partially closed** | The `publishToNearestBlackThunkFrame` / `pickLargestLayer` / `lookupInPartialChain` functions audit cited were **deleted today** (Phase 3.3b/3.3g, 14:59-15:28). `CFF_TAINTED` is dead code (read at `vm.cc:3573` but never set). The replacement is `Thunk::shapeCell` (Phase 1.5), which has the same divergence-from-TW *in principle* but is opt-in and not the production default. **The architectural divergence remains; the specific code the audit pointed to is gone.** |
| S3 (`OP_ATTRS_REC_INIT` publishes for all non-empty attrsets) | **STALE** | Same as S2: publish path deleted Phase 3.3. The `OP_ATTRS_REC_INIT_TAIL` / `OP_ATTRS_LET_REC_INIT` split (#546) survives but uses `shapeCell` instead of registry. |
| S4 (`OP_WITH_LOOKUP` swallows per-scope BlackholeError) | **OPEN** | `vm.cc:686-694` catch unchanged. v3 advances to outer scope where TW throws. |
| S5 (Inherit-from from-exprs not unconditionally thunkified) | **OPEN — and worse** | The narrow `self.X` heuristic is still gated at `lower.cc:1817` (`NIX_V3_SELF_DOT_MAX_LEVEL=4`). Phase 3.2 flipped `NIX_V3_INHERIT_FROM_THUNK_ALL` default-on, which IS the "unconditional thunkify" the audit recommended, but pays 100x slowdown on nixpkgs and so is the actual blocker for full-nixpkgs eval. The audit's "S5 → S3 → S2" causal chain has S2/S3 removed today but S5 unchanged, leaving the perf gap as the active blocker. |
| S6 (function equality returns false) | **OPEN** | `vm.cc:539-546`. `f == f` returns false on v3; AssertionError on TW. |
| S7 (`valueLess` lacks Path branch) | **OPEN** | `vm.cc:557-589`. `builtins.sort builtins.lessThan [./a ./b]` fails on v3. |
| S8 (`__toString` non-string falls through to outPath) | **OPEN** | `vm.cc:6643-6651`. Plus the separate outPath-returning-Path path (`vm.cc:6653-6660`) skips `copyPathToStore`. |
| S9 (`builtins.throw`/`abort` reject non-string) | **OPEN** | `primops.cc:790-796, :1439-1444`. TW coerces via `coerceToString`. |
| S10 (v3 standalone fake-store for `builtins.path` and `derivationStrict`) | **OPEN** | `primops.cc:4944-4950, :6287`. Only relevant for v3-standalone — bridged v3 routes through TW. Means "v3-standalone is not a viable derivation evaluator". |
| S11 (`primReadFile` doesn't scan for store-path refs) | **OPEN** | `primops.cc:2510-2568`. TW does `PathRefScanSink::fromPaths(refs)` at `primops.cc:2237-2263`. Real impact in nixpkgs (`readFile` on patch files etc.). |

### Error-quality (cosmetic but pervasive)

All open. From the audit:
- 89 `std::runtime_error("v3 OP_*: ...")` in `vm.cc`, 91 in
  `primops.cc`. No source position. No "while evaluating" trace
  chain. No `withSuggestions`. Not derived from `nix::EvalError`,
  `TypeError`, `AssertionError`, `StackOverflowError`,
  `InfiniteRecursionError`. Only `ThrownError` and `BlackholeError`
  are typed.
- `OP_ASSERT` says `"v3 OP_ASSERT: assertion failed"` — no
  position, no failing condition, no `assertEqValues` diff.
- Stack overflow hardcoded `kMaxCallDepth = 5000` (`vm.cc:96`);
  not wired to `settings.maxCallDepth`.
- Multiple `*.err.exp` golden tests will drift when v3 errors
  surface.

### Performance / observability gaps

All open. Specifically:
- **Slot mutation on first force in `OP_GET_LOCAL_FORCE`** —
  `vm.cc:1869-1914` doesn't write WHNF in place. Subsequent reads
  pay Tag::Thunk → Evaluated indirection.
- **Profiler hooks** — `NIX_COUNT_CALLS`,
  `EvalProfiler::preFunctionCallHook` / `postFunctionCallHook`,
  `nrFunctionCalls`, `functionCalls[lambda]`, `primOpCalls`,
  `primOpTimerStack`, `--trace-function-calls`, `debugTraceStacker`
  — **zero call sites in v3 source** (`grep` confirms only audit
  files mention them).
- **`trylevel` not tracked** in v3 → REPL debugger cannot skip
  inside-try frames.
- **TCO iteration cap** 10⁷ (`vm.cc:3038`) — v3-specific. Programs
  that work on v3 may exhaust at unexpected depth.

---

## Realistic short-term blockers to ship v3-default-on

In priority order (ascending difficulty within each block):

### Must-fix correctness (P0)

1. **C1 — non-Bool LHS type-check** on boolean opcodes. 4-site
   diff in `vm.cc:2138-2164`. Half a day.
2. **C2 — missing-required-formal validation** in `OP_CALL` after
   the extras check (`vm.cc:3398-3413`). Lambda-name + formal-name
   + position needed in error. ~1 day.
3. **C3 — `primAddErrorContext` exception-type preservation.**
   Nested catches for `AssertionError`, `ThrownError`, `BlackholeError`,
   `AbortError`. `primops.cc:2712-2725`. Half a day.
4. **C4 — `primV3CallBridge1::fallbackToTreeWalker` uniform `throw;`.**
   `primops.cc:3017-3019`. 1-line fix.
5. **C5 / C6 — string coercion + path-with-context.** ~half a day.
6. **S1 retirement** — eliminate `Tag::Blackhole` value flow,
   replace with typed `InfiniteRecursionError`. Touches ~14 sites.
   Audit calls this "not a one-CL change" — likely 2-3 days.

### Must-fix functionality (still P0, gating real workloads)

7. **Full nixpkgs eval completable.** Either:
   - **Path A:** finish cell-everywhere (`NIX_V3_CELL_EVERYWHERE`
     default-on), retire the registry (already done), AND close the
     S5-class cycle source via Phase 2 lazy inherit-from. Phase 4
     alloc work is making it cheap enough to be feasible. Open-ended
     estimate; days-to-weeks.
   - **Path B:** drop standalone-v3 ambitions; pin to v3-fhook +
     STG default-on (the post-#547 mode that works on nixpkgs at
     1.4-1.5× TW). Less ambitious but ships faster.
8. **Bench harness measuring real workloads in CI.** Currently
   nothing measures `hello.drvPath` or cardano-node in any v3
   mode. Until the regression net catches "v3 can build a real
   package", every default-flip risks invisible regression.

### Must-fix observability (P1, blocks debugging in production)

9. **Wire `kMaxCallDepth` to `settings.maxCallDepth`** (`vm.cc:96`
   and call site `vm.cc:2953`). ~1 hour.
10. **Wire profiler hooks** into `OP_CALL` / `OP_CALL_PRIMOP` /
    `callClosure` per audit's P1 recommendation §9. Without this,
    `NIX_COUNT_CALLS` is silent under v3 — users will hit it and
    file bugs.
11. **Minimal error-trace stack** with positions + "while
    evaluating X" frames. Audit's P1 §10: "thread-local vector
    pushed at OP_CALL_PRIMOP and selected OP_ATTRS_* sites, drained
    on throw, is mechanically straightforward and would close 80%
    of the error-quality gap."

### Must-fix architecture (P2, harder but blocks v3-direct as goal)

12. **Bridge boundary policy uniform.** Today's mix of `throw;`
    (good), `throw std::runtime_error(ex.what())` (drops type),
    `dynamic_cast<BlackholeError *>` (works but verbose) is the
    proximate cause of half the cross-stack pathology. The right
    policy is a unified bridge-shim that re-throws originals and
    classifies BlackholeError separately. The audit's
    Cross-cutting #3 recommendation. ~3-5 days.
13. **Inversion path either-or.** Either complete it (#458 step
    7 onwards: lift formals-closure refusal, finish standalone
    primops) or formally PAUSE per `REVIEW_2026-05-09.md` agent
    5's recommendation. Today it's neither — half-implemented and
    not-paused.
14. **Audit drift mechanisation.** `REVIEW_2026-05-09.md` agent 4
    proposed `make check-flags-doc-sync` and `make check-lode-index`.
    Today the audit dated 2026-05-11 15:57 still cites
    `publishToNearestBlackThunkFrame` and `pickLargestLayer` —
    functions deleted at 15:05-15:28 the same day. The cleanup
    moves faster than the audit, the audit moves faster than the
    docs, and the docs are the only thing users see.

---

## Items the audit raised but I downgraded

These are real but lower priority than the list above:

- **S6 (function equality on v3 returns false instead of throwing).**
  Real, reproducible (`let f = x: x; in f == f`), but no real-world
  Nix code I can find relies on the TW throw behaviour for
  control flow. Worth fixing for parity, not load-bearing.
- **Subtle divergences §"forceValueDeep cycle detection keyed
  differently", §"primGenericClosure tag-prefixed string hash",
  §"JSON parse errors", §"primReadDir DT_UNKNOWN".** Real, narrow
  impact. Worth a single tracking ticket each, none individually
  blocking.
- **TCO iteration cap of 10⁷.** Real, no observed real-world
  trigger. Worth a settings hook eventually.

---

## What's NOT in this report (because other agents cover it)

- Bench numbers, perf trajectory, regression timing — see
  `PROGRESS_PERF_TRAJECTORY.md` (sibling file).
- Per-opcode semantic divergence detail — see the six sub-agent
  reports (`force_blackhole_semantics.md`, etc.).
- Optimizer pipeline status — see `OPTIMIZATION_PLAN.md` /
  `OPTIMIZER_REPORT_2026-05-07.md` and `REVIEW_2026-05-09.md` §2.

---

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group.
SPDX-License-Identifier: Apache-2.0
