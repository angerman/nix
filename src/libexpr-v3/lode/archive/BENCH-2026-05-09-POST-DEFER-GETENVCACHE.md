# v3 perf — post-deferring + hot-path getenv cache (2026-05-09)

Date: 2026-05-09
System: aarch64-darwin (M4)
Comparand: in-tree TW (`build/src/nix/nix eval`)
Measurement tool: `src/libexpr-v3/test/bench-eval-only.sh` (V3_TIMING
phase split + NIX_VM_STATS hook entries to surface VM-vs-FFI
breakdown).  N=3, take min.

## Headline (final)

|  Workload     | TW(s)  | v3.run | v3.run/TW | post-#530 | this session |
|---|---|---|---|---|---|
| fib30         | 0.42s  | **252ms**  | **0.59x** | 906ms     | **-72%**       |
| fib33         | 1.62s  | **1059ms** | **0.65x** | (extrap. 3786ms) | **-72%** |
| ackermann-3-7 | 0.18s  | **109ms**  | **0.62x** | (extrap. 480ms)  | **-77%** |

**v3 has overtaken the tree-walker on the canonical compute-bound
microbenchmarks.**  Headline ratios under 1.0× = v3 is faster than
TW.

## Progression

The session arc was three distinct optimization rounds:

| stage                               | fib30 v3.run | v3.run/TW |
|---|---|---|
| baseline (post-#530)                | 906ms        | 2.40x     |
| + #542 deferring (single-use elim)  | 835ms        | 1.96x     |
| + #542 follow-up (And/Or/Impl/If)   | (≈ same)     | (≈ same)  |
| + #538 hot-path getenv cache (vm.cc) | 547ms       | 1.28x     |
| + #538 follow-up (forceValue cache) | **252ms**    | **0.59x** |

Tight numeric loops:
  - fib went from **2.4× slower than TW (post-#530) to 1.3-1.4× slower**.
  - ackermann is at **TW parity (1.08×)**.

Process-bound workloads (path-deep-30, letrec-fix, list-build-1k,
fold-add-10k, with-deep) are all sub-5ms v3.run, dominated by `nix
eval` startup (~52ms baseline).  Not optimisation-targets.

## Patches landed this session

1. **#540 — `opt_occur.cc` (analyseOccurrence)** — foundation
   pass for downstream optimisations; classifies VarIds as
   Param/Dead/OnceLinear/OnceCaptured/Many.  Skips Lambda::freeVars,
   MkThunk::freeVars, LetRec::outerUpvalues / lexicalWiths captured-
   list entries to avoid double-counting after `computeFreeVars`
   runs.

2. **#542 — emit-time deferring + binary fast paths** — for
   OnceLinear bindings whose value is consumed by the immediately-
   following op, skip the SET/GET pair.  Implemented as a small
   `pendingDefer` stack maintained by Emitter, with op-level fast
   paths that consume from pending top:
     - Binary fast path: `Add/Sub/Mul/Div/Eq/NEq/Less/App/
       AttrSelectDyn/HasAttrDyn/ConcatLists/Update`
     - Unary fast path: `Not/AttrSelect/HasAttr/RecBindingSlotRef/
       Force`
   Deferring is gated by `NIX_V3_NO_DEFER` env var; default-on.
   Saves ~7-8% on fib's tight `if k < 2 then ... else ...` shape.

3. **#542 follow-up — extend unary fast path to And/Or/Impl/If**
   — the four short-circuit / control-flow heads whose lhs/cond is
   consumed by their op (popped off the runtime stack).  The If
   case lets `if k < 2` chain with the binary fast path on Less to
   collapse `<expr-cond>; SET; GET; BRANCH_FALSE` to `<expr-cond>;
   BRANCH_FALSE` — confirmed via FileCheck on the disasm.

4. **#538 follow-up — cache hot-path getenv calls (vm.cc)** —
   seven diagnostic env-var lookups (`V3_DBG_FINAL_CALL`,
   `V3_DBG_OP_CALL_POST`, `V3_DBG_TC_PRE`, `V3_DBG_MAKE_PREV`,
   `V3_DBG_MAKE_RES`, `V3_DBG_MAKE_SUPER`, `V3_DBG_MAKE_SUPER_ALL`)
   were calling `std::getenv()` on every dispatch.  On macOS,
   `getenv` walks the process env table linearly with `strcmp(3)`
   (fast in absolute terms — ~50ns — but a death sentence at
   fib33's 5.7M OP_CALL invocations).  Converted to cached
   `static const bool` with `__builtin_expect(s_dbg_X, 0)
   [[unlikely]]`.  Saved ~35-50% on hot loops.

5. **#538 follow-up — cache forceValue per-call getenv** — the
   public `forceValue(VMState &, Value)` entry point at vm.cc:5897
   was calling `std::getenv("V3_DBG_FORCE_CALLSITE")` on every
   invocation for an STG-12 cycle-trace diagnostic.  forceValue is
   on the slow path of OP_FORCE / get-local-force-on-thunk, but for
   fib it fires millions of times.  Cached → fib/ackermann dropped
   another 43-53%, putting v3 **faster than TW** on these
   microbenchmarks.  This was the single biggest win of the
   session.

6. **#538 follow-up — cache primops + hook getenvs** — two more
   sites: `primops.cc:3323` (V3_DBG_FORCE_ATTR_ENTRY in forceAttr,
   hot for nixpkgs-shape eval) and `v3_hook.cc:128`
   (NIX_V3_NO_CONTENT_CACHE in cacheLowerHandle).  Off fib's hot
   path so no fib-level number moves, but closes the audit of
   inline non-cached getenvs in v3 production code.

## Tests added

- 12 occurrence-analysis tests (positive, negative,
  double-counting regression after `computeFreeVars`).
- 3 IR dumper / FileCheck infrastructure tests.
- 5 deferring tests (positive shape via FileCheck on disasm,
  correctness, kill-switch, LetRec regression).
- 1 unary-fast-path test for `If(Less)` chain.

All 142/142 v3 lang tests + 3/3 libexpr-v3 unit tests pass under
the full optimisation set.

## Items deferred

- **#543 direct threading via computed goto** — analysis showed
  this is too invasive for a single session.  The dispatch loop has
  61 cases over 4300 lines, with 136 `break;` statements many of
  which are inside nested `for`/`while` loops or conditional
  bodies.  A clean rewrite to GCC's `&&label` extension would
  require careful per-case classification of every `break`
  (case-exit vs. inner-loop-exit).  Estimated win on real-world
  workloads is 10-15% based on CPython 3.11 evidence; not pursued.
  Filed as task #543; revisit when there is a clean window for a
  ~5000-line dispatch-loop refactor.

- **#544 typed numeric opcodes** — already covered by the existing
  in-place int-int fast paths in OP_ADD/SUB/MUL/DIV/EQ/NEQ/LESS
  (see #536).  Adding separate OP_ADD_II opcodes would only help
  with static type analysis or quickening — neither of which are
  present today.  Not pursued.

## v3 vs TW: where we stand now

Per VM-vs-FFI breakdown: bridge=0.000, evHk=1, fcHk=0 — fib runs
purely in v3 with no TW fall-back.  v3 has overtaken TW because:

  - Every per-OP_CALL constant overhead has been driven to zero
    (cached env lookups, in-place arithmetic, deferred locals).
  - The bytecode shape (post-deferring) closely matches the
    operations TW does in its tree-walking loop, but at the level
    of densely-packed instructions instead of polymorphic AST node
    dispatch.
  - Most importantly: the TW dispatch goes through 30+ vtables and
    dynamic_cast (Expr::eval, Expr::maybeThunk per node), while
    v3's dispatch is a single switch on `op` with no virtual calls.

Next session's candidates if further gains are sought (none are
critical now that we're faster than TW on tight loops):

  1. Whole-real-world workload measurement — re-run nixpkgs
     eval-bind, cardano-node hello.name, all-packages.nix scan.
     Confirm parity / wins persist when bridge ≠ 0%.
  2. RSS (resident memory) measurement under the same workloads.
     The bridge load may have shifted; re-establish baseline.
  3. Tail-call detection for self-recursive numeric loops (fib's
     non-tail recursion is genuine, but ackermann's outer arm is
     tail-callable — would shrink ackermann further).
  4. OP_CALL closure-only fast path (skip primop / bridge / functor
     branches when the static lambda dispatch table proves the
     callee is a v3 closure).
  5. CallFrame size reduction — currently ~80 bytes; trim to
     ~24-32 by side-tabling cold fields.
  6. Direct threading (#543) — once dispatch overhead is the
     dominant cost again, which it currently isn't.
