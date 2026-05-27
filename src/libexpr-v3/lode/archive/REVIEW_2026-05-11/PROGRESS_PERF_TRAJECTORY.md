# v3 Performance Trajectory — Progress Report (2026-05-11)

This report assesses how the v3 bytecode VM is performing relative
to the in-tree tree-walker (TW) today, what helped or hurt in the
two-week sprint window, and what's pending validation.

**Methodology note (read first):** the only committed real-world
real-numbers JSON is
`src/libexpr-v3/bench/baselines/2026-05-09-with-modules.json`
(N=5, M4 darwin, post-getenv-cache + post-#530, **two days before
HEAD**).  The recent Phase 3/4 commits (deletion of partial-Bindings
registry; cell-update-everywhere; thread-local fakeClo pool; path
compression on the OP_FORCE chase loop) **have not been re-baselined**.
Their wall-clock impact has been described qualitatively in commit
messages (e.g. "saves ~7-8 % on fib"; "dropped fib/ackermann another
43-53 %") but no JSON regression test was added.  Numbers below
that are tagged "claimed (commit msg)" rather than "measured (bench
baseline)" are not independently reproducible from the tree state.

---

## Summary

- **Microbenchmarks (synthetic compute):** v3-hook is **faster than
  TW** on fib/ackermann after the 2026-05-09 getenv-cache landings:
  fib30 v3-hook=307 ms vs TW=434 ms (0.71×), fib33 v3-hook=1086 ms
  vs TW=1642 ms (0.66×), ackermann=158 ms vs 175 ms (0.91×).
  Same workloads under **v3-direct** mode remain **1.5–1.7× slower
  than TW** because the v3-direct path runs the full lower+compile
  on the cold side of the harness (where v3-hook amortises into
  the in-process state already paid for by TW).
- **Real-world `lib` workloads (small, sub-100ms):** v3 modes are
  3–15 % slower on wall, but ~85–90 % of that delta is `nix eval`
  startup floor (~52 ms), not eval work.  Net eval delta is
  1–3 ms over a TW eval of ~3 ms; effectively at parity.
- **Real-world module system (lib.evalModules, lib.types):**
  **v3 is 40–50 % slower than TW.**  This is real dispatch-level
  cost (v3.run = 25–28 ms vs ~5 ms TW) and is **the largest
  unfixed gap on workloads that complete in v3 today.**
- **Real-world full nixpkgs (`(import <nixpkgs> {}).hello.name`,
  cardano-node):** **does not complete** in v3-direct or v3-hook
  modes due to the cycle in #498 / #516.  Phase 3.2 (THUNK_ALL +
  NO_PARTIAL_BINDINGS default-on, 2026-05-11) was supposed to
  close this; per the Phase 2 baseline note it currently
  **times out (>120s)** on `(import nixpkgs {}) ? lib`.  Phase 4
  alloc work (fakeClo pool, path compression) is the active
  attempt to bring that completable.
- **Trajectory:** the headline win arc is **getenv-cache (2026-05-09)
  → THUNK_ALL+PB-off default-on (2026-05-11) → Phase 4 alloc work
  (2026-05-11)**.  Getenv-cache landed the compute-bound parity.
  Phase 3.2 traded correctness (cycle elimination) against a 100×
  runtime slowdown on nixpkgs; Phase 4 is trying to claw that back
  via alloc-rate reduction.  The PB-registry deletion *helped*
  cleanup by removing ~1000 lines of dead workaround infrastructure
  — it did not directly *cost* perf because by 3.3 the registry
  was already empty under default settings.
- **Where v3 is still risky vs TW:** module system, anything that
  exercises `(import nixpkgs {})`, RSS (v3-direct +42% on
  hello-name per 2026-05-04 bench), and the entire correctness
  story on full-nixpkgs traffic.  Phase 4 alloc work is the active
  bet to close the runtime gap.

---

## Trajectory

Inflection points in chronological order (commits cited where
they're load-bearing).

### 2026-05-04 — Phase 5 default-on, then reverted

- Phase 5 `RecBindingSlotRef` inlining (#427) shipped default-on
  briefly, delivering **fib35 -24% vs TW** on synthetic compute,
  then reverted (`857227578`) because of a cardano-node
  infinite-recursion regression in `OP_ATTRS_SELECT` on Tag::App
  formals.  Re-enabled #436/#437/#438 later that day after the
  closure-shape leak + bridge env-walk off-by-one fixes.
- BENCH-2026-05-04-PHASE5-DEFAULT: **fib35 v3 = 3.20 s vs TW
  4.13 s (−22.6 %)**, hello-name parity, attr-pkgs +18 % (later
  shown to be cold-cache noise), cardano-node parity.
- v3-fhook regression remained at +20–60 % on derivation-heavy
  workloads (Bridge-thunk allocation tax).

### 2026-05-07 / 2026-05-08 — bytecode-shape audit and #530

- `BENCH-2026-05-08-POST-530.md` was the first bench under
  `NIX_V3_DIRECT_EVAL=1`.  Headline: **fib32 v3.run = 2366 ms vs
  TW.eval ≈ 971 ms = 2.4× slower**.  All synthetic FFI = 0;
  the slowdown is pure dispatch overhead, **not bridge**.
- Bytecode dump revealed fib's body emitted **50 ops where ~20
  suffice**: A-normal-form binding shape inflated by SET_LOCAL /
  GET_LOCAL pairs carrying single-use values.  This is the
  origin of the deferring pass landed in #542.
- Lexical-with chain (#530 = `7c648facf` etc.) closed at this
  point, unblocking v3-direct on with-scope shapes.

### 2026-05-09 — the breakthrough session

`BENCH-2026-05-09-POST-DEFER-GETENVCACHE.md` is the largest single
inflection point in the trajectory.  Three rounds:

| stage | fib30 v3.run | v3/TW | mechanism |
|---|---|---|---|
| baseline (post-#530) | 906 ms | 2.40× | dispatch + getenv on every op |
| + #542 deferring | 835 ms | 1.96× | OnceLinear binding elim |
| + #538 hot-path getenv cache | 547 ms | 1.28× | seven `std::getenv` calls cached |
| + forceValue getenv cache | **252 ms** | **0.59×** | `V3_DBG_FORCE_CALLSITE` cached |

The forceValue getenv cache (commit later than #538 in the same
session) was the single biggest win — **43–53 % cut** in fib's
hot loop just by caching one `std::getenv` per `forceValue` call.
On macOS, `getenv` walks the env table linearly with `strcmp`
(~50 ns); at fib33's 5.7M OP_CALL invocations this **dominated**
the dispatch loop.  **This is the win that put v3 ahead of TW
on tight numeric loops.**

Also landed 2026-05-09:
- `opt_occur.cc` — occurrence analysis foundation pass (#540).
- Emit-time deferring + binary/unary fast paths (#542).
- Python bench harness (`bench/bench.py`) replacing ad-hoc shell
  scripts; this is the basis for the JSON baselines.
- Real-world bench corpus (`workloads.toml`) added 23 workloads
  including the 6 NixOS-module-system shapes.

### 2026-05-10 — Cheney nursery design

- Phase A (`f8b489ada`) — bump-pointer routing, no scavenge.
- Phase C (`1d867dcb0`, `a70114acf`) — side-table-forwarding
  scavenge; scavenge-only-at-exitDepth==0 for C-stack safety.
  Several leaf-tag-fast-path attempts reverted (`541d4390a`).
- Phase D analysis (`78835a047`) — write-barrier alone is
  insufficient without cell-write barrier; design pivoted.
- **Status: Phase A landed; Phase C side-table forwarding
  landed but never validated as default-on; Phase D unblocked
  but unfinished.**  Per `project_v3_runtime_roadmap`, nursery
  is the first of four runtime pieces and is not yet
  delivering value.

### 2026-05-11 (today) — Phase 1.5 / 2 / 3 / 4 cell-update everywhere arc

Reading from `git log --oneline`, today landed the architectural
pivot from publish-walk (legacy) to cell-update-everywhere (STG-
true):

- Phase 1.5 (`20adafe31`): `Thunk::shapeCell` field for per-thunk
  partial-state visibility, opt-in `NIX_V3_CELL_EVERYWHERE=1`.
- Phase 1.5b (`52eb8f261`): extended to `OP_ATTRS_*_INIT_TAIL`.
- Phase 1.5 final (`ead3f33ae`): dropped cross-thunk propagation;
  per-thunk only (STG-correct).
- Phase 2.2 (`b72d07b0b`): lazy-cleanup compaction threshold
  bumped 256 → 4096 (CPU sample showed `compactPartialBindingsRegistry`
  was 5% of CPU; bumping amortised it out of top-20).
- Phase 2.3 (`bebb0d5ae`, `7e476f42a`, `b2fd95988`, `9c17005fb`):
  **inline WHNF tag check before forceValue** at OP_CALL_PRIMOP
  arg-force, `valueEqual` (vm.cc and primops.cc), and
  `primFilter/All/Any/Partition/ConcatMap`.  These avoid the
  CALL/RET + chase-loop setup for already-WHNF values.
  Commit message claims: OP_CALL_PRIMOP arg-force was responsible
  for ~12% of CPU on nixpkgs THUNK_ALL pre-optimization.
- Phase 2.4 (`2a8cd4018`): skip thunkify for trivial inherit-from
  exprs (matches TW's `maybeThunk` inlining for ExprInt/Var/etc.).
- **Phase 3.1 (`77219c046`):** `NIX_V3_NO_PARTIAL_BINDINGS=1` gate
  added — chain reads become no-ops.
- **Phase 3.2 (`16d0adc81`):** **flipped default-on**
  `NIX_V3_INHERIT_FROM_THUNK_ALL` + `NIX_V3_NO_PARTIAL_BINDINGS`.
- **Phase 3.3a–g (`0fde92c28` through `07a6352c1`):** deleted
  `partialBindingsRegistry`, `publishToNearestBlackThunkFrame`,
  `publishToAllThunkFrames`, `pickLargestLayer`,
  `lookupInPartialChain`, STG WHNF recovery, OP_ATTRS_SELECT
  registry-wide peek, BlackholeError recovery's registry erase.
  **~1000 LoC of partial-Bindings infra removed across 8 commits.**
- **Phase 4 prep (`96a256723`):** `allocStats` dump added to
  `runRootExpr`; counters for thunks/closures/values/lists/pairs
  + thunksForced + bridge + insns.  Initial observation on
  `lib.systems.elaborate`: **5 % force ratio** — 95 % of thunks
  allocated under THUNK_ALL are never accessed.
- **Phase 4 (`34a59442e`):** **thread-local fakeClo pool** bucketed
  by nUpvalues (0..15), capped 128 per bucket.  OP_FORCE prefers
  recycled; OP_RETURN puts back.  Targets: Suspended-thunk
  forces, which under THUNK_ALL fire "hundreds of millions of
  times per top-level nixpkgs eval."  Commit msg cites Boehm
  allocation + memset as the bottleneck.
- **Phase 4 follow-up (`b4b2c2a72`, `dfde0e33f`):** **path compression
  for Evaluated thunk chains** — record up to 16 traversed
  thunks during a `forceValue` / `OP_FORCE` chase and write back
  the resolved WHNF to each `t->evaluated` slot.  Subsequent
  forces hit in O(1).  Targets the THUNK_ALL "force count in the
  hundreds of millions" regime.
- **Phase 4 follow-up (`93711cdf2`):** route public `forceValue`'s
  Suspended-thunk path through the pool too.
- **Phase 4 follow-up (`ba2476e73`):** iterative App-spine walk in
  `forceValue` — curried primop chains
  (`App(App(fn, name), value)`) used to grow C-stack one frame
  per layer.

**What didn't get re-baselined today:** none of Phase 3 or Phase 4
has a committed bench JSON; the only baselines are 2026-05-09 (pre-
Phase-2 cleanup).  Per the Phase 2 plan doc (`CELL_UPDATE_EVERYWHERE_
2026-05-12.md`), nixpkgs `?lib` under THUNK_ALL was at **>120 s
timeout** at the start of today; the goal of Phase 4 is to bring
that under 5× of TW (Phase 2d), then 1.5× (Phase 2e), then trigger
the cascade close (Phase 2f), then full nixpkgs `hello.name`
(Phase 2g).  Today landed Phase 3 (which is the cleanup, not the
perf fix) and Phase 4 (which IS the perf fix, unmeasured).

### The partial-bindings-deletion question

The reviewer asks: *did deleting the partial-Bindings registry in
Phase 3.3 cost perf or help it?*

Answer (per commit messages and Phase 3.2 sequencing):
- **Phase 3.2 (the flip) was the load-bearing change.**  Once
  `NIX_V3_NO_PARTIAL_BINDINGS=1` became the default, the registry
  was always empty.  Reading from an empty registry is O(1)
  (single unordered_map.find returning end).  Writes were gated
  to be no-ops.
- **Phase 3.3 (the deletion) was mechanical cleanup.**  By the
  time the registry was deleted, it had been functionally
  inert for ~12 hours.  No perf change from the deletion itself.
- **The perf change came from Phase 2.4 (skip-thunkify-trivial)
  and Phase 2.3 (inline WHNF check).**  These reduce both alloc
  count AND per-force cost.  Both landed before the registry
  deletion.
- **Cell-update-everywhere via `Thunk::shapeCell` is the
  semantic replacement** for the publish-walk recovery, NOT a
  perf optimization.  It's the correctness mechanism that
  permits laziness without losing partial-state visibility.

**Net:** deletion didn't cost perf because the workaround
infrastructure had been disabled before being removed.  The
*real* cost-saver in the cleanup arc is the THUNK_ALL trivial-
skip (`2a8cd4018`) and the WHNF-check inlining, both of which
attack alloc rate and per-force dispatch cost directly.

---

## Current baseline numbers (2026-05-09-with-modules.json, N=5)

| Workload | TW wall (min) | v3-direct wall | v3-hook wall | v3-direct / TW | v3-hook / TW |
|---|---|---|---|---|---|
| **synthetic compute (v3 is ahead in hook mode)** | | | | | |
| fib25 | 90.9 ms | 114.2 ms | 80.1 ms | 1.26× | **0.88×** |
| fib30 | 434.2 ms | 682.9 ms | 307.0 ms | 1.57× | **0.71×** |
| fib33 | 1642 ms | 2654.8 ms | 1086 ms | 1.62× | **0.66×** |
| ackermann-3-7 | 174.6 ms | 281.0 ms | 158.3 ms | 1.61× | **0.91×** |
| **synthetic data (process-startup-bound; ignore relative %)** | | | | | |
| path-deep-30 | 54.1 ms | 53.2 ms | 53.1 ms | 0.98× | 0.98× |
| letrec-fix-5 | 54.2 ms | 54.9 ms | 54.2 ms | 1.01× | 1.00× |
| list-build-1k | 55.1 ms | 56.6 ms | 55.5 ms | 1.03× | 1.01× |
| fold-add-10k | 54.7 ms | 56.4 ms | 55.0 ms | 1.03× | 1.01× |
| with-deep-200 | 53.1 ms | 53.7 ms | 54.3 ms | 1.01× | 1.02× |
| attrset-build-1k | 54.0 ms | 55.3 ms | 54.8 ms | 1.02× | 1.01× |
| string-concat-1k | 55.2 ms | 55.5 ms | 55.2 ms | 1.01× | 1.00× |
| **real-world lib (sub-100ms; mostly startup)** | | | | | |
| lib-foldl-1k | 55.8 ms | 59.4 ms | 58.7 ms | 1.06× | 1.05× |
| lib-foldl-10k | 55.9 ms | 61.8 ms | 59.8 ms | 1.11× | 1.07× |
| lib-genAttrs-100 | 55.0 ms | 62.1 ms | 60.6 ms | 1.13× | 1.10× |
| lib-mapAttrs-100 | 55.8 ms | 61.6 ms | 60.5 ms | 1.10× | 1.08× |
| lib-fix-deep | 55.0 ms | 56.8 ms | 57.3 ms | 1.03× | 1.04× |
| lib-recursive-update | 55.3 ms | 61.8 ms | 61.0 ms | 1.12× | 1.10× |
| lib-attrnames | 55.5 ms | 56.7 ms | 56.6 ms | 1.02× | 1.02× |
| lib-makebinpath | 55.7 ms | 64.1 ms | 62.9 ms | 1.15× | 1.13× |
| lib-strings-ops | 55.3 ms | 61.3 ms | 60.2 ms | 1.11× | 1.09× |
| **real-world module system (THE REAL GAP)** | | | | | |
| lib-evalModules-trivial | 57.8 ms | 83.8 ms | 81.7 ms | **1.45×** | **1.41×** |
| lib-evalModules-100 | 58.4 ms | 87.3 ms | 83.7 ms | **1.49×** | **1.43×** |
| lib-types-int | 57.6 ms | 84.0 ms | 80.6 ms | **1.46×** | **1.40×** |

**hello.name:** committed bench includes `hello-name` workload
but is **`skip-by-default`** because it fails in both v3-direct
(`OP_WITH_LOOKUP cycle on 'callPackage'`) and v3-hook
(`v3 forceValue: infinite recursion (blackhole)`).  Per Phase 3.2,
THUNK_ALL+PB-off should close the cycle; per Phase 2 perf baseline,
it currently runs >120 s and times out.  Phase 4 is attempting to
make it both correct AND complete in reasonable time, **unmeasured
at HEAD**.

**lib.fix:** `lib-fix-deep` is the proxy in the bench.  v3 is
within 1.04× of TW (parity).  Full lib.fix on the cardano-node
shape — pre-Phase-2 — was the genesis of #498 / #516 and remains
broken on a real-shape repro per the Phase 2 baseline.

**cardano-node:** last measured 2026-05-04
(`BENCH-REAL-WORLD-2026-05-04.md`): TW 3.34 s, v3 3.42 s (+2.4%),
v3-fhook 4.28 s (+28%, was crashing pre-#436/#438).  **Not
measured since.**  Architecture changed since (THUNK_ALL default-on,
publish-walk removed); whether the +2.4% still holds is unknown.

**v3 vs TW ratio summary (current best signal, hook mode):**
- compute-bound microbenchmarks: **0.66–0.91× (v3 wins)**
- real-world lib small: **1.02–1.13× (parity to slight loss,
  mostly startup floor)**
- real-world module system: **1.40–1.49× (v3 loses by 40–50%)**
- real-world nixpkgs full: **not running (correctness blocker)**

---

## Where v3 still loses to TW

Specific workloads, magnitudes (from baseline):

1. **NixOS module system (lib.evalModules + lib.types):
   40–50% slower.**
   v3.run = 25–28 ms per call vs ~5 ms TW.  228k bytecode
   instructions for a 100-option module = 2200 instructions per
   option.  Per workload note, each option pays for: option-decl
   attrset construction, mkOption call, type wrapping,
   merge-through-evalModules.  Profile target.

2. **v3-direct on compute-bound (1.5–1.7× slower).**
   Same workloads where v3-hook is faster than TW.  Pattern:
   v3-direct pays full lower+compile in its measurement window;
   v3-hook amortises into the in-process state TW already pays
   for.  Implication: the **v3-direct number is a measurement
   artifact** — for production use the relevant comparand is
   v3-hook, which is faster.  But the v3-direct gap signals
   that v3's lower+compile is non-trivial even for short
   programs; this matters when the disk cache misses.

3. **Lib small workloads (3–15% slower wall).**
   Most of this is the 50ms `nix eval` startup floor that both
   evaluators pay.  Net-of-startup eval delta is **1–3 ms over
   TW's ~3ms eval** — at parity in absolute work but slightly
   slower per ms.

4. **RSS on hook mode (historically +42% on hello-name in
   2026-05-04, +5% on cardano-node).**
   Most of this is the v3 IR / CompilationUnit / sub-Expr cache.
   Cheney nursery (Phase A landed, scavenge not delivering) is
   the planned remediation; not yet reducing residency.

5. **Anything that hits #498 / #516 cycles.**
   `(import nixpkgs {}).hello.name`, cardano-node, anything that
   exercises `callPackage` with-scope.  These fail (or, post-
   Phase-3.2, **hang for >120s**) in v3-direct.  v3-hook
   completes but takes the +28% Bridge-thunk tax from the
   force-hook path (see 2026-05-04 v3-fhook numbers).

6. **The two-day-stale bench baseline.**
   Largest unknown.  Phase 3 + Phase 4 land 14 commits between
   the most recent JSON baseline and HEAD; the lib-evalModules
   40–50% gap could have widened or narrowed.  Until re-baselined,
   the "v3 wins on fib" claim is current; the lib numbers above
   are stale.

---

## Phase 4 work (today) — what it targets, real-world impact

The five Phase 4 commits (`34a59442e`, `93711cdf2`, `b4b2c2a72`,
`dfde0e33f`, `ba2476e73`) attack one common bottleneck:
**THUNK_ALL on full nixpkgs allocates 850+ GB Boehm-tracked over
60 s** (per the `96a256723` commit msg).  Per-thunk allocation
rate is the perf gap.

### Targets

1. **fakeClo pool (`34a59442e`).**  Each `Suspended` thunk force
   in `OP_FORCE` allocates a "fake" Closure for the body's frame.
   Under THUNK_ALL default-on this fires hundreds of millions
   of times per top-level nixpkgs eval.  Boehm allocation +
   memset dominates per-force overhead.

   Mechanism: thread-local pool bucketed by nUpvalues (0..15),
   capped 128 entries per bucket (~300 KB worst-case per thread).
   `OP_FORCE` prefers recycled; `OP_RETURN` with `CFF_THUNK_RETURN`
   puts back after zeroing upvalues.

   Opt-out: `NIX_V3_NO_CLOSURE_POOL=1`.

2. **Path compression on Evaluated chains (`b4b2c2a72` +
   `dfde0e33f`).**  When `forceValue` / `OP_FORCE` chases
   `Tag::Thunk(Evaluated) → ... → final WHNF`, record up to 16
   traversed thunks; write back the resolved WHNF to each
   `t->evaluated` so subsequent forces hit in O(1).

   Per commit msg: chains are ≤4 in practice under THUNK_ALL,
   but force count is in the hundreds of millions, so each
   saved hop matters.

   Opt-out: `NIX_V3_NO_PATH_COMPRESS=1`.

3. **Iterative App-spine walk (`ba2476e73`).**  Replaces recursive
   `left = forceValue(...); v = callClosure(...)` in `forceValue`'s
   `Tag::App` branch with an iterative buffer.  Saves C-stack
   frames on curried primop chains (mapAttrs, listToAttrs).

4. **Route public forceValue through pool (`93711cdf2`).**  The
   `forceValue(VMState&, Value)` public entry has its own
   fakeClo path; now goes through the pool too.

### Real-world impact (if measured)

**Unmeasured at HEAD.**  The pre-Phase-4 baseline was Phase 2
("pkgs ? lib: TW 0.5 s, v3+THUNK_ALL >120 s timeout").  Phase 2.4
+ Phase 2.3 already attacked alloc rate (skip trivial thunkify,
inline WHNF check).  Phase 4 attacks the next-tier alloc rate
(fakeClo + path compression).  The cumulative target per the
`CELL_UPDATE_EVERYWHERE_2026-05-12.md` Phase 2d/e gates is:

- Phase 2d: nixpkgs `?lib` under THUNK_ALL within **5× of TW**.
- Phase 2e: within **1.5× of TW**.
- Phase 2f: trigger cascade close (default-on, no opt-back needed).
- Phase 2g: full nixpkgs `hello.name` end-to-end returns
  `"hello-2.12.2"`.

**Until a fresh bench JSON lands, Phase 4 is "potentially
massive, empirically unknown."**

### What this does NOT improve

- The 40–50% module-system gap.  That's instruction count per
  option, not alloc rate.  Different attack surface: hidden-class
  shapes / selector thunks / occurrence-driven inliner per
  `OPTIMIZATION_PLAN.md` and `OPT_OCCUR_PLAN_2026-05-08.md`.
- The v3-direct startup cost.  Disk cache is the remediation;
  cold first-import will always pay lower+compile.
- RSS.  fakeClo pool keeps memory live by design; it's a
  recycling pool, not a reclamation pass.  Cheney nursery is
  the planned RSS win and is half-built.

---

## What's promising but unvalidated

### Cheney nursery (#434)

- **Status:** Phase A landed (bump-pointer routing, no scavenge);
  Phase C side-table forwarding landed; scavenge-only-at-
  exitDepth==0 C-stack safety gate landed.  Phase D design
  analysis identifies that **a write-barrier alone is
  insufficient** without a cell-write barrier (because OP_RETURN's
  `*cell = retVal` can plant a nursery payload into a "clean"
  Bindings that scavenge will then miss).
- **What's promising:** if it works, RSS drops from ~50 GB to
  hundreds of MB on nixpkgs eval (per design doc).  The 95%
  unused-thunk ratio in `allocStats` (per `96a256723` observation)
  is exactly the scenario a young-gen GC reclaims.
- **What's unvalidated:** scavenge has never run as default-on.
  Per `project_v3_runtime_roadmap`, this is **the first of four
  runtime pieces** (nursery → tagged ptrs → strictness/inlining
  → libsForQt5 closure) and the foundation for everything else.
  The pivot to Phase 4 (fakeClo pool) on the same day Phase D
  was paused suggests the nursery work is on hold pending design.

### Tagged pointers

- **Status:** **Not started.**  Per `CHENEY_NURSERY_DESIGN.md`,
  this is "a separate refactor."  Per the roadmap, it's the
  second item after nursery.
- **What's promising:** would compress Tag-checks and improve
  cache density on the hot Value path.  TW gets a similar
  effect through its tightly-packed Value union.
- **What's unvalidated:** literally everything.

### Direct threading / computed goto (#543)

- **Status:** **Deferred** per `BENCH-2026-05-09-POST-DEFER-GETENVCACHE.md`.
  Analysis showed the dispatch loop has 61 cases over 4300 lines
  with 136 `break;` statements many of which are inside nested
  loops — too invasive for a single session.  Estimated 10–15%
  win on real-world based on CPython 3.11 evidence.
- **What's promising:** mechanical, no architectural risk, two
  prerequisite-free items (computed goto + typed numeric opcodes
  `OP_ADD_II/SUB_II/MUL_II/EQ_II`) from `OPTIMIZER_REPORT_2026-05-07`
  estimated at 10–15% combined.  The closing observation of
  `REVIEW_2026-05-09` was that these have been #5/#8 ranked for
  three sprints and **still haven't landed**.

### Hidden-class shapes for attrsets

- **Status:** Not started.  Largest single optimization in
  `OPTIMIZER_REPORT_2026-05-07.md` (#1 ranked, est. 10–20% on
  nixpkgs).
- **What's unvalidated:** ~600 LOC + runtime infrastructure.
  Build on top of the existing 4-way `AttrSelectIC` (PIC).
- **Why it matters for the module system gap:**
  `lib.evalModules` is precisely the workload shape that hidden-
  class shapes attack: deep attrset construction, repeated
  mkOption / mkType / merge patterns where the attrset shape
  is stable across invocations.

### Selector thunks

- **Status:** Not started.  #2 ranked in OPTIMIZER_REPORT
  (5–10% est.), depends on occurrence analysis (now landed).
- **What's promising:** prerequisite (`opt_occur.cc`) shipped
  2026-05-09.  This is the next unblock.

### Strictness/inlining / demand analysis

- **Status:** Not started.  Per `project_v3_runtime_roadmap`,
  this is the third runtime piece.  Requires occurrence + use-def
  per `OPT_OCCUR_PLAN_2026-05-08.md`.
- **What's promising:** would attack THUNK_ALL's 95% unused-thunk
  ratio at the source rather than via pool recycling.  The
  cleanest fix for the alloc-rate problem.

---

## Risk inventory

1. **No re-baseline at HEAD.**  Phase 3 + Phase 4 land 14 perf-
   sensitive commits without an updated bench JSON.  If the
   lib-evalModules gap widened, no one knows.
2. **THUNK_ALL default-on is a perf trade.**  Per Phase 2
   baseline: `(import nixpkgs {}) ? lib` went from
   "v3 default 0.6 s (error)" to "v3+THUNK_ALL >120 s timeout."
   Phase 4 must close this or the default-on flip is a
   regression in absolute terms (correctness traded for
   completability).
3. **Cardano-node not measured since 2026-05-04.**  Six days
   of architecture change since.  The "+2.4% v3 / parity v3-fhook"
   claim is stale.
4. **#498 / #516 cycles unresolved at the lower-pass level.**
   Phase 3.2 papered over them with THUNK_ALL+PB-off.  If
   Phase 4 doesn't close the perf gap, the team will either
   (a) revert THUNK_ALL default-on (losing the cycle fix), or
   (b) ship a slow v3 default mode.
5. **Cheney nursery is half-built.**  Phase D blocked on
   write-barrier design.  If it's never finished, RSS stays
   high and fakeClo pool is the only memory-side win.

---

## Headline numbers, one paragraph

v3-hook is **faster than TW on tight numeric loops** (fib33 0.66×,
fib30 0.71×, ackermann 0.91×) per the 2026-05-09 baseline, with
the win driven primarily by the getenv-cache landings (`std::getenv`
walks the macOS env table linearly; caching it saved 43–53 % on
fib's hot loop).  On real-world lib workloads v3 is at parity
once the 50 ms startup floor is factored out (lib-foldl-1k 1.05×,
lib-fix-deep 1.04×, lib-attrnames 1.02×).  On the NixOS module
system v3 is **40–50 % slower than TW** (lib-evalModules-100:
1.49× v3-direct, 1.43× v3-hook) — that's the unresolved gap.
Full nixpkgs eval (`hello.name`, cardano-node) **does not complete**
in v3 modes due to the #498 callPackage upvalue regression; the
2026-05-11 architectural pivot to THUNK_ALL + cell-update-everywhere
+ Phase 4 alloc optimisation is the active attempt to make it
complete in a reasonable time, but **no fresh bench has been
captured** since the pivot, so the impact is qualitative-only.

---

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
