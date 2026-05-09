# v3 perf — post-deferring + hot-path getenv cache (2026-05-09)

Date: 2026-05-09
System: aarch64-darwin (M4)
Comparand: in-tree TW (`build/src/nix/nix eval`)
Measurement tool: `src/libexpr-v3/test/bench-eval-only.sh` (V3_TIMING
phase split + NIX_VM_STATS hook entries to surface VM-vs-FFI
breakdown).  N=3, take min.

## Headline

|  Workload     | TW(s)  | v3.run | v3.run/TW | post-#530 | this session |
|---|---|---|---|---|---|
| fib30         | 0.43s  | 547ms  | **1.28x** | 906ms     | -39%         |
| fib33         | 1.62s  | 2306ms | **1.42x** | (extrap. 3786ms) | -39%  |
| ackermann-3-7 | 0.18s  | 190ms  | **1.08x** | (extrap. 480ms)  | -60%  |

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

4. **#538 follow-up — cache hot-path getenv calls** — seven
   diagnostic env-var lookups (`V3_DBG_FINAL_CALL`,
   `V3_DBG_OP_CALL_POST`, `V3_DBG_TC_PRE`, `V3_DBG_MAKE_PREV`,
   `V3_DBG_MAKE_RES`, `V3_DBG_MAKE_SUPER`, `V3_DBG_MAKE_SUPER_ALL`)
   were calling `std::getenv()` on every dispatch.  On macOS,
   `getenv` walks the process env table linearly with `strcmp(3)`
   (fast in absolute terms — ~50ns — but a death sentence at
   fib33's 5.7M OP_CALL invocations).  Converted to cached
   `static const bool` with `__builtin_expect(s_dbg_X, 0)
   [[unlikely]]`.  This is the single biggest win of the session.

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

## What's left for fib's remaining 1.3× gap

Per VM-vs-FFI breakdown: bridge=0.000, evHk=1, fcHk=0 — fib runs
purely in v3 with no TW fall-back.  The remaining gap is in
OP_CALL/OP_RETURN per-recursion overhead:

  - OP_CALL builds a fresh `CallFrame` (8 fields) and pushes it on
    `vm.frames`.  TW's `callFunction` is also frame-y but C++
    inlining + small-vector growth get more out of LLVM than our
    case body.
  - OP_RETURN does work to chase Evaluated-state thunks (rec-attr
    cycle handling) and `valueStack.resize(fStackBase)` even when
    the resize is a no-op.  Some of this is paid only by thunk
    frames; OP_CALL closure frames might benefit from a stripped
    return path.
  - Each force/get_local_force does a tag dispatch + branch even
    on already-forced values.

Next session's highest-ROI candidates (in priority order):
  1. Tail-call detection for self-recursive numeric loops (fib's
     non-tail recursion is genuine, but ackermann's outer arm is
     tail-callable).
  2. OP_CALL closure-only fast path (skip primop / bridge / functor
     branches when the static lambda dispatch table proves the
     callee is a v3 closure).
  3. CallFrame size reduction — currently ~80 bytes; trim to
     ~24-32 by side-tabling cold fields (caller cu, with-base,
     thunk pointer when nullptr).
  4. Direct threading (#543) — once dispatch overhead is the
     dominant cost again post-(1)(2)(3).
