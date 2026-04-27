# v3 Optimization Plan (post-parity, October 2026)

This is a synthesis of four parallel research investigations into where the
v3 evaluator can still be made faster, after reaching synthetic+real-world
parity with the tree-walker.  Each finding is critically reviewed and ranked
by leverage.

## Headline data (5-second sample on `nix-instantiate --eval --strict --expr <25k-pkg-scan>`)

| Metric                  | tree-walker | v3 (NIX_USE_V3=1) |
|-------------------------|-------------|-------------------|
| user CPU                | 9.06s       | 8.95s             |
| wall                    | 10.68s      | 7.78s             |
| peak RSS                | 223 MB      | 223 MB            |
| dominating call frames  | tree-walker | **tree-walker**   |

v3 produces byte-identical results but its bytecode VM does not appear in the
hot path on real-world workloads.  This is the headline finding.

## Critical insights

### #1 — Cutover scope is the dominant problem.

`EvalState::v3EvalHook` (`src/libexpr/eval.cc:1212`) only catches the
**outermost** `EvalState::eval(Expr*, Value&)` call.  Once v3 produces a
result, every subsequent forcing inside tree-walker primops uses
`forceValue` (`src/libexpr/include/nix/expr/eval-inline.hh:96-149`,
specifically line 110: `expr->eval(*this, *env, v)`) which dispatches
directly to `ExprX::eval` virtual methods, bypassing the hook.

**Implication:** all VM-internal optimizations (NaN-boxing, computed-goto,
arena allocator, register VM) are diminishing returns until v3 owns more of
the evaluation path.  Optimizing a code path that runs <5% of the time is
not the leverage point.

### #2 — `derivationStrict` is the second biggest leak.

Every `mkDerivation` in nixpkgs evaluation crosses the v3↔tree-walker
bridge through `primDerivationStrict` in `src/libexpr-v3/primops.cc:2302`.
For a 30-attribute derivation, the bridge round-trip is roughly:
- `v3ToTreeWalker`: ~6–12 µs (recursive recreate of the input attrs)
- tree-walker's `derivationStrict`: real cost (hash, store path)
- `treeWalkerToV3`: ~10 µs (sort dominates)

A nixpkgs-wide eval with ~10 000 derivations spends ≈ 100 ms in bridge
overhead alone, plus the cascading tree-walker primop calls on the result
graph.

### #3 — Memory parity is misleading.

v3's malloc-based allocator and tree-walker share the same Boehm GC arena
(by virtue of the bridge).  We measure 223 MB peak on both; this is
mostly dominated by the imported nixpkgs AST + cache, not v3's own heap.
v3-only allocation patterns (`allocBindings`, `allocList`, `allocClosure`,
`allocThunkSuspended`) are roughly:

| Type         | size header + tail           | typical/eval |
|--------------|------------------------------|--------------|
| Value        | 16 B                         | many         |
| ListVec      | 8 + 16 N                     | medium       |
| Bindings     | 8 + 24 N                     | medium       |
| Closure      | 24 + 16 N                    | many         |
| Thunk        | 32 + 16 N                    | many         |
| Env          | 16 + 16 N                    | few (let/with only) |

These all go through `std::malloc`.  An arena would help — but only after
issue #1 is fixed; otherwise v3's allocator is rarely hot.

### #4 — Computed-goto is not the bottleneck right now.

Synthetic fib33 shows ~30% of time in instruction-fetch and ~15% in switch
dispatch.  Computed-goto would yield 5–10% on dispatch-bound code.  But on
real nixpkgs evaluation, dispatch is invisible (everything is in
tree-walker).  Implementing computed-goto without first fixing the cutover
optimizes the wrong code.

## Critical review of the four research reports

The cutover analysis (Option 1 — patch `forceValue` to consult a
process-global v3 cache before calling `expr->eval`) is the most concrete
and lowest-effort path to actually moving the CPU bar.  But the reports
under-stated one risk: the v3 cache is keyed by `Expr*`, and the v3
compilation pipeline only compiles whole expressions, not arbitrary
sub-Exprs.  When `forceValue` is called on a thunk holding `ExprSelect *`,
the `ExprSelect` itself isn't a thing v3 has seen — only its parent was
lowered.  We need to either (a) lower at finer granularity, or (b) ensure
the cache also accepts "this Expr is part of CompilationUnit X with entry
offset Y" so `forceValue` can resume v3 mid-program.

The allocator agent's NaN-boxing analysis is solid but the call-site churn
estimate (~200 sites) is optimistic.  Every `v.payload.X` access in
`vm.cc`, `primops.cc`, `cli/v3-eval.cc`, `v3_hook.cc` would need a macro.
The bigger issue is that NaN-boxing forces float values to be stored as
NaN payloads, costing precision unless we heap-box doubles — which
defeats the win.  Stick with arena + polymorphism first.

The bridge-cost report's 100 ms estimate for derivationStrict overhead is
ballpark-reasonable but should be measured before committing 3–4 weeks to
native re-implementation.  Add per-primop counters first.

## Prioritized plan

The optimizations cluster into four phases.  Each phase should be
**measured before the next is started** — the leverage hierarchy is steep
and skipping the measurement step risks optimizing the wrong layer.

### Phase 1 — Cutover correctness (highest leverage, ~1–2 weeks)

The single biggest gain: make v3 actually run the bulk of nixpkgs
evaluation.  Current state: ~5% of CPU is in v3.  Target: >50%.

- **CO-1.** Add a public `v3CacheLookup(const nix::Expr *)` symbol exported
  from `libnixexprv3` (extends `v3HookCache` in `src/libexpr-v3/v3_hook.cc`).
  Returns `(CompilationUnit*, entryOffset)` or null.
- **CO-2.** Modify `EvalState::forceValue` (`src/libexpr/include/nix/expr/eval-inline.hh:96-149`)
  so that before calling `expr->eval(*this, *env, v)` (line ~110) it
  consults `v3CacheLookup`.  If a hit, run v3 from that entry offset and
  fill `v` via the bridge.
- **CO-3.** Extend the v3 lowering pipeline to accept *any* `Expr*` as an
  entry point and pre-populate the cache during the initial lower.  Each
  reachable sub-Expr gets a (CU, offset) record so `forceValue` can resume.
- **CO-4.** Add a runtime counter pair (`v3ForceHits`, `v3ForceMisses`) to
  validate the hook is actually being taken.  Print via `NIX_VM_STATS=1`.
- **CO-5.** Re-profile the 25k-package nixpkgs scan; v3 functions (like
  `dispatchLoop`, `OP_FORCE`, `OP_GET_LOCAL_FORCE`) should now appear in
  the hot path.  Compare wall-clock and user CPU vs the baseline above.

Stop here if Phase 1 does not move the needle.

### Phase 2 — Bridge reduction (medium leverage, ~3–4 weeks)

After Phase 1, derivationStrict is the next biggest leak.

- **BR-1.** Per-primop call counters + microbench harness.  Concrete:
  add `Alloc::primCallCounters[]` to `src/libexpr-v3/include/v3/primop.hh`,
  bumped on every entry, dumped under `NIX_VM_STATS=1`.
- **BR-2.** Validate that derivationStrict is the dominant bridging primop
  on a real nixpkgs target (`hello`, `git`, `vim`).  Confirm or refute the
  100 ms estimate.
- **BR-3.** Native `derivationStrict` in v3.  Mirrors `nix::Derivation`,
  uses libnixstore for `hashDerivationModulo` + path construction.
  Removes the round-trip through `treeWalkerToV3`.  This is the 3–4 week
  block.
- **BR-4.** Native `builtins.path` (NAR hash); only worthwhile if BR-3
  shows real savings.

### Phase 3 — VM internals (low–medium leverage, ~1–2 weeks)

Now that v3 owns the hot path, the dispatch/allocator wins are visible.

- **VM-1.** Frame pre-allocation: replace `vm.valueStack.resize(newBase + nLocals)`
  in `OP_CALL` (`src/libexpr-v3/vm.cc:769`) with a bump-pointer over a
  pre-reserved 16k-slot slab.  Eliminates the per-call zero-fill.
- **VM-2.** Polymorphic `Bindings` (the design doc's plan): inline-store
  size-1 and size≤8 entries directly in the header, skip the FAM
  allocation.  Should drop ~20–30% of attrset allocations on nixpkgs.
- **VM-3.** Per-EvalState arena allocator for `Bindings` + `ListVec` +
  `Env`.  Allocations that don't escape into tree-walker stay in the arena
  (deallocated at eval-end).  Tracked via `allocStats`.
- **VM-4.** Bytecode disk cache (mirroring v2's mechanism in
  `src/libexpr/eval.cc`); serializes `CompilationUnit` keyed by source-file
  hash.  Real win on repeated CLI invocations, not on a single eval.

### Phase 4 — Architectural (defer until Phase 1–3 are exhausted)

These are large, risky changes.  Do not attempt without measurement
showing they're warranted.

- **A-1.** Computed-goto dispatch — 5–10% gain on dispatch-bound code,
  ~300 LOC change.  Postpone until opcode set is frozen.
- **A-2.** NaN-boxing throughout (16 B → 8 B Value).  Massive ABI change
  (~200 call sites) with float-precision cost.  Only if profiling shows
  Value cache footprint is the problem.
- **A-3.** Register-VM redesign (slot-based, no operand stack).  This is
  v4 territory.  Skip in v3.

## Concrete first step

Phase 1 / CO-1 + CO-2.  Hooking `forceValue` is mechanically small
(~50 lines in libnixexpr + ~30 lines exporting from libnixexprv3) and
will tell us within a day whether v3 can actually move the CPU bar on
nixpkgs.  Only after measuring Phase 1's impact should we commit time to
the bigger items.

## 2026-04-27 diagnosis update

While exploring CO-1/CO-4 (counter-based validation), discovered the
"v3 is at parity with tree-walker" claim was based on output equality
rather than evidence the hook was firing.  Concrete findings:

1. **libnixexprv3 was being silently dropped by the linker.**  macOS
   `-dead_strip_dylibs` removes shared libs with no direct symbol
   reference from the binary.  The static initializer that sets
   `EvalState::v3EvalHook = &v3EvalEntry` was the only consumer, and the
   linker can't see static-init code as "used".  Result: v3EvalHook
   stayed `nullptr` and every `EvalState::eval` fell through to
   tree-walker.

2. **An explicit installer fixed the linking** (added
   `nix::v3::installEvalHook()` called from `nix::mainWrapped`).  After
   this, the hook IS called.  But: `nix-instantiate --eval --strict
   --expr '1'` SEGFAULTS with v3.

3. **The Expr that nix-instantiate hands to EvalState::eval is wrapped.**
   Even for `--expr '1'`, the parsed AST is wrapped (via getAutoArgs
   bindings + autoCallFunction's lambda machinery) and v3's
   `lowerNixExpr` produces 273 instructions across 13 lambda functions.
   `run()` returns `Tag::Closure` because the top-level entry function
   *constructs* a closure that needs to be called with autoargs — it
   doesn't evaluate to the literal value directly.

4. **v3ToTreeWalker has a bug bridging the resulting closure.**  The
   crash is inside the bridge converter when handling `Tag::Closure` at
   nix-instantiate's call shape.

The implication: **the cutover is more broken than measured tests
showed**, because tree-walker producing the right answer for
`NIX_USE_V3=1` was a false positive.  All the `nix-instantiate`-based
"cutover" tests were running tree-walker, not v3.

Concrete state (rolled back to known-good):

- The installer + main.cc reference are removed (the linker drops the
  v3 lib again, hook stays null, tree-walker handles everything).
- 142/142 standalone v3-eval tests still pass — v3 itself is correct
  on direct inputs.
- 142/142 "cutover" tests still pass — but they're really tree-walker
  tests, not v3-cutover tests.

Phase 1 is **not** "1–2 weeks".  Realistic estimate for actually
shipping the cutover:

- CO-1 (link the lib): trivial, but the **installer + main.cc edit
  must be re-applied** once v3 can actually run nix-instantiate's
  wrapped Exprs.
- CO-2 (hook forceValue): blocked on CO-3.
- CO-3 (handle wrapped Exprs / autoargs / autoCallFunction): the
  real work.  v3 needs to either: (a) handle the autoargs lambda
  pattern in lowerNixExpr, (b) recognise that the top-level returns
  a Closure that should be auto-applied, or (c) the v3ToTreeWalker
  bridge needs to package the v3 closure into a Tag::Lambda nix::Value
  that tree-walker's `autoCallFunction` can invoke.  Bug fix in the
  bridge plus a test case is the minimum.
- CO-4 (counters): trivial, already drafted.
- CO-5 (re-profile): can only run after CO-3 lands.

Best estimate: **3–5 weeks of focused work** on CO-3 alone, given that
shaking out wrapped-Expr edge cases tends to surface a long tail of
subtle issues (autoargs, recursive scope, closure-of-thunk shapes).
